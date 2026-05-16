#version 330 core

// HDR10 Dither Fragment Shader
//
// Same color processing pipeline as hdr10_fragment.glsl, with Bayer dithering
// applied before sRGB OETF to reduce banding on 8-bit displays.
//
// Color processing pipeline (SDR output):
//   1. Sample 16-bit RGBA texture (0-65535 range)
//   2. Normalize to 0-1 range
//   3. Apply EOTF (PQ/HLG/Gamma/sRGB) → linear light
//   4. Diffuse white normalization (for PQ: divide by diffuseWhiteNits)
//   5. Gamut conversion (BT.2020/BT.709/P3 → Display P3 via 3x3 matrix)
//   6. Apply Bayer dithering (in linear light)
//   7. sRGB OETF (linear → sRGB curve + gamut) for SDR display
//   8. Output RGBA8 (0-1 range, sRGB encoded)

in vec2 vTexCoord;
out vec4 fragColor;

uniform usampler2D texture16bit;

// Uniforms for color processing
uniform int   eotfType;           // 0=PQ, 1=HLG, 2=Gamma, 3=sRGB
uniform int   sourceGamut;        // 0=BT2020, 1=BT709, 2=P3
uniform float gammaValue;         // Gamma value (when eotfType == 2)
uniform float diffuseWhiteNits;   // Diffuse white brightness in nits (default: 203)
uniform float hdrBrightness;      // HDR brightness multiplier (default: 1.0)
uniform mat3  gamutMatrix;        // 3x3 gamut conversion matrix (source → Display P3)

// ========== EOTF Functions ==========

// PQ EOTF (SMPTE ST 2084)
vec3 pqEotf(vec3 pq, float diffuseWhite) {
    const float m1 = 2610.0 / 4096.0 * (1.0 / 4.0);
    const float m2 = 2523.0 / 4096.0 * 128.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 4096.0 * 32.0;
    const float c3 = 2392.0 / 4096.0 * 32.0;

    vec3 p = pow(pq, vec3(1.0 / m2));
    vec3 num = max(p - vec3(c1), vec3(0.0));
    vec3 den = vec3(c2) - vec3(c3) * p;
    vec3 linear = pow(num / den, vec3(1.0 / m1));

    return linear * 10000.0 / diffuseWhite;
}

// HLG EOTF (ARIB STD-B67)
vec3 hlgEotf(vec3 hlg) {
    const float a = 0.17883277;
    const float b = 0.28466892;
    const float c = 0.55991073;

    vec3 linear;
    for (int i = 0; i < 3; i++) {
        if (hlg[i] <= 0.5) {
            linear[i] = hlg[i] * hlg[i] / 3.0;
        } else {
            linear[i] = (exp((hlg[i] - c) / a) + b) / 12.0;
        }
    }
    return linear * 4.0;
}

// Gamma EOTF
vec3 gammaEotf(vec3 gamma, float gammaVal) {
    return pow(gamma, vec3(gammaVal));
}

// sRGB EOTF (piecewise)
vec3 srgbEotf(vec3 srgb) {
    vec3 linear;
    for (int i = 0; i < 3; i++) {
        if (srgb[i] <= 0.04045) {
            linear[i] = srgb[i] / 12.92;
        } else {
            linear[i] = pow((srgb[i] + 0.055) / 1.055, 2.4);
        }
    }
    return linear;
}

// ========== sRGB OETF (for SDR output) ==========
vec3 srgbOetf(vec3 linear) {
    vec3 srgb;
    for (int i = 0; i < 3; i++) {
        if (linear[i] <= 0.0031308) {
            srgb[i] = linear[i] * 12.92;
        } else {
            srgb[i] = 1.055 * pow(linear[i], 1.0 / 2.4) - 0.055;
        }
    }
    return srgb;
}

// ========== Bayer Dithering ==========
// 4x4 Bayer matrix gives 16 levels (4-bit), added to reduce banding on 8-bit displays
float bayerDither4x4(vec2 position)
{
    int x = int(mod(position.x, 4.0));
    int y = int(mod(position.y, 4.0));

    // Pre-computed Bayer matrix values (0-15 normalized to ±0.5, then scaled to 1/256)
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

    // Normalize to 0-1 range
    vec3 color = vec3(raw.rgb) / 65535.0;

    // ========== EOTF Conversion ==========
    vec3 linear;
    if (eotfType == 0) {
        linear = pqEotf(color, diffuseWhiteNits);
    } else if (eotfType == 1) {
        linear = hlgEotf(color);
    } else if (eotfType == 2) {
        linear = gammaEotf(color, gammaValue);
    } else {
        linear = srgbEotf(color);
    }

    // ========== Gamut Conversion ==========
    if (sourceGamut != 2) {
        linear = gamutMatrix * linear;
    }

    // ========== HDR Brightness Adjustment ==========
    linear *= hdrBrightness;

    // ========== Bayer Dithering (in linear light) ==========
    float dither = bayerDither4x4(pixelPos);
    linear += dither;

    // ========== sRGB OETF (for SDR output) ==========
    vec3 outputColor = srgbOetf(linear);

    fragColor = vec4(outputColor, 1.0);
}
