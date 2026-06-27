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

#include <QtGlobal>

#ifdef Q_OS_WIN

#include "DXGISwapChain.h"

#include <d3d11.h>
#include <wrl/client.h>

// Windows SDK defines macros that conflict with Qt/enum names
#ifdef IN
#undef IN
#endif
#ifdef OUT
#undef OUT
#endif

#include <QWidget>
#include <QTimer>
#include <QPainter>
#include <QHideEvent>
#include <memory>
#include <vector>

#include <video/VideoFrame.h>
#include <ui/views/HDR10Widget.h>  // For HDR10_EOTF, HDR10_ColorGamut enums

using Microsoft::WRL::ComPtr;

// Forward declarations
class FrameHandler;

namespace video
{

/**
 * @brief Windows DXGI HDR 渲染 Widget
 *
 * 使用 DXGI swap chain + D3D11 实现 Windows 原生 HDR 显示。
 * 参考 HDR10WidgetMacEDR 的设计模式，作为 Windows 平台的 HDR 渲染后端。
 *
 * 架构：
 * - 继承 QWidget，通过 WA_NativeWindow 获取原生 HWND
 * - 内部封装 DXGISwapChain 管理 FP16 scRGB swap chain
 * - D3D11 渲染管线：全屏四边形 + 自定义 pixel shader
 * - 支持 PQ/HLG/Gamma/sRGB EOTF 和 BT.2020/BT.709/P3 色域转换
 * - 支持 16-bit 纹理上传和 HDR 亮度控制
 */
class HDR10WidgetWinDXGI : public QWidget
{
  Q_OBJECT

public:
  explicit HDR10WidgetWinDXGI(QWidget *parent = nullptr);
  ~HDR10WidgetWinDXGI() override;

  // ── 数据设置 (与 HDR10Widget 接口一致) ──
  void setFrame(const VideoFrame &frame);
  void setFrameHandler(FrameHandler *handler) { m_frameHandler = handler; }
  void setBitDepth(int bits) { m_bitDepth = bits; }
  void setDithering(bool enable) { m_ditheringEnabled = enable; }
  void setZoom(double zoom);
  void setMoveOffset(QPointF offset);
  void setShowRawData(bool show) { m_showRawData = show; }

  // ── EOTF 和色域控制 (与 HDR10Widget 接口一致) ──
  void setEOTF(HDR10_EOTF eotf) { m_eotf = eotf; m_frameNeedsUpdate = true; update(); }
  void setColorGamut(HDR10_ColorGamut gamut) { m_colorGamut = gamut; m_frameNeedsUpdate = true; update(); }
  void setGammaValue(float gamma) { m_gammaValue = gamma; m_frameNeedsUpdate = true; update(); }
  void setDiffuseWhiteNits(float nits) { m_diffuseWhiteNits = nits; m_frameNeedsUpdate = true; update(); }
  void setHDRBrightness(float brightness) { m_hdrBrightness = brightness; m_frameNeedsUpdate = true; update(); }
  void setPremultipliedAlpha(bool enabled) { m_premultipliedAlpha = enabled; update(); }
  void setDebugOutput(float mode) { m_debugOutput = mode; m_frameNeedsUpdate = true; update(); }

  // ── 状态查询 ──
  bool isHDRActive() const;
  QString getRendererInfo() const { return m_rendererInfo; }

  // ── Overlay drawing (called by SplitViewWidget's overlay widget) ──
  void drawPixelValues(QPainter *painter);
  void drawZoomIndicator(QPainter *painter);
  void drawPixelRulers(QPainter *painter);

signals:
  void hdrStatusChanged(bool hdrActive, bool systemHandlesTonemapping,
                        float maxNits, float sdrWhiteNits);

protected:
  void resizeEvent(QResizeEvent *event) override;
  void paintEvent(QPaintEvent *event) override;
  void showEvent(QShowEvent *event) override;
  void hideEvent(QHideEvent *event) override;
  bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;

private:
  void initD3D();
  void createShaders();
  void createGeometry();
  void createSampler();
  void updateTexture();
  void render();
  void drawOverlayTexture(ID3D11DeviceContext *ctx, int w, int h);
  void updateHDRStatus();

  // ── DXGI / D3D11 资源 ──
  std::unique_ptr<DXGISwapChain> m_swapChain;

  ComPtr<ID3D11VertexShader>     m_vertexShader;
  ComPtr<ID3D11PixelShader>      m_pixelShader;
  ComPtr<ID3D11PixelShader>      m_overlayShader;
  ComPtr<ID3D11InputLayout>      m_inputLayout;
  ComPtr<ID3D11Buffer>           m_vertexBuffer;
  ComPtr<ID3D11Buffer>           m_constantBuffer;
  ComPtr<ID3D11Buffer>           m_viewConstantBuffer;
  ComPtr<ID3D11ShaderResourceView> m_textureSRV;
  ComPtr<ID3D11ShaderResourceView> m_overlaySRV;
  ComPtr<ID3D11SamplerState>     m_samplerState;
  ComPtr<ID3D11Texture2D>        m_texture;
  ComPtr<ID3D11Texture2D>        m_overlayTexture;
  ComPtr<ID3D11BlendState>       m_overlayBlendState;
  ComPtr<ID3D11BlendState>       m_hdrBlendState; // Premultiplied alpha blend for HDR pipeline

  // ── 纹理信息 ──
  QSize m_textureSize;
  QSize m_overlaySize;
  bool  m_textureNeedsUpdate{false};

  // ── 帧数据 ──
  VideoFrame    m_currentFrame;
  QSize         m_frameSize;
  bool          m_frameNeedsUpdate{false};

  // ── 渲染参数 ──
  int  m_bitDepth{10};
  int  m_sourceBitDepth{8};
  bool m_ditheringEnabled{false};
  bool m_initialized{false};
  bool m_initFailed{false};   // Prevent infinite retry on shader compile failure
  bool m_showRawData{false};

  // ── EDR 色彩处理参数 ──
  HDR10_EOTF       m_eotf{HDR10_EOTF::SRGB};
  HDR10_ColorGamut m_colorGamut{HDR10_ColorGamut::BT709};
  float            m_gammaValue{2.2f};
  float            m_diffuseWhiteNits{203.0f};
  float            m_hdrBrightness{1.0f};
  bool             m_premultipliedAlpha{true};
  float            m_debugOutput{0.0f};  // 0=normal, 1=raw, 2=raw*10, 3=after EOTF

  // ── 视图控制 ──
  double  m_zoom{1.0};
  QPointF m_moveOffset{0, 0};

  // ── 其他 ──
  FrameHandler *m_frameHandler{nullptr};
  QString       m_rendererInfo;
  QTimer       *m_renderTimer{nullptr};
  QTimer       *m_hdrPollTimer{nullptr};  // Poll for HDR/ACM state changes
  int           m_frameCount{0};

  // ── HDR/ACM 状态追踪 ──
  bool m_lastHdrActive{false};
  bool m_lastSystemTonemapping{false};
};

} // namespace video

#endif // Q_OS_WIN
