import os
import sys
import argparse
import json
import gc
import numpy as np
import torch
import torch.nn.functional as F
from pathlib import Path
from PIL import Image, ImageFilter

# ==========================================
# 🚑 兼容性补丁：修复 PyTorch 2.1.2 兼容性
# ==========================================
try:
    import torch.utils._pytree as _pytree

    if not hasattr(_pytree, "register_pytree_node") and hasattr(_pytree, "_register_pytree_node"):
        _pytree.register_pytree_node = _pytree._register_pytree_node
except:
    pass

# 引入模型库
from transformers import AutoModelForDepthEstimation, AutoImageProcessor, AutoModelForImageSegmentation
from diffusers import StableDiffusionInpaintPipeline

# === 路径配置 ===
BASE_DIR = Path(__file__).parent.absolute()
MODEL_DIR = BASE_DIR / "models"
OUTPUT_DIR = BASE_DIR / "output"

# 检查本地模型
PATH_DEPTH = MODEL_DIR / "depth_anything_v2"
PATH_SEG = MODEL_DIR / "rmbg_1_4"
PATH_SD = MODEL_DIR / "sd_inpainting"

if not PATH_DEPTH.exists() or not PATH_SEG.exists() or not PATH_SD.exists():
    print(f"❌ Error: Local models not found in {MODEL_DIR}")
    sys.exit(1)

DEVICE = "cuda" if torch.cuda.is_available() else "cpu"
print(f"⚙️ Running on device: {DEVICE} (Torch: {torch.__version__})")


# === 辅助函数 ===
def cleanup():
    gc.collect()
    if torch.cuda.is_available():
        torch.cuda.empty_cache()


# === 模型加载 (纯本地) ===

def get_depth_utils():
    print(f"Loading Depth Model from: {PATH_DEPTH.name}")
    processor = AutoImageProcessor.from_pretrained(PATH_DEPTH, local_files_only=True)
    model = AutoModelForDepthEstimation.from_pretrained(PATH_DEPTH, local_files_only=True).to(DEVICE)
    return model, processor


def get_seg_model():
    print(f"Loading Seg Model from: {PATH_SEG.name}")
    model = AutoModelForImageSegmentation.from_pretrained(PATH_SEG, trust_remote_code=True, local_files_only=True).to(
        DEVICE)
    model.eval()
    return model


def get_inpainting_pipe():
    print(f"Loading SD Pipeline from: {PATH_SD.name}")
    pipe = StableDiffusionInpaintPipeline.from_pretrained(
        PATH_SD,
        torch_dtype=torch.float16 if DEVICE == "cuda" else torch.float32,
        local_files_only=True,
        use_safetensors=True,
        variant="fp16"
    ).to(DEVICE)
    if DEVICE == "cuda":
        pipe.enable_attention_slicing()
    return pipe


# === 核心逻辑 ===

def estimate_depth(image_pil):
    """生成深度图"""
    model, processor = get_depth_utils()

    if image_pil.mode != "RGB":
        image_pil = image_pil.convert("RGB")

    inputs = processor(images=image_pil, return_tensors="pt").to(DEVICE)
    with torch.no_grad():
        depth = model(**inputs).predicted_depth

    h, w = image_pil.size[::-1]
    depth = torch.nn.functional.interpolate(
        depth.unsqueeze(1), size=(h, w), mode="bicubic", align_corners=False
    )

    depth_min, depth_max = depth.min(), depth.max()
    depth_norm = (depth - depth_min) / (depth_max - depth_min)
    depth_uint8 = (depth_norm * 255.0).cpu().numpy().astype(np.uint8)[0, 0]

    del model, processor, inputs, depth
    cleanup()

    return Image.fromarray(depth_uint8, mode="L")


def generate_mask(image_pil):
    """RMBG-1.4 分割"""
    model = get_seg_model()

    orig_w, orig_h = image_pil.size
    input_size = (1024, 1024)

    im_tensor = image_pil.resize(input_size, Image.BILINEAR)
    im_arr = np.array(im_tensor).astype(np.float32) / 255.0
    im_arr = (im_arr - 0.5) / 0.5
    im_tensor = torch.from_numpy(im_arr).permute(2, 0, 1).unsqueeze(0).float().to(DEVICE)

    with torch.no_grad():
        preds = model(im_tensor)

    while isinstance(preds, (list, tuple)):
        preds = preds[0]

    if hasattr(preds, 'pred'):
        preds = preds.pred
    elif hasattr(preds, 'logits'):
        preds = preds.logits

    preds = F.interpolate(preds, size=(orig_h, orig_w), mode='bilinear', align_corners=False)

    result = (preds[0][0] > 0).cpu().numpy()
    if preds.max() <= 1.0:
        result = (preds[0][0] > 0.5).cpu().numpy()

    mask_pil = Image.fromarray((result * 255).astype('uint8')).convert("L")

    del model, im_tensor, preds
    cleanup()
    return mask_pil


