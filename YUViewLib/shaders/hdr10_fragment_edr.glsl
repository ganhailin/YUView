#version 330 core

// macOS EDR HDR Fragment Shader
// 
// This shader outputs float values that can exceed 1.0, which triggers macOS EDR
// (Extended Dynamic Range) on displays that support it.
//
// Color processing pipeline:
//   1. Sample 16-bit RGBA texture (0-65535 range)
//   2. Normalize to 0-1 range
//   3. Apply EOTF (PQ/HLG/Gamma/sRGB) → linear light
//   4. Diffuse white normalization (for PQ: divide by diffuseWhiteNits)
//   5. Gamut conversion (BT.2020/BT.709/P3 → Display P3 via 3x3 matrix)
//   6. HDR brightness scaling
//   7. Output RGBA16Float (values > 1.0 automatically mapped to HDR by macOS)
//
// EDR on macOS:
//   SDR range: 0.0 - 1.0 (maps to ~100 nits)
//   EDR range: 1.0 - maxEDR (maps to display's HDR capability, up to ~1600 nits)
//   macOS automatically handles the tone mapping for values > 1.0

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
// Input: 0-1 (PQ encoded), Output: linear light normalized (diffuseWhite = 1.0)
vec3 pqEotf(vec3 pq, float diffuseWhite) {
    const float m1 = 2610.0 / 4096.0 * (1.0 / 4.0);  // 0.1593017578
    const float m2 = 2523.0 / 4096.0 * 128.0;         // 78.84375
    const float c1 = 3424.0 / 4096.0;                  // 0.8359375
    const float c2 = 2413.0 / 4096.0 * 32.0;          // 18.8515625
    const float c3 = 2392.0 / 4096.0 * 32.0;          // 18.6875
    
    vec3 p = pow(pq, vec3(1.0 / m2));
    vec3 num = max(p - vec3(c1), vec3(0.0));
    vec3 den = vec3(c2) - vec3(c3) * p;
    vec3 linear = pow(num / den, vec3(1.0 / m1));
    
    // linear is 0-1 normalized to 10000 nits peak
    // Normalize so diffuseWhite nits → 1.0
    // Values > 1.0 are HDR highlights (will trigger EDR)
    return linear * 10000.0 / diffuseWhite;
}

// HLG EOTF (ARIB STD-B67)
// Output: 0-1 for SDR range, >1.0 for HDR highlights
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
    return linear;
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

void main()
{
    // Sample 16-bit unsigned integer texture
    uvec4 raw = texture(texture16bit, vTexCoord);
    
    // Normalize to 0-1 range
    // Data is stored in 16-bit range (0-65535):
    //   8-bit source: values scaled by 257 (0-255 → 0-65535)
    //   10-bit source: values left-shifted by 6 (0-1023 → 0-65472)
    //   16-bit source: native values (0-65535)
    vec3 color = vec3(raw.rgb) / 65535.0;
    
    // ========== EOTF Conversion ==========
    vec3 linear;
    if (eotfType == 0) {
        // PQ: normalized so diffuseWhite → 1.0, highlights > 1.0
        linear = pqEotf(color, diffuseWhiteNits);
    } else if (eotfType == 1) {
        // HLG: 0-1 SDR range, >1.0 HDR highlights
        linear = hlgEotf(color);
    } else if (eotfType == 2) {
        // Gamma
        linear = gammaEotf(color, gammaValue);
    } else {
        // sRGB (SDR - values stay in 0-1 range)
        linear = srgbEotf(color);
    }
    
    // ========== Gamut Conversion ==========
    // Convert from source gamut to Display P3
    if (sourceGamut != 2) {  // Skip if already P3
        linear = gamutMatrix * linear;
    }
    
    // ========== HDR Brightness Adjustment ==========
    // Values > 1.0 trigger macOS EDR on supported displays
    linear *= hdrBrightness;
    
    // Output: RGBA16Float allows values > 1.0
    // macOS EDR automatically maps extended range to display HDR capability
    fragColor = vec4(linear, 1.0);
}