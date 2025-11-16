#version 300 es
layout(location = 0) in vec3 iPos;
uniform float iTime;
out vec3 outColor;

void main() {
    vec3 color = vec3(0.0);
    
    // perform an arbitrary number of mathematically heavy calculations
    for (int i = 0; i < 1000000; i++) {
        float x = sin(iPos.x + iTime) * cos(iPos.y * float(i) * 0.0001) * tan(iPos.z + float(i) * 0.0001);
        float y = cos(iPos.y + iTime) * sin(iPos.x * float(i) * 0.0001) * pow(iPos.z + iTime, 2.0);
        float z = tan(iPos.z + iTime) * cos(iPos.x * float(i) * 0.0001) * sin(iPos.y + float(i) * 0.0001);
        
        color += vec3(x, y, z) * 0.00001;
    }

    outColor = color;
}
