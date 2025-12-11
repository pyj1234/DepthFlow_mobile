#include <jni.h>
#include <string>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>
#include <vector>
#include <chrono>
#include <cstring>
#include <cmath>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define LOG_TAG "DepthFlow"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

#define CHECK_VK(x) if((x)!=VK_SUCCESS) { LOGE("VK Error: %d", x); }

struct vec2 { float x, y; };
struct UBO {
    float height, steady, focus, zoom;
    float isometric, dolly, invert, mirror;
    vec2 offset; vec2 center;
    vec2 origin; float time; float aspect;
    vec2 screenSize; vec2 imgSize;
    float inpaint, quality, vig, sat;
    float con, bri, gam, sep;
    float gray, pad1, pad2, pad3;
};

VkInstance instance; VkDevice device; VkPhysicalDevice physicalDevice;
VkQueue graphicsQueue; VkSurfaceKHR surface; VkSwapchainKHR swapchain;
std::vector<VkImage> scImages; std::vector<VkImageView> scViews; std::vector<VkFramebuffer> scFBs;
VkRenderPass renderPass; VkPipelineLayout pipeLayout; VkDescriptorSetLayout descLayout;
VkPipeline pipeline; VkCommandPool cmdPool; VkCommandBuffer cmdBuf;
VkDescriptorPool descPool; VkDescriptorSet descSet;
VkBuffer uboBuf; VkDeviceMemory uboMem;
VkSemaphore semImage, semRender; VkFence fence;
VkExtent2D scExtent; AAssetManager* assetMgr = nullptr;

struct Tex { VkImage img; VkDeviceMemory mem; VkImageView view; VkSampler smp; };
std::vector<Tex> textures(5); UBO ubo = {}; bool isInit = false;
float g_w = 100.0f, g_h = 100.0f;

// --- Helper Functions ---
uint32_t findMemType(uint32_t typeFilter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties memProps; vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);
    for(uint32_t i=0; i<memProps.memoryTypeCount; i++)
        if((typeFilter & (1<<i)) && (memProps.memoryTypes[i].propertyFlags & props) == props) return i;
    return 0;
}

void createBuf(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, VkBuffer& buf, VkDeviceMemory& mem) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size=size; bi.usage=usage; bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(device, &bi, nullptr, &buf);
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(device, buf, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=mr.size; ai.memoryTypeIndex=findMemType(mr.memoryTypeBits, props);
    vkAllocateMemory(device, &ai, nullptr, &mem); vkBindBufferMemory(device, buf, mem, 0);
}

void oneTimeCmd(std::function<void(VkCommandBuffer)> func) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, cmdPool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb; vkAllocateCommandBuffers(device, &ai, &cb);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi); func(cb); vkEndCommandBuffer(cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &cb};
    vkQueueSubmit(graphicsQueue, 1, &si, VK_NULL_HANDLE); vkQueueWaitIdle(graphicsQueue);
    vkFreeCommandBuffers(device, cmdPool, 1, &cb);
}

void transLayout(VkImage img, VkImageLayout oldL, VkImageLayout newL) {
    oneTimeCmd([&](VkCommandBuffer cb){
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.oldLayout=oldL; b.newLayout=newL;
        b.image=img; b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = (oldL==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)?VK_ACCESS_TRANSFER_WRITE_BIT:0;
        b.dstAccessMask = (newL==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)?VK_ACCESS_TRANSFER_WRITE_BIT:VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    });
}

