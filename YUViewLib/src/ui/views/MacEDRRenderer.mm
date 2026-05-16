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

// Must include QtGlobal first to get Q_OS_MAC defined
#include <QtGlobal>

#ifdef Q_OS_MAC

#include "MacEDRRenderer.h"

#include <QWindow>
#include <QGuiApplication>
#include <QDebug>

// macOS 和 Metal 头文件
#import <Cocoa/Cocoa.h>
// objc/runtime.h no longer needed - removed object_setClass hack
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/QuartzCore.h>

namespace video
{

// 视频顶点数据结构 (全屏四边形 + UV)
struct VideoVertex
{
  float position[2];  // 顶点位置
  float texCoord[2];  // 纹理坐标
};

// 视频着色器 Uniforms
struct VideoUniforms
{
  int   eotfType;           // 0=PQ, 1=HLG, 2=Gamma, 3=sRGB
  int   sourceGamut;        // 0=BT2020, 1=BT709, 2=P3
  float gammaValue;         // Gamma 值
  float diffuseWhiteNits;   // 漫射白亮度 (nits)
  float gamutMatrix[9];     // 3x3 色域转换矩阵 (行优先)
};

// ========== 色域转换矩阵 ==========

// BT.2020 → Display P3 (通过 XYZ 中间空间)
static const float BT2020_TO_P3[9] = {
    1.343578f, -0.282180f, -0.061404f,
   -0.065298f,  1.075788f, -0.010490f,
    0.002822f, -0.019594f,  1.016915f
};

// BT.709 → Display P3
static const float BT709_TO_P3[9] = {
    0.822462f,  0.177536f, -0.000004f,
    0.033194f,  0.966807f, -0.000000f,
    0.017085f,  0.072414f,  0.910644f
};

// P3 → P3 (identity)
static const float P3_TO_P3[9] = {
    1.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 1.0f
};

static const float *getGamutMatrix(MacEDR_ColorGamut gamut)
{
  switch (gamut)
  {
    case MacEDR_ColorGamut::BT2020: return BT2020_TO_P3;
    case MacEDR_ColorGamut::BT709:  return BT709_TO_P3;
    case MacEDR_ColorGamut::P3:     return P3_TO_P3;
    default:                         return BT2020_TO_P3;
  }
}

MacEDRRenderer::MacEDRRenderer()
{
}

MacEDRRenderer::~MacEDRRenderer()
{
  // Release Metal objects that we own (not owned by NSView).
  // During application shutdown, the NSView hierarchy is destroyed first,
  // which releases m_metalLayer (set as view.layer). We must NOT release
  // it again here — it would be a double-free / dangling pointer.
  //
  // Safe to release (not owned by view):
  //   m_device, m_commandQueue — created directly from MTLCreateSystemDefaultDevice
  //   m_pipelineStateVideo, m_videoVertexBuffer, m_rgbaTexture — created from device
  //
  // NOT safe to release (owned by view, released when view deallocs):
  //   m_metalLayer — set as view.layer, view releases it
  //   m_overlayLayer — added as sublayer, view releases it

  if (m_device)          { CFRelease(m_device); m_device = nullptr; }
  if (m_commandQueue)    { CFRelease(m_commandQueue); m_commandQueue = nullptr; }
  if (m_pipelineStateVideo) { CFRelease(m_pipelineStateVideo); m_pipelineStateVideo = nullptr; }
  if (m_videoVertexBuffer)  { CFRelease(m_videoVertexBuffer); m_videoVertexBuffer = nullptr; }
  if (m_rgbaTexture)     { CFRelease(m_rgbaTexture); m_rgbaTexture = nullptr; }
}

bool MacEDRRenderer::initialize(QWindow *window)
{
  if (m_initialized)
    return true;

  if (!window)
  {
    qWarning() << "MacEDRRenderer: null window";
    return false;
  }

  // 获取窗口的 NSView
  void *nativeWindow = reinterpret_cast<void *>(window->winId());
  if (!nativeWindow)
  {
    qWarning() << "MacEDRRenderer: failed to get native window handle";
    return false;
  }

  NSView *view = (__bridge NSView *)nativeWindow;

  // 创建 Metal 设备
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (!device)
  {
    qWarning() << "MacEDRRenderer: failed to create Metal device";
    return false;
  }
  m_device = (__bridge_retained void *)device;

  // 创建命令队列
  id<MTLCommandQueue> commandQueue = [device newCommandQueue];
  if (!commandQueue)
  {
    qWarning() << "MacEDRRenderer: failed to create command queue";
    return false;
  }
  m_commandQueue = (__bridge_retained void *)commandQueue;

  // ========== 配置 CAMetalLayer 启用 EDR ==========

  CAMetalLayer *metalLayer = [CAMetalLayer layer];
  metalLayer.device = device;
  metalLayer.pixelFormat = MTLPixelFormatRGBA16Float;  // 16-bit float 支持 HDR
  metalLayer.framebufferOnly = NO;
  metalLayer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;

  // 启用 EDR (Extended Dynamic Range)
  if (@available(macOS 10.15, *))
  {
    metalLayer.wantsExtendedDynamicRangeContent = YES;

    // 设置扩展色域 (Extended Linear Display P3)
    // 这是 macOS EDR 的关键：输出值 > 1.0 时，系统自动映射到显示器 HDR 能力
    CGColorSpaceRef colorSpace =
        CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearDisplayP3);
    if (colorSpace)
    {
      metalLayer.colorspace = colorSpace;
      CGColorSpaceRelease(colorSpace);
      m_edrSupported = true;
      qInfo() << "MacEDRRenderer: EDR enabled with Extended Linear Display P3 color space";
    }
    else
    {
      qWarning() << "MacEDRRenderer: failed to create extended color space, falling back to sRGB";
      CGColorSpaceRef srgbSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
      if (srgbSpace)
      {
        metalLayer.colorspace = srgbSpace;
        CGColorSpaceRelease(srgbSpace);
      }
      m_edrSupported = false;
    }
  }
  else
  {
    // macOS 10.15 以下不支持 EDR
    CGColorSpaceRef srgbSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    if (srgbSpace)
    {
      metalLayer.colorspace = srgbSpace;
      CGColorSpaceRelease(srgbSpace);
    }
    m_edrSupported = false;
    qInfo() << "MacEDRRenderer: macOS < 10.15, EDR not available";
  }

