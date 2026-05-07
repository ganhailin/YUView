#version 330 core

in vec2 vTexCoord;
out vec4 fragColor;

uniform usampler2D texture16bit;
uniform int bitDepth;

float bayerDither4x4(vec2 position, int bitDepth)
{
    int x = int(mod(position.x, 4.0));
    int y = int(mod(position.y, 4.0));
    
    int matrix[16] = int[](
        0,  8,  2, 10,
       12,  4, 14,  6,
        3, 11,  1,  9,
       15,  7, 13,  5
    );
    
    int idx = x + y * 4;
    float threshold = float(matrix[idx]) / 16.0 - 0.5;
    float range = float(1 << bitDepth);
    return threshold / range;
}

void main()
{
    uvec4 raw = texture(texture16bit, vTexCoord);
    vec2 pixelPos = gl_FragCoord.xy;
    
    float dither = bayerDither4x4(pixelPos, bitDepth);
    
    float maxValue = float((1 << bitDepth) - 1);
    vec3 color = vec3(raw.rgb) / 65535.0;
    color = color * maxValue + dither;
    color = clamp(color, 0.0, maxValue);
    color = color / maxValue;
    
    fragColor = vec4(color, 1.0);
}
