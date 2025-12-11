#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D texImage;
layout(binding = 1) uniform sampler2D texDepth;
layout(binding = 2) uniform sampler2D texImageBg;
layout(binding = 3) uniform sampler2D texDepthBg;
layout(binding = 4) uniform sampler2D texMask;

// UBO 保持不变
layout(binding = 5) uniform UBO {
    float height, steady, focus, zoom;
    float isometric, dolly, invert, mirror;
    vec2 offset; vec2 center;
    vec2 origin; float time; float aspect;
    vec2 screenSize; vec2 imgSize;
    float inpaint, quality, vig, sat;
    float con, bri, gam, sep;
    float gray, pad1, pad2, pad3;
} u;

// 纹理采样 (保持不变)
vec4 gtexture(sampler2D tex, vec2 uv, float mirror) {
    if (mirror > 0.5) uv = abs(fract(uv * 0.5 + 0.5) * 2.0 - 1.0);
    else uv = clamp(uv, 0.0, 1.0);
    return texture(tex, uv);
}

struct RayResult { vec2 uv; float val; float steep; bool oob; };

// 核心算法 (保持你原本正常的逻辑)
RayResult RayMarch(vec2 uv, vec2 dir, sampler2D depthMap, float mirror, float h, float inv) {
    RayResult res;
    res.uv = uv; res.oob = false;
    int steps = int(mix(30.0, 60.0, u.quality));
    float stepSize = 1.0 / float(steps);
    vec2 delta = dir * h * 0.5;
    float lastH = 0.0;

    for(int i=0; i<steps; i++) {
        float t = float(i) * stepSize;
        vec2 curUV = uv + delta * t;
        float d = texture(depthMap, curUV).r;
        if(inv > 0.5) d = 1.0 - d;
        float rayH = 1.0 - t;
        if(rayH < d) { // Hit
            float weight = (d - rayH) / ((d - rayH) - (lastH - (rayH + stepSize)) + 0.0001);
            res.uv = uv + delta * (t - stepSize * weight);
            res.val = d;
            break;
        }
        lastH = d;
        res.uv = curUV;
    }

    // 既然你手机支持这个，就保留它，这是效果最好的
    float dx = dFdx(res.val); float dy = dFdy(res.val);
    res.steep = sqrt(dx*dx + dy*dy) * 10.0;

    if(res.uv.x < 0.0 || res.uv.x > 1.0 || res.uv.y < 0.0 || res.uv.y > 1.0) res.oob = true;
    return res;
}

void main() {
    // === 1. UV 适配 (保持你原本正常的逻辑) ===
    // 之前改这里导致了平铺问题，现在改回来
    float scrRatio = u.screenSize.x / u.screenSize.y;
    float imgRatio = u.imgSize.x / u.imgSize.y;
    float fitRatio = scrRatio / imgRatio;

    vec2 uv = fragTexCoord - 0.5;
    uv.y /= fitRatio;
    uv /= u.zoom;
    vec2 baseUV = uv + 0.5;

    // 黑边裁切
    if(baseUV.x < 0.0 || baseUV.x > 1.0 || baseUV.y < 0.0 || baseUV.y > 1.0) {
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // === 2. 视差计算 ===
    vec2 rayDir = -u.offset;
    rayDir += vec2(sin(u.time*0.5), cos(u.time*0.4)) * 0.01 * u.steady;
    rayDir += uv * u.focus * 0.1;

    // 前景计算
    RayResult fg = RayMarch(baseUV, rayDir, texDepth, u.mirror, u.height, u.invert);

    // === 3. 背景计算 (关键修改点！) ===
    // [PC逻辑复刻]
    // 1. 背景不要乘 0.5，要和前景完全同步 (rayDir)
    // 2. 强制 mirror = 1.0
    vec2 bgDir = rayDir;
    RayResult bg = RayMarch(baseUV, bgDir, texDepthBg, 1.0, u.height, u.invert);

    // 获取颜色
    vec4 cFG = gtexture(texImage, fg.uv, u.mirror);
    vec4 cBG = gtexture(texImageBg, bg.uv, 1.0); // 背景强制镜像

    // === 4. 混合遮罩 (保持你原本正常的逻辑) ===
    float mOrg = texture(texMask, baseUV).r;
    float mDst = texture(texMask, fg.uv).r;
    float subj = max(mOrg, mDst);

    // PC 端的阈值微调
    float inpaint = max(u.inpaint, 0.05); // 防止为0
    float baseM = smoothstep(inpaint, inpaint+0.1, fg.steep);

    // 主体区域保护 (Steepness 容忍度 x3)
    float subjM = smoothstep(inpaint*3.0, inpaint*3.0+0.3, fg.steep);

    float mask = mix(baseM, subjM, smoothstep(0.2, 0.5, subj));

    if(fg.oob) {
        if(subj < 0.5) mask = 1.0;
        else mask = mix(0.0, 1.0, smoothstep(0.5, 0.8, fg.steep));
    }

    outColor = mix(cFG, cBG, mask);
}