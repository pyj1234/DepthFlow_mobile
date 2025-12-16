#version 450
precision highp float;

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D texImage;
layout(binding = 1) uniform sampler2D texDepth;
layout(binding = 2) uniform sampler2D texImageBg;
layout(binding = 3) uniform sampler2D texDepthBg;
layout(binding = 4) uniform sampler2D texMask;

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

// 镜像采样
vec4 gtexture(sampler2D tex, vec2 uv) {
    vec2 mUV = abs(fract(uv * 0.5 + 0.5) * 2.0 - 1.0);
    return textureLod(tex, mUV, 0.0);
}

// 获取深度
float getDepth(sampler2D map, vec2 uv, float inv) {
    vec2 mUV = abs(fract(uv * 0.5 + 0.5) * 2.0 - 1.0);
    float d = textureLod(map, mUV, 0.0).r;
    if(inv > 0.5) d = 1.0 - d;
    return d;
}

struct RayResult { vec2 uv; float val; float steep; };

// 核心视差算法
RayResult RayMarch(vec2 uv, vec2 dir, sampler2D depthMap, float h, float inv) {
    RayResult res;
    res.uv = uv;

    // 动态步数 (限制最大步数防止过载)
    float offsetLen = length(u.offset);
    float qualityMod = mix(30.0, 80.0, u.quality);
    int steps = int(qualityMod + min(offsetLen, 2.0) * 40.0);
    steps = min(steps, 80);

    float stepSize = 1.0 / float(steps);
    vec2 delta = dir * h * 0.5;

    vec2 currUV = uv;
    float currRayH = 1.0;
    float prevRayH = 1.0;
    float currD = 0.0;
    float prevD = 0.0;
    int hitIndex = -1;

    // 1. 线性搜索
    for(int i=0; i<steps; i++) {
        currD = getDepth(depthMap, currUV, inv);
        currRayH = 1.0 - float(i) * stepSize;
        if(currRayH < currD) {
            hitIndex = i;
            break;
        }
        prevRayH = currRayH;
        prevD = currD;
        currUV += delta * stepSize;
    }

    // 2. 二分查找
    if(hitIndex != -1) {
        vec2 uvBefore = currUV - delta * stepSize;
        vec2 uvAfter = currUV;
        float hBefore = prevRayH;
        float hAfter = currRayH;
        vec2 finalUV = uvAfter;
        float finalD = currD;

        for(int j=0; j<5; j++) {
            vec2 midUV = mix(uvBefore, uvAfter, 0.5);
            float midRayH = mix(hBefore, hAfter, 0.5);
            float midD = getDepth(depthMap, midUV, inv);

            if(midRayH < midD) {
                uvAfter = midUV;
                hAfter = midRayH;
                finalUV = midUV;
                finalD = midD;
            } else {
                uvBefore = midUV;
                hBefore = midRayH;
            }
        }
        res.uv = finalUV;
        res.val = finalD;
    } else {
        res.uv = currUV;
        res.val = currD;
    }

    // === [修复] 优化锯齿状拉伸 ===
    // 使用像素级采样而非固定的 0.01，解决边缘粗糙问题
    vec2 px = vec2(0.002);
    if(u.imgSize.x > 0.0) px = 1.0 / u.imgSize;

    // 使用中心差分计算梯度 (更平滑)
    float d_l = getDepth(depthMap, res.uv - vec2(px.x, 0.0), inv);
    float d_r = getDepth(depthMap, res.uv + vec2(px.x, 0.0), inv);
    float d_u = getDepth(depthMap, res.uv - vec2(0.0, px.y), inv);
    float d_d = getDepth(depthMap, res.uv + vec2(0.0, px.y), inv);

    float gradX = abs(d_l - d_r);
    float gradY = abs(d_u - d_d);
    float gradient = max(gradX, gradY);

    float safeOffset = min(offsetLen, 1.0);
    // 提高灵敏度系数以适配更精细的采样 (6.0 -> 25.0)
    res.steep = gradient * safeOffset * 25.0;

    return res;
}

void main() {
    float scrRatio = u.screenSize.x / u.screenSize.y;
    float imgRatio = u.imgSize.x / u.imgSize.y;
    float fitRatio = scrRatio / imgRatio;

    vec2 uv = fragTexCoord - 0.5;
    uv.y /= fitRatio;
    uv /= u.zoom;
    vec2 baseUV = uv + 0.5;

    if(baseUV.x < 0.0 || baseUV.x > 1.0 || baseUV.y < 0.0 || baseUV.y > 1.0) {
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec2 rayDir = -u.offset;
    // 移除呼吸动画，保证初始居中
    rayDir += uv * u.focus * 0.1;

    RayResult fg = RayMarch(baseUV, rayDir, texDepth, u.height, u.invert);
    RayResult bg = RayMarch(baseUV, rayDir, texDepthBg, u.height, u.invert);

    vec4 cFG = gtexture(texImage, fg.uv);
    vec4 cBG = gtexture(texImageBg, bg.uv);

    // 混合遮罩
    float mOrg = gtexture(texMask, baseUV).r;
    float mDst = gtexture(texMask, fg.uv).r;
    float subj = max(mOrg, mDst);
    subj = smoothstep(0.1, 0.6, subj);

    // 提高 inpaint 基础阈值
    float inpaint = max(u.inpaint, 0.15);

    float baseM = smoothstep(inpaint, inpaint + 0.2, fg.steep);

    // === [修复] 优化主体拉伸保护 ===
    // 降低保护阈值，当检测到明显撕裂时，允许背景显示，切断拉伸
    float safeThreshold = inpaint + 0.35;
    float subjM = smoothstep(safeThreshold, safeThreshold + 0.5, fg.steep);

    // 额外的拉伸保护: 如果像素偏移过远，强制使用背景
    float stretchDist = distance(fg.uv, baseUV);
    float limit = length(u.offset) * u.height * 2.0;
    if(limit > 0.0 && stretchDist > limit) {
        subjM = 1.0;
        baseM = 1.0;
    }

    float mask = mix(baseM, subjM, subj);

    vec4 finalColor = mix(cFG, cBG, mask);

    if(u.sat > 0.1 && u.sat != 1.0) {
        vec3 gray = vec3(dot(finalColor.rgb, vec3(0.299, 0.587, 0.114)));
        finalColor.rgb = mix(gray, finalColor.rgb, u.sat);
    }


    outColor = finalColor;
}