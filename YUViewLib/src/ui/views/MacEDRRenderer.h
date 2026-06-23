/*  This file is part of YUView - The YUV player with advanced analytics toolset
 *   <https://github.com/IENT/YUView>
 *   Copyright (C) 2015  Institut für Nachrichtentechnik, RWTH Aachen University, GERMANY
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 3 of the License, or
 *   (at your option) any later version.
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

#include <QWindow>
#include <QSize>
#include <QImage>
#include <cstdint>
#include <memory>

#include <video/VideoFrame.h>
#include <common/ColorPipeline.h>

namespace video
{

// Use unified color:: enums
using MacEDR_EOTF       = color::EOTF;
using MacEDR_ColorGamut = color::ColorGamut;

/**
 * @brief Metal 渲染器，支持 macOS EDR (Extended Dynamic Range)
 *
 * 关键功能：
 * 1. 配置 CAMetalLayer 启用 EDR
 * 2. 使用扩展色域 (Extended Linear Display P3)
 * 3. 使用 RGBA16Float 渲染目标支持扩展范围输出 (>1.0)
 * 4. 着色器执行 EOTF 转换 + 色域映射 + HDR 亮度缩放
 *
 * 数据流：
 *   VideoFrame 16-bit RGBA → 纹理上传(RGBA16UI)
 *   → EOTF(PQ/HLG/sRGB/Gamma) → diffuse white 归一化
 *   → 色域转换(BT.2020/BT.709/P3 → Display P3)
 *   → HDR 亮度调整 → EDR 输出 (RGBA16Float, values can exceed 1.0)
 */
class MacEDRRenderer
{
public:
  MacEDRRenderer();
  ~MacEDRRenderer();

  /**
   * @brief 初始化渲染器，配置 CAMetalLayer 和 EDR
   *
   * 步骤：
   * 1. 获取 QWindow 的 NSView
   * 2. 创建 Metal 设备和命令队列
   * 3. 创建 CAMetalLayer，配置 EDR：
   *    - wantsExtendedDynamicRangeContent = YES
   *    - colorspace = kCGColorSpaceExtendedLinearDisplayP3
   *    - pixelFormat = MTLPixelFormatRGBA16Float
   * 4. 检查 NSScreen EDR 支持
   * 5. 创建渲染管线
   */
  bool initialize(QWindow *window);

  void resize(int width, int height);
  void render();

  // EDR 状态
  bool isEDRSupported() const { return m_edrSupported; }
  float getMaxEDRValue() const { return m_maxEDRValue; }

  // HDR 亮度控制 (>1.0 触发 EDR)
  void setHDRBrightness(float brightness);
  void setDiffuseWhite(float nits);

  // 色彩处理
  void setEOTF(MacEDR_EOTF eotf);
  void setColorGamut(MacEDR_ColorGamut gamut);
  void setGammaValue(float gamma);

  // 帧数据
  bool loadFrame(const VideoFrame &frame);
  bool hasFrame() const { return m_hasFrame; }

  // View control (zoom and pan, same as HDR10Widget OpenGL)
  void setZoom(double zoom);
  void setMoveOffset(QPointF offset);

  // Overlay layer (pixel values, zoom indicator above Metal)
  void setOverlayImage(const QImage &image);

  // View state (for vertex calculation in render())
  double  m_zoom{1.0};
  QPointF m_moveOffset{0, 0};

private:
  bool createRenderPipeline();
  bool createVideoVertexBuffer();
  bool createTextures(int width, int height);
  bool updateTextureData(const uint16_t *data, int width, int height);

  // Native handles (Objective-C objects stored as void* for C++ compatibility)
  void *m_device{nullptr};
  void *m_commandQueue{nullptr};
  void *m_metalLayer{nullptr};
  void *m_overlayLayer{nullptr};  // CALayer for pixel value overlay above Metal

  // Pipeline states
  void *m_pipelineStateVideo{nullptr};

  // Buffers
  void *m_videoVertexBuffer{nullptr};

  // Textures
  void *m_rgbaTexture{nullptr};    // 16-bit RGBA texture for video frame

  // Size
  int m_width{0};
  int m_height{0};
  int m_frameWidth{0};
  int m_frameHeight{0};
  QSize m_textureSize;

  // State
  bool m_initialized{false};
  bool m_edrSupported{false};
  bool m_hasFrame{false};
  float m_maxEDRValue{1.0f};
  float m_hdrBrightness{1.0f};
  float m_diffuseWhiteNits{203.0f};
  float m_gammaValue{2.2f};

  // Color processing
  MacEDR_EOTF       m_eotf{MacEDR_EOTF::SRGB};
  MacEDR_ColorGamut m_colorGamut{MacEDR_ColorGamut::BT709};
};

} // namespace video

#endif // Q_OS_MAC