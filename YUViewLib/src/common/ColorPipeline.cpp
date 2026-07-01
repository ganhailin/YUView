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
#include <algorithm>
#include <cstring>
#include <QColorSpace>
#include <QColorTransform>
#include <QDebug>
#include <QtGui/QtGui>

#include <common/FunctionsGui.h>

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

/// Check if two 3×3 float matrices are approximately equal
static bool matricesEqual(const float *a, const float *b, float eps = 0.002f)
{
  for (int i = 0; i < 9; ++i)
    if (std::abs(a[i] - b[i]) > eps)
      return false;
  return true;
}

// s15Fixed16Number: 32-bit = sign + 15.16 fixed point
static float s15f16(uint8_t const *p)
{
  int32_t v = (int32_t(p[0]) << 24) | (int32_t(p[1]) << 16)
            | (int32_t(p[2]) << 8)  | int32_t(p[3]);
  return float(v) / 65536.0f;
}

/// Parse ICC profile to extract colorant (rXYZ/gXYZ/bXYZ) and white point
/// (wtpt) tags. Returns true if all four tags were found.
static bool parseIccPrimaries(const QByteArray &iccData,
                              float rXYZ[3], float gXYZ[3],
                              float bXYZ[3], float wXYZ[3])
{
  if (iccData.size() < 132)
    return false;

  auto read32 = [&](int off) -> uint32_t {
    auto p = reinterpret_cast<const uint8_t *>(iccData.constData()) + off;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16)
         | (uint32_t(p[2]) << 8)  | uint32_t(p[3]);
  };

  // Tag count at offset 128
  uint32_t tagCount = read32(128);
  qDebug() << "[ICC] file size:" << iccData.size() << "tag count:" << tagCount;
  if (iccData.size() < 132 + int(tagCount) * 12)
    return false;

  bool foundR = false, foundG = false, foundB = false, foundW = false;
  const uint32_t tagR = 0x7258595A; // 'rXYZ'
  const uint32_t tagG = 0x6758595A; // 'gXYZ'
  const uint32_t tagB = 0x6258595A; // 'bXYZ'
  const uint32_t tagW = 0x77747074; // 'wtpt'

  for (uint32_t i = 0; i < tagCount; ++i)
  {
    int off = 132 + int(i) * 12;
    uint32_t sig  = read32(off);
    uint32_t dataOff = read32(off + 4);
    uint32_t dataSize = read32(off + 8);

    if (sig == tagR && dataOff + 20 <= uint32_t(iccData.size()))
    {
      auto p = reinterpret_cast<const uint8_t *>(iccData.constData()) + dataOff + 8; // skip type+sig
      qDebug() << "[ICC tag rXYZ] off:" << dataOff << "raw data bytes:"
               << Qt::hex << p[0] << p[1] << p[2] << p[3]
                        << p[4] << p[5] << p[6] << p[7]
                        << p[8] << p[9] << p[10] << p[11];
      rXYZ[0] = s15f16(p); rXYZ[1] = s15f16(p + 4); rXYZ[2] = s15f16(p + 8);
      qDebug() << "  -> rXYZ:" << rXYZ[0] << rXYZ[1] << rXYZ[2];
      foundR = true;
    }
    else if (sig == tagG && dataOff + 20 <= uint32_t(iccData.size()))
    {
      auto p = reinterpret_cast<const uint8_t *>(iccData.constData()) + dataOff + 8;
      gXYZ[0] = s15f16(p); gXYZ[1] = s15f16(p + 4); gXYZ[2] = s15f16(p + 8);
      foundG = true;
    }
    else if (sig == tagB && dataOff + 20 <= uint32_t(iccData.size()))
    {
      auto p = reinterpret_cast<const uint8_t *>(iccData.constData()) + dataOff + 8;
      bXYZ[0] = s15f16(p); bXYZ[1] = s15f16(p + 4); bXYZ[2] = s15f16(p + 8);
      foundB = true;
    }
    else if (sig == tagW && dataOff + 20 <= uint32_t(iccData.size()))
    {
      auto p = reinterpret_cast<const uint8_t *>(iccData.constData()) + dataOff + 8;
      wXYZ[0] = s15f16(p); wXYZ[1] = s15f16(p + 4); wXYZ[2] = s15f16(p + 8);
      foundW = true;
    }
  }

  return foundR && foundG && foundB && foundW;
}

