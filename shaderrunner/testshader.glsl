#version 300 es
layout(location = 0) in vec3 iPos;
uniform float iTime;
uniform int iFrame;
out vec3 outColor;

void main() {
    outColor = vec3(
        sin(iPos.x + iTime),
        sin(iPos.y + iTime*2.0f),
        sin(iPos.z + iTime*4.0f)
    );
    
    outColor = (0.5f + 0.5f*outColor) * 0.2f;
}
