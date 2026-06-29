/*  This file is part of YUView - The YUV player with advanced analytics toolset
 *   <https://github.com/IENT/YUView>
 *   Copyright (C) 2015  Institut für Nachrichtentechnik, RWTH Aachen University, GERMANY
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   In addition, as a special exception, the copyright holders give
 *   permission to link the code of portions of this program with the
 *   OpenSSL library under certain conditions as described in each
 *   individual source file, and distribute linked combinations including
 *   the two.
 *
 *   You must obey the GNU General Public License in all respects for all
 *   of the code used other than OpenSSL. If you modify file(s) with
 *   this exception, you may extend this exception to your version of the
 *   file(s), but you are not obligated to do so. If you delete
 *   this exception statement from your version. If you delete this exception
 *   statement from all source files in the program, then also delete it here.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "ColorPipeline.h"

#include <cmath>
#include <QColorSpace>
#include <QColorTransform>
#include <QtGui/QtGui>

namespace video::color {

// ── Gamut conversion matrices (row-major 3×3, via XYZ D65) ────────

// Identity (for same→same)
static const float IDENTITY[9] = {
    1.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 1.0f
};

// BT.2020 → BT.709 (used by DXGI scRGB, P709 primaries)
static const float BT2020_TO_BT709[9] = {
     1.6604900368e+00f, -5.8763847956e-01f, -7.2851557227e-02f,
    -1.2455039399e-01f,  1.1328984194e+00f, -8.3480253896e-03f,
    -1.8151044561e-02f, -1.0057861035e-01f,  1.1187296549e+00f
};

// BT.2020 → Display P3 (used by OpenGL/Metal)
static const float BT2020_TO_P3[9] = {
    1.343578f, -0.282180f, -0.061404f,
   -0.065298f,  1.075788f, -0.010490f,
    0.002822f, -0.019594f,  1.016915f
};

// BT.709 → BT.709 = identity

// BT.709 → Display P3
static const float BT709_TO_P3[9] = {
    0.822462f,  0.177536f, -0.000004f,
    0.033194f,  0.966807f, -0.000000f,
    0.017085f,  0.072414f,  0.910644f
};

// P3 → BT.709 (inverse of BT709_TO_P3, used by DXGI)
static const float P3_TO_BT709[9] = {
     1.2249389281e+00f, -2.2493772528e-01f, -2.2415925083e-06f,
    -4.2055920309e-02f,  1.0420564505e+00f,  1.3042267034e-06f,
    -1.9637885940e-02f, -7.8636645004e-02f,  1.0982732700e+00f
};

// P3 → P3 = identity

const float *getGamutMatrix(ColorGamut source, ColorGamut target)
{
  if (source == target)
    return IDENTITY;

  switch (source)
  {
    case ColorGamut::BT2020:
      return (target == ColorGamut::BT709) ? BT2020_TO_BT709 : BT2020_TO_P3;
    case ColorGamut::BT709:
      return (target == ColorGamut::BT709) ? IDENTITY : BT709_TO_P3;
    case ColorGamut::P3:
      return (target == ColorGamut::BT709) ? P3_TO_BT709 : IDENTITY;
    default:
      return IDENTITY;
  }
}

// BT.2020 chromaticity coordinates (CIE 1931 xy) — Qt 5 has no Bt2020 enum
static const QPointF BT2020_WHITE(0.3127f,  0.3290f);
static const QPointF BT2020_RED  (0.708f,   0.292f);
static const QPointF BT2020_GREEN(0.170f,   0.797f);
static const QPointF BT2020_BLUE (0.131f,   0.046f);

/// Build a QColorSpace with the given gamut and Linear transfer function.
static QColorSpace makeLinearColorSpace(ColorGamut g)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
  // Qt 6: all standard gamuts have dedicated Primaries enum values
  QColorSpace::Primaries p;
  switch (g)
  {
    case ColorGamut::BT2020: p = QColorSpace::Primaries::Bt2020;  break;
    case ColorGamut::BT709:  p = QColorSpace::Primaries::SRgb;    break;
    case ColorGamut::P3:     p = QColorSpace::Primaries::DciP3D65; break;
    default:                 p = QColorSpace::Primaries::SRgb;    break;
  }
  return QColorSpace(p, QColorSpace::TransferFunction::Linear);
#else
  // Qt 5: no Bt2020 enum — use chromaticity coordinates for BT.2020
  switch (g)
  {
    case ColorGamut::BT2020:
      return QColorSpace(BT2020_WHITE, BT2020_RED, BT2020_GREEN, BT2020_BLUE,
                         QColorSpace::TransferFunction::Linear);
    case ColorGamut::BT709:
      return QColorSpace(QColorSpace::Primaries::SRgb,
                         QColorSpace::TransferFunction::Linear);
    case ColorGamut::P3:
      return QColorSpace(QColorSpace::Primaries::DciP3D65,
                         QColorSpace::TransferFunction::Linear);
    default:
      return QColorSpace(QColorSpace::Primaries::SRgb,
                         QColorSpace::TransferFunction::Linear);
  }
#endif
}

/// Get gamut matrix for a display QColorSpace (handles Custom primaries).
/// Uses precomputed matrices for standard primaries, computes from color
/// transform for Custom primaries.
/// @param matrixOut  float[9] row-major output buffer, always filled
const float *getGamutMatrixForDisplay(ColorGamut          source,
                                      const QColorSpace &display,
                                      float              matrixOut[9])
{
  if (!display.isValid())
    return getGamutMatrix(source, ColorGamut::BT709);

  auto primaries = display.primaries();

  // Standard primaries — use precomputed matrix
  switch (primaries)
  {
    case QColorSpace::Primaries::SRgb:
      return getGamutMatrix(source, ColorGamut::BT709);
    case QColorSpace::Primaries::DciP3D65:
      return getGamutMatrix(source, ColorGamut::P3);
    default:
      break;
  }

  // Custom primaries — compute matrix from Qt color transform.
  // Both source and target must use Linear transfer function so the
  // transform only handles primaries/chromatic adaptation, not encoding.
  QColorSpace srcCS = makeLinearColorSpace(source);

  // Target: if display has standard primaries use the enum, otherwise
  // keep its (possibly ICC-derived) custom primaries with Linear transfer.
  QColorSpace dstCS;
  if (primaries == QColorSpace::Primaries::Custom)
  {
    dstCS = QColorSpace(display);
    dstCS.setTransferFunction(QColorSpace::TransferFunction::Linear);
  }
  else
  {
    dstCS = QColorSpace(primaries, QColorSpace::TransferFunction::Linear);
  }

  QColorTransform xform = srcCS.transformationToColorSpace(dstCS);

  // Map RGB unit vectors to build the 3×3 matrix.
  // QColor stores float components internally, so values > 1.0
  // (common for wide-gamut → narrow-gamut conversion) are preserved.
  auto mapFloat = [&](float r, float g, float b) {
    return xform.map(QColor::fromRgbF(qreal(r), qreal(g), qreal(b)));
  };

  auto r = mapFloat(1.0f, 0.0f, 0.0f);
  auto g = mapFloat(0.0f, 1.0f, 0.0f);
  auto b = mapFloat(0.0f, 0.0f, 1.0f);

  // Row-major 3×3: each basis vector is a column
  matrixOut[0] = float(r.redF());   matrixOut[1] = float(g.redF());   matrixOut[2] = float(b.redF());
  matrixOut[3] = float(r.greenF()); matrixOut[4] = float(g.greenF()); matrixOut[5] = float(b.greenF());
  matrixOut[6] = float(r.blueF());  matrixOut[7] = float(g.blueF());  matrixOut[8] = float(b.blueF());

  return matrixOut;
}

// ── Output space helpers ──────────────────────────────────────────

bool shouldApplyTonemapping(const DisplayInfo &info)
{
  // macOS Metal: never — system compositor handles EDR→SDR
  if (info.outputSpace == OutputColorSpace::EDR_P3)
    return false;
  // DXGI: only when system is NOT handling tonemapping (SDR without ACM)
  if (info.outputSpace == OutputColorSpace::SCRGB_BT709)
    return !info.systemHandlesTonemapping;
  // OpenGL: always (SDR only, sRGB OETF clips hard)
  return true;
}

bool shouldApplySRGBOETF(const DisplayInfo &info)
{
  // Only OpenGL SDR output needs sRGB OETF encoding.
  // scRGB and EDR outputs are linear — the compositor/display handles OETF.
  return info.outputSpace == OutputColorSpace::SRGB;
}

// ── Shader generation: shared EOTF constants ──────────────────────

// All three shader languages share the same mathematical constants.
// The functions below generate language-specific syntax but identical math.

QString generateGLSLColorProcessing()
{
  // GLSL 330 core — used by HDR10Widget (OpenGL)
  // Expects uniforms: eotfType (int), sourceGamut (int), gammaValue (float),
  //   diffuseWhiteNits (float), hdrBrightness (float), gamutMatrix (mat3),
  //   systemHandlesTonemapping (float), applySRGBOETF (float)
  return QStringLiteral(R"GLSL(
// ========== EOTF Functions ==========

// PQ EOTF (SMPTE ST 2084)
// Input: 0-1 (PQ encoded), Output: linear light normalized (diffuseWhite = 1.0)
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
// Output: 0-1 for SDR range, >1.0 for HDR highlights (peak = 4× diffuse white)
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
// Input: color in 0-1 range (normalized from 16-bit texture)
// Output: final color for fragment output
vec3 processColor(vec3 color)
{
    // ── EOTF Conversion ──
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

    // ── Gamut Conversion ──
    if (sourceGamut != 2) {
        linear = gamutMatrix * linear;
    }

    // ── HDR Brightness Adjustment ──
    linear *= hdrBrightness;

    // ── Reinhard Tonemapping (SDR without system tonemapping) ──
    if (systemHandlesTonemapping < 0.5) {
        float luminance = dot(linear, vec3(0.2126, 0.7152, 0.0722));
        float mappedLum = luminance / (1.0 + luminance);
        if (luminance > 0.001)
            linear *= mappedLum / luminance;
    }

    // ── sRGB OETF (SDR output only) ──
    if (applySRGBOETF > 0.5) {
        linear = srgbOetf(linear);
    }

    return linear;
}
)GLSL");
}

QString generateHLSLColorProcessing()
{
  // HLSL (Shader Model 4.0) — used by HDR10WidgetWinDXGI (D3D11)
  // Expects cbuffer Constants with: eotf, colorGamut, gammaValue,
  //   diffuseWhiteNits, hdrBrightness, sdrWhiteNits, hdrActive,
  //   systemHandlesTonemapping, debugOutput
  // Expects gamut matrices as static const float3x3
  return QStringLiteral(R"HLSL(
// ── EOTF Functions ──

static const float PQ_m1 = 0.1593017578125;
static const float PQ_m2 = 78.84375;
static const float PQ_c1 = 0.8359375;
static const float PQ_c2 = 18.8515625;
static const float PQ_c3 = 18.6875;

float pqToLinear(float pqValue)
{
    float v = pow(max(pqValue, 0.0f), 1.0f / PQ_m2);
    float num = max(v - PQ_c1, 0.0f);
    float den = max(PQ_c2 - PQ_c3 * v, 1e-10f);
    return pow(num / den, 1.0f / PQ_m1) * 10000.0f;
}
float3 pqToLinear(float3 pq)
{
    return float3(pqToLinear(pq.r), pqToLinear(pq.g), pqToLinear(pq.b));
}

float hlgToLinear(float hlgValue)
{
    const float a = 0.17883277;
    const float b = 0.28466892;
    const float c = 0.55991073;
    if (hlgValue <= 0.5)
        return hlgValue * hlgValue / 3.0;
    return (exp((hlgValue - c) / a) + b) / 12.0;
}
float3 hlgToLinear(float3 hlg)
{
    return float3(hlgToLinear(hlg.r), hlgToLinear(hlg.g), hlgToLinear(hlg.b));
}

float sRGBToLinear(float c)
{
    if (c <= 0.04045f)
        return c / 12.92f;
    return pow((c + 0.055f) / 1.055f, 2.4f);
}
float3 sRGBToLinear(float3 srgb)
{
    return float3(sRGBToLinear(srgb.r), sRGBToLinear(srgb.g), sRGBToLinear(srgb.b));
}

float3 applyEOTF(float3 coded)
{
    if (eotf == 0) return pqToLinear(coded);
    if (eotf == 1) return hlgToLinear(coded) * 4.0;
    if (eotf == 2) return pow(coded, gammaValue);
    return sRGBToLinear(coded);
}

// ── Gamut conversion matrices (source → BT.709, scRGB primaries) ──

static const float3x3 BT2020toBT709 = {
     1.6604900368e+00f, -5.8763847956e-01f, -7.2851557227e-02f,
    -1.2455039399e-01f,  1.1328984194e+00f, -8.3480253896e-03f,
    -1.8151044561e-02f, -1.0057861035e-01f,  1.1187296549e+00f
};
static const float3x3 P3toBT709 = {
     1.2249389281e+00f, -2.2493772528e-01f, -2.2415925083e-06f,
    -4.2055920309e-02f,  1.0420564505e+00f,  1.3042267034e-06f,
    -1.9637885940e-02f, -7.8636645004e-02f,  1.0982732700e+00f
};
float3 applyGamutConversion(float3 linColor)
{
    if (colorGamut == 0) return mul(BT2020toBT709, linColor);
    if (colorGamut == 2) return mul(P3toBT709, linColor);
    return linColor;
}

// ── Color processing pipeline ──
// Input: texColor.rgb in 0-1 range
// Output: scRGB linear (1.0 = 80 nits, can be >1.0 for HDR)
float3 processColor(float3 texColor)
{
    float3 linColor = applyEOTF(texColor);
    linColor = applyGamutConversion(linColor);

    // ── Map to scRGB (1.0 = 80 nits) ──
    float3 output;
    if (eotf == 0) {
        // PQ: absolute nits → scRGB
        output = linColor / 80.0;
    } else if (eotf == 1) {
        // HLG: relative, peak = 4× SDR white → scRGB
        output = linColor * (sdrWhiteNits * 4.0 / 80.0);
    } else {
        // Gamma / sRGB: relative, 1.0 = SDR white → scRGB
        output = linColor * (sdrWhiteNits / 80.0);
    }

    output *= hdrBrightness;

    // ── Reinhard tonemapping only when system is NOT handling it ──
    if (systemHandlesTonemapping < 0.5f)
    {
        float luminance = dot(output, float3(0.2126f, 0.7152f, 0.0722f));
        float mappedLum = luminance / (1.0f + luminance);
        if (luminance > 0.001f)
            output *= mappedLum / luminance;
    }

    return output;
}
)HLSL");
}

QString generateMSLColorProcessing()
{
  // Metal Shading Language — used by MacEDRRenderer
  // Metal never does tonemapping (macOS compositor handles EDR→SDR).
  // Expects: constant VideoUniforms &uniforms, constant float &hdrBrightness
  return QStringLiteral(R"MSL(
// ========== EOTF Functions ==========

float3 pqEotf(float3 pq, float diffuseWhiteNits) {
    const float m1 = 2610.0 / 4096.0 * (1.0 / 4.0);
    const float m2 = 2523.0 / 4096.0 * 128.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 4096.0 * 32.0;
    const float c3 = 2392.0 / 4096.0 * 32.0;
    float3 p = pow(pq, 1.0 / m2);
    float3 num = max(p - c1, 0.0);
    float3 den = c2 - c3 * p;
    float3 linear = pow(num / den, 1.0 / m1);
    return linear * 10000.0 / diffuseWhiteNits;
}

float3 hlgEotf(float3 hlg) {
    const float a = 0.17883277;
    const float b = 0.28466892;
    const float c = 0.55991073;
    float3 linear;
    for (int i = 0; i < 3; i++) {
        if (hlg[i] <= 0.5) {
            linear[i] = hlg[i] * hlg[i] / 3.0;
        } else {
            linear[i] = (exp((hlg[i] - c) / a) + b) / 12.0;
        }
    }
    return linear * 4.0;
}

float3 gammaEotf(float3 gamma, float gammaValue) {
    return pow(gamma, gammaValue);
}

float3 srgbEotf(float3 srgb) {
    float3 linear;
    for (int i = 0; i < 3; i++) {
        if (srgb[i] <= 0.04045) {
            linear[i] = srgb[i] / 12.92;
        } else {
            linear[i] = pow((srgb[i] + 0.055) / 1.055, 2.4);
        }
    }
    return linear;
}

// ========== Color processing pipeline ==========
// Input: color in 0-1 range
// Output: linear light for EDR output (values >1.0 trigger EDR)
// No tonemapping — macOS compositor handles it.
float3 processColor(float3 color, int eotfType, int sourceGamut,
                    float gammaValue, float diffuseWhiteNits,
                    float3x3 gamutMatrix, float hdrBrightness)
{
    // ── EOTF Conversion ──
    float3 linear;
    if (eotfType == 0) {
        linear = pqEotf(color, diffuseWhiteNits);
    } else if (eotfType == 1) {
        linear = hlgEotf(color);
    } else if (eotfType == 2) {
        linear = gammaEotf(color, gammaValue);
    } else {
        linear = srgbEotf(color);
    }

    // ── Gamut Conversion (source → Display P3) ──
    if (sourceGamut != 2) {
        linear = gamutMatrix * linear;
    }

    // ── HDR Brightness Adjustment ──
    linear *= hdrBrightness;

    // No tonemapping — macOS system compositor handles EDR→display

    return linear;
}
)MSL");
}

} // namespace video::color
