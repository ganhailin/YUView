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

#pragma once

#include <QColorSpace>
#include <QString>

namespace video::color {

/// Electro-Optical Transfer Function (code value → linear light)
enum class EOTF
{
  PQ    = 0,  // SMPTE ST 2084 (Perceptual Quantizer) — HDR10/DolbyVision
  HLG   = 1,  // ARIB STD-B67 (Hybrid Log-Gamma) — broadcast HDR
  Gamma = 2,  // Pure power-law gamma curve
  SRGB  = 3   // IEC 61966-2-1 sRGB (piecewise)
};

/// Source color gamut (primaries)
enum class ColorGamut
{
  BT2020 = 0,  // ITU-R BT.2020 — ultra-wide gamut for HDR
  BT709  = 1,  // ITU-R BT.709 — standard HDTV gamut
  P3     = 2   // DCI-P3 / Display P3 — wide gamut (Apple displays)
};

/// Output color space — determined by the rendering backend's swap chain
enum class OutputColorSpace
{
  SRGB,         // OpenGL QOpenGLWidget: 8-bit sRGB, SDR only
  SCRGB_BT709,  // Windows DXGI: FP16 scRGB (linear, BT.709 primaries)
  EDR_P3        // macOS Metal: RGBA16Float, Extended Linear Display P3
};

/// Display capabilities — queried at runtime by each backend
struct DisplayInfo
{
  OutputColorSpace outputSpace{OutputColorSpace::SRGB};

  // Windows DXGI fields
  bool  hdrActive{false};                   // HDR mode active
  bool  systemHandlesTonemapping{false};    // HDR or ACM active — system does tonemapping
  float sdrWhiteNits{80.0f};                // scRGB 1.0 = this many nits

  // macOS Metal fields
  float maxEDRValue{1.0f};                  // max EDR multiplier (e.g. 16.0 = 16× SDR)
};

/// User-configurable color processing parameters
struct ColorConfig
{
  EOTF       eotf{EOTF::SRGB};
  ColorGamut sourceGamut{ColorGamut::BT709};
  float      gammaValue{2.2f};
  float      diffuseWhiteNits{203.0f};  // diffuse white reference (nits)
  float      hdrBrightness{1.0f};       // global brightness multiplier
  bool       ditheringEnabled{false};   // Bayer 4×4 dithering (OpenGL only)
};

/// Source color configuration that travels with an image/frame rather
/// than the renderer.  These values describe how the pixel code values
/// were encoded (transfer function + primaries + gamma exponent) and are
/// independent of the display or rendering backend.
struct SourceColorConfig
{
  EOTF       eotf{EOTF::SRGB};
  ColorGamut sourceGamut{ColorGamut::BT709};
  float      gammaValue{2.2f};

  bool operator==(const SourceColorConfig &o) const
  {
    return eotf == o.eotf && sourceGamut == o.sourceGamut && gammaValue == o.gammaValue;
  }
  bool operator!=(const SourceColorConfig &o) const { return !(*this == o); }

  /// Serialize to a compact string (e.g. "3;1;2.2") for playlist persistence.
  QString toString() const;
  /// Deserialize from the string produced by toString(). Returns true on success.
  bool fromString(const QString &str);
};

// ── Gamut matrix lookup ───────────────────────────────────────────

/// Get a 3×3 gamut conversion matrix (row-major, 9 floats).
/// Converts from source gamut to target gamut via XYZ D65 intermediate.
const float *getGamutMatrix(ColorGamut source, ColorGamut target);

/// Get a 3×3 gamut conversion matrix for a display QColorSpace.
/// Handles Custom primaries by computing the matrix from the ICC profile
/// using Qt's color transform. Standard primaries use precomputed matrices.
/// @param matrixOut  float[9] row-major output buffer (always filled)
/// @return pointer to matrixOut (for convenience, same as matrixOut)
const float *getGamutMatrixForDisplay(ColorGamut          source,
                                      const QColorSpace   &display,
                                      float               matrixOut[9]);

// ── Output space helpers ──────────────────────────────────────────

/// Returns true if the backend should apply Reinhard tonemapping.
/// Decision: SDR without system tonemapping (OpenGL, or DXGI without HDR/ACM).
bool shouldApplyTonemapping(const DisplayInfo &info);

/// Returns true if the backend should apply sRGB OETF after color processing.
/// Only OpenGL (SRGB output) needs this — scRGB and EDR outputs are linear.
bool shouldApplySRGBOETF(const DisplayInfo &info);
// ── CPU color processing (for QPainter/Software path) ────────────

/// Process a single pixel through the full color pipeline on the CPU.
/// Mirrors the GLSL/HLSL/MSL `processColor()` function:
///   1. EOTF (PQ/HLG/Gamma/sRGB) -> linear light
///   2. Gamut conversion (sourceGamut -> BT.709)
///   3. Reinhard tonemapping (always for SDR Software path)
///   4. sRGB OETF (linear -> sRGB encoded)
///
/// @param rgb     Input pixel, each channel in [0, 1]
/// @param config  Source color configuration (EOTF, gamut, gamma)
/// @return        Processed pixel, each channel in [0, 1], sRGB-encoded
Q_DECL_UNUSED QPointF processColorCPU(qreal r, qreal g, qreal b,
                                      const SourceColorConfig &config);

/// Convert an entire QImage in-place through the Software color pipeline.
/// Applies EOTF -> gamut conversion -> Reinhard tonemapping -> sRGB OETF.
/// @param image   The image to transform (8-bit ARGB). Modified in-place.
/// @param config  Source color configuration
void applyColorTransformToImage(QImage &image, const SourceColorConfig &config);
} // namespace video::color
