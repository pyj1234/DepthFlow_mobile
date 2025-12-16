import os
import sys
import argparse
import json
import gc
import torch
import numpy as np
import torch.nn.functional as F
from pathlib import Path
from PIL import Image, ImageFilter
from torchvision.transforms.functional import normalize
from transformers import AutoModelForImageSegmentation, AutoImageProcessor
from diffusers import StableDiffusionInpaintPipeline

# --- 全局配置 ---
# 通过环境变量 DEPTHFLOW_MODEL_DIR 指定模型存放路径，默认为 ./models
MODEL_DIR = Path(os.environ.get("DEPTHFLOW_MODEL_DIR", "./models"))
# 全局模型缓存
_DEPTH_ESTIMATOR = None
_SEG_MODEL = None
_INPAINT_PIPE = None


# --- 模型加载函数 ---

def load_depth_estimator(model_name="depth-anything/Depth-Anything-V2-small-hf"):
    """加载深度估计模型"""
    global _DEPTH_ESTIMATOR
    if _DEPTH_ESTIMATOR is None:
        print(f"✨ Loading Depth Estimator: {model_name}...")
        try:
            _DEPTH_ESTIMATOR = AutoModelForImageSegmentation.from_pretrained(
                model_name,
                trust_remote_code=True,
                cache_dir=MODEL_DIR,
                local_files_only=False
            ).to("cuda" if torch.cuda.is_available() else "cpu")
            _DEPTH_ESTIMATOR.eval()
        except Exception as e:
            print(f"❌ Failed to load depth estimator: {e}")
            raise
    return _DEPTH_ESTIMATOR


def load_seg_model():
    """加载图像分割模型 (RMBG-1.4)"""
    global _SEG_MODEL
    if _SEG_MODEL is None:
        print("✨ Loading Segmentation Model (RMBG-1.4)...")
        try:
            _SEG_MODEL = AutoModelForImageSegmentation.from_pretrained(
                "briaai/RMBG-1.4",
                trust_remote_code=True,
                cache_dir=MODEL_DIR,
                local_files_only=False
            ).to("cuda" if torch.cuda.is_available() else "cpu")
            _SEG_MODEL.eval()
        except Exception as e:
            print(f"❌ Failed to load segmentation model: {e}")
            raise
    return _SEG_MODEL


def load_inpainting_pipeline():
    """加载Stable Diffusion图像修复模型"""
    global _INPAINT_PIPE
    if _INPAINT_PIPE is None:
        print("✨ Loading Stable Diffusion Inpainting Pipeline...")
        try:
            _INPAINT_PIPE = StableDiffusionInpaintPipeline.from_pretrained(
                "runwayml/stable-diffusion-inpainting",
                torch_dtype=torch.float16,
                variant="fp16",
                cache_dir=MODEL_DIR,
                local_files_only=False
            ).to("cuda" if torch.cuda.is_available() else "cpu")
            _INPAINT_PIPE.enable_model_cpu_offload()
        except Exception as e:
            print(f"❌ Failed to load inpainting pipeline: {e}")
            raise
    return _INPAINT_PIPE


# --- 核心处理函数 ---

def normalize_and_convert_depth(depth_pil: Image.Image) -> Image.Image:
    """将浮点深度图归一化并转换为8位灰度图"""
    if depth_pil.mode == 'F':
        depth_np = np.array(depth_pil, dtype=np.float32)
        d_min, d_max = depth_np.min(), depth_np.max()
        if d_max > d_min:
            depth_np = (depth_np - d_min) / (d_max - d_min)
        else:
            depth_np = np.full_like(depth_np, 0.5)
        depth_np = (depth_np * 255.0).astype('uint8')
        return Image.fromarray(depth_np, mode='L')
    return depth_pil.convert('L')


