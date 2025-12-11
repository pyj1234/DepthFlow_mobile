# PC风格深度流效果移植指南
## 🎯 完成的移植

成功将PC端OpenGL版本的深度流算法移植到Android Vulkan版本！

### ✅ 新增功能

1. **完整的PC风格算法**：
   - 复杂的深度光线追踪计算
   - 等距视角支持
   - 推拉效果（dolly zoom）
   - 智能主体遮罩处理

2. **增强的视差效果**：
   - 更精确的深度计算
   - 基于PC端的混合逻辑
   - 边界和越界检测
   - 深度陡峭度分析

3. **智能背景合成**：
   - 主体区域优先显示前景
   - 背景智能填充
   - 防止背景在不适当区域显示

## 🔧 核心算法改进

### 1. 深度计算增强

```glsl
// PC风格的深度因子计算
float depth_factor = (1.0 - depth) * (1.0 + focus * depth);

// 等距视角旋转
if (abs(isometric) > 0.01) {
    float angle = isometric * 0.785398; // 45度
    vec2 rotated = vec2(
        parallax.x * cos(angle) - parallax.y * sin(angle),
        parallax.x * sin(angle) + parallax.y * cos(angle)
    );
    parallax = rotated;
}
```

### 2. 智能遮罩系统

```glsl
// 主体区域智能处理
float subject_region = smoothstep(0.2, 0.5, subject_mask);
float subject_steep_threshold = inpaint_limit * 3.0;
float subject_steep_mask = smoothstep(subject_steep_threshold, subject_steep_threshold + 0.3, steep);

float final_mask = mix(base_mask, subject_steep_mask, subject_region);
```

### 3. 后台填充逻辑

```glsl
// 越界时的特殊处理
if (fg.oob) {
    if (subject_mask < 0.5) {
        final_mask = 1.0; // 非主体区域显示背景
    } else {
        final_mask = mix(0.0, 1.0, smoothstep(0.5, 0.8, steep));
    }
}
```

## 📱 新增参数说明

### UBO参数结构

```cpp
struct UniformBufferObject {
    // p0: height, steady, focus, zoom
    float height;         // 深度强度 (0.0-1.0)
    float steady;         // 稳定性 (0.0-1.0) 
    float focus;          // 焦点 (0.0-1.0)
    float zoom;           // 缩放 (>0.0)
    
    // p1: isometric, dolly, invert, mirror  
    float isometric;      // 等距视角 (-1.0-1.0)
    float dolly;          // 推拉效果 (0.0-1.0)
    float invert;         // 反转深度 (0.0-1.0)
    float mirror;         // 镜像 (0.0-1.0)
    
    // p2: offset.x, offset.y, center.x, center.y
    vec2 offset;          // 偏移向量
    vec2 center;          // 中心点
    
    // p3: origin.x, origin.y, time, aspect
    vec2 origin;          // 原点
    float time;           // 动画时间
    float aspect;         // 屏幕宽高比
    
    // p4: inpaint_limit, quality, vig_enable, colors_saturation
    float inpaint_limit;   // 修复限制阈值
    float quality;         // 渲染质量 (0.0-1.0)
    float vig_enable;      // 晕影启用
    float colors_saturation; // 饱和度
    
    // p5: colors_contrast, colors_brightness, colors_gamma, colors_sepia
    float colors_contrast;   // 对比度
    float colors_brightness; // 亮度
    float colors_gamma;      // 伽马
    float colors_sepia;      // 棕褐色效果
    
    // p6: colors_grayscale, aa_strength, padding1, padding2
    float colors_grayscale;   // 灰度
    float aa_strength;       // 抗锯齿强度
};
```

## 🎮 使用建议

### 基础参数设置

```cpp
// 电影般深度流效果
g_ubo.height = 0.15f;         // 中等深度强度
g_ubo.steady = 0.1f;          // 较低稳定性，更多动态
g_ubo.focus = 0.5f;            // 标准焦点
g_ubo.zoom = 1.0f;             // 无额外缩放
g_ubo.inpaint_limit = 0.3f;     // 适中的修复限制
g_ubo.quality = 0.7f;          // 高质量
```

### 动态效果

```cpp
// 等距视角效果（侧视）
g_ubo.isometric = 0.3f;        // 轻微侧视角

// 推拉效果（动态缩放）
g_ubo.dolly = sin(time * 2.0) * 0.1f;

// 动态偏移（相机移动）
g_ubo.offset.x = sin(time * 0.8) * 0.05f;
g_ubo.offset.y = cos(time * 0.6) * 0.05f;
```

### 视觉风格调整

```cpp
// 电影风格
g_ubo.colors_contrast = 1.1f;    // 轻微增强对比度
g_ubo.colors_saturation = 1.2f;  // 增强饱和度
g_ubo.colors_gamma = 0.9f;       // 轻微降低伽马

// 棕褐色怀旧效果
g_ubo.colors_sepia = 0.3f;

// 黑白效果
g_ubo.colors_grayscale = 1.0f;
```

## 🔍 性能优化

### 质量vs性能平衡

```cpp
// 高质量（高端设备）
g_ubo.quality = 0.8f;          // 高质量采样

// 中等质量（主流设备）
g_ubo.quality = 0.5f;          // 平衡质量和性能

// 低质量（低端设备）
g_ubo.quality = 0.2f;          // 优先性能
```

### 帧率优化

如果遇到性能问题，可以：

1. **降低质量参数**：`quality = 0.3f`
2. **减少动态效果**：降低偏移和缩放动画幅度
3. **简化后处理**：禁用晕影 `vig_enable = 0.0f`

## 🚀 构建和安装

```bash
# 清理并构建
./gradlew clean assembleDebug

# 安装到设备
./gradlew installDebug

# 查看日志
adb logcat | grep DepthFlow
```

## 🎨 预期效果

安装后应该看到：

- ✅ **3D深度效果**：真实的立体深度感
- ✅ **平滑动画**：自然的视差移动
- ✅ **智能合成**：主体与背景完美融合
- ✅ **电影质感**：专业级的视觉效果
- ✅ **触摸交互**：响应的触摸控制

## 🔧 故障排除

如果效果不理想：

1. **检查纹理加载**：确保5个纹理都正确加载
2. **调整参数**：尝试不同的height和focus值
3. **质量设置**：降低quality参数提高性能
4. **Aspect Ratio**：确保屏幕比例正确

---

**移植完成日期**：2025-12-10  
**版本**：DepthFlow Mobile v2.0-PC-Style  
**状态**：✅ 完成并可运行
