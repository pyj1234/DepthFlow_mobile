cmake_minimum_required(VERSION 3.18.1)
project("depthflow_mobile")

# =======================================================
# 1. 查找 glslc 编译器 (修复路径问题)
# =======================================================
if(NOT NDK_DIR)
    set(NDK_DIR ${ANDROID_NDK})
endif()

# 自动判断系统，找到正确的 glslc
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    set(GLSLC_EXECUTABLE "${NDK_DIR}/shader-tools/windows-x86_64/glslc.exe")
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    set(GLSLC_EXECUTABLE "${NDK_DIR}/shader-tools/darwin-x86_64/glslc")
else()
    set(GLSLC_EXECUTABLE "${NDK_DIR}/shader-tools/linux-x86_64/glslc")
endif()

# =======================================================
# 2. 修正路径 (这是你报错的核心原因！！！)
# =======================================================
# 以前是 CMAKE_SOURCE_DIR (cpp目录)，现在改成指向 ../shaders
set(SHADER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../shaders")
set(SHADER_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../assets/shaders")

# 自动创建输出目录
file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR})

# =======================================================
# 3. 编译函数
# =======================================================
function(compile_shader SHADER_NAME)
    set(INPUT_FILE "${SHADER_SOURCE_DIR}/${SHADER_NAME}")
    set(OUTPUT_FILE "${SHADER_OUTPUT_DIR}/${SHADER_NAME}.spv")

    add_custom_command(
            OUTPUT ${OUTPUT_FILE}
            COMMAND ${GLSLC_EXECUTABLE} ${INPUT_FILE} -o ${OUTPUT_FILE}
            DEPENDS ${INPUT_FILE}
            COMMENT "Compiling ${SHADER_NAME} to SPIR-V"
    )
    list(APPEND SPV_SHADERS ${OUTPUT_FILE})
    set(SPV_SHADERS ${SPV_SHADERS} PARENT_SCOPE)
endfunction()

# =======================================================
# 4. 指定你的 Shader 文件
# =======================================================
# 确保你的文件夹里真的是这两个名字，大小写敏感！
compile_shader("quad.vert")
compile_shader("depthflow.frag")

# 创建编译目标
add_custom_target(shader_compilation ALL DEPENDS ${SPV_SHADERS})

# =======================================================
# 5. C++ 库配置
# =======================================================
add_library(depthflow_mobile SHARED native-lib.cpp)

find_library(log-lib log)
find_library(android-lib android)
find_library(vulkan-lib vulkan)

target_link_libraries(depthflow_mobile ${log-lib} ${android-lib} ${vulkan-lib})

# =======================================================
# 6. 添加依赖 (确保先编译 Shader)
# =======================================================
add_dependencies(depthflow_mobile shader_compilation)