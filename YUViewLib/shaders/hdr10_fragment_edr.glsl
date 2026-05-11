#version 330 core

in vec2 vTexCoord;
out vec4 fragColor;

uniform usampler2D texture16bit;
uniform float maxDisplayBrightness;    // Maximum brightness of display in nits (e.g., 100.0 for SDR, 1000.0 for HDR)
uniform float maxContentBrightness;    // Maximum brightness of content in nits (from HDR metadata)
uniform float edrHeadroom;             // EDR headroom multiplier (e.g., 4.0 for 4x SDR brightness)
uniform int transferFunction;          // 0 = Linear, 1 = PQ (SMPTE ST 2084), 2 = HLG (ARIB STD-B67)
uniform int colorSpace;                // 0 = BT.709, 1 = BT.2020, 2 = P3-D65

// Constants for PQ (SMPTE ST 2084)
const float PQ_M1 = 2610.0 / 4096.0 / 4.0;   // 0.1593017578125
const float PQ_M2 = 2523.0 / 4096.0 * 128.0; // 78.84375
const float PQ_C1 = 3424.0 / 4096.0;         // 0.8359375
const float PQ_C2 = 2413.0 / 4096.0 * 32.0;  // 18.8515625
const float PQ_C3 = 2392.0 / 4096.0 * 32.0;  // 18.6875
const float PQ_MAX_NITS = 10000.0;

// Constants for HLG (ARIB STD-B67)
const float HLG_A = 0.17883277;
const float HLG_B = 0.28466892;
const float HLG_C = 0.55991073;
const float HLG_MAX_NITS = 1000.0;

// BT.709 to linear RGB conversion matrix
const mat3 BT709_TO_RGB = mat3(
    1.0,  0.0,      0.0,
    0.0,  1.0,      0.0,
    0.0,  0.0,      1.0
);

// BT.2020 to linear RGB conversion matrix
const mat3 BT2020_TO_RGB = mat3(
    1.716651188,   -0.35567078,   -0.253366281,
    -0.666684351,   1.616481237,   0.015768538,
    0.017639857,   -0.042770613,   1.025103748
);

// P3-D65 to linear RGB conversion matrix (sRGB/linear RGB)
const mat3 P3D65_TO_RGB = mat3(
    1.224940176,   -0.224940176,    0.0,
    -0.042056955,    1.042056955,    0.0,
    -0.019637554,   -0.078636046,    1.09827355
);

// PQ inverse EOTF (decode PQ to linear)
float pqToLinear(float pq)
{
    float np = pow(pq, 1.0 / PQ_M2);
    float linear = pow(max(np - PQ_C1, 0.0) / (PQ_C2 - PQ_C3 * np), 1.0 / PQ_M1);
    return linear * PQ_MAX_NITS;  // Convert to nits
}

// HLG inverse OETF (decode HLG to linear)
float hlgToLinear(float hlg)
{
    if (hlg <= 0.5)
    {
        return hlg * hlg / 3.0 * HLG_MAX_NITS;
    }
    else
    {
        return (exp((hlg - HLG_C) / HLG_A) + HLG_B) / 12.0 * HLG_MAX_NITS;
    }
}

// Apply color space conversion
vec3 convertColorSpace(vec3 rgb, int space)
{
    if (space == 1)  // BT.2020
    {
        return BT2020_TO_RGB * rgb;
    }
    else if (space == 2)  // P3-D65
    {
        return P3D65_TO_RGB * rgb;
    }
    // BT.709 is default (identity)
    return rgb;
}

void main()
{
    // Sample the 16-bit texture
    uvec4 raw = texture(texture16bit, vTexCoord);

    // Convert to normalized float (0.0 to 1.0 based on 16-bit range)
    vec3 color = vec3(raw.rgb) / 65535.0;

    // Apply inverse transfer function to get linear light values
    vec3 linearColor;
    if (transferFunction == 1)  // PQ
    {
        linearColor.r = pqToLinear(color.r);
        linearColor.g = pqToLinear(color.g);
        linearColor.b = pqToLinear(color.b);
    }
    else if (transferFunction == 2)  // HLG
    {
        linearColor.r = hlgToLinear(color.r);
        linearColor.g = hlgToLinear(color.g);
        linearColor.b = hlgToLinear(color.b);
    }
    else  // Linear (assume content is already linear or in gamma space)
    {
        // For linear input, assume values are normalized to maxContentBrightness
        // Scale from [0, 1] to [0, maxContentBrightness] nits
        linearColor = color * maxContentBrightness;
    }

    // Apply color space conversion if needed
    linearColor = convertColorSpace(linearColor, colorSpace);

    // Normalize to display capabilities
    // For SDR displays: scale to 100 nits (1.0 in normalized linear)
    // For HDR displays: scale to maxDisplayBrightness, with EDR headroom
    float targetWhite = 100.0;  // SDR white level (100 nits)

    // Calculate the scaling factor based on content and display capabilities
    float contentScale = maxContentBrightness / targetWhite;
    float displayScale = maxDisplayBrightness / targetWhite;
    float edrScale = edrHeadroom;

    // Scale the color values:
    // 1. First normalize to the target white level
    // 2. Then apply EDR headroom scaling
    // This allows values > 1.0 for HDR highlights
    vec3 outputColor = linearColor / targetWhite;

    // Apply tone mapping if content brightness exceeds display capabilities
    float maxOutput = max(max(outputColor.r, outputColor.g), outputColor.b);
    if (maxOutput > edrScale)
    {
        // Simple clipping for now (could use more sophisticated tone mapping)
        // Scale down to fit within EDR headroom
        float scale = edrScale / maxOutput;
        outputColor *= scale;
    }

    // Clamp to valid range (EDR allows values > 1.0, but we should still clamp to reasonable limits)
    outputColor = clamp(outputColor, 0.0, edrScale * 2.0);

    fragColor = vec4(outputColor, 1.0);
}
