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

#include "DXGISwapChain.h"

#ifdef Q_OS_WIN

#include <QDebug>

#include <windows.h>
#include <winuser.h>

// QDC_ONLY_ACTIVE_PATH is not always in older SDK headers
#ifndef QDC_ONLY_ACTIVE_PATH
#define QDC_ONLY_ACTIVE_PATH 0x00000002
#endif

DXGISwapChain::DXGISwapChain()  = default;
DXGISwapChain::~DXGISwapChain() = default;

bool DXGISwapChain::initialize(HWND hwnd, int width, int height)
{
  m_size = QSize(width, height);

  if (!createDevice())
  {
    qCritical() << "[DXGISwapChain] createDevice failed";
    return false;
  }

  if (!createSwapChain(hwnd, width, height))
  {
    qCritical() << "[DXGISwapChain] createSwapChain failed";
    return false;
  }

  if (!detectHDRCapabilities())
    qWarning() << "[DXGISwapChain] detectHDRCapabilities failed, assuming SDR";

  qInfo() << "[DXGISwapChain] initialized:" << width << "x" << height
          << "HDR active:" << m_caps.hdrActive
          << "Max luminance:" << m_caps.maxLuminance << "nits";

  return true;
}

void DXGISwapChain::resize(int width, int height)
{
  if (width == m_size.width() && height == m_size.height())
    return;

  m_size = QSize(width, height);
  m_rtv.Reset();

  if (m_swapChain1)
  {
    HRESULT hr = m_swapChain1->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr))
    {
      qWarning() << "[DXGISwapChain] resize - ResizeBuffers failed:" << Qt::hex << hr;
      return;
    }
  }

  ComPtr<ID3D11Texture2D> backBuffer;
  HRESULT hr = m_swapChain1->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
  if (SUCCEEDED(hr))
    m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_rtv);
  else
    qWarning() << "[DXGISwapChain] resize - GetBuffer failed:" << Qt::hex << hr;

  detectHDRCapabilities();
}

bool DXGISwapChain::isHDRActive() const
{
  return m_caps.hdrActive;
}

bool DXGISwapChain::isACMActive() const
{
  return m_caps.acmActive;
}

bool DXGISwapChain::systemHandlesTonemapping() const
{
  return m_caps.systemHandlesTonemapping;
}

void DXGISwapChain::refreshCapabilities()
{
  detectHDRCapabilities();
}

DXGISwapChain::HDRCapabilities DXGISwapChain::getCapabilities() const
{
  return m_caps;
}

void DXGISwapChain::beginFrame(float clearR, float clearG, float clearB, float clearA)
{
  float clearColor[4] = {clearR, clearG, clearB, clearA};
  m_context->ClearRenderTargetView(m_rtv.Get(), clearColor);
}

void DXGISwapChain::endFrame(bool vsync)
{
  if (m_swapChain1)
  {
    UINT syncInterval = vsync ? 1 : 0;
    UINT flags = 0;
    // DXGI_PRESENT_ALLOW_TEARING requires specific swap chain flags
    HRESULT hr = m_swapChain1->Present(syncInterval, flags);
    if (FAILED(hr))
    {
      static int presentErrorCount = 0;
      if (presentErrorCount < 5)
      {
        qWarning() << "[DXGISwapChain] Present failed:" << Qt::hex << hr;
        presentErrorCount++;
      }
    }
  }
}

// ─── Private helpers ────────────────────────────────────────────────

bool DXGISwapChain::createDevice()
{
  UINT createDeviceFlags = 0;
#ifdef _DEBUG
  createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

  D3D_FEATURE_LEVEL featureLevels[] = {
    D3D_FEATURE_LEVEL_11_1,
    D3D_FEATURE_LEVEL_11_0,
  };

  D3D_FEATURE_LEVEL featureLevel;
  HRESULT hr = D3D11CreateDevice(
    nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
    featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION,
    &m_device, &featureLevel, &m_context);

  if (FAILED(hr))
  {
    qCritical() << "DXGISwapChain: D3D11CreateDevice failed:" << Qt::hex << hr;
    return false;
  }

  qInfo() << "[DXGISwapChain] D3D11 device created, feature level:" << Qt::hex << featureLevel;
  return true;
}

bool DXGISwapChain::createSwapChain(HWND hwnd, int width, int height)
{
  ComPtr<IDXGIDevice> dxgiDevice;
  HRESULT hr = m_device.As(&dxgiDevice);
  if (FAILED(hr)) { qCritical() << "DXGISwapChain: Failed to get IDXGIDevice"; return false; }

  ComPtr<IDXGIAdapter> adapter;
  hr = dxgiDevice->GetAdapter(&adapter);
  if (FAILED(hr)) { qCritical() << "DXGISwapChain: Failed to get adapter"; return false; }

  hr = adapter->GetParent(IID_PPV_ARGS(&m_factory));
  if (FAILED(hr)) { qCritical() << "DXGISwapChain: Failed to get IDXGIFactory2"; return false; }

  DXGI_SWAP_CHAIN_DESC1 desc = {};
  desc.Width              = width;
  desc.Height             = height;
  desc.Format             = DXGI_FORMAT_R16G16B16A16_FLOAT;
  desc.Stereo             = FALSE;
  desc.SampleDesc.Count   = 1;
  desc.SampleDesc.Quality = 0;
  desc.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount        = 2;
  desc.Scaling            = DXGI_SCALING_STRETCH;
  desc.SwapEffect         = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  desc.AlphaMode          = DXGI_ALPHA_MODE_IGNORE;
  desc.Flags              = 0;

  hr = m_factory->CreateSwapChainForHwnd(m_device.Get(), hwnd, &desc, nullptr, nullptr, &m_swapChain1);
  if (FAILED(hr))
  {
    qCritical() << "[DXGISwapChain] CreateSwapChainForHwnd failed:" << Qt::hex << hr;
    return false;
  }
  hr = m_swapChain1.As(&m_swapChain3);
  if (SUCCEEDED(hr))
  {
    hr = m_swapChain3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
    if (FAILED(hr))
      qWarning() << "[DXGISwapChain] SetColorSpace1 failed:" << Qt::hex << hr;
    else
      qInfo() << "[DXGISwapChain] scRGB color space set (FP16, linear)";
  }
  else
  {
    qWarning() << "[DXGISwapChain] IDXGISwapChain3 not available, cannot set color space";
  }

  m_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

  ComPtr<ID3D11Texture2D> backBuffer;
  hr = m_swapChain1->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
  if (FAILED(hr)) { qCritical() << "DXGISwapChain: GetBuffer failed"; return false; }

  hr = m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_rtv);
  if (FAILED(hr)) { qCritical() << "[DXGISwapChain] CreateRenderTargetView failed:" << Qt::hex << hr; return false; }

  return true;
}

