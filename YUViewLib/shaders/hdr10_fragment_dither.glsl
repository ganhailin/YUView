#version 330 core

in vec2 vTexCoord;
out vec4 fragColor;

uniform usampler2D texture16bit;

// Dithering is always to 8-bit output, since this shader is used when displaying
// high bit-depth content on standard 8-bit monitors to reduce banding.
// The 4x4 Bayer matrix gives 16 levels (4-bit), which is added to 8-bit output
// to simulate higher precision through spatial distribution.

float bayerDither4x4(vec2 position)
{
    int x = int(mod(position.x, 4.0));
    int y = int(mod(position.y, 4.0));

    // Pre-computed Bayer matrix values (0-15 normalized to ±0.5, then scaled to 1/256)
    // Formula: (value / 16.0 - 0.5) / 256.0
    float matrix[16] = float[](
        -0.001953125,   // 0:  (0/16 - 0.5) / 256
         0.000000000,   // 8:  (8/16 - 0.5) / 256
        -0.001464844,   // 2:  (2/16 - 0.5) / 256
         0.000488281,   // 10: (10/16 - 0.5) / 256
         0.000976562,   // 12: (12/16 - 0.5) / 256
        -0.000976562,   // 4:  (4/16 - 0.5) / 256
         0.001464844,   // 14: (14/16 - 0.5) / 256
        -0.000488281,   // 6:  (6/16 - 0.5) / 256
        -0.001220703,   // 3:  (3/16 - 0.5) / 256
         0.000732422,   // 11: (11/16 - 0.5) / 256
        -0.001708984,   // 1:  (1/16 - 0.5) / 256
         0.000244141,   // 9:  (9/16 - 0.5) / 256
         0.001708984,   // 15: (15/16 - 0.5) / 256
        -0.000244141,   // 7:  (7/16 - 0.5) / 256
         0.001220703,   // 13: (13/16 - 0.5) / 256
        -0.000732422    // 5:  (5/16 - 0.5) / 256
    );

    return matrix[x + y * 4];
}

void main()
{
    uvec4 raw = texture(texture16bit, vTexCoord);
    vec2 pixelPos = gl_FragCoord.xy;

    // Data is always stored in 16-bit range (0-65535)
    // Normalize to 0.0-1.0 range first
    vec3 color = vec3(raw.rgb) / 65535.0;

    // Apply Bayer dithering to reduce banding when displaying on 8-bit monitors
    float dither = bayerDither4x4(pixelPos);

    color = color + dither;

    fragColor = vec4(color, 1.0);
}
