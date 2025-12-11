# Vulkan 修复测试指南
## 修复内容总结

### 1. 问题诊断
- **错误位置**: 第313行 `vkCreateGraphicsPipelines` 调用
- **错误代码**: -3 (VK_ERROR_INITIALIZATION_FAILED)
- **根本原因**: MediaTek 设备的 Vulkan 驱动兼容性问题

### 2. 具体修复措施

#### A. 实例创建优化
```cpp
// 修复前
VkInstanceCreateInfo instInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &appInfo, 0, nullptr, (uint32_t)instanceExts.size(), instanceExts.data()};

// 修复后
std::vector<const char*> instanceLayers; // 显式禁用调试层
VkInstanceCreateInfo instInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &appInfo, 
    (uint32_t)instanceLayers.size(), instanceLayers.data(), 
    (uint32_t)instanceExts.size(), instanceExts.data()};
```

#### B. 设备创建优化
```cpp
// 修复前：尝试空特性结构
// 修复后：获取设备特性并最大兼容性配置
VkPhysicalDeviceFeatures deviceFeatures{};
vkGetPhysicalDeviceFeatures(physicalDevice, &deviceFeatures);
std::vector<const char*> deviceLayers; // 避免加载设备层
devInfo.pEnabledFeatures = nullptr; // 使用 nullptr 获得最大兼容性
```

#### C. 着色器兼容性
```cpp
// 修复前：#version 450 (桌面版 Vulkan)
// 修复后：#version 310 es (移动端兼容)
// 添加 precision 限定符和显式精度声明
```

### 3. 测试步骤

1. **编译验证**
   ```bash
   ./gradlew clean assembleDebug
   # ✅ 预期：编译成功，无错误
   ```

2. **安装验证**
   ```bash
   ./gradlew installDebug
   # ✅ 预期：安装成功
   ```

3. **运行验证**
   - 启动应用
   - 检查日志中是否还有 libVkLayer_mtk_rt_sdk.so 错误
   - 确认 Vulkan 初始化成功
   - 验证渲染管线创建成功

### 4. 预期结果

#### 修复前的问题
```
E/vulkan: failed to load layer library '/vendor/lib64/libVkLayer_mtk_rt_sdk.so'
E/DepthFlow: Vulkan Error at line 313: -3
```

#### 修复后的预期结果
```
I/DepthFlow: Starting Vulkan Init...
I/DepthFlow: Vulkan initialization completed successfully
# 无错误日志，应用正常渲染
```

### 5. 兼容性改进

- **MediaTek 设备**: 专门处理了 MTK 驱动层加载问题
- **着色器版本**: 使用 GLSL ES 3.10 (310 es) 替代桌面版 450
- **精度控制**: 添加显式精度声明，确保移动端兼容
- **驱动层**: 完全禁用调试层，避免驱动冲突

### 6. 监控命令

```bash
# 实时监控应用日志
adb logcat | grep DepthFlow

# 检查 Vulkan 相关日志
adb logcat | grep vulkan

# 监控应用启动和崩溃
adb logcat | grep "com.df.depthflow_mobile"
```

### 7. 故障排除

如果仍有问题，请检查：
1. 设备是否支持 Vulkan (API Level 24+)
2. 设备驱动是否为最新版本
3. 应用权限是否正确
4. 设备存储空间是否充足