void createFallback(Tex& t, uint8_t r, uint8_t g, uint8_t b) {
    uint8_t p[] = {r, g, b, 255};
    VkBuffer sb; VkDeviceMemory sm; createBuf(4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, sb, sm);
    void* d; vkMapMemory(device, sm, 0, 4, 0, &d); memcpy(d, p, 4); vkUnmapMemory(device, sm);

    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ii.imageType=VK_IMAGE_TYPE_2D; ii.extent={1,1,1}; ii.mipLevels=1; ii.arrayLayers=1; ii.format=VK_FORMAT_R8G8B8A8_UNORM; ii.tiling=VK_IMAGE_TILING_OPTIMAL; ii.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT; ii.samples=VK_SAMPLE_COUNT_1_BIT;
    vkCreateImage(device, &ii, nullptr, &t.img);
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(device, t.img, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, mr.size, findMemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    vkAllocateMemory(device, &ai, nullptr, &t.mem); vkBindImageMemory(device, t.img, t.mem, 0);

    transLayout(t.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    oneTimeCmd([&](VkCommandBuffer cb){ VkBufferImageCopy r{}; r.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; r.imageExtent={1,1,1}; vkCmdCopyBufferToImage(cb, sb, t.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r); });
    transLayout(t.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vkDestroyBuffer(device, sb, nullptr); vkFreeMemory(device, sm, nullptr);

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0, t.img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCreateImageView(device, &vi, nullptr, &t.view);
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; si.magFilter=VK_FILTER_NEAREST; si.minFilter=VK_FILTER_NEAREST;
    vkCreateSampler(device, &si, nullptr, &t.smp);
}

void loadTex(const char* name, Tex& t) {
    if(!assetMgr) { createFallback(t, 255, 0, 255); return; }
    AAsset* f = AAssetManager_open(assetMgr, name, AASSET_MODE_BUFFER);
    if(!f) { LOGE("Missing: %s", name); createFallback(t, 255, 0, 0); return; }
    size_t len = AAsset_getLength(f); std::vector<unsigned char> buf(len); AAsset_read(f, buf.data(), len); AAsset_close(f);

    int w, h, c; unsigned char* p = stbi_load_from_memory(buf.data(), len, &w, &h, &c, 4);
    if(!p) { createFallback(t, 255, 255, 0); return; }
    if(strcmp(name, "image.png")==0) { g_w=(float)w; g_h=(float)h; }

    VkDeviceSize sz = w*h*4;
    VkBuffer sb; VkDeviceMemory sm; createBuf(sz, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, sb, sm);
    void* d; vkMapMemory(device, sm, 0, sz, 0, &d); memcpy(d, p, sz); vkUnmapMemory(device, sm); stbi_image_free(p);

    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ii.imageType=VK_IMAGE_TYPE_2D; ii.extent={(uint32_t)w,(uint32_t)h,1}; ii.mipLevels=1; ii.arrayLayers=1; ii.format=VK_FORMAT_R8G8B8A8_UNORM; ii.tiling=VK_IMAGE_TILING_OPTIMAL; ii.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT; ii.samples=VK_SAMPLE_COUNT_1_BIT;
    vkCreateImage(device, &ii, nullptr, &t.img);
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(device, t.img, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, mr.size, findMemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    vkAllocateMemory(device, &ai, nullptr, &t.mem); vkBindImageMemory(device, t.img, t.mem, 0);

    transLayout(t.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    oneTimeCmd([&](VkCommandBuffer cb){ VkBufferImageCopy r{}; r.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; r.imageExtent={(uint32_t)w,(uint32_t)h,1}; vkCmdCopyBufferToImage(cb, sb, t.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r); });
    transLayout(t.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vkDestroyBuffer(device, sb, nullptr); vkFreeMemory(device, sm, nullptr);

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0, t.img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCreateImageView(device, &vi, nullptr, &t.view);
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; si.magFilter=VK_FILTER_LINEAR; si.minFilter=VK_FILTER_LINEAR;
    vkCreateSampler(device, &si, nullptr, &t.smp);
}

bool initVK(ANativeWindow* win) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "DF", 0, "", 0, VK_API_VERSION_1_0};
    std::vector<const char*> exts = {"VK_KHR_surface", "VK_KHR_android_surface"};
    VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app, 0, nullptr, (uint32_t)exts.size(), exts.data()};
    vkCreateInstance(&ii, nullptr, &instance);

    VkAndroidSurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, nullptr, 0, win};
    auto cs = (PFN_vkCreateAndroidSurfaceKHR)vkGetInstanceProcAddr(instance, "vkCreateAndroidSurfaceKHR"); cs(instance, &si, nullptr, &surface);

    uint32_t c; vkEnumeratePhysicalDevices(instance, &c, nullptr); std::vector<VkPhysicalDevice> devs(c); vkEnumeratePhysicalDevices(instance, &c, devs.data()); physicalDevice=devs[0];
    float p=1.0f; VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, 0, 1, &p};
    std::vector<const char*> de = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, nullptr, 0, 1, &qi, 0, nullptr, 1, de.data()};
    vkCreateDevice(physicalDevice, &di, nullptr, &device); vkGetDeviceQueue(device, 0, 0, &graphicsQueue);

    VkSurfaceCapabilitiesKHR caps; vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &caps); scExtent = caps.currentExtent;
    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, nullptr, 0, surface, caps.minImageCount+1, VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, caps.currentExtent, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, nullptr, caps.currentTransform, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_PRESENT_MODE_FIFO_KHR, VK_TRUE};
    vkCreateSwapchainKHR(device, &sci, nullptr, &swapchain);

    vkGetSwapchainImagesKHR(device, swapchain, &c, nullptr); scImages.resize(c); vkGetSwapchainImagesKHR(device, swapchain, &c, scImages.data());
    scViews.resize(c); scFBs.resize(c);
    for(int i=0; i<c; i++) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0, scImages[i], VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCreateImageView(device, &vi, nullptr, &scViews[i]);
    }

    VkAttachmentDescription ad{0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
    VkAttachmentReference ar{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sd{0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, nullptr, 1, &ar};
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, nullptr, 0, 1, &ad, 1, &sd, 0, nullptr};
    vkCreateRenderPass(device, &rpi, nullptr, &renderPass);

    for(int i=0; i<c; i++) {
        VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, nullptr, 0, renderPass, 1, &scViews[i], scExtent.width, scExtent.height, 1};
        vkCreateFramebuffer(device, &fbi, nullptr, &scFBs[i]);
    }

    std::vector<VkDescriptorSetLayoutBinding> b(6);
    for(int i=0; i<5; i++) b[i] = { (uint32_t)i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    b[5] = { 5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo dsli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 6, b.data()};
    vkCreateDescriptorSetLayout(device, &dsli, nullptr, &descLayout);

    auto readSpv = [](const char* name) {
        if(!assetMgr) return std::vector<char>();
        AAsset* f = AAssetManager_open(assetMgr, name, AASSET_MODE_BUFFER);
        if(!f) return std::vector<char>();
        size_t sz = AAsset_getLength(f); std::vector<char> buf(sz); AAsset_read(f, buf.data(), sz); AAsset_close(f); return buf;
    };
    auto vc = readSpv("shaders/quad.vert.spv");
    auto fc = readSpv("shaders/depthflow.frag.spv");
    if(vc.empty() || fc.empty()) { LOGE("SHADER MISSING"); return false; }

    VkShaderModule vm, fm;
    VkShaderModuleCreateInfo vmi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, vc.size(), (uint32_t*)vc.data()}; vkCreateShaderModule(device, &vmi, nullptr, &vm);
    VkShaderModuleCreateInfo fmi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, fc.size(), (uint32_t*)fc.data()}; vkCreateShaderModule(device, &fmi, nullptr, &fm);

    VkPipelineShaderStageCreateInfo ss[] = { {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vm, "main"}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fm, "main"} };
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &descLayout};
    vkCreatePipelineLayout(device, &pli, nullptr, &pipeLayout);

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport vp{0, 0, (float)scExtent.width, (float)scExtent.height, 0, 1}; VkRect2D sr{{0,0}, scExtent};
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, nullptr, 0, 1, &vp, 1, &sr};
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, nullptr, 0, VK_FALSE, VK_FALSE, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE, VK_FALSE, 0, 0, 0, 1.0f};
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, nullptr, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba{VK_FALSE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, 0xF};
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, nullptr, 0, VK_FALSE, VK_LOGIC_OP_COPY, 1, &cba};
    VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, nullptr, 0, 2, ss, &vi, &ia, nullptr, &vps, &rs, &ms, nullptr, &cb, nullptr, pipeLayout, renderPass, 0};
    vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipeline);

    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0};
    vkCreateCommandPool(device, &cpi, nullptr, &cmdPool);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, cmdPool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    vkAllocateCommandBuffers(device, &cai, &cmdBuf);

    VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
    vkCreateSemaphore(device, &semi, nullptr, &semImage); vkCreateSemaphore(device, &semi, nullptr, &semRender); vkCreateFence(device, &fi, nullptr, &fence);

    createBuf(sizeof(UBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, uboBuf, uboMem);

    loadTex("image.png", textures[0]);
    loadTex("depth.png", textures[1]);
    loadTex("image_bg.png", textures[2]);
    loadTex("depth_bg.png", textures[3]);
    loadTex("subject_mask.png", textures[4]);

    VkDescriptorPoolSize dps[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 1, 2, dps};
    vkCreateDescriptorPool(device, &dpi, nullptr, &descPool);
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, descPool, 1, &descLayout};
    vkAllocateDescriptorSets(device, &dai, &descSet);

    std::vector<VkWriteDescriptorSet> wds(6); std::vector<VkDescriptorImageInfo> dii(5);
    for(int i=0; i<5; i++) {
        dii[i] = {textures[i].smp, textures[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        wds[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descSet, (uint32_t)i, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &dii[i], nullptr, nullptr};
    }
    VkDescriptorBufferInfo dbi{uboBuf, 0, sizeof(UBO)};
    wds[5] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descSet, 5, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &dbi, nullptr};
    vkUpdateDescriptorSets(device, 6, wds.data(), 0, nullptr);

    return true;
}

