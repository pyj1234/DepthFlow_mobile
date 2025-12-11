#!/usr/bin/env python3
"""
简化的着色器编译脚本
如果没有编译器，就创建一个基本可用的版本
"""
import os

def create_basic_shader():
    """创建一个基本的可运行版本用于测试"""
    assets_dir = "app/src/main/assets"
    
    # 检查现有的.spv文件
    vert_spv = f"{assets_dir}/quad.vert.spv"
    frag_spv = f"{assets_dir}/depthflow.frag.spv"
    
    if os.path.exists(vert_spv) and os.path.exists(frag_spv):
        print("✓ 现有着色器文件存在，保留使用")
        return True
    else:
        print("✗ 需要重新编译着色器文件")
        
        # 提示用户手动编译
        print("\n请手动执行以下命令重新编译着色器：")
        print("需要安装Vulkan SDK或Android NDK中的glslc编译器")
        print("")
        print("编译命令：")
        print("glslc app/src/main/assets/quad.vert -o app/src/main/assets/quad.vert.spv")
        print("glslc app/src/main/assets/depthflow.frag -o app/src/main/assets/depthflow.frag.spv")
        print("")
        print("或者下载Vulkan SDK: https://vulkan.lunarg.com/")
        print("")
        return False

if __name__ == "__main__":
    create_basic_shader()
