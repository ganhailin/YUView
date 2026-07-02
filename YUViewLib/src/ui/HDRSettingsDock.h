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

#include <QWidget>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QLabel>

#include <ui/views/HDR10Widget.h>

// Forward declarations
class splitViewWidget;

namespace Ui
{
class HDRSettingsDock;
}

class HDRSettingsDock : public QWidget
{
  Q_OBJECT

public:
  explicit HDRSettingsDock(QWidget *parent = nullptr);
  ~HDRSettingsDock();

  void setSplitViewWidget(splitViewWidget *splitView);

  // Get current values
  video::HDR10_EOTF eotf() const;
  video::HDR10_ColorGamut colorGamut() const;
  float gammaValue() const;
  float diffuseWhiteNits() const;
  float hdrBrightness() const;
  bool ditheringEnabled() const;

  // Set HDR info label
  void setHDRInfo(bool hdrActive, bool systemHandlesTonemapping,
                  float maxNits, float sdrWhiteNits);

signals:
  void settingsChanged();

private slots:
  void onEOTFChanged(int index);
  void onRenderingModeChanged(int index);
  void onAnySettingChanged();

private:
  void applySettings();
  void loadSettings();
  void updateDitheringState();

  QComboBox      *m_comboRenderingMode{};
  QComboBox      *m_comboEOTF{};
  QComboBox      *m_comboGamut{};
  QDoubleSpinBox *m_spinGamma{};
  QDoubleSpinBox *m_spinDiffuseWhite{};
  QDoubleSpinBox *m_spinBrightness{};
  QCheckBox      *m_checkDithering{};
  QLabel         *m_labelHDRInfo{};
  QLabel         *m_labelGamma{};

  splitViewWidget *m_splitView{};
};