extern "C" {
JNIEXPORT jboolean JNICALL Java_com_df_depthflow_1mobile_MainActivity_initVulkan(JNIEnv* env, jobject, jobject am, jobject s) {
    if(isInit) return JNI_TRUE;
    assetMgr = AAssetManager_fromJava(env, am);
    ANativeWindow* win = ANativeWindow_fromSurface(env, s);
    if(initVK(win)) {
        isInit=true; ubo.height=0.05f; ubo.steady=0.5f; ubo.zoom=1.0f; ubo.quality=0.5f; ubo.inpaint=0.01f;
        return JNI_TRUE;
    }
    return JNI_FALSE;
}
JNIEXPORT void JNICALL Java_com_df_depthflow_1mobile_MainActivity_setParams(JNIEnv*, jobject, jfloat x, jfloat y, jfloat z, jfloat h) {
ubo.offset.x=x; ubo.offset.y=y; ubo.zoom=z; ubo.height=h;
}
JNIEXPORT void JNICALL Java_com_df_depthflow_1mobile_MainActivity_drawFrame(JNIEnv*, jobject) {
if(!isInit) return;
vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX); vkResetFences(device, 1, &fence);
uint32_t i; if(vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, semImage, VK_NULL_HANDLE, &i) != VK_SUCCESS) return;