  // view.layer now owns metalLayer — use __bridge (no extra retain)
  m_metalLayer = (__bridge void *)metalLayer;

  // 将 MetalLayer 添加到 NSView
  view.wantsLayer = YES;
  view.layer = metalLayer;

// NOTE: Do NOT modify the NSView class (e.g., object_setClass) to override
  // hitTest:. Qt's internal NSView subclass uses KVO and dynamic method
  // signatures that break when the class is swapped, causing crashes like
  // "method signature argument cannot be nil" or "displayLayer: unrecognized selector".
  // Instead, mouse event forwarding is handled on the Qt side via event filters
  // installed on the QWindow in HDR10WidgetMacEDR.cpp.

  // 检查显示器的 EDR 支持
  NSScreen *screen = view.window.screen;
  if (screen)
  {
    if (@available(macOS 10.15, *))
    {
      m_maxEDRValue = screen.maximumPotentialExtendedDynamicRangeColorComponentValue;
      qInfo() << "MacEDRRenderer: Screen max EDR value:" << m_maxEDRValue;

      if (m_maxEDRValue > 1.0f)
        qInfo() << "MacEDRRenderer: HDR display detected with EDR support";
      else
        qInfo() << "MacEDRRenderer: SDR display or EDR not available";
    }
  }

  // 设置初始大小
  NSRect frame = view.frame;
  m_width = (int)frame.size.width;
  m_height = (int)frame.size.height;
  metalLayer.drawableSize = CGSizeMake(m_width, m_height);

