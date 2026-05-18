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

#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

// Windows SDK defines macros that conflict with Qt/enum names
#ifdef IN
#undef IN
#endif
#ifdef OUT
#undef OUT
#endif

#include <QSize>

using Microsoft::WRL::ComPtr;

/**
 * @brief 封装 DXGI swap chain 和 D3D11 设备，用于 HDR 渲染
 *
 * 创建 FP16 scRGB swap chain，支持 Windows HDR (Advanced Color)。
 * 参考 Microsoft 官方文档：
 * https://learn.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range
 */
class DXGISwapChain
{
public:
  DXGISwapChain();
  ~DXGISwapChain();

  bool initialize(HWND hwnd, int width, int height);
  void resize(int width, int height);

  bool isHDRActive() const;

  struct HDRCapabilities
  {
    bool   hdrSupported{false};
    bool   hdrActive{false};
    float  maxLuminance{0.0f};
    float  minLuminance{0.0f};
    float  maxFullFrameLuminance{0.0f};
    float  sdrWhiteNits{80.0f};  // Windows SDR white level in nits (default 80)
    QSize  displaySize;
  };
  HDRCapabilities getCapabilities() const;

  void beginFrame(float clearR = 0.0f, float clearG = 0.0f, float clearB = 0.0f, float clearA = 1.0f);
  void endFrame(bool vsync = true);

  ID3D11DeviceContext *context() const { return m_context.Get(); }
  ID3D11Device        *device() const { return m_device.Get(); }
  ID3D11RenderTargetView *rtv() const { return m_rtv.Get(); }
  QSize size() const { return m_size; }

private:
  bool createDevice();
  bool createSwapChain(HWND hwnd, int width, int height);
  bool detectHDRCapabilities();

  ComPtr<ID3D11Device>        m_device;
  ComPtr<ID3D11DeviceContext> m_context;
  ComPtr<IDXGISwapChain1>     m_swapChain1;
  ComPtr<IDXGISwapChain3>     m_swapChain3;
  ComPtr<IDXGIFactory2>       m_factory;
  ComPtr<IDXGIOutput6>        m_output6;
  ComPtr<ID3D11RenderTargetView> m_rtv;

  QSize m_size;
  HDRCapabilities m_caps;
};

#endif // Q_OS_WIN
