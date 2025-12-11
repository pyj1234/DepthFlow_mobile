#!/usr/bin/env python3
"""
创建测试用的 PNG 图像文件，用于 DepthFlow Mobile 测试
"""
import os
from PIL import Image, ImageDraw
import numpy as np

def create_test_image(filename, width=512, height=512, color_pattern="gradient"):
    """创建测试图像"""
    if color_pattern == "gradient":
        # 创建渐变图像
        image = Image.new('RGB', (width, height))
        draw = ImageDraw.Draw(image)
        
        for x in range(width):
            for y in range(height):
                r = int(255 * (x / width))
                g = int(255 * (y / height))
                b = 128
                draw.point((x, y), fill=(r, g, b))
    
    elif color_pattern == "checkerboard":
        # 创建棋盘图案
        image = Image.new('RGB', (width, height))
        draw = ImageDraw.Draw(image)
        
        square_size = 32
        for x in range(0, width, square_size):
            for y in range(0, height, square_size):
                if ((x // square_size) + (y // square_size)) % 2 == 0:
                    draw.rectangle([x, y, x + square_size, y + square_size], fill=(255, 255, 255))
                else:
                    draw.rectangle([x, y, x + square_size, y + square_size], fill=(0, 0, 0))
    
    elif color_pattern == "circles":
        # 创建圆形图案
        image = Image.new('RGB', (width, height), color=(50, 100, 150))
        draw = ImageDraw.Draw(image)
        
        center_x, center_y = width // 2, height // 2
        for i in range(10):
            radius = 20 + i * 20
            color = (255 - i * 25, i * 25, 128)
            draw.ellipse([center_x - radius, center_y - radius, center_x + radius, center_y + radius], 
                        outline=color, width=3)
    
    image.save(filename)
    print(f"Created: {filename}")

def create_depth_image(filename, width=512, height=512):
    """创建深度图（灰度）"""
    # 创建径向渐变深度图
    image = Image.new('L', (width, height))  # L模式 = 灰度
    pixels = np.array(image)
    
    center_x, center_y = width // 2, height // 2
    max_radius = min(width, height) // 2
    
    for x in range(width):
        for y in range(height):
            distance = np.sqrt((x - center_x)**2 + (y - center_y)**2)
            depth = max(0, min(255, int(255 * (1 - distance / max_radius))))
            pixels[y, x] = depth
    
    depth_image = Image.fromarray(pixels, mode='L')
    depth_image.save(filename)
    print(f"Created: {filename}")

def create_mask_image(filename, width=512, height=512):
    """创建遮罩图（中心圆形为前景）"""
    image = Image.new('L', (width, height), color=0)  # 黑色背景
    draw = ImageDraw.Draw(image)
    
    center_x, center_y = width // 2, height // 2
    radius = min(width, height) // 3
    
    # 白色圆形作为前景
    draw.ellipse([center_x - radius, center_y - radius, center_x + radius, center_y + radius], 
                fill=255)
    
    image.save(filename)
    print(f"Created: {filename}")

def main():
    """创建所有测试资产"""
    assets_dir = "app/src/main/assets"
    
    # 确保目录存在
    os.makedirs(assets_dir, exist_ok=True)
    
    print("Creating test assets for DepthFlow Mobile...")
    
    # 创建前景图像和深度图
    create_test_image(f"{assets_dir}/image.png", 512, 512, "circles")
    create_depth_image(f"{assets_dir}/depth.png", 512, 512)
    
    # 创建背景图像和深度图
    create_test_image(f"{assets_dir}/image_bg.png", 512, 512, "gradient")
    create_depth_image(f"{assets_dir}/depth_bg.png", 512, 512)
    
    # 创建遮罩
    create_mask_image(f"{assets_dir}/subject_mask.png", 512, 512)
    
    print("\nTest assets created successfully!")
    print(f"Assets location: {assets_dir}")
    print("\nNow you can:")
    print("1. Build and run the Android app")
    print("2. Or replace these with your own mobile_assets from PC export")

if __name__ == "__main__":
    main()