  // 创建渲染管线
  if (!createRenderPipeline())
  {
    qWarning() << "MacEDRRenderer: failed to create render pipeline";
    // Release already-retained ObjC objects before returning
    if (m_metalLayer)      { CFRelease(m_metalLayer); m_metalLayer = nullptr; }
    if (m_device)          { CFRelease(m_device); m_device = nullptr; }
    if (m_commandQueue)    { CFRelease(m_commandQueue); m_commandQueue = nullptr; }
    return false;
  }

  // 创建视频顶点缓冲区
  if (!createVideoVertexBuffer())
  {
    qWarning() << "MacEDRRenderer: failed to create video vertex buffer";
    // Release already-retained ObjC objects before returning
    if (m_metalLayer)      { CFRelease(m_metalLayer); m_metalLayer = nullptr; }
    if (m_device)          { CFRelease(m_device); m_device = nullptr; }
    if (m_commandQueue)    { CFRelease(m_commandQueue); m_commandQueue = nullptr; }
    if (m_pipelineStateVideo) { CFRelease(m_pipelineStateVideo); m_pipelineStateVideo = nullptr; }
    return false;
  }

  // ========== 创建 Overlay CALayer (像素值显示在 Metal 层上方) ==========
  // 注意：不能用额外的 Qt Widget (WA_NativeWindow) 作为 overlay，
  // 因为额外的 NSView 会干扰 Metal QWindow 的 KVO 链，导致崩溃
  // "method signature argument cannot be nil"。
  // 使用原生 CALayer 叠加在 Metal 层上方，完全绕过 Qt 的 NSView 管理。
  {
    CALayer *overlayLayer = [CALayer layer];
    overlayLayer.frame = view.bounds;
    overlayLayer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
    overlayLayer.opacity = 1.0;
    overlayLayer.contentsGravity = kCAGravityTopLeft;
    // Add above the Metal layer in the layer hierarchy
    [view.layer addSublayer:overlayLayer];
    // Use __bridge (no retain) — the view owns the layer via addSublayer.
    // We only need a weak reference to update contents later.
    m_overlayLayer = (__bridge void *)overlayLayer;
  }

  m_initialized = true;
  qInfo() << "MacEDRRenderer: initialized successfully";
  return true;
}

void MacEDRRenderer::resize(int width, int height)
{
  m_width = width;
  m_height = height;

  if (m_metalLayer)
  {
    CAMetalLayer *layer = (__bridge CAMetalLayer *)m_metalLayer;
    layer.drawableSize = CGSizeMake(width, height);
  }

  // Update overlay layer frame
  if (m_overlayLayer)
  {
    CALayer *overlay = (__bridge CALayer *)m_overlayLayer;
    overlay.frame = CGRectMake(0, 0, width, height);
  }
}

void MacEDRRenderer::setOverlayImage(const QImage &image)
{
  if (!m_overlayLayer || image.isNull())
    return;

  CALayer *overlay = (__bridge CALayer *)m_overlayLayer;

  // Convert QImage to CGImage and set as layer contents
  QImage rgbaImage = image.convertedTo(QImage::Format_RGBA8888);
  CGImageRef cgImage = rgbaImage.toCGImage();
  if (cgImage)
  {
    overlay.contents = (__bridge id)cgImage;
    CGImageRelease(cgImage);
  }
}

