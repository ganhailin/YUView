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

/**
 * @brief macOS EDR 检测辅助类
 *
 * Objective-C++ 实现文件 (MacEDRUtil.mm) 中的原生 macOS API 调用
 * 不能在纯 C++ 文件中使用。此头文件提供纯 C++ 接口来查询
 * macOS EDR 支持状态。
 *
 * 使用方式：
 *   auto info = MacEDRUtil::queryEDRSupport();
 *   if (info.edrSupported) {
 *     qDebug() << "EDR max value:" << info.maxEDRValue;
 *   }
 */
struct MacEDRInfo
{
  bool  edrSupported{false};
  float maxEDRValue{1.0f};
};

class MacEDRUtil
{
public:
  /**
   * @brief 查询当前主屏幕的 EDR 支持状态
   *
   * 检查 NSScreen.maximumPotentialExtendedDynamicRangeColorComponentValue
   * 如果值 > 1.0，表示当前显示器支持 EDR (HDR 高亮可超过 SDR 白色)
   *
   * @return MacEDRInfo 包含 edrSupported 和 maxEDRValue
   */
  static MacEDRInfo queryEDRSupport();

  /**
   * @brief 查询指定窗口所在屏幕的 EDR 支持状态
   *
   * @param windowHandle QWidget::winId() 返回的值
   * @return MacEDRInfo
   */
  static MacEDRInfo queryEDRSupportForWindow(void *windowHandle);
};

#endif // Q_OS_MAC