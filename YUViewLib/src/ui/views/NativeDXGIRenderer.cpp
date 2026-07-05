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

#include "NativeDXGIRenderer.h"

#ifdef Q_OS_WIN

#include <QDebug>
#include <QFontMetrics>
#include <QPainter>
#include <QResizeEvent>
#include <QSettings>
#include <QWindow>
#include <d3dcompiler.h>
#include <windowsx.h>

#include <video/yuv/videoHandlerYUV.h>
#include <common/ColorPipeline.h>
#include <common/FunctionsGui.h>

#include <cmath>
#include <cstdint>

namespace video {

// ─── HLSL Shaders ──────────────────────────────────────────────────

/** Fullscreen quad vertex shader with zoom and pan support */
static const char *g_vertexShaderSrc = R"(
struct VS_INPUT
{
    float2 pos : POSITION;
    float2 uv  : TEXCOORD;
};

struct VS_OUTPUT
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD;
};

cbuffer ViewConstants : register(b1)
{
    float2 panOffset;       // Pan offset in NDC (-1..1)
    float2 zoomScale;       // Zoom scale X and Y (1.0 = fit to view)
};

VS_OUTPUT main(VS_INPUT input)
{
    VS_OUTPUT output;
    // Apply zoom: scale the quad vertices independently per axis
    float2 scaledPos = input.pos / zoomScale;
    // Apply pan: offset in NDC space
    scaledPos += panOffset;
    output.pos = float4(scaledPos, 0.0f, 1.0f);
    output.uv  = input.uv;
    return output;
}
)";

/** HDR pixel shader with EOTF and color gamut conversion
 *
 *  Ported from opengl_fragment.glsl.
 *  Input:  16-bit RGBA texture (DXGI_FORMAT_R16G16B16A16_UNORM → sampled as float4 0..1)
 *  Output: scRGB linear values (can be >1.0 for HDR)
 */
static const char *g_pixelShaderSrc = R"(
Texture2D    hdrTexture : register(t0);
SamplerState texSampler : register(s0);

cbuffer Constants : register(b0)
{
    int   eotf;                    // RendererEOTF enum: 0=PQ, 1=HLG, 2=Gamma, 3=sRGB
    float gammaValue;              // Gamma exponent (only when eotf=2)
    float diffuseWhiteNits;        // Diffuse white reference (nits) from user settings
    float hdrBrightness;           // HDR brightness multiplier
    // --- 16-byte boundary (offset 16) ---
    float sdrWhiteNits;            // Windows system SDR white level in nits (scRGB 1.0 = this)
    float hdrActive;               // 1.0 if HDR active
    float systemHandlesTonemapping;// 1.0 if system (HDR or ACM) does tonemapping — skip Reinhard
    float debugOutput;             // 0=normal, 1=raw texture, 2=raw*10, 3=after EOTF
    // --- 16-byte boundary (offset 32) ---
    int   premultipliedAlpha;      // 1 = source is premultiplied, 0 = shader premultiplies
    float3 _pad;                   // pad to 16-byte boundary before matrix
    // --- 16-byte boundary (offset 48) ---
    // Gamut conversion matrix (row-major from C++, packed column-major for HLSL).
    // When system handles color (HDR or ACM): source → BT.709, compositor does the rest.
    // When system is off: source → display gamut, we do it ourselves.
    float3x3 gamutMatrix;          // offset 48-95
};

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD;
};

// ── PQ (ST.2084) EOTF ─────────────────────────────────────────────

static const float PQ_m1 = 0.1593017578125;  // 2610/16384
static const float PQ_m2 = 78.84375;          // 2523/32
static const float PQ_c1 = 0.8359375;         // 3424/4096
static const float PQ_c2 = 18.8515625;        // 2413/128
static const float PQ_c3 = 18.6875;           // 2392/128

float pqToLinear(float pqValue)
{
    float v = pow(max(pqValue, 0.0f), 1.0f / PQ_m2);
    float num = max(v - PQ_c1, 0.0f);
    float den = max(PQ_c2 - PQ_c3 * v, 1e-10f);
    // ST.2084 defines 1.0 = 10000 nits, so multiply by 10000
    return pow(num / den, 1.0f / PQ_m1) * 10000.0f;
}

float3 pqToLinear(float3 pq)
{
    return float3(pqToLinear(pq.r), pqToLinear(pq.g), pqToLinear(pq.b));
}

// ── HLG EOTF ──────────────────────────────────────────────────────

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

// ── sRGB EOTF ─────────────────────────────────────────────────────

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

// ── EOTF dispatch ─────────────────────────────────────────────────

float3 applyEOTF(float3 coded)
{
    if (eotf == 0) return pqToLinear(coded);       // PQ
    if (eotf == 1) return hlgToLinear(coded);      // HLG
    if (eotf == 2) return pow(coded, gammaValue);  // Gamma
    return sRGBToLinear(coded);                    // sRGB
}

// Gamut conversion matrix is uploaded via cbuffer (gamutMatrix) and applied
// unconditionally in applyGamutConversion() — no static matrices needed.

float3 applyGamutConversion(float3 linColor)
{
    // gamutMatrix is uploaded column-major (via packGamutMatrixForHLSL).
    // HLSL mul(M, v) = M·v (matrix × column vector).
    return mul(gamutMatrix, linColor);
}

// ── Main ──────────────────────────────────────────────────────────

