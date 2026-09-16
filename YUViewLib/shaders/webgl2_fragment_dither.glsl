#version 300 es

precision highp float;
precision highp int;
precision highp usampler2D;

in vec2 vTexCoord;
out vec4 fragColor;

uniform usampler2D texture16bit;
uniform int   eotfType;
uniform float gammaValue;
uniform float diffuseWhiteNits;
uniform float hdrBrightness;
uniform mat3  gamutMatrix;
uniform float systemHandlesTonemapping;
uniform float applySRGBOETF;
uniform int   premultipliedAlpha;

vec3 pqEotf(vec3 pq, float diffuseWhite)
{
    const float m1 = 2610.0 / 4096.0 * (1.0 / 4.0);
    const float m2 = 2523.0 / 4096.0 * 128.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 4096.0 * 32.0;
    const float c3 = 2392.0 / 4096.0 * 32.0;
    vec3 p = pow(pq, vec3(1.0 / m2));
    vec3 num = max(p - vec3(c1), vec3(0.0));
    vec3 den = vec3(c2) - vec3(c3) * p;
    return pow(num / den, vec3(1.0 / m1)) * 10000.0 / diffuseWhite;
}

vec3 hlgEotf(vec3 hlg)
{
    const float a = 0.17883277;
    const float b = 0.28466892;
    const float c = 0.55991073;
    vec3 linear;
    for (int i = 0; i < 3; ++i) {
        if (hlg[i] <= 0.5)
            linear[i] = hlg[i] * hlg[i] / 3.0;
        else
            linear[i] = (exp((hlg[i] - c) / a) + b) / 12.0;
    }
    return linear * 4.0;
}

vec3 gammaEotf(vec3 gamma, float gammaVal)
{
    return pow(gamma, vec3(gammaVal));
}

vec3 srgbEotf(vec3 srgb)
{
    vec3 linear;
    for (int i = 0; i < 3; ++i) {
        if (srgb[i] <= 0.04045)
            linear[i] = srgb[i] / 12.92;
        else
            linear[i] = pow((srgb[i] + 0.055) / 1.055, 2.4);
    }
    return linear;
}

vec3 srgbOetf(vec3 linear)
{
    vec3 srgb;
    for (int i = 0; i < 3; ++i) {
        if (linear[i] <= 0.0031308)
            srgb[i] = linear[i] * 12.92;
        else
            srgb[i] = 1.055 * pow(linear[i], 1.0 / 2.4) - 0.055;
    }
    return srgb;
}

float bayerDither4x4(vec2 position)
{
    int x = int(mod(position.x, 4.0));
    int y = int(mod(position.y, 4.0));
    float matrix[16] = float[16](
        -0.001953125,  0.000000000, -0.001464844,  0.000488281,
         0.000976562, -0.000976562,  0.001464844, -0.000488281,
        -0.001220703,  0.000732422, -0.001708984,  0.000244141,
         0.001708984, -0.000244141,  0.001220703, -0.000732422);
    return matrix[x + y * 4];
}

void main()
{
    uvec4 raw = texture(texture16bit, vTexCoord);
    vec3 color = vec3(raw.rgb) / 65535.0;
    float alpha = float(raw.a) / 65535.0;
    if (premultipliedAlpha == 0)
        color *= alpha;

    vec3 linear;
    if (eotfType == 0)
        linear = pqEotf(color, diffuseWhiteNits);
    else if (eotfType == 1)
        linear = hlgEotf(color);
    else if (eotfType == 2)
        linear = gammaEotf(color, gammaValue);
    else
        linear = srgbEotf(color);

    linear = gamutMatrix * linear;
    linear *= hdrBrightness;
    if (systemHandlesTonemapping < 0.5) {
        float luminance = dot(linear, vec3(0.2126, 0.7152, 0.0722));
        float mappedLum = luminance / (1.0 + luminance);
        if (luminance > 0.001)
            linear *= mappedLum / luminance;
    }

    linear += bayerDither4x4(gl_FragCoord.xy);
    if (applySRGBOETF > 0.5)
        linear = srgbOetf(linear);
    fragColor = vec4(linear, alpha);
}
