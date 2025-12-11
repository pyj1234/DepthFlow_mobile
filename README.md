# DepthFlow Mobile - Android Vulkan 渲染器
这是一个基于 Vulkan 的 Android 应用，用于渲染从 PC 端导出的深度流动画效果。

## 项目结构

```
app/src/main/
├── assets/                    # 资源文件
│   ├── quad.vert             # 顶点着色器源码
│   ├── depthflow.frag        # 片元着色器源码
│   └── config.json           # 配置文件
├── cpp/                      # Native C++ 代码
│   ├── CMakeLists.txt        # CMake 构建配置
│   └── native-lib.cpp        # Vulkan 渲染器实现
├── java/com/df/depthflow_mobile/
│   └── MainActivity.java     # 主活动类
└── AndroidManifest.xml       # 应用清单文件
```

## 编译要求

- Android Studio Arctic Fox 或更高版本
- Android NDK 21 或更高版本
- Android SDK API Level 24+
- 支持 Vulkan 的 Android 设备
- CMake 3.10.2+

## 编译步骤

1. **克隆或打开项目**
   ```
   在 Android Studio 中打开该项目
   ```

2. **配置 NDK**
   - 在 Android Studio 中：Tools -> SDK Manager -> SDK Tools
   - 勾选 "NDK (Side by side)" 和 "CMake"
   - 点击 Apply 安装

3. **编译项目**
   - 点击 Build -> Make Project 或按 Ctrl+F9
   - 确保没有编译错误

## 使用方法

### PC 端数据准备

1. 使用 Python 脚本导出 mobile_assets：
   ```bash
   uv run depthflow input -i input.jpg main --export-mobile
   ```

2. 将生成的文件夹内容复制到 Android 项目的 `app/src/main/assets/`：
   ```
   mobile_assets/
   ├── image.png          # 原始图像
   ├── depth.png          # 深度图
   ├── image_bg.png       # 背景图像
   ├── depth_bg.png       # 背景深度图
   ├── subject_mask.png   # 主体遮罩
   └── config.json        # 配置参数
   ```

### Android 端运行

1. 在支持 Vulkan 的 Android 设备上运行应用
2. 应用会显示一个全屏的动画效果
3. 动画参数会随时间自动变化

## 技术实现

### 核心特性

- **Vulkan 渲染**：使用 Vulkan API 进行高性能 GPU 渲染
- **实时动画**：基于时间的轨道运动动画
- **深度效果**：使用深度图实现视差效果
- **遮罩处理**：支持前景背景分离

### 渲染管线

1. **顶点着色器**：生成覆盖全屏的三角形
2. **片元着色器**：
   - 采样深度图和颜色纹理
   - 计算视差偏移
   - 应用遮罩混合前景和背景
   - 添加时间动画效果

3. **Uniform Buffer**：传递动画参数到着色器

### 关键组件

- **MainActivity**：管理 SurfaceView 和渲染线程
- **native-lib.cpp**：Vulkan 初始化和渲染逻辑
- **着色器**：GLSL 450，编译为 SPIR-V

## 调试和优化

### 日志查看

使用 logcat 查看应用日志：
```bash
adb logcat | grep DepthFlow
```

### 性能优化建议

1. **纹理压缩**：使用 ASTC 或 ETC2 格式压缩纹理
2. **Mipmap**：为纹理生成多级渐远纹理
3. **批处理**：合并渲染调用减少状态切换
4. **异步加载**：后台加载纹理资源

## 常见问题

### Q: 应用闪退
A: 检查设备是否支持 Vulkan，确保 API Level >= 24

### Q: 着色器编译错误
A: 确保使用的是 GLSL 450 语法，检查 Vulkan 验证层输出

### Q: 纹理无法加载
A: 检查 assets 文件夹中是否包含所有必需的 PNG 文件

## 扩展开发

### 添加新功能

1. **新的动画模式**：在 `updateUniformBuffer()` 中添加新的参数计算
2. **交互控制**：添加触摸输入处理
3. **后处理效果**：在片元着色器中添加更多的视觉效果

### 自定义着色器

修改 `depthflow.frag` 文件，实现自定义的视觉效果：
- 更复杂的深度计算
- 额外的遮罩层
- 后处理滤镜

## 许可证

该项目基于原 DepthFlow 项目开发，遵循相应的开源许可证。