float4 main(PS_INPUT input) : SV_TARGET
{
    // Sample the 16-bit texture (UNORM → 0..1)
    float4 texColor = hdrTexture.Sample(texSampler, input.uv);
    float alpha = texColor.a;

    // Premultiply in encoding domain if source is non-premultiplied.
    // This must happen before EOTF for gamma/sRGB-encoded content.
    if (premultipliedAlpha == 0) {
        texColor.rgb *= alpha;
    }

    // DEBUG: passthrough mode - output raw texture values directly
    // Set debugOutput via constant buffer: 0=normal, 1=raw, 2=raw*10, 3=after EOTF
    if (debugOutput > 0.5f && debugOutput < 1.5f)
        return texColor;
    if (debugOutput > 1.5f && debugOutput < 2.5f)
        return float4(texColor.rgb * 10.0, alpha);

    // Apply EOTF: coded value → linear light
    float3 linColor = applyEOTF(texColor.rgb);

    // DEBUG: output after EOTF only
    if (debugOutput > 2.5f && debugOutput < 3.5f)
        return float4(linColor, alpha);

    // Apply color gamut conversion
    linColor = applyGamutConversion(linColor);

    // ── Map to scRGB (1.0 = 80 nits) ───────────────────────────────
    // PQ (eotf==0):  pqToLinear returns absolute nits (0..10000)
    //                → divide by 80 to get scRGB units
    // HLG (eotf==1): hlgToLinear returns relative 0..1
    //                → peak = 4× sdrWhiteNits, divide by 80
    // Gamma (eotf==2): pow(coded, gamma) returns relative 0..1
    //                → 1.0 = sdrWhiteNits, divide by 80
    // sRGB (eotf==3): sRGBToLinear returns relative 0..1
    //                → 1.0 = sdrWhiteNits, divide by 80

    float3 output;
    if (eotf == 0)
    {
        // PQ: absolute nits → scRGB (÷80)
        output = linColor / 80.0;
    }
    else if (eotf == 1)
    {
        // HLG: relative 0..1, peak = 4× system SDR white → scRGB
        output = linColor * (sdrWhiteNits * 4.0 / 80.0);
    }
    else
    {
        // Gamma / sRGB: relative 0..1, 1.0 = system SDR white → scRGB
        output = linColor * (sdrWhiteNits / 80.0);
    }

    // Apply user brightness adjustment
    output *= hdrBrightness;

    // If system is NOT handling tonemapping (SDR without ACM), apply Reinhard
    if (systemHandlesTonemapping < 0.5f)
    {
        float luminance = dot(output, float3(0.2126f, 0.7152f, 0.0722f));
        float mappedLum = luminance / (1.0f + luminance);
        if (luminance > 0.001f)
            output *= mappedLum / luminance;
    }

    return float4(output, alpha);
}
)";

/** Overlay pixel shader: blend RGBA8 overlay texture on top of rendered frame.
 *  Uses alpha blending: result = overlay.rgb * overlay.a + existing * (1 - overlay.a)
 *  Keeps the existing HDR values for non-overlay pixels.
 */
static const char *g_overlayShaderSrc = R"(
Texture2D    overlayTexture : register(t0);
SamplerState overlaySampler : register(s0);

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD;
};

float4 main(PS_INPUT input) : SV_TARGET
{
    float4 overlay = overlayTexture.Sample(overlaySampler, input.uv);
    // overlay is RGBA8_UNORM: premultiplied alpha blending
    // We can't read the existing render target in PS_4_0 without MRT,
    // so we use additive blending via blend state.
    // The blend state does: src * srcAlpha + dst * (1 - srcAlpha)
    return overlay;
}
)";

// ─── Vertex structure ──────────────────────────────────────────────

struct Vertex
{
  float x, y;
  float u, v;
};

static const Vertex g_quadVertices[] = {
  {-1.0f,  1.0f, 0.0f, 0.0f},
  { 1.0f,  1.0f, 1.0f, 0.0f},
  {-1.0f, -1.0f, 0.0f, 1.0f},
  { 1.0f, -1.0f, 1.0f, 1.0f},
};

struct ConstantBuffer
{
  int   eotf;                      // offset 0
  float gammaValue;                // offset 4
  float diffuseWhiteNits;          // offset 8
  float hdrBrightness;             // offset 12
  // --- 16-byte boundary (offset 16) ---
  float sdrWhiteNits;              // offset 16
  float hdrActive;                 // offset 20
  float systemHandlesTonemapping;  // offset 24
  float debugOutput;               // offset 28
  // --- 16-byte boundary (offset 32) ---
  int   premultipliedAlpha;        // offset 32
  float _pad[3];                   // offset 36-47, align matrix to 16-byte boundary
  // --- 16-byte boundary (offset 48) ---
  // Gamut matrix packed for HLSL float3x3 (column-major, 3 float4 slots).
  // packGamutMatrixForHLSL() writes the flat float[9] column-wise as
  // [col0.xyz, 0, col1.xyz, 0, col2.xyz, 0].
  float gamutMatrixPacked[12];     // offset 48-95
};

/// Repack a flat row-major float[9] gamut matrix into the HLSL cbuffer
/// layout: HLSL float3x3 is column-major (3 columns × 4 floats each).
/// flatMatrix is row-major: flatMatrix[row*3+col].
/// We write COLUMN-wise so HLSL's internal column-major representation
/// matches the mathematical M (no transpose).
/// @param flatMatrix  9 floats, row-major (row 0 = [0..2], etc.)
/// @param packedOut   12 floats output (3 columns × 4, last float per col unused)
static void packGamutMatrixForHLSL(const float flatMatrix[9], float packedOut[12])
{
  // Column 0 (red mapping): flatMatrix[0], flatMatrix[3], flatMatrix[6]
  packedOut[0] = flatMatrix[0];
  packedOut[1] = flatMatrix[3];
  packedOut[2] = flatMatrix[6];
  packedOut[3] = 0.0f;
  // Column 1 (green mapping): flatMatrix[1], flatMatrix[4], flatMatrix[7]
  packedOut[4] = flatMatrix[1];
  packedOut[5] = flatMatrix[4];
  packedOut[6] = flatMatrix[7];
  packedOut[7] = 0.0f;
  // Column 2 (blue mapping): flatMatrix[2], flatMatrix[5], flatMatrix[8]
  packedOut[8] = flatMatrix[2];
  packedOut[9] = flatMatrix[5];
  packedOut[10] = flatMatrix[8];
  packedOut[11] = 0.0f;
}

struct ViewConstantBuffer
{
  float panOffsetX;
  float panOffsetY;
  float zoomScaleX;
  float zoomScaleY;
};

