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
 *   this exception statement from all source files in the program, then
 *   also delete it here.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You must have received a copy of the GNU General Public License
 *   along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <QtGlobal>

#ifdef Q_OS_MAC

#include <video/VideoFrame.h>
#include <common/ColorPipeline.h>

#include <QWidget>
#include <QWindow>
#include <memory>

namespace video
{

// Forward declaration
class FrameHandler;
class MacEDRRenderer;

// Aliases — use unified color:: enums
using MacEDR_EOTF       = color::EOTF;
using MacEDR_ColorGamut = color::ColorGamut;

/**
 * @brief macOS EDR HDR 显示 Widget
 *
 * 在 macOS 上使用 Metal 渲染层实现 EDR (Extended Dynamic Range) 显示。
 * 值超过 1.0 的高亮区域将自动映射到显示器 HDR 能力。
 *
 * 实现方式（参考 QtHDRDemo）：
 * 1. 创建 QWindow 作为 Metal Surface 容器
 * 2. 配置 CAMetalLayer 启用 EDR (wantsExtendedDynamicRangeContent = YES)
 * 3. 设置色域为 Extended Linear Display P3
 * 4. 使用 RGBA16Float 渲染目标支持扩展范围输出
 * 5. 着色器执行 EOTF 转换 + 色域映射 + HDR 亮度缩放
 *
 * 色彩处理管线：
 *   16-bit RGBA 数据 → EOTF(PQ/HLG/sRGB/Gamma) → diffuse white 归一化
 *   → 色域转换(BT.2020/BT.709/P3 → Display P3) → HDR 亮度调整 → EDR 输出
 */
class HDR10WidgetMacEDR : public QWidget
{
  Q_OBJECT

public:
  explicit HDR10WidgetMacEDR(QWidget *parent = nullptr);
  ~HDR10WidgetMacEDR() override;

  // 数据设置
  void setFrame(const VideoFrame &frame);
  void setFrameHandler(FrameHandler *handler);
  void setBitDepth(int bits);

  // 渲染控制
  void setDithering(bool enable);
  void setZoom(double zoom);
  void setMoveOffset(QPointF offset);
  void setShowRawData(bool show);

  // EOTF 和色域设置 — uses unified color:: enums
  void setEOTF(color::EOTF eotf);
  void setColorGamut(color::ColorGamut gamut);
  void setGammaValue(float gamma);
  void setDiffuseWhiteNits(float nits);
  void setHDRBrightness(float brightness);

  // 状态查询
  bool isEDRSupported() const;
  float getMaxEDRValue() const;
  QString getRendererInfo() const;

  // 像素值覆盖层（与 HDR10Widget 相同的接口）
  void updatePixelOverlay();
  void drawPixelValues(QPainter *painter);
  void drawZoomIndicator(QPainter *painter);
  void drawPixelRulers(QPainter *painter);

protected:
  void resizeEvent(QResizeEvent *event) override;
  void paintEvent(QPaintEvent *event) override;
  void showEvent(QShowEvent *event) override;
  bool eventFilter(QObject *watched, QEvent *event) override;  // Forward events from Metal QWindow to parent

private:
  bool initializeRenderer();

  // Metal renderer (Objective-C++ implementation)
  std::unique_ptr<MacEDRRenderer> m_renderer;
  QWindow                        *m_containerWindow = nullptr;
  QWidget                        *m_containerWidget = nullptr;

  // Frame data
  VideoFrame  m_currentFrame;
  QSize       m_frameSize;
  bool        m_frameNeedsUpdate{false};

  // Bit depth
  int  m_bitDepth{10};
  int  m_sourceBitDepth{8};

  // EDR state
  bool m_initialized{false};
  bool m_edrSupported{false};
  float m_maxEDRValue{1.0f};

  // Color processing parameters — unified color:: enums
  color::EOTF       m_eotf{color::EOTF::SRGB};
  color::ColorGamut m_colorGamut{color::ColorGamut::BT709};
  float      m_gammaValue{2.2f};
  float      m_diffuseWhiteNits{203.0f};
  float      m_hdrBrightness{1.0f};

  // View control
  double  m_zoom{1.0};
  QPointF m_moveOffset{0, 0};
  bool    m_showRawData{false};
  bool    m_ditheringEnabled{false};

  FrameHandler *m_frameHandler{nullptr};

  // Renderer info string
  QString m_rendererInfo;
};

} // namespace video

#endif // Q_OS_MAC