#version 330 core

in vec2 vTexCoord;
out vec4 fragColor;

uniform usampler2D texture16bit;

void main()
{
    uvec4 raw = texture(texture16bit, vTexCoord);
    // Data is always stored in 16-bit range (0-65535), regardless of source bit depth
    // For 8-bit source: values are scaled by 257 (0-255 -> 0-65535)
    // For 10-bit source: values are left-shifted by 6 (0-1023 -> 0-65472)
    // For 16-bit source: values are native (0-65535)
    vec3 color = vec3(raw.rgb) / 65535.0;
    fragColor = vec4(color, 1.0);
}
