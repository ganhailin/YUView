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
 *   of the code used other than OpenSSL. If you modify file(s) with this
 *   exception, you may extend this exception to your version of the
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

#include <video/VideoFrame.h>

#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QWidget>

namespace video
{

// Forward declaration
class FrameHandler;

/**
 * @brief EOTF (Electro-Optical Transfer Function) 类型
 *
 * 用于将编码值转换为线性光输出。
 * PQ 和 HLG 是 HDR 标准，Gamma 和 sRGB 是 SDR 标准。
 * 在 macOS EDR 模式下，PQ/HLG 输出值可以超过 1.0，
 * 系统自动映射到显示器 HDR 能力。
 */
enum class HDR10_EOTF
{
  PQ     = 0,  // SMPTE ST 2084 (Perceptual Quantizer) - HDR10/DolbyVision
  HLG    = 1,  // ARIB STD-B67 (Hybrid Log-Gamma) - broadcast HDR
  Gamma  = 2,  // Pure power-law gamma curve
  SRGB   = 3   // IEC 61966-2-1 sRGB (piecewise)
};

/**
 * @brief 色域类型
 *
 * HDR 内容通常使用 BT.2020 色域，需要转换到显示器的 Display P3 色域。
 */
enum class HDR10_ColorGamut
{
  BT2020 = 0,  // ITU-R BT.2020 - ultra-wide gamut for HDR
  BT709  = 1,  // ITU-R BT.709 - standard HDTV gamut
  P3     = 2   // DCI-P3 / Display P3 - wide gamut (Apple displays)
};

class HDR10Widget : public QOpenGLWidget, protected QOpenGLFunctions
{
  Q_OBJECT

public:
  explicit HDR10Widget(QWidget *parent = nullptr);
  ~HDR10Widget() override;

  void setFrame(const VideoFrame &frame);
  void setFrameHandler(FrameHandler *handler) { m_frameHandler = handler; }
  void setBitDepth(int bits) { m_bitDepth = bits; }
  void setDithering(bool enable);
  void setZoom(double zoom);
  void setMoveOffset(QPointF offset);
  void setShowRawData(bool show) {
    if (m_showRawData == show) return;
    m_showRawData = show;
    update();
    updatePixelOverlay();
  }

  // EOTF and color gamut control (used in macOS EDR shader path)
  void setEOTF(HDR10_EOTF eotf) { m_eotf = eotf; update(); }
  void setColorGamut(HDR10_ColorGamut gamut) { m_colorGamut = gamut; update(); }
  void setGammaValue(float gamma) { m_gammaValue = gamma; update(); }
  void setDiffuseWhiteNits(float nits) { m_diffuseWhiteNits = nits; update(); }
  void setHDRBrightness(float brightness) { m_hdrBrightness = brightness; update(); }

  // State query
  bool supports10bit() const { return m_supports10bit; }
  QString getOpenGLInfo() const { return m_openglInfo; }

protected:
  void initializeGL() override;
  void resizeGL(int w, int h) override;
  void paintGL() override;
  void updatePixelOverlay();
  void drawPixelValues(QPainter *painter);
  void drawZoomIndicator(QPainter *painter);
  void drawPixelRulers(QPainter *painter);

private:
  void initShaders();
  void initGeometry();
  void updateTexture();

  // Pixel overlay widget for drawing pixel values
  class PixelOverlay : public QWidget
  {
  public:
    explicit PixelOverlay(HDR10Widget *parent);
    void paintEvent(QPaintEvent *event) override;

  private:
    HDR10Widget *hdrWidget;
  };

  QOpenGLShaderProgram *m_program{nullptr};
  QOpenGLShaderProgram *m_programDither{nullptr};
  QOpenGLBuffer         m_vbo{QOpenGLBuffer::VertexBuffer};
  QOpenGLVertexArrayObject m_vao;

  GLuint m_textureId{0};
  QSize    m_textureSize;       // Cache texture size for reuse

  VideoFrame m_currentFrame;
  bool       m_frameNeedsUpdate{false};
  QSize      m_frameSize;

  int  m_bitDepth{10};           // Display/render bit depth (for OpenGL shader normalization)
  int  m_sourceBitDepth{8};      // Source bit depth (for pixel value display)
  bool m_ditheringEnabled{false};
  bool m_initialized{false};
  bool m_supports10bit{false};
  bool m_showRawData{false};

  QString m_openglInfo;

  // EDR and color processing parameters (used by EDR shader on macOS)
  HDR10_EOTF       m_eotf{HDR10_EOTF::SRGB};
  HDR10_ColorGamut m_colorGamut{HDR10_ColorGamut::BT709};
  float            m_gammaValue{2.2f};
  float            m_diffuseWhiteNits{203.0f};
  float            m_hdrBrightness{1.0f};


  double m_zoom{1.0};
  QPointF m_moveOffset{0, 0};

  PixelOverlay *m_pixelOverlay{nullptr};
  FrameHandler *m_frameHandler{nullptr};  // For getting original pixel values
};

} // namespace video