/// Build 3×3 gamut conversion matrix from ICC primaries.
/// Computes: source RGB → XYZ → display RGB.
/// All primaries are D50-referenced XYZ (from ICC tags).
/// @param matrixOut row-major 3×3 output [source RGB → display RGB]
static void buildMatrixFromIcc(ColorGamut source,
                               const float rXYZ[3], const float gXYZ[3],
                               const float bXYZ[3],
                               float matrixOut[9])
{
  // ── Source primaries in D50 XYZ ────────────────────────────────
  static const float sRGB_r[3] = {0.4360f, 0.2225f, 0.0139f};
  static const float sRGB_g[3] = {0.3851f, 0.7169f, 0.0971f};
  static const float sRGB_b[3] = {0.1431f, 0.0606f, 0.7141f};

  static const float P3_r[3] = {0.5151f, 0.2412f, -0.0011f};
  static const float P3_g[3] = {0.2920f, 0.6923f, 0.0419f};
  static const float P3_b[3] = {0.1571f, 0.0666f, 0.7841f};

  static const float B2020_r[3] = {0.6735f, 0.2790f, 0.0020f};
  static const float B2020_g[3] = {0.1658f, 0.6754f, 0.0300f};
  static const float B2020_b[3] = {0.1250f, 0.0456f, 0.7969f};

  const float *sr, *sg, *sb;
  switch (source)
  {
    case ColorGamut::P3:     sr=P3_r; sg=P3_g; sb=P3_b; break;
    case ColorGamut::BT2020: sr=B2020_r; sg=B2020_g; sb=B2020_b; break;
    default:                 sr=sRGB_r; sg=sRGB_g; sb=sRGB_b; break;
  }

  // ── Helper: invert 3×3 matrix ─────────────────────────────────
  auto inv3 = [](const float A[9], float inv[9]) -> bool {
    float det = A[0]*(A[4]*A[8]-A[5]*A[7])
              - A[1]*(A[3]*A[8]-A[5]*A[6])
              + A[2]*(A[3]*A[7]-A[4]*A[6]);
    if (std::abs(det) < 1e-10f) return false;
    float id = 1.0f / det;
    inv[0] =  (A[4]*A[8]-A[5]*A[7]) * id;
    inv[1] = -(A[1]*A[8]-A[2]*A[7]) * id;
    inv[2] =  (A[1]*A[5]-A[2]*A[4]) * id;
    inv[3] = -(A[3]*A[8]-A[5]*A[6]) * id;
    inv[4] =  (A[0]*A[8]-A[2]*A[6]) * id;
    inv[5] = -(A[0]*A[5]-A[2]*A[3]) * id;
    inv[6] =  (A[3]*A[7]-A[4]*A[6]) * id;
    inv[7] = -(A[0]*A[7]-A[1]*A[6]) * id;
    inv[8] =  (A[0]*A[4]-A[1]*A[3]) * id;
    return true;
  };

  // ── Helper: multiply 3×3 matrices C = A × B ───────────────────
  auto mul3 = [](const float A[9], const float B[9], float C[9]) {
    for (int row = 0; row < 3; ++row)
      for (int col = 0; col < 3; ++col)
        C[row*3+col] = A[row*3+0]*B[0*3+col]
                     + A[row*3+1]*B[1*3+col]
                     + A[row*3+2]*B[2*3+col];
  };

  // ── Step 1: source RGB → XYZ (D50), columns = source primaries ─
  float src2XYZ[9] = {
    sr[0], sg[0], sb[0],
    sr[1], sg[1], sb[1],
    sr[2], sg[2], sb[2]
  };

  // ── Step 2: display RGB → XYZ (D50), columns = ICC primaries ───
  float dst2XYZ[9] = {
    rXYZ[0], gXYZ[0], bXYZ[0],
    rXYZ[1], gXYZ[1], bXYZ[1],
    rXYZ[2], gXYZ[2], bXYZ[2]
  };

  // ── Step 3: invert display RGB→XYZ to get XYZ→display RGB ─────
  float XYZ2dst[9];
  if (!inv3(dst2XYZ, XYZ2dst))
  {
    auto m = getGamutMatrix(source, ColorGamut::BT709);
    std::copy(m, m + 9, matrixOut);
    return;
  }

  // ── Step 4: source RGB → XYZ → display RGB ─────────────────────
  mul3(XYZ2dst, src2XYZ, matrixOut);
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
  auto desc = display.description();

  // ── Tier 1: primaries enum ──────────────────────────────────
  switch (primaries)
  {
    case QColorSpace::Primaries::SRgb:
      return getGamutMatrix(source, ColorGamut::BT709);
    case QColorSpace::Primaries::DciP3D65:
      return getGamutMatrix(source, ColorGamut::P3);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    case QColorSpace::Primaries::Bt2020:
      return getGamutMatrix(source, ColorGamut::BT2020);
#endif
    default:
      break;
  }

  // ── Tier 2: compute matrix from QColorTransform ──────────────
  QColorSpace srcCS = makeLinearColorSpace(source);

  // Target: use display primaries with Linear transfer function
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

  // Row-major 3×3: basis-vector images form the columns of M.
  // r = M·e0 → column 0, g = M·e1 → column 1, b = M·e2 → column 2.
  // Fill column-wise so matrixOut matches the row-major M used by the
  // precomputed matrices (e.g. BT709_TO_P3) and the downstream
  // row-major→column-major layout conversion in HDR10Widget.
  matrixOut[0] = float(r.redF());   matrixOut[3] = float(r.greenF()); matrixOut[6] = float(r.blueF());  // column 0
  matrixOut[1] = float(g.redF());   matrixOut[4] = float(g.greenF()); matrixOut[7] = float(g.blueF());  // column 1
  matrixOut[2] = float(b.redF());   matrixOut[5] = float(b.greenF()); matrixOut[8] = float(b.blueF());  // column 2

  // ── Tier 4: parse ICC binary to compute gamut matrix ──────────
  const auto &iccData = functionsGui::getCachedIccData();
  const char *match = "(computed from QColorTransform)";
  if (!iccData.isEmpty())
  {
    float ir[3], ig[3], ib[3], iw[3];
    if (parseIccPrimaries(iccData, ir, ig, ib, iw))
    {
      buildMatrixFromIcc(source, ir, ig, ib, matrixOut);
      match = "ICC (rXYZ/gXYZ/bXYZ computed)";
    }
  }

  static bool logged = false;
  if (!logged)
  {
    logged = true;
    qDebug() << "[getGamutMatrixForDisplay]"
             << "\n  display:" << desc
             << "\n  primaries:" << static_cast<int>(primaries)
             << "transfer:" << static_cast<int>(display.transferFunction())
             << "gamma:" << display.gamma()
             << "\n  source:" << static_cast<int>(source)
             << "matrix match:" << match
             << "\n  basis vectors (source→display):"
             << "\n    r:" << r.redF() << r.greenF() << r.blueF()
             << "\n    g:" << g.redF() << g.greenF() << g.blueF()
             << "\n    b:" << b.redF() << b.greenF() << b.blueF()
             << "\n  matrix (row-major):"
             << "\n    [" << matrixOut[0] << matrixOut[1] << matrixOut[2] << "]"
             << "\n    [" << matrixOut[3] << matrixOut[4] << matrixOut[5] << "]"
             << "\n    [" << matrixOut[6] << matrixOut[7] << matrixOut[8] << "]";
  }

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
  // Expects uniforms: eotfType (int), gammaValue (float),
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
    // The matrix is always valid (identity for same→same), so apply
    // unconditionally — no need for a sourceGamut shortcut.
    linear = gamutMatrix * linear;

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
float3 processColor(float3 color, int eotfType,
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
    // The matrix is always valid (identity for same→same), so apply
    // unconditionally — no need for a sourceGamut shortcut.
    linear = gamutMatrix * linear;

    // ── HDR Brightness Adjustment ──
    linear *= hdrBrightness;

    // No tonemapping — macOS system compositor handles EDR→display

    return linear;
}
)MSL");
}

} // namespace video::color
