#version 330 core

in vec2 vTexCoord;
out vec4 fragColor;

uniform usampler2D texture16bit;
uniform int bitDepth;

void main()
{
    uvec4 raw = texture(texture16bit, vTexCoord);
    float maxValue = float((1 << bitDepth) - 1);
    vec3 color = vec3(raw.rgb) / maxValue;
    fragColor = vec4(color, 1.0);
}