void MacEDRRenderer::render()
{
  if (!m_initialized || !m_device || !m_commandQueue || !m_metalLayer)
    return;

  if (!m_hasFrame || !m_rgbaTexture)
    return;

  id<MTLDevice>         device        = (__bridge id<MTLDevice>)m_device;
  id<MTLCommandQueue>   commandQueue  = (__bridge id<MTLCommandQueue>)m_commandQueue;
  CAMetalLayer          *metalLayer   = (__bridge CAMetalLayer *)m_metalLayer;
  id<MTLRenderPipelineState> pipeline = (__bridge id<MTLRenderPipelineState>)m_pipelineStateVideo;

  // ========== 计算顶点坐标 (与 HDR10Widget OpenGL paintGL 对齐) ==========

  // Calculate vertex positions that match SplitViewWidget's behavior
  // Same logic as HDR10Widget::paintGL() for consistent zoom/pan behavior
  int widgetW = m_width;
  int widgetH = m_height;
  int frameW = m_frameWidth;
  int frameH = m_frameHeight;

  if (frameW <= 0 || frameH <= 0 || widgetW <= 0 || widgetH <= 0)
    return;

  // Calculate display size in pixels (same as SplitViewWidget)
  double displayW = frameW * m_zoom;
  double displayH = frameH * m_zoom;

  // Convert to NDC (Normalized Device Coordinates: -1 to 1)
  // NDC X = (pixelX / widgetW) * 2 - 1
  // NDC Y = 1 - (pixelY / widgetH) * 2 (flip Y because QPainter Y is down, Metal Y is up)
  double ndcOffsetX = m_moveOffset.x() / (widgetW * 0.5);
  double ndcOffsetY = -m_moveOffset.y() / (widgetH * 0.5); // Flip Y for Metal

  double ndcW = displayW / (widgetW * 0.5);
  double ndcH = displayH / (widgetH * 0.5);

  float left   = static_cast<float>(ndcOffsetX - ndcW * 0.5);
  float right  = static_cast<float>(ndcOffsetX + ndcW * 0.5);
  float bottom = static_cast<float>(ndcOffsetY - ndcH * 0.5);
  float top    = static_cast<float>(ndcOffsetY + ndcH * 0.5);

  // Texture coords (flipped Y to fix upside-down, same as HDR10Widget)
  VideoVertex vertices[] = {
      {{left,  bottom}, {0.0f, 1.0f}},  // Bottom-left
      {{right, bottom}, {1.0f, 1.0f}},  // Bottom-right
      {{left,  top},    {0.0f, 0.0f}},  // Top-left
      {{right, top},    {1.0f, 0.0f}},  // Top-right
  };

  // 获取当前 drawable
  id<CAMetalDrawable> drawable = [metalLayer nextDrawable];
  if (!drawable)
    return;

  // 创建命令缓冲区
  id<MTLCommandBuffer> commandBuffer = [commandQueue commandBuffer];

  // 创建渲染描述符
  MTLRenderPassDescriptor *renderPassDesc = [MTLRenderPassDescriptor renderPassDescriptor];
  renderPassDesc.colorAttachments[0].texture = drawable.texture;
  renderPassDesc.colorAttachments[0].loadAction = MTLLoadActionClear;
  renderPassDesc.colorAttachments[0].storeAction = MTLStoreActionStore;
  renderPassDesc.colorAttachments[0].clearColor = MTLClearColorMake(0.14, 0.14, 0.14, 1.0);

  id<MTLRenderCommandEncoder> encoder =
      [commandBuffer renderCommandEncoderWithDescriptor:renderPassDesc];

  // 设置渲染管线
  [encoder setRenderPipelineState:pipeline];

  // 设置动态计算的顶点缓冲区 (zoom/pan 调整后的坐标)
  id<MTLBuffer> vertexBuffer = (__bridge id<MTLBuffer>)m_videoVertexBuffer;
  // Copy updated vertex data into the existing shared buffer
  memcpy([vertexBuffer contents], vertices, sizeof(vertices));
  [encoder setVertexBuffer:vertexBuffer offset:0 atIndex:0];

  // 设置纹理
  id<MTLTexture> rgbaTexture = (__bridge id<MTLTexture>)m_rgbaTexture;
  [encoder setFragmentTexture:rgbaTexture atIndex:0];

  // 设置 VideoUniforms
  VideoUniforms uniforms;
  uniforms.eotfType = static_cast<int>(m_eotf);
  uniforms.sourceGamut = static_cast<int>(m_colorGamut);
  uniforms.gammaValue = m_gammaValue;
  uniforms.diffuseWhiteNits = m_diffuseWhiteNits;
  const float *matrix = getGamutMatrix(m_colorGamut);
  for (int i = 0; i < 9; ++i)
    uniforms.gamutMatrix[i] = matrix[i];

  [encoder setFragmentBytes:&uniforms length:sizeof(VideoUniforms) atIndex:0];

  // 设置 HDR 亮度
  float brightness = m_hdrBrightness;
  [encoder setFragmentBytes:&brightness length:sizeof(float) atIndex:1];

  // 绘制四边形 (triangle strip)
  [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];

  [encoder endEncoding];

  // 呈现
  [commandBuffer presentDrawable:drawable];
  [commandBuffer commit];
}

