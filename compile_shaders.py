#!/usr/bin/env python3
"""
着色器编译脚本
由于找不到本地glslc，使用在线编译API或者其他方法重新编译着色器
"""
import os
import subprocess
import sys
import requests
import base64

def try_local_glslc():
    """尝试找到并使用本地的glslc编译器"""
    possible_paths = [
        "C:/VulkanSDK/*/Bin/glslc.exe",
        "C:/Program Files/VulkanSDK/*/Bin/glslc.exe",
        "C:/Android/Sdk/ndk/*/toolchains/llvm/prebuilt/windows-x86_64/bin/glslc.exe",
    ]
    
    for pattern in possible_paths:
        try:
            import glob
            matches = glob.glob(pattern)
            if matches:
                return matches[-1]  # 使用最新版本
        except:
            continue
    
    # 尝试直接在PATH中查找
    try:
        result = subprocess.run(["where", "glslc"], capture_output=True, text=True)
        if result.returncode == 0:
            return result.stdout.strip().split('\n')[0]
    except:
        pass
    
    return None

def compile_shader_with_glslc(glslc_path, input_file, output_file):
    """使用glslc编译着色器"""
    cmd = [glslc_path, input_file, "-o", output_file]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode == 0:
            print(f"✓ 成功编译 {input_file} -> {output_file}")
            return True
        else:
            print(f"✗ 编译失败 {input_file}: {result.stderr}")
            return False
    except Exception as e:
        print(f"✗ 编译异常 {input_file}: {e}")
        return False

def compile_shader_online(shader_source, shader_type):
    """使用在线API编译着色器（备选方案）"""
    try:
        # 这里可以使用Shader编译API，比如：
        # - shader-playground
        # - 其他在线编译服务
        
        # 暂时返回None，表示在线编译不可用
        print("在线编译暂不可用，请安装Vulkan SDK")
        return None
    except Exception as e:
        print(f"在线编译失败: {e}")
        return None

def main():
    print("=== 着色器编译脚本 ===")
    
    shader_dir = "app/src/main/assets"
    vert_shader = f"{shader_dir}/quad.vert"
    frag_shader = f"{shader_dir}/depthflow.frag"
    vert_spv = f"{shader_dir}/quad.vert.spv"
    frag_spv = f"{shader_dir}/depthflow.frag.spv"
    
    # 检查源文件是否存在
    if not os.path.exists(vert_shader):
        print(f"✗ 找不到顶点着色器源码: {vert_shader}")
        return False
    
    if not os.path.exists(frag_shader):
        print(f"✗ 找不到片元着色器源码: {frag_shader}")
        return False
    
    # 尝试找到本地glslc
    glslc_path = try_local_glslc()
    if glslc_path and os.path.exists(glslc_path):
        print(f"✓ 找到glslc编译器: {glslc_path}")
        
        # 编译顶点着色器
        success = compile_shader_with_glslc(glslc_path, vert_shader, vert_spv)
        if not success:
            return False
            
        # 编译片元着色器
        success = compile_shader_with_glslc(glslc_path, frag_shader, frag_spv)
        if not success:
            return False
            
        print("✓ 所有着色器编译完成！")
        return True
    else:
        print("✗ 未找到glslc编译器")
        print("请安装Vulkan SDK或Android NDK来获取glslc")
        print("")
        print("推荐安装方法:")
        print("1. 下载Vulkan SDK: https://vulkan.lunarg.com/")
        print("2. 或使用Android Studio的SDK Manager安装NDK")
        
        # 备选方案：尝试在线编译
        print("\n尝试备选编译方案...")
        
        try:
            with open(vert_shader, 'r') as f:
                vert_source = f.read()
            with open(frag_shader, 'r') as f:
                frag_source = f.read()
            
            vert_compiled = compile_shader_online(vert_source, "vertex")
            frag_compiled = compile_shader_online(frag_source, "fragment")
            
            if vert_compiled and frag_compiled:
                with open(vert_spv, 'wb') as f:
                    f.write(vert_compiled)
                with open(frag_spv, 'wb') as f:
                    f.write(frag_compiled)
                print("✓ 使用在线编译成功！")
                return True
        except Exception as e:
            print(f"备选编译也失败了: {e}")
        
        return False

if __name__ == "__main__":
    success = main()
    if not success:
        print("\n=== 手动编译说明 ===")
        print("如果自动编译失败，请手动执行:")
        print("")
        print("glslc app/src/main/assets/quad.vert -o app/src/main/assets/quad.vert.spv")
        print("glslc app/src/main/assets/depthflow.frag -o app/src/main/assets/depthflow.frag.spv")
        print("")
    
    sys.exit(0 if success else 1)