// ─── NativeDXGIRenderer implementation ────────────────────────────

NativeDXGIRenderer::NativeDXGIRenderer(QWidget *parent)
  : QWidget(parent)
{
  setAttribute(Qt::WA_NativeWindow);
  setAutoFillBackground(false);

  m_renderTimer = new QTimer(this);
  connect(m_renderTimer, &QTimer::timeout, this, [this]() {
    if (m_initialized && m_frameNeedsUpdate)
      render();
  });

  // Poll for HDR/ACM state changes every 2 seconds.
  // WM_DISPLAYCHANGE only fires on resolution/bpp changes, NOT on ACM toggle,
  // so we need polling to detect ACM on/off or HDR on/off transitions.
  m_hdrPollTimer = new QTimer(this);
  connect(m_hdrPollTimer, &QTimer::timeout, this, [this]() {
    if (m_swapChain)
    {
      m_swapChain->refreshCapabilities();
      updateHDRStatus();
    }
  });

  // Normal rendering mode
  m_debugOutput = 0.0f;
}

NativeDXGIRenderer::~NativeDXGIRenderer()
{
  m_renderTimer->stop();
  if (m_hdrPollTimer)
    m_hdrPollTimer->stop();
}

void NativeDXGIRenderer::setFrame(const VideoFrame &frame)
{
  const uint16_t *newData = frame.getData16bit();
  const uint16_t *oldData =
      m_currentFrame.is16bitGenerateFrom8bit() ? nullptr : m_currentFrame.getData16bit();
  std::shared_ptr<QImage> newImage8 = frame.getImage8bit();
  std::shared_ptr<QImage> oldImage8 = m_currentFrame.getImage8bit();

  if (newData != oldData || frame.getSize() != m_frameSize || newImage8 != oldImage8)
  {
    m_currentFrame     = frame;
    if (!newData)
      m_currentFrame.clear16bitBuffer();
    m_frameSize        = frame.getSize();
    m_frameNeedsUpdate = true;
    m_textureNeedsUpdate = true;
    update();
  }
}

void NativeDXGIRenderer::setZoom(double zoom)
{
  if (m_zoom != zoom)
  {
    m_zoom = zoom;
    m_frameNeedsUpdate = true;
    update();
  }
}

void NativeDXGIRenderer::setMoveOffset(QPointF offset)
{
  m_moveOffset = offset;
  m_frameNeedsUpdate = true;
}

bool NativeDXGIRenderer::isHDRActive() const
{
  if (m_swapChain)
    return m_swapChain->isHDRActive();
  return false;
}

void NativeDXGIRenderer::resizeEvent(QResizeEvent *event)
{
  QWidget::resizeEvent(event);
  if (m_swapChain)
  {
    int physW = static_cast<int>(event->size().width() * devicePixelRatio());
    int physH = static_cast<int>(event->size().height() * devicePixelRatio());
    m_swapChain->resize(physW, physH);
  }
}

void NativeDXGIRenderer::paintEvent(QPaintEvent *)
{
  if (!m_initialized && !m_initFailed)
  {
    initD3D();
    if (m_initialized)
    {
      if (!m_renderTimer->isActive())
        m_renderTimer->start(16);
      if (m_hdrPollTimer && !m_hdrPollTimer->isActive())
        m_hdrPollTimer->start(2000);
      updateHDRStatus();
    }
  }
  // Overlay is drawn via D3D texture blend in render(), not via QPainter
}

void NativeDXGIRenderer::showEvent(QShowEvent *event)
{
  QWidget::showEvent(event);
  if (!m_initialized && !m_initFailed)
  {
    initD3D();
    if (m_initialized)
      updateHDRStatus();
  }

  if (m_initialized && !m_renderTimer->isActive())
    m_renderTimer->start(16);
  if (m_initialized && m_hdrPollTimer && !m_hdrPollTimer->isActive())
    m_hdrPollTimer->start(2000);
}

void NativeDXGIRenderer::hideEvent(QHideEvent *event)
{
  QWidget::hideEvent(event);

  if (m_renderTimer->isActive())
    m_renderTimer->stop();
  if (m_hdrPollTimer && m_hdrPollTimer->isActive())
    m_hdrPollTimer->stop();
  m_frameNeedsUpdate = false;
}