static auto t0 = std::chrono::high_resolution_clock::now();
ubo.time = std::chrono::duration<float>(std::chrono::high_resolution_clock::now()-t0).count();
ubo.screenSize = {(float)scExtent.width, (float)scExtent.height};
ubo.imgSize = {g_w, g_h};

void* d; vkMapMemory(device, uboMem, 0, sizeof(UBO), 0, &d); memcpy(d, &ubo, sizeof(UBO)); vkUnmapMemory(device, uboMem);

vkResetCommandBuffer(cmdBuf, 0);
VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; vkBeginCommandBuffer(cmdBuf, &bi);
VkRenderPassBeginInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, renderPass, scFBs[i], {{0,0}, scExtent}, 1};
VkClearValue cv={{0,0,0,1}}; rpi.pClearValues=&cv;
vkCmdBeginRenderPass(cmdBuf, &rpi, VK_SUBPASS_CONTENTS_INLINE);
vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout, 0, 1, &descSet, 0, nullptr);
vkCmdDraw(cmdBuf, 3, 1, 0, 0);
vkCmdEndRenderPass(cmdBuf); vkEndCommandBuffer(cmdBuf);

VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 1, &semImage, &wait, 1, &cmdBuf, 1, &semRender};
vkQueueSubmit(graphicsQueue, 1, &si, fence);
VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1, &semRender, 1, &swapchain, &i};
vkQueuePresentKHR(graphicsQueue, &pi); vkQueueWaitIdle(graphicsQueue);
}
JNIEXPORT void JNICALL Java_com_df_depthflow_1mobile_MainActivity_cleanup(JNIEnv*, jobject) { if(isInit) vkDeviceWaitIdle(device); }
}