void MacEDRRenderer::setHDRBrightness(float brightness)
{
  m_hdrBrightness = brightness;
}

void MacEDRRenderer::setDiffuseWhite(float nits)
{
  if (nits < 10)   nits = 10;
  if (nits > 500)  nits = 500;
  m_diffuseWhiteNits = nits;
}

void MacEDRRenderer::setEOTF(MacEDR_EOTF eotf)
{
  m_eotf = eotf;
}

void MacEDRRenderer::setColorGamut(MacEDR_ColorGamut gamut)
{
  m_colorGamut = gamut;
}

void MacEDRRenderer::setGammaValue(float gamma)
{
  m_gammaValue = gamma;
}

void MacEDRRenderer::setZoom(double zoom)
{
  m_zoom = zoom;
}

void MacEDRRenderer::setMoveOffset(QPointF offset)
{
  m_moveOffset = offset;
}

bool MacEDRRenderer::loadFrame(const VideoFrame &frame)
{
  if (!m_initialized)
    return false;

  if (!frame.isValid())
    return false;

  const int w = frame.width();
  const int h = frame.height();

  // Ensure 16-bit buffer exists
  if (!frame.has16bitBuffer())
  {
    const_cast<VideoFrame &>(frame).generate16bitBuffer();
  }

  const uint16_t *data = frame.getData16bit();
  if (!data)
    return false;

  // Recreate texture if size changed
  if (w != m_frameWidth || h != m_frameHeight || !m_rgbaTexture)
  {
    if (!createTextures(w, h))
      return false;
    m_frameWidth = w;
    m_frameHeight = h;
  }

  // Update texture data
  if (!updateTextureData(data, w, h))
    return false;

  m_hasFrame = true;
  return true;
}

