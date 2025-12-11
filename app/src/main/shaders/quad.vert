#version 450
layout(location = 0) out vec2 fragTexCoord;

void main() {
    // 使用一个大三角形覆盖全屏 (不需要 VBO)
    vec2 pos[3] = vec2[](
        vec2(-1.0, -1.0),
        vec2( 3.0, -1.0),
        vec2(-1.0, 3.0)
    );
    gl_Position = vec4(pos[gl_VertexIndex], 0.0, 1.0);
    fragTexCoord = pos[gl_VertexIndex] * 0.5 + 0.5;
}