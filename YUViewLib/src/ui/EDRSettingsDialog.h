/*  This file is part of YUView - The YUV player with advanced analytics toolset
 *   <https://github.com/IENT/YUView>
 *   Copyright (C) 2015  Institut für Nachrichtstechnik, RWTH Aachen University, GERMANY
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

#include <QDialog>
#include <ui/views/HDR10WidgetMacEDR.h>

#include "ui_edrSettingsDialog.h"

/**
 * @brief EDR 显示设置对话框
 *
 * 允许用户配置 macOS Metal EDR 渲染器的色彩处理参数：
 * - EOTF (电光转换函数): PQ / HLG / Gamma / sRGB
 * - Color Gamut (源色域): BT.2020 / BT.709 / P3
 * - Gamma 值 (仅当 EOTF=Gamma 时有效)
 * - Diffuse White 漫射白亮度 (nits)
 * - HDR Brightness 亮度倍率 (>1.0 触发 EDR)
 */
class EDRSettingsDialog : public QDialog
{
  Q_OBJECT

public:
  explicit EDRSettingsDialog(QWidget *parent = nullptr);

  // Get current values
  video::HDR10WidgetMacEDR::EOTF eotf() const;
  video::HDR10WidgetMacEDR::ColorGamut colorGamut() const;
  float gammaValue() const;
  float diffuseWhiteNits() const;
  float hdrBrightness() const;

  // Set EDR info label (called before exec)
  void setEDRInfo(bool supported, float maxEDR);

private slots:
  void onEOTFChanged(int index);

private:
  Ui::EDRSettingsDialog ui;
};

#endif // Q_OS_MAC