def generate_background_ai(image_pil: Image.Image, prompt: str) -> tuple[Image.Image, Image.Image]:
    """生成AI背景并返回主体mask"""
    print("🎨 Generating AI background and mask...")
    device = "cuda" if torch.cuda.is_available() else "cpu"
    seg_model = load_seg_model(device)
    pipe = load_inpainting_pipeline()
    orig_w, orig_h = image_pil.size

    # 1. 分割
    processor = AutoImageProcessor.from_pretrained("briaai/RMBG-1.4")
    inputs = processor(images=image_pil, return_tensors="pt").to(device)
    with torch.no_grad():
        outputs = seg_model(**inputs)
    mask = processor.post_process_masks(outputs.pred, inputs["original_sizes"])[0][0]
    mask = Image.fromarray((mask.numpy() * 255).astype('uint8')).convert("L")

    # 2. 面积检测
    mask_arr = np.array(mask)
    coverage_ratio = np.sum(mask_arr > 128) / mask_arr.size
    if coverage_ratio > 0.30:
        print(f"⚠️ Subject is too large ({coverage_ratio:.1%}). Skipping inpainting.")
        return image_pil, mask

    # 3. 修复
    mask = mask.filter(ImageFilter.MaxFilter(25))

    def align_8(x): return x - (x % 8)

    sd_w, sd_h = align_8(orig_w), align_8(orig_h)
    sd_in_img = image_pil.resize((sd_w, sd_h), Image.Resampling.LANCZOS)
    sd_in_mask = mask.resize((sd_w, sd_h), Image.Resampling.NEAREST)

    result = \
    pipe(prompt=prompt, image=sd_in_img, mask_image=sd_in_mask, num_inference_steps=20, guidance_scale=7.5).images[0]
    result = result.resize((orig_w, orig_h), Image.Resampling.LANCZOS)

    gc.collect()
    torch.cuda.empty_cache()
    return result, mask


def estimate_depth(image_pil: Image.Image) -> Image.Image:
    """估计图像深度"""
    print("🔍 Estimating depth...")
    device = "cuda" if torch.cuda.is_available() else "cpu"
    model = load_depth_estimator()
    processor = AutoImageProcessor.from_pretrained("depth-anything/Depth-Anything-V2-small-hf")
    inputs = processor(images=image_pil, return_tensors="pt").to(device)
    with torch.no_grad():
        outputs = model(**inputs)
        depth = outputs.predicted_depth
    depth = Image.fromarray((depth.numpy() * 255 / depth.max()).astype('uint8'))
    gc.collect()
    torch.cuda.empty_cache()
    return depth


# --- 主函数 ---

def generate_assets(input_path: str, output_dir: str):
    """主生成流程"""
    input_path = Path(input_path)
    output_path = Path(output_dir)
    output_path.mkdir(parents=True, exist_ok=True)
    print(f"📦 Assets will be exported to: {output_path.absolute()}")

    # 1. 加载图像
    print(f"📷 Loading image: {input_path}")
    image_pil = Image.open(input_path).convert("RGB")

    # 2. 估计前景深度
    fg_depth_pil = estimate_depth(image_pil)
    fg_depth_pil = normalize_and_convert_depth(fg_depth_pil)

    # 3. 生成背景和遮罩
    bg_pil, mask_pil = generate_background_ai(image_pil, "background, nature, realistic, high quality")

    # 4. 估计背景深度
    bg_depth_pil = estimate_depth(bg_pil)
    bg_depth_pil = normalize_and_convert_depth(bg_depth_pil)

    # 5. 保存资产
    assets = {
        "image": image_pil,
        "depth": fg_depth_pil,
        "image_bg": bg_pil,
        "depth_bg": bg_depth_pil,
        "subject_mask": mask_pil
    }
    for name, pil_img in assets.items():
        mode = "L" if "depth" in name or "mask" in name else "RGB"
        if pil_img.mode != mode: pil_img = pil_img.convert(mode)
        pil_img.save(output_path / f"{name}.png")

    # 6. 导出配置
    config = {
        "height": 0.20, "steady": 0.0, "focus": 0.0, "zoom": 1.0,
        "isometric": 0.0, "offset_x": 0.0, "offset_y": 0.0,
        "resolution": (image_pil.width, image_pil.height)
    }
    with open(output_path / "config.json", "w") as f:
        json.dump(config, f, indent=4)

    print("✅ Mobile assets generated successfully!")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Generate mobile assets for DepthFlow.")
    parser.add_argument("-i", "--input", required=True, help="Path to the input image.")
    parser.add_argument("-o", "--output", default="mobile_assets", help="Directory to save the assets.")
    args = parser.parse_args()

    if not Path(args.input).exists():
        print(f"❌ Error: Input file not found at {args.input}")
        sys.exit(1)

    generate_assets(args.input, args.output)