bool DXGISwapChain::detectHDRCapabilities()
{
  m_caps = HDRCapabilities{};

  ComPtr<IDXGIOutput> output;
  HRESULT hr = m_swapChain1->GetContainingOutput(&output);
  if (FAILED(hr))
  {
    // Fallback: enumerate outputs
    ComPtr<IDXGIDevice> dxgiDevice;
    hr = m_device.As(&dxgiDevice);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr)) return false;

    UINT i = 0;
    while (adapter->EnumOutputs(i, &output) != DXGI_ERROR_NOT_FOUND)
    {
      DXGI_OUTPUT_DESC outDesc;
      if (SUCCEEDED(output->GetDesc(&outDesc)))
      {
        m_caps.displaySize = QSize(
          outDesc.DesktopCoordinates.right - outDesc.DesktopCoordinates.left,
          outDesc.DesktopCoordinates.bottom - outDesc.DesktopCoordinates.top);
      }
      i++;
    }
    return false;
  }

  DXGI_OUTPUT_DESC outDesc;
  hr = output->GetDesc(&outDesc);
  if (SUCCEEDED(hr))
  {
    m_caps.displaySize = QSize(
      outDesc.DesktopCoordinates.right - outDesc.DesktopCoordinates.left,
      outDesc.DesktopCoordinates.bottom - outDesc.DesktopCoordinates.top);
  }

  hr = output.As(&m_output6);
  if (SUCCEEDED(hr))
  {
    DXGI_OUTPUT_DESC1 desc1;
    hr = m_output6->GetDesc1(&desc1);
    if (SUCCEEDED(hr))
    {
      m_caps.hdrSupported = true;
      m_caps.hdrActive = (desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
      m_caps.maxLuminance = desc1.MaxLuminance;
      m_caps.minLuminance = desc1.MinLuminance;
      m_caps.maxFullFrameLuminance = desc1.MaxFullFrameLuminance;
    }
    else
    {
      qWarning() << "[DXGISwapChain] GetDesc1 failed:" << Qt::hex << hr;
    }
  }
  else
  {
    qWarning() << "[DXGISwapChain] IDXGIOutput6 not available, HDR detection not supported";
  }

  // ── Detect ACM (Advanced Color Management) SDR mode ──
  // When ACM is active in SDR mode, Windows also performs automatic color
  // management and tonemapping (like the macOS compositor). We should NOT
  // apply our own Reinhard tonemapping in this case.
  //
  // Detection heuristic: In SDR mode (hdrActive == false), if IDXGIOutput6
  // reports MaxLuminance > 80 nits, ACM is likely active. Without ACM, SDR
  // mode typically reports MaxLuminance of 0 or 80.
  m_caps.acmActive = (!m_caps.hdrActive && m_caps.maxLuminance > 80.0f);

  // System handles tonemapping when HDR is active OR ACM SDR is active
  m_caps.systemHandlesTonemapping = m_caps.hdrActive || m_caps.acmActive;

  qInfo() << "[DXGISwapChain] HDR:" << m_caps.hdrActive
          << "ACM:" << m_caps.acmActive
          << "systemTonemap:" << m_caps.systemHandlesTonemapping
          << "maxLum:" << m_caps.maxLuminance;

  // ── Get Windows SDR white level (system-wide HDR brightness slider) ──
  // Uses DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL (type 11).
  // SDRWhiteLevel / 1000 * 80 = nits (e.g. 1000 = 80 nits, 2500 = 200 nits).
  UINT32 pathCount, modeCount;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATH, &pathCount, &modeCount) == ERROR_SUCCESS)
  {
    auto paths = std::make_unique<DISPLAYCONFIG_PATH_INFO[]>(pathCount);
    auto modes = std::make_unique<DISPLAYCONFIG_MODE_INFO[]>(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATH, &pathCount, paths.get(),
                           &modeCount, modes.get(), nullptr) == ERROR_SUCCESS)
    {
      DISPLAYCONFIG_SDR_WHITE_LEVEL sdrLevel = {};
      sdrLevel.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
      sdrLevel.header.size = sizeof(sdrLevel);
      sdrLevel.header.adapterId = paths[0].targetInfo.adapterId;
      sdrLevel.header.id = paths[0].targetInfo.id;

      if (DisplayConfigGetDeviceInfo(&sdrLevel.header) == ERROR_SUCCESS && sdrLevel.SDRWhiteLevel > 0)
      {
        float sdrNits = (sdrLevel.SDRWhiteLevel / 1000.0f) * 80.0f;
        m_caps.sdrWhiteNits = sdrNits;

      }
    }
  }

  return true;
}

#endif // Q_OS_WIN