def get_smart_inpaint_mask(mask_pil, image_size, max_parallax_percent=0.04):
    """
    计算智能修补遮罩 (Rim Mask) - 性能优化版
    先缩小处理再放大，解决大分辨率下 PIL MaxFilter 卡死的问题
    """
    w, h = image_size

    # === 关键优化 ===
    # 将处理分辨率限制在 1024 像素以内
    # PIL 的 MaxFilter 算法复杂度随半径平方增长，缩小处理可提速百倍
    process_max_dim = 1024
    scale_factor = 1.0

    if max(w, h) > process_max_dim:
        scale_factor = process_max_dim / max(w, h)
        process_w = int(w * scale_factor)
        process_h = int(h * scale_factor)
        # 使用 Nearest 缩放遮罩以保持二值特性
        mask_processing = mask_pil.resize((process_w, process_h), Image.Resampling.NEAREST)
    else:
        process_w, process_h = w, h
        mask_processing = mask_pil

    # 在缩小后的尺寸上计算偏移量
    offset_px = int(min(process_w, process_h) * max_parallax_percent)

    # 1. 外扩 (Dilation)
    mask_dilated = mask_processing.filter(ImageFilter.MaxFilter(size=offset_px * 2 + 1))

    # 2. 内缩 (Erosion)
    safe_zone_radius = int(offset_px * 1.5)
    mask_eroded = mask_processing.filter(ImageFilter.MinFilter(size=safe_zone_radius * 2 + 1))

    # 3. 计算环形区域
    arr_dilated = np.array(mask_dilated).astype(np.float32)
    arr_eroded = np.array(mask_eroded).astype(np.float32)

    arr_final = arr_dilated - arr_eroded
    arr_final = np.clip(arr_final, 0, 255)

    # 特殊情况处理
    if np.sum(arr_eroded) < 100:
        arr_final = arr_dilated

    result = Image.fromarray(arr_final.astype(np.uint8), mode="L")

    # === 恢复原始尺寸 ===
    if scale_factor != 1.0:
        # 放大回去，使用 Bilinear 让边缘稍微平滑一点点
        result = result.resize((w, h), Image.Resampling.BILINEAR)

    return result

def generate_background(image_pil, mask_pil, prompt):
    """SD Inpainting (智能边缘修补版)"""

    # 强制 RGB
    if image_pil.mode != "RGB":
        image_pil = image_pil.convert("RGB")

    w, h = image_pil.size

    print("🧠 Calculating parallax-aware inpaint mask...")
    # 计算智能遮罩
    smart_mask = get_smart_inpaint_mask(mask_pil, (w, h), max_parallax_percent=0.04)

    # 检查是否需要修补
    mask_arr = np.array(smart_mask)
    inpaint_area_ratio = np.sum(mask_arr > 128) / mask_arr.size
    print(f"ℹ️ Inpaint Area Ratio: {inpaint_area_ratio:.1%}")

    if inpaint_area_ratio < 0.001:
        print("⚡ Subject is static or too small, skipping inpainting.")
        return image_pil

    pipe = get_inpainting_pipe()

    # 缩放至 8 的倍数 (最高 1024)
    process_w = 1024 if w > 1024 else (w // 8) * 8
    process_h = 1024 if h > 1024 else (h // 8) * 8

    img_in = image_pil.resize((process_w, process_h), Image.Resampling.LANCZOS)
    mask_in = smart_mask.resize((process_w, process_h), Image.Resampling.NEAREST)

    print("🎨 Generating background (Rim Inpainting)...")
    result = pipe(
        prompt=prompt,
        negative_prompt="bad quality, distorted, ugly, text, watermark, foreground object, person, clothes, skin",
        image=img_in,
        mask_image=mask_in,
        num_inference_steps=25,
        guidance_scale=7.5,
        strength=1.0  # 100% 重绘遮罩区域
    ).images[0]

    del pipe
    cleanup()

    # 恢复原始尺寸
    result = result.resize((w, h), Image.Resampling.LANCZOS)

    # === 关键步骤：合成 ===
    # 仅替换 smart_mask 覆盖的区域 (边缘)，保留原始背景和物体深层中心
    # 这样可以防止背景闪烁，并解决大物体修补困难的问题
    final_comp = Image.composite(result, image_pil, smart_mask)

    return final_comp


# === 主流程 ===

def main(input_path, output_dir, prompt):
    input_file = Path(input_path)
    if not input_file.exists():
        print(f"❌ Input file not found: {input_file}")
        return

    output_path = Path(output_dir)
    output_path.mkdir(parents=True, exist_ok=True)
    print(f"🚀 Processing: {input_file.name}")

    img = Image.open(input_file).convert("RGB")

    print("\n--- Step 1: Foreground Depth ---")
    depth_fg = estimate_depth(img)

    print("\n--- Step 2: Segmentation (Mask) ---")
    mask = generate_mask(img)

    print("\n--- Step 3: Background Generation ---")
    img_bg = generate_background(img, mask, prompt)

    print("\n--- Step 4: Background Depth ---")
    depth_bg = estimate_depth(img_bg)

    print("\n💾 Saving assets...")
    assets = {
        "image": img,
        "depth": depth_fg,
        "image_bg": img_bg,
        "depth_bg": depth_bg,
        "subject_mask": mask
    }

    for name, pil_obj in assets.items():
        pil_obj.save(output_path / f"{name}.png")

    config = {
        "height": 0.20,
        "steady": 0.0,
        "focus": 0.0,
        "zoom": 1.0,
        "isometric": 0.0,
        "offset_x": 0.0,
        "offset_y": 0.0,
        "resolution": img.size
    }
    with open(output_path / "config.json", "w") as f:
        json.dump(config, f, indent=4)

    print(f"✅ Success! Assets saved to: {output_path.absolute()}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("-i", "--input", required=True, help="Input image path")
    parser.add_argument("-o", "--output", default="output", help="Output directory")
    parser.add_argument("-p", "--prompt", default="background, nature, realistic, high quality")
    args = parser.parse_args()

    main(args.input, args.output, args.prompt)