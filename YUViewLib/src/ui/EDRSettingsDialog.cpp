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

#include <QtGlobal>

#ifdef Q_OS_MAC

#include "EDRSettingsDialog.h"

#include <QSettings>

EDRSettingsDialog::EDRSettingsDialog(QWidget *parent)
    : QDialog(parent)
{
  ui.setupUi(this);

  // Load current values from QSettings
  QSettings settings;

  int eotfIndex = settings.value("View/EDR_EOTF", 3).toInt(); // default: sRGB
  ui.comboBoxEOTF->setCurrentIndex(eotfIndex);

  int gamutIndex = settings.value("View/EDR_ColorGamut", 1).toInt(); // default: BT.709
  ui.comboBoxGamut->setCurrentIndex(gamutIndex);

  double gamma = settings.value("View/EDR_Gamma", 2.2).toDouble();
  ui.doubleSpinBoxGamma->setValue(gamma);

  double diffuseWhite = settings.value("View/EDR_DiffuseWhite", 203.0).toDouble();
  ui.doubleSpinBoxDiffuseWhite->setValue(diffuseWhite);

  double brightness = settings.value("View/EDR_Brightness", 1.0).toDouble();
  ui.doubleSpinBoxBrightness->setValue(brightness);

  // Enable/disable Gamma input based on EOTF selection
  onEOTFChanged(eotfIndex);

  // Connect EOTF combo box change
  connect(ui.comboBoxEOTF, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &EDRSettingsDialog::onEOTFChanged);
}

void EDRSettingsDialog::onEOTFChanged(int index)
{
  // Gamma value is only applicable when EOTF is set to "Gamma" (index 2)
  bool gammaEnabled = (index == 2);
  ui.doubleSpinBoxGamma->setEnabled(gammaEnabled);
  ui.labelGamma->setEnabled(gammaEnabled);
}

video::HDR10WidgetMacEDR::EOTF EDRSettingsDialog::eotf() const
{
  return static_cast<video::HDR10WidgetMacEDR::EOTF>(ui.comboBoxEOTF->currentIndex());
}

video::HDR10WidgetMacEDR::ColorGamut EDRSettingsDialog::colorGamut() const
{
  return static_cast<video::HDR10WidgetMacEDR::ColorGamut>(ui.comboBoxGamut->currentIndex());
}

float EDRSettingsDialog::gammaValue() const
{
  return static_cast<float>(ui.doubleSpinBoxGamma->value());
}

float EDRSettingsDialog::diffuseWhiteNits() const
{
  return static_cast<float>(ui.doubleSpinBoxDiffuseWhite->value());
}

float EDRSettingsDialog::hdrBrightness() const
{
  return static_cast<float>(ui.doubleSpinBoxBrightness->value());
}

void EDRSettingsDialog::setEDRInfo(bool supported, float maxEDR)
{
  if (supported)
    ui.labelEDRInfo->setText(QString("EDR: Supported (max %1x)\n"
                                     "HDR Brightness > 1.0 will trigger EDR highlights.")
                                 .arg(maxEDR));
  else
    ui.labelEDRInfo->setText(QString("EDR: Not supported on this display.\n"
                                     "HDR Brightness has no effect without EDR capability."));
}

#endif // Q_OS_MAC
