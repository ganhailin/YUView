#version 330 core

// HDR10 Standard Fragment Shader
//
// Unified color processing pipeline (shared logic with HLSL/MSL backends):
//   1. Sample 16-bit RGBA texture (0-65535 range)
//   2. Normalize to 0-1 range
//   3. Apply EOTF (PQ/HLG/Gamma/sRGB) → linear light
//   4. Diffuse white normalization (for PQ: divide by diffuseWhiteNits)
//   5. Gamut conversion (source → Display P3 via 3x3 matrix)
//   6. Reinhard tonemapping (SDR without system tonemapping)
//   7. sRGB OETF (linear → sRGB curve) for SDR display
//   8. Output RGBA8 (0-1 range, sRGB encoded)

in vec2 vTexCoord;
out vec4 fragColor;

uniform usampler2D texture16bit;

// Uniforms for color processing
uniform int   eotfType;                    // 0=PQ, 1=HLG, 2=Gamma, 3=sRGB
uniform int   sourceGamut;                 // 0=BT2020, 1=BT709, 2=P3
uniform float gammaValue;                  // Gamma value (when eotfType == 2)
uniform float diffuseWhiteNits;            // Diffuse white brightness in nits (default: 203)
uniform float hdrBrightness;               // HDR brightness multiplier (default: 1.0)
uniform mat3  gamutMatrix;                 // 3x3 gamut conversion matrix (source → Display P3)
uniform float systemHandlesTonemapping;    // 1.0 = system does tonemapping, skip Reinhard
uniform float applySRGBOETF;               // 1.0 = apply sRGB OETF for SDR output

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

// ========== Color processing pipeline ==========
vec3 processColor(vec3 color)
{
    // EOTF Conversion
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

    // Gamut Conversion
    if (sourceGamut != 2) {
        linear = gamutMatrix * linear;
    }

    // HDR Brightness Adjustment
    linear *= hdrBrightness;

    // Reinhard Tonemapping (SDR without system tonemapping)
    if (systemHandlesTonemapping < 0.5) {
        float luminance = dot(linear, vec3(0.2126, 0.7152, 0.0722));
        float mappedLum = luminance / (1.0 + luminance);
        if (luminance > 0.001)
            linear *= mappedLum / luminance;
    }

    // sRGB OETF (SDR output only)
    if (applySRGBOETF > 0.5) {
        linear = srgbOetf(linear);
    }

    return linear;
}

void main()
{
    uvec4 raw = texture(texture16bit, vTexCoord);
    vec3 color = vec3(raw.rgb) / 65535.0;
    vec3 outputColor = processColor(color);
    fragColor = vec4(outputColor, 1.0);
}