void NativeDXGIRenderer::initD3D()
{
  HWND hwnd = reinterpret_cast<HWND>(winId());
  if (!hwnd)
  {
    qWarning() << "[NativeDXGIRenderer] No native window handle";
    m_initFailed = true;
    return;
  }

  m_swapChain = std::make_unique<DXGISwapChain>();
  int physW = static_cast<int>(width() * devicePixelRatio());
  int physH = static_cast<int>(height() * devicePixelRatio());
  if (!m_swapChain->initialize(hwnd, physW, physH))
  {
    qCritical() << "[NativeDXGIRenderer] Failed to initialize DXGI swap chain";
    m_initFailed = true;
    return;
  }

  auto *device = m_swapChain->device();

  // ── Compile shaders ──────────────────────────────────────────────

  ComPtr<ID3DBlob> vsBlob, psBlob, errorBlob;
  HRESULT hr;

  hr = D3DCompile(g_vertexShaderSrc, strlen(g_vertexShaderSrc),
    "vs", nullptr, nullptr, "main", "vs_4_0", 0, 0, &vsBlob, &errorBlob);
  if (FAILED(hr))
  {
    qCritical() << "[NativeDXGIRenderer] VS compile error:"
                << (char *)errorBlob->GetBufferPointer();
    m_initFailed = true;
    return;
  }
  device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
    nullptr, &m_vertexShader);

  hr = D3DCompile(g_pixelShaderSrc, strlen(g_pixelShaderSrc),
    "ps", nullptr, nullptr, "main", "ps_4_0", 0, 0, &psBlob, &errorBlob);
  if (FAILED(hr))
  {
    qCritical() << "[NativeDXGIRenderer] PS compile error:"
                << (char *)errorBlob->GetBufferPointer();
    m_initFailed = true;
    return;
  }
  device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
    nullptr, &m_pixelShader);

  // ── Compile overlay shader ───────────────────────────────────────

  hr = D3DCompile(g_overlayShaderSrc, strlen(g_overlayShaderSrc),
    "ps", nullptr, nullptr, "main", "ps_4_0", 0, 0, &psBlob, &errorBlob);
  if (FAILED(hr))
  {
    qCritical() << "[NativeDXGIRenderer] Overlay PS compile error:"
                << (char *)errorBlob->GetBufferPointer();
    m_initFailed = true;
    return;
  }
  device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
    nullptr, &m_overlayShader);

  // ── Overlay blend state (alpha blending) ─────────────────────────

  D3D11_BLEND_DESC blendDesc = {};
  blendDesc.RenderTarget[0].BlendEnable = TRUE;
  blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
  blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
  blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  device->CreateBlendState(&blendDesc, &m_overlayBlendState);

  // ── HDR pipeline blend state (premultiplied alpha) ──────────────
  // The shader premultiplies RGB by alpha in the encoding domain.
  // Blend: src * 1 + dst * (1 - srcAlpha), matching OpenGL and Metal paths.
  D3D11_BLEND_DESC hdrBlendDesc = {};
  hdrBlendDesc.RenderTarget[0].BlendEnable = TRUE;
  hdrBlendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
  hdrBlendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  hdrBlendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  hdrBlendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  hdrBlendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  hdrBlendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  hdrBlendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  device->CreateBlendState(&hdrBlendDesc, &m_hdrBlendState);

  // ── Input layout ─────────────────────────────────────────────────

  D3D11_INPUT_ELEMENT_DESC layout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  device->CreateInputLayout(layout, ARRAYSIZE(layout),
    vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &m_inputLayout);

  // ── Vertex buffer ────────────────────────────────────────────────

  D3D11_BUFFER_DESC vbDesc = {};
  vbDesc.Usage     = D3D11_USAGE_DEFAULT;
  vbDesc.ByteWidth = sizeof(g_quadVertices);
  vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  D3D11_SUBRESOURCE_DATA vbData = { g_quadVertices };
  device->CreateBuffer(&vbDesc, &vbData, &m_vertexBuffer);

  // ── Constant buffer (pixel shader) ───────────────────────────────

  D3D11_BUFFER_DESC cbDesc = {};
  cbDesc.Usage          = D3D11_USAGE_DYNAMIC;
  cbDesc.ByteWidth      = sizeof(ConstantBuffer);
  cbDesc.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
  cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  device->CreateBuffer(&cbDesc, nullptr, &m_constantBuffer);

  // ── View constant buffer (vertex shader, register b1) ────────────

  D3D11_BUFFER_DESC vcbDesc = {};
  vcbDesc.Usage          = D3D11_USAGE_DYNAMIC;
  vcbDesc.ByteWidth      = sizeof(ViewConstantBuffer);
  vcbDesc.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
  vcbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  device->CreateBuffer(&vcbDesc, nullptr, &m_viewConstantBuffer);

  // ── Sampler state ────────────────────────────────────────────────

  D3D11_SAMPLER_DESC samplerDesc = {};
  samplerDesc.Filter         = D3D11_FILTER_MIN_MAG_MIP_POINT;
  samplerDesc.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
  samplerDesc.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
  samplerDesc.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
  samplerDesc.MaxAnisotropy  = 1;
  samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
  samplerDesc.MinLOD         = 0;
  samplerDesc.MaxLOD         = D3D11_FLOAT32_MAX;
  device->CreateSamplerState(&samplerDesc, &m_samplerState);

  auto caps = m_swapChain->getCapabilities();
  m_rendererInfo = QString("DXGI HDR Renderer, D3D11, HDR %1, Max %2 nits")
    .arg(caps.hdrActive ? "Active" : "Inactive")
    .arg(caps.hdrActive ? caps.maxLuminance : 80.0f, 0, 'f', 0);

  m_initialized = true;
  qInfo() << "[NativeDXGIRenderer] initD3D complete:" << m_rendererInfo;
}

void NativeDXGIRenderer::updateTexture()
{
  if (!m_initialized || !m_swapChain)
  {
    qWarning() << "[NativeDXGIRenderer] updateTexture: not initialized";
    return;
  }

  auto *device = m_swapChain->device();
  auto *ctx    = m_swapChain->context();

  // Get 16-bit data from VideoFrame
  if (!m_currentFrame.getData16bit())
  {
    qWarning() << "[NativeDXGIRenderer] updateTexture: no 16-bit data, trying to generate from 8-bit";
    // No 16-bit data available, try to generate from 8-bit
    const_cast<VideoFrame &>(m_currentFrame).generate16bitBuffer();
    m_sourceBitDepth = 8;
  }
  else
  {
    m_sourceBitDepth = m_bitDepth;
  }

  const uint16_t *data = m_currentFrame.getData16bit();
  if (!data)
  {
    qWarning() << "[NativeDXGIRenderer] updateTexture: no data after generation";
    return;
  }

  int w = m_frameSize.width();
  int h = m_frameSize.height();
  if (w <= 0 || h <= 0)
  {
    qWarning() << "[NativeDXGIRenderer] updateTexture: invalid frame size" << w << "x" << h;
    return;
  }

  bool sizeChanged = (m_textureSize != m_frameSize);

  if (sizeChanged || !m_texture)
  {
    // Release old resources
    m_textureSRV.Reset();
    m_texture.Reset();

    // Create new texture: DXGI_FORMAT_R16G16B16A16_UNORM
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width          = w;
    texDesc.Height         = h;
    texDesc.MipLevels      = 1;
    texDesc.ArraySize      = 1;
    texDesc.Format         = DXGI_FORMAT_R16G16B16A16_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage          = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags      = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA texData = {};
    texData.pSysMem     = data;
    texData.SysMemPitch = w * 8;  // 4 channels × 2 bytes

    HRESULT hr = device->CreateTexture2D(&texDesc, &texData, &m_texture);
    if (FAILED(hr))
    {
      qWarning() << "[NativeDXGIRenderer] CreateTexture2D failed:" << Qt::hex << hr;
      return;
    }
    // Shader resource view
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format                    = DXGI_FORMAT_R16G16B16A16_UNORM;
    srvDesc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels       = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;

    hr = device->CreateShaderResourceView(m_texture.Get(), &srvDesc, &m_textureSRV);
    if (FAILED(hr))
    {
      qWarning() << "[NativeDXGIRenderer] CreateShaderResourceView failed:" << Qt::hex << hr;
      return;
    }

    m_textureSize = m_frameSize;
  }
  else
  {
    // Update existing texture
    ctx->UpdateSubresource(m_texture.Get(), 0, nullptr, data, w * 8, 0);
  }

  m_textureNeedsUpdate = false;
}