bool MacEDRRenderer::createRenderPipeline()
{
  id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;

  // ========== 视频渲染着色器 (内联 Metal Shading Language) ==========
  //
  // 着色器管线：
  //   1. 采样 RGBA16UI 纹理 → 16-bit 无符号整数数据
  //   2. 归一化到 0-1 范围 (/ 65535.0)
  //   3. 应用 EOTF (PQ/HLG/Gamma/sRGB) → 线性光
  //   4. 漫射白归一化 (对于 PQ: 除以 diffuseWhiteNits)
  //   5. 色域转换 (BT.2020/BT.709/P3 → Display P3)
  //   6. HDR 亮度缩放
  //   7. 输出到 RGBA16Float (values can exceed 1.0 → EDR)

  NSString *shaderSource =
      @"#include <metal_stdlib>\n"
      @"using namespace metal;\n"
      @"\n"
      @"struct VideoVertexIn {\n"
      @"    float2 position [[attribute(0)]];\n"
      @"    float2 texCoord [[attribute(1)]];\n"
      @"};\n"
      @"\n"
      @"struct VertexOut {\n"
      @"    float4 position [[position]];\n"
      @"    float2 texCoord;\n"
      @"};\n"
      @"\n"
      @"struct VideoUniforms {\n"
      @"    int eotfType;\n"
      @"    int sourceGamut;\n"
      @"    float gammaValue;\n"
      @"    float diffuseWhiteNits;\n"
      @"    float gamutMatrix[9];\n"
      @"};\n"
      @"\n"
      @"// ========== EOTF 函数 ==========\n"
      @"\n"
      @"// PQ EOTF (SMPTE ST 2084)\n"
      @"// Input: 0-1 (PQ encoded), Output: linear light (nits)\n"
      @"// Normalized so diffuseWhiteNits → 1.0\n"
      @"float3 pqEotf(float3 pq, float diffuseWhiteNits) {\n"
      @"    const float m1 = 2610.0 / 4096.0 * (1.0 / 4.0);  // 0.1593017578\n"
      @"    const float m2 = 2523.0 / 4096.0 * 128.0;         // 78.84375\n"
      @"    const float c1 = 3424.0 / 4096.0;                  // 0.8359375\n"
      @"    const float c2 = 2413.0 / 4096.0 * 32.0;          // 18.8515625\n"
      @"    const float c3 = 2392.0 / 4096.0 * 32.0;          // 18.6875\n"
      @"    float3 p = pow(pq, 1.0 / m2);\n"
      @"    float3 num = max(p - c1, 0.0);\n"
      @"    float3 den = c2 - c3 * p;\n"
      @"    float3 linear = pow(num / den, 1.0 / m1);\n"
      @"    // linear is now 0-1 (normalized to 10000 nits peak)\n"
      @"    // Multiply by 10000 to get nits, then divide by diffuseWhiteNits\n"
      @"    return linear * 10000.0 / diffuseWhiteNits;\n"
      @"}\n"
      @"\n"
      @"// HLG EOTF (ARIB STD-B67)\n"
      @"// Output: 0-1 range (scene linear), HDR highlights can exceed 1.0\n"
      @"float3 hlgEotf(float3 hlg) {\n"
      @"    const float a = 0.17883277;\n"
      @"    const float b = 0.28466892;\n"
      @"    const float c = 0.55991073;\n"
      @"    float3 linear;\n"
      @"    for (int i = 0; i < 3; i++) {\n"
      @"        if (hlg[i] <= 0.5) {\n"
      @"            linear[i] = hlg[i] * hlg[i] / 3.0;\n"
      @"        } else {\n"
      @"            linear[i] = (exp((hlg[i] - c) / a) + b) / 12.0;\n"
      @"        }\n"
      @"    }\n"
      @"    return linear;\n"
      @"}\n"
      @"\n"
      @"// Gamma EOTF\n"
      @"float3 gammaEotf(float3 gamma, float gammaValue) {\n"
      @"    return pow(gamma, gammaValue);\n"
      @"}\n"
      @"\n"
      @"// sRGB EOTF\n"
      @"float3 srgbEotf(float3 srgb) {\n"
      @"    float3 linear;\n"
      @"    for (int i = 0; i < 3; i++) {\n"
      @"        if (srgb[i] <= 0.04045) {\n"
      @"            linear[i] = srgb[i] / 12.92;\n"
      @"        } else {\n"
      @"            linear[i] = pow((srgb[i] + 0.055) / 1.055, 2.4);\n"
      @"        }\n"
      @"    }\n"
      @"    return linear;\n"
      @"}\n"
      @"\n"
      @"// ========== 顶点着色器 ==========\n"
      @"vertex VertexOut videoVertexShader(VideoVertexIn in [[stage_in]]) {\n"
      @"    VertexOut out;\n"
      @"    out.position = float4(in.position, 0.0, 1.0);\n"
      @"    out.texCoord = in.texCoord;\n"
      @"    return out;\n"
      @"}\n"
      @"\n"
      @"// ========== 片段着色器 ==========\n"
      @"// 处理来自 YUView VideoFrame 的 16-bit RGBA 数据\n"
      @"// 数据在纹理中以 RGBA16UI 格式存储 (0-65535 范围)\n"
      @"// 归一化到 0-1 后应用 EOTF 和色域转换\n"
      @"fragment float4 videoFragmentShader(\n"
      @"    VertexOut in [[stage_in]],\n"
      @"    texture2d<uint> rgbaTexture [[texture(0)]],\n"
      @"    constant VideoUniforms &uniforms [[buffer(0)]],\n"
      @"    constant float &hdrBrightness [[buffer(1)]]) {\n"
      @"\n"
      @"    constexpr sampler textureSampler(coord::normalized, filter::nearest,\n"
      @"                                     address::clamp_to_edge);\n"
      @"\n"
      @"    // 采样 16-bit 无符号整数纹理\n"
      @"    uint4 raw = rgbaTexture.sample(textureSampler, in.texCoord);\n"
      @"\n"
      @"    // 归一化到 0-1 范围\n"
      @"    // 数据存储在 16-bit 范围 (0-65535):\n"
      @"    //   8-bit源: ×257 扩展\n"
      @"    //   10-bit源: <<6 扩展\n"
      @"    //   16-bit源: 原生值\n"
      @"    float3 color = float3(raw.rgb) / 65535.0;\n"
      @"\n"
      @"    // ========== EOTF 转换 ==========\n"
      @"    // 将编码值转换为线性光\n"
      @"    float3 linear;\n"
      @"    if (uniforms.eotfType == 0) {\n"
      @"        // PQ: 归一化到漫射白\n"
      @"        linear = pqEotf(color, uniforms.diffuseWhiteNits);\n"
      @"    } else if (uniforms.eotfType == 1) {\n"
      @"        // HLG: peak luminance = 4 × diffuse white\n"
      @"        // Standard HLG EOTF outputs 1.0 for diffuse white (signal 1.0)\n"
      @"        // Scale by 4.0 so peak = 4 × diffuse white\n"
      @"        linear = hlgEotf(color) * 4.0;\n"
      @"    } else if (uniforms.eotfType == 2) {\n"
      @"        // Gamma\n"
      @"        linear = gammaEotf(color, uniforms.gammaValue);\n"
      @"    } else {\n"
      @"        // sRGB\n"
      @"        linear = srgbEotf(color);\n"
      @"    }\n"
      @"\n"
      @"    // ========== 色域转换 ==========\n"
      @"    // 从源色域转换到 Display P3\n"
      @"    // gamutMatrix 是 3x3 行优先矩阵\n"
      @"    // Metal float3x3 构造器接受列向量\n"
      @"    float3 col0 = float3(uniforms.gamutMatrix[0], uniforms.gamutMatrix[3], uniforms.gamutMatrix[6]);\n"
      @"    float3 col1 = float3(uniforms.gamutMatrix[1], uniforms.gamutMatrix[4], uniforms.gamutMatrix[7]);\n"
      @"    float3 col2 = float3(uniforms.gamutMatrix[2], uniforms.gamutMatrix[5], uniforms.gamutMatrix[8]);\n"
      @"    float3x3 gamutMatrix = float3x3(col0, col1, col2);\n"
      @"\n"
      @"    if (uniforms.sourceGamut != 2) {\n"
      @"        linear = gamutMatrix * linear;\n"
      @"    }\n"
      @"\n"
      @"    // ========== HDR 亮度调整 ==========\n"
      @"    // SDR 范围: 0.0-1.0 (100 nits)\n"
      @"    // EDR 范围: 0.0-maxEDR (可达 1600 nits 或更高)\n"
      @"    // 值超过 1.0 的部分由 macOS 自动映射到显示器 HDR 能力\n"
      @"    linear *= hdrBrightness;\n"
      @"\n"
      @"    return float4(linear, 1.0);\n"
      @"}\n";

  // 编译着色器
  NSError *error = nil;
  id<MTLLibrary> library = [device newLibraryWithSource:shaderSource options:nil error:&error];
  if (!library)
  {
    qWarning() << "MacEDRRenderer: failed to compile shader:"
               << QString::fromNSString(error.localizedDescription);
    return false;
  }

  id<MTLFunction> vertexFunction = [library newFunctionWithName:@"videoVertexShader"];
  id<MTLFunction> fragmentFunction = [library newFunctionWithName:@"videoFragmentShader"];

  if (!vertexFunction || !fragmentFunction)
  {
    qWarning() << "MacEDRRenderer: failed to get shader functions";
    return false;
  }

  // 创建渲染管线描述符
  MTLRenderPipelineDescriptor *pipelineDesc = [[MTLRenderPipelineDescriptor alloc] init];
  pipelineDesc.vertexFunction = vertexFunction;
  pipelineDesc.fragmentFunction = fragmentFunction;
  pipelineDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;

  // 配置顶点描述符
  MTLVertexDescriptor *vertexDesc = [[MTLVertexDescriptor alloc] init];

  // Position (float2)
  vertexDesc.attributes[0].format = MTLVertexFormatFloat2;
  vertexDesc.attributes[0].offset = offsetof(VideoVertex, position);
  vertexDesc.attributes[0].bufferIndex = 0;

  // TexCoord (float2)
  vertexDesc.attributes[1].format = MTLVertexFormatFloat2;
  vertexDesc.attributes[1].offset = offsetof(VideoVertex, texCoord);
  vertexDesc.attributes[1].bufferIndex = 0;

  vertexDesc.layouts[0].stride = sizeof(VideoVertex);
  vertexDesc.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

  pipelineDesc.vertexDescriptor = vertexDesc;

  id<MTLRenderPipelineState> pipelineState =
      [device newRenderPipelineStateWithDescriptor:pipelineDesc error:&error];
  if (!pipelineState)
  {
    qWarning() << "MacEDRRenderer: failed to create pipeline state:"
               << QString::fromNSString(error.localizedDescription);
    return false;
  }

  m_pipelineStateVideo = (__bridge_retained void *)pipelineState;
  return true;
}