void NativeDXGIRenderer::render()
{
  if (!isVisible())
    return;

  if (!m_initialized || !m_swapChain)
  {
    qWarning() << "[NativeDXGIRenderer] render: not initialized";
    return;
  }

  // Update texture if needed
  if (m_textureNeedsUpdate)
    updateTexture();

  if (!m_textureSRV)
  {
    qWarning() << "[NativeDXGIRenderer] render: no texture SRV";
    return;
  }

  auto *ctx = m_swapChain->context();
  auto caps = m_swapChain->getCapabilities();

  // ── Update pixel shader constant buffer (register b0) ────────────

  ConstantBuffer cb{};
  cb.eotf                     = static_cast<int>(m_eotf);
  cb.gammaValue               = m_gammaValue;
  cb.diffuseWhiteNits         = m_diffuseWhiteNits;
  cb.hdrBrightness            = m_hdrBrightness;
  cb.sdrWhiteNits             = caps.sdrWhiteNits;
  cb.hdrActive                = caps.hdrActive ? 1.0f : 0.0f;
  // Reinhard tonemapping is needed only when:
  //   1. The system is NOT handling tonemapping (SDR without ACM), AND
  //   2. The content is HDR (PQ/HLG) — linear light may exceed 1.0 and needs compression.
  // SDR content (sRGB/Gamma) has linear values already in 0-1 range, so skip Reinhard.
  bool isHDREOTF = (m_eotf == video::RendererEOTF::PQ || m_eotf == video::RendererEOTF::HLG);
  cb.systemHandlesTonemapping = (caps.systemHandlesTonemapping || !isHDREOTF) ? 1.0f : 0.0f;
  cb.debugOutput              = m_debugOutput;
  cb.premultipliedAlpha       = m_premultipliedAlpha ? 1 : 0;

  // Gamut matrix: source → display (or source → BT.709 when system handles it).
  //   System handles (HDR or ACM): target = BT.709 — compositor maps to display
  //   System off: target = display's actual gamut — we do it ourselves
  float customMatrix[9];
  const float *gamutMat;
  if (caps.systemHandlesTonemapping)
  {
    gamutMat = color::getGamutMatrix(m_colorGamut, color::ColorGamut::BT709);
  }
  else
  {
    auto displayCS = functionsGui::getDisplayColorSpace();
    if (displayCS.isValid())
    {
      gamutMat = color::getGamutMatrixForDisplay(m_colorGamut, displayCS, customMatrix);
    }
    else
    {
      gamutMat = color::getGamutMatrix(m_colorGamut, color::ColorGamut::BT709);
    }
  }
  float packedMatrix[12];
  packGamutMatrixForHLSL(gamutMat, packedMatrix);
  memcpy(cb.gamutMatrixPacked, packedMatrix, 12 * sizeof(float));

  D3D11_MAPPED_SUBRESOURCE mapped;
  if (SUCCEEDED(ctx->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
  {
    memcpy(mapped.pData, &cb, sizeof(cb));
    ctx->Unmap(m_constantBuffer.Get(), 0);
  }

  // ── Update view constant buffer (register b1) ────────────────────
  // Compute zoom and pan in NDC space, matching OpenGL OpenGLRenderer behavior
  // m_zoom and m_moveOffset are in logical pixels from SplitViewWidget,
  // but swap chain is in physical pixels. Scale to match.

  float dpr = devicePixelRatio();
  float widgetW = static_cast<float>(m_swapChain->size().width());
  float widgetH = static_cast<float>(m_swapChain->size().height());
  float frameW  = static_cast<float>(m_frameSize.width());
  float frameH  = static_cast<float>(m_frameSize.height());

  // Calculate display size with zoom (scale zoom to physical pixels)
  float displayW = frameW * static_cast<float>(m_zoom) * dpr;
  float displayH = frameH * static_cast<float>(m_zoom) * dpr;

  // Convert moveOffset (logical pixels) to NDC in physical pixel space
  float ndcOffsetX = static_cast<float>(m_moveOffset.x() * dpr) / (widgetW * 0.5f);
  float ndcOffsetY = -static_cast<float>(m_moveOffset.y() * dpr) / (widgetH * 0.5f);

  // Zoom: displayW/widgetW gives NDC half-width ratio
  // In shader: pos / zoomScale, so zoomScale = widgetW/displayW
  // zoom=1.0 means fit-to-window (displayW = widgetW for landscape)
  float zoomScaleX = (displayW > 0) ? (widgetW / displayW) : 1.0f;
  float zoomScaleY = (displayH > 0) ? (widgetH / displayH) : 1.0f;

  ViewConstantBuffer vcb;
  vcb.panOffsetX = ndcOffsetX;
  vcb.panOffsetY = ndcOffsetY;
  vcb.zoomScaleX = zoomScaleX;
  vcb.zoomScaleY = zoomScaleY;

  if (SUCCEEDED(ctx->Map(m_viewConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
  {
    memcpy(mapped.pData, &vcb, sizeof(vcb));
    ctx->Unmap(m_viewConstantBuffer.Get(), 0);
  }

  // ── Clear to background color ────────────────────────────────────
  // Always clear, even when there's no texture (e.g. no file selected).
  // Background color is sRGB, convert to linear, then scale to scRGB absolute luminance.
  QSettings settings;
  QColor bgColor = settings.value("View/BackgroundColor", QColor(35, 35, 35)).value<QColor>();
  auto srgbToLinear = [](float c) -> float {
    return (c <= 0.04045f) ? (c / 12.92f) : powf((c + 0.055f) / 1.055f, 2.4f);
  };
  float bgScale = caps.sdrWhiteNits / 80.0f;
  float bgR = srgbToLinear((float)bgColor.redF()) * bgScale;
  float bgG = srgbToLinear((float)bgColor.greenF()) * bgScale;
  float bgB = srgbToLinear((float)bgColor.blueF()) * bgScale;
  m_swapChain->beginFrame(bgR, bgG, bgB);

  // Pipeline
  UINT stride = sizeof(Vertex);
  UINT offset = 0;
  ctx->IASetVertexBuffers(0, 1, m_vertexBuffer.GetAddressOf(), &stride, &offset);
  ctx->IASetInputLayout(m_inputLayout.Get());
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

  ctx->VSSetShader(m_vertexShader.Get(), nullptr, 0);
  ctx->VSSetConstantBuffers(1, 1, m_viewConstantBuffer.GetAddressOf());
  ctx->PSSetShader(m_pixelShader.Get(), nullptr, 0);
  ctx->PSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());
  ctx->PSSetShaderResources(0, 1, m_textureSRV.GetAddressOf());
  ctx->PSSetSamplers(0, 1, m_samplerState.GetAddressOf());

  auto *rtv = m_swapChain->rtv();
  ctx->OMSetRenderTargets(1, &rtv, nullptr);

  D3D11_VIEWPORT vp = {};
  vp.Width    = widgetW;
  vp.Height   = widgetH;
  vp.MinDepth = 0.0f;
  vp.MaxDepth = 1.0f;
  ctx->RSSetViewports(1, &vp);

  // Enable premultiplied alpha blending for HDR content
  FLOAT blendFactor[4] = {0, 0, 0, 0};
  ctx->OMSetBlendState(m_hdrBlendState.Get(), blendFactor, 0xFFFFFFFF);

  ctx->Draw(4, 0);

  // ── Draw overlay (pixel values, zoom, rulers) via texture blend ──
  drawOverlayTexture(ctx, static_cast<int>(widgetW), static_cast<int>(widgetH));

  m_swapChain->endFrame(true);
  m_frameCount++;

  m_frameNeedsUpdate = false;
}

// ========== Pixel value / zoom / ruler drawing (from NativeEDRRenderer) ==========

static const double SHOW_PIXEL_VALUES_ZOOM_THRESHOLD = 4.0;

void NativeDXGIRenderer::drawPixelValues(QPainter *painter)
{
  if (!m_showRawData || m_zoom < SHOW_PIXEL_VALUES_ZOOM_THRESHOLD)
    return;

  if (!m_currentFrame.isValid() || m_frameSize.isEmpty())
    return;

  int widgetW = width();
  int widgetH = height();
  int frameW = m_frameSize.width();
  int frameH = m_frameSize.height();

  double displayW = frameW * m_zoom;
  double displayH = frameH * m_zoom;

  double videoLeft = widgetW * 0.5 + m_moveOffset.x() - displayW * 0.5;
  double videoTop  = widgetH * 0.5 + m_moveOffset.y() - displayH * 0.5;

  int xMin = static_cast<int>(std::max(0.0, -videoLeft / m_zoom));
  int yMin = static_cast<int>(std::max(0.0, -videoTop / m_zoom));
  int xMax = static_cast<int>(std::min(static_cast<double>(frameW - 1), (widgetW - videoLeft) / m_zoom));
  int yMax = static_cast<int>(std::min(static_cast<double>(frameH - 1), (widgetH - videoTop) / m_zoom));

  if (xMin > xMax || yMin > yMax)
    return;

  QFont font = painter->font();
  font.setPointSize(10);
  painter->setFont(font);

  const uint16_t *frameData = m_currentFrame.getData16bit();
  if (!frameData)
    return;

  QSettings settings;
  const bool showHex = settings.value("ShowPixelValuesHex", false).toBool();
  const int maxDisplayVal = (1 << m_sourceBitDepth) - 1;
  const int maxVal16 = 65535;
  const int halfMax16 = maxVal16 / 2;

  if (m_frameHandler)
  {
    auto *yuvHandler = dynamic_cast<video::yuv::videoHandlerYUV *>(m_frameHandler);
    const bool isYUV = (yuvHandler != nullptr);

    int subsamplingX = 1;
    int subsamplingY = 1;
    int chromaOffsetFullX = 0;
    int chromaOffsetFullY = 0;
    bool chromaPresent = false;

    if (isYUV)
    {
      auto format = yuvHandler->getSrcPixelFormat();
      subsamplingX = format.getSubsamplingHor();
      subsamplingY = format.getSubsamplingVer();
      chromaOffsetFullX = format.getChromaOffset().x / 2;
      chromaOffsetFullY = format.getChromaOffset().y / 2;
      chromaPresent = (format.getSubsampling() != video::yuv::Subsampling::YUV_400);
    }

    const int rowStride = frameW * 4;

    for (int y = yMin; y <= yMax; ++y)
    {
      double pxTop = videoTop + y * m_zoom;
      int baseIdx = y * rowStride;

      for (int x = xMin; x <= xMax; ++x)
      {
        double pxLeft = videoLeft + x * m_zoom;
        QRect pixelRect(static_cast<int>(pxLeft), static_cast<int>(pxTop),
                        static_cast<int>(m_zoom), static_cast<int>(m_zoom));

        auto pixelValues = m_frameHandler->getPixelValues(QPoint(x, y), 0);
        if (pixelValues.isEmpty())
          continue;

        QStringList lines;
        for (const auto &pair : pixelValues)
        {
          QString label = pair.first;
          if (isYUV && chromaPresent && (label == "U" || label == "V"))
          {
            if ((x - chromaOffsetFullX) % subsamplingX != 0 ||
                (y - chromaOffsetFullY) % subsamplingY != 0)
              continue;
          }
          lines.append(label + pair.second);
        }

        int idx = baseIdx + x * 4;
        int r = frameData[idx];
        int g = frameData[idx + 1];
        int b = frameData[idx + 2];

        int brightness = (299 * r + 587 * g + 114 * b) / 1000;
        painter->setPen(brightness < halfMax16 ? Qt::white : Qt::black);

        QString text = lines.join("\n");
        painter->drawText(pixelRect, Qt::AlignCenter, text);
      }
    }
  }
  else
  {
    // Fallback: use 16-bit RGB buffer
    const int rowStride = frameW * 4;

    for (int y = yMin; y <= yMax; ++y)
    {
      double pxTop = videoTop + y * m_zoom;
      int baseIdx = y * rowStride;

      for (int x = xMin; x <= xMax; ++x)
      {
        double pxLeft = videoLeft + x * m_zoom;
        QRect pixelRect(static_cast<int>(pxLeft), static_cast<int>(pxTop),
                        static_cast<int>(m_zoom), static_cast<int>(m_zoom));

        int idx = baseIdx + x * 4;
        uint16_t r = frameData[idx];
        uint16_t g = frameData[idx + 1];
        uint16_t b = frameData[idx + 2];

        int rv = (r * maxDisplayVal) / maxVal16;
        int gv = (g * maxDisplayVal) / maxVal16;
        int bv = (b * maxDisplayVal) / maxVal16;

        int brightness = (299 * rv + 587 * gv + 114 * bv) / 1000;
        painter->setPen(brightness < (maxDisplayVal / 2) ? Qt::white : Qt::black);

        QString text;
        if (showHex)
          text = QString("R%1\nG%2\nB%3").arg(rv, 0, 16).arg(gv, 0, 16).arg(bv, 0, 16);
        else
          text = QString("R%1\nG%2\nB%3").arg(rv).arg(gv).arg(bv);

        painter->drawText(pixelRect, Qt::AlignCenter, text);
      }
    }
  }
}

void NativeDXGIRenderer::drawZoomIndicator(QPainter *painter)
{
  if (m_zoom == 1.0)
    return;

  QString zoomString = QString("x") + QString::number(m_zoom, 'g', (m_zoom < 0.5) ? 4 : 2);

  QFont font("helvetica", 24);
  painter->setRenderHint(QPainter::TextAntialiasing);
  painter->setPen(QColor(Qt::black));
  painter->setFont(font);

  QPoint pos(10, QFontMetrics(font).height());
  painter->drawText(pos, zoomString);
}

void NativeDXGIRenderer::drawPixelRulers(QPainter *painter)
{
  if (!m_frameHandler || m_zoom < 32.0)
    return;

  if (!m_currentFrame.isValid() || m_frameSize.isEmpty())
    return;

  QFont valueFont("helvetica", 10);
  painter->setFont(valueFont);

  int widgetW = width();
  int widgetH = height();
  int frameW = m_frameSize.width();
  int frameH = m_frameSize.height();

  double displayW = frameW * m_zoom;
  double displayH = frameH * m_zoom;
  double videoLeft = widgetW * 0.5 + m_moveOffset.x() - displayW * 0.5;
  double videoTop  = widgetH * 0.5 + m_moveOffset.y() - displayH * 0.5;

  int xMin = static_cast<int>(std::max(0.0, -videoLeft / m_zoom));
  int xMax = static_cast<int>(std::min(static_cast<double>(frameW - 1), (widgetW - videoLeft) / m_zoom));
  int yMin = static_cast<int>(std::max(0.0, -videoTop / m_zoom));
  int yMax = static_cast<int>(std::min(static_cast<double>(frameH - 1), (widgetH - videoTop) / m_zoom));

  // X pixel indicators (horizontal ruler on top edge)
  for (int x = xMin; x <= xMax; ++x)
  {
    int xPosOnScreen = static_cast<int>(videoLeft + x * m_zoom);

    painter->setPen(QPen(Qt::white));
    painter->drawLine(xPosOnScreen, 0, xPosOnScreen, 5);
    painter->setPen(QPen(Qt::black));
    painter->drawLine(xPosOnScreen + 1, 0, xPosOnScreen + 1, 5);

    if ((m_zoom >= 128 || x % 5 == 0) && x != frameW)
    {
      QString numberText = QString::number(x);
      QFontMetrics metrics(valueFont);
      QSize rectSize = metrics.size(0, numberText) + QSize(4, 0);
      QPoint rectPosTopLeft(xPosOnScreen + static_cast<int>(m_zoom / 2) - rectSize.width() / 2, 2);
      QRect textRect(rectPosTopLeft, rectSize);

      painter->fillRect(textRect, Qt::white);
      painter->setPen(QPen(Qt::black));
      painter->drawText(textRect, Qt::AlignCenter, numberText);
    }
  }

  // Y pixel indicators (vertical ruler on left edge)
  for (int y = yMin; y <= yMax; ++y)
  {
    int yPosOnScreen = static_cast<int>(videoTop + y * m_zoom);

    painter->setPen(QPen(Qt::white));
    painter->drawLine(0, yPosOnScreen, 5, yPosOnScreen);
    painter->setPen(QPen(Qt::black));
    painter->drawLine(0, yPosOnScreen + 1, 5, yPosOnScreen + 1);

    if ((m_zoom >= 128 || y % 5 == 0) && y != frameH)
    {
      QString numberText = QString::number(y);
      QFontMetrics metrics(valueFont);
      QSize rectSize = metrics.size(0, numberText) + QSize(4, 0);
      QPoint rectPosTopLeft(2, yPosOnScreen + static_cast<int>(m_zoom / 2) - rectSize.height() / 2);
      QRect textRect(rectPosTopLeft, rectSize);

      painter->fillRect(textRect, Qt::white);
      painter->setPen(QPen(Qt::black));
      painter->drawText(textRect, Qt::AlignCenter, numberText);
    }
  }
}

void NativeDXGIRenderer::drawOverlayTexture(ID3D11DeviceContext *ctx, int w, int h)
{
  // w, h are physical pixels (swap chain size)
  // drawPixelValues/drawZoomIndicator/drawPixelRulers use width()/height()
  // which are logical pixels. We scale the painter so they draw correctly.
  // m_zoom and m_moveOffset are in logical pixels (from SplitViewWidget),
  // and the overlay drawing functions expect logical pixels, so no DPR conversion needed.
  float dpr = (float)w / (float)qMax(1, width());

  QImage overlayImage(w, h, QImage::Format_RGBA8888);
  overlayImage.fill(Qt::transparent);

  {
    QPainter painter(&overlayImage);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.scale(dpr, dpr);
    drawPixelValues(&painter);
    drawZoomIndicator(&painter);
    drawPixelRulers(&painter);
  }

  // Check if overlay has any non-transparent pixels
  bool hasContent = false;
  for (int y = 0; y < h && !hasContent; y++)
  {
    const uchar *scanline = overlayImage.constScanLine(y);
    for (int x = 0; x < w; x++)
    {
      if (scanline[x * 4 + 3] != 0) { hasContent = true; break; }
    }
  }
  if (!hasContent)
    return;

  // Upload to D3D texture
  auto *device = m_swapChain->device();

  bool sizeChanged = (m_overlaySize != QSize(w, h));
  if (sizeChanged || !m_overlayTexture)
  {
    m_overlaySRV.Reset();
    m_overlayTexture.Reset();

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = w;
    texDesc.Height = h;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA texData = {};
    texData.pSysMem = overlayImage.constBits();
    texData.SysMemPitch = w * 4;

    HRESULT hr = device->CreateTexture2D(&texDesc, &texData, &m_overlayTexture);
    if (FAILED(hr)) return;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    hr = device->CreateShaderResourceView(m_overlayTexture.Get(), &srvDesc, &m_overlaySRV);
    if (FAILED(hr)) return;

    m_overlaySize = QSize(w, h);
  }
  else
  {
    ctx->UpdateSubresource(m_overlayTexture.Get(), 0, nullptr,
      overlayImage.constBits(), w * 4, 0);
  }

  // Draw overlay quad with alpha blending
  // Unbind view constant buffer so overlay uses full-screen [-1,1] coordinates
  float blendFactor[4] = {1, 1, 1, 1};
  ctx->OMSetBlendState(m_overlayBlendState.Get(), blendFactor, 0xFFFFFFFF);

  // Set identity view transform for overlay (full screen)
  ViewConstantBuffer identityVCB;
  identityVCB.panOffsetX = 0;
  identityVCB.panOffsetY = 0;
  identityVCB.zoomScaleX = 1.0f;
  identityVCB.zoomScaleY = 1.0f;
  D3D11_MAPPED_SUBRESOURCE mapped;
  if (SUCCEEDED(ctx->Map(m_viewConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
  {
    memcpy(mapped.pData, &identityVCB, sizeof(identityVCB));
    ctx->Unmap(m_viewConstantBuffer.Get(), 0);
  }
  ctx->VSSetConstantBuffers(1, 1, m_viewConstantBuffer.GetAddressOf());

  ctx->PSSetShader(m_overlayShader.Get(), nullptr, 0);
  ctx->PSSetShaderResources(0, 1, m_overlaySRV.GetAddressOf());
  ctx->PSSetSamplers(0, 1, m_samplerState.GetAddressOf());
  ID3D11Buffer *nullCB = nullptr;
  ctx->PSSetConstantBuffers(0, 1, &nullCB);

  ctx->Draw(4, 0);

  // Restore blend state
  ctx->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFF);
}

void NativeDXGIRenderer::updateHDRStatus()
{
  if (!m_swapChain)
    return;
  auto caps = m_swapChain->getCapabilities();

  // Only emit if something actually changed (avoid redundant signal storms)
  if (caps.hdrActive == m_lastHdrActive &&
      caps.systemHandlesTonemapping == m_lastSystemTonemapping)
    return;

  m_lastHdrActive = caps.hdrActive;
  m_lastSystemTonemapping = caps.systemHandlesTonemapping;

  qInfo() << "[NativeDXGIRenderer] HDR:" << caps.hdrActive
          << "ACM:" << caps.acmActive
          << "systemTonemap:" << caps.systemHandlesTonemapping
          << "maxNits:" << caps.maxLuminance
          << "sdrWhite:" << caps.sdrWhiteNits;

  emit rendererStatusChanged(caps.hdrActive, caps.systemHandlesTonemapping,
                        caps.maxLuminance, caps.sdrWhiteNits);

  // Force re-render with updated tonemapping state
  m_frameNeedsUpdate = true;
  update();
}

bool NativeDXGIRenderer::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
  if (eventType == "windows_generic_MSG" || eventType == "windows_dispatcher_MSG")
  {
    MSG *msg = reinterpret_cast<MSG *>(message);
    if (msg->message == WM_DISPLAYCHANGE)
    {
      // WM_DISPLAYCHANGE fires on resolution/bpp changes (monitor connect/disconnect,
      // display mode change). It does NOT fire on ACM toggle or HDR on/off —
      // those are handled by the 2-second poll timer.
      // On receipt, trigger an immediate refresh instead of waiting for the next poll.
      qInfo() << "[NativeDXGIRenderer] WM_DISPLAYCHANGE:"
              << "bpp=" << msg->wParam
              << "res=" << LOWORD(msg->lParam) << "x" << HIWORD(msg->lParam);
      QTimer::singleShot(100, this, [this]() {
        if (m_swapChain)
        {
          m_swapChain->refreshCapabilities();
          updateHDRStatus();
        }
      });
    }
  }
  return QWidget::nativeEvent(eventType, message, result);
}

} // namespace video

#endif // Q_OS_WIN