bool MacEDRRenderer::createVideoVertexBuffer()
{
  id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;

  // Full-screen quad (triangle strip)
  // Position: NDC coordinates (-1 to 1)
  // TexCoord: texture coordinates (0 to 1)
  VideoVertex vertices[] = {
      {{-1.0f, -1.0f}, {0.0f, 1.0f}},  // Bottom-left
      {{ 1.0f, -1.0f}, {1.0f, 1.0f}},  // Bottom-right
      {{-1.0f,  1.0f}, {0.0f, 0.0f}},  // Top-left
      {{ 1.0f,  1.0f}, {1.0f, 0.0f}},  // Top-right
  };

  id<MTLBuffer> vertexBuffer = [device newBufferWithBytes:vertices
                                                    length:sizeof(vertices)
                                                   options:MTLResourceStorageModeShared];
  if (!vertexBuffer)
  {
    qWarning() << "MacEDRRenderer: failed to create vertex buffer";
    return false;
  }

  m_videoVertexBuffer = (__bridge_retained void *)vertexBuffer;
  return true;
}

bool MacEDRRenderer::createTextures(int width, int height)
{
  id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;

#if !__has_feature(objc_arc)
  if (m_rgbaTexture) [(id)m_rgbaTexture release];
#endif

  // RGBA16UI 纹理描述符 (与 OpenGL GL_RGBA16UI 对应)
  // 用于存储 16-bit 无符号整数 RGBA 数据
  MTLTextureDescriptor *rgbaDesc = [[MTLTextureDescriptor alloc] init];
  rgbaDesc.pixelFormat = MTLPixelFormatRGBA16Uint;  // 16-bit unsigned integer per channel
  rgbaDesc.width = width;
  rgbaDesc.height = height;
  rgbaDesc.usage = MTLTextureUsageShaderRead;
  rgbaDesc.storageMode = MTLStorageModeShared;

  id<MTLTexture> rgbaTexture = [device newTextureWithDescriptor:rgbaDesc];
  if (!rgbaTexture)
  {
    qWarning() << "MacEDRRenderer: failed to create RGBA texture";
    return false;
  }

  m_rgbaTexture = (__bridge_retained void *)rgbaTexture;
  m_textureSize = QSize(width, height);

  qInfo() << "MacEDRRenderer: created textures" << width << "x" << height;
  return true;
}

bool MacEDRRenderer::updateTextureData(const uint16_t *data, int width, int height)
{
  if (!m_rgbaTexture)
    return false;

  id<MTLTexture> rgbaTexture = (__bridge id<MTLTexture>)m_rgbaTexture;

  MTLRegion region = MTLRegionMake2D(0, 0, width, height);
  [rgbaTexture replaceRegion:region
                mipmapLevel:0
                  withBytes:data
                bytesPerRow:width * 4 * sizeof(uint16_t)];  // RGBA, 4 channels × 2 bytes

  return true;
}

} // namespace video

#endif // Q_OS_MAC