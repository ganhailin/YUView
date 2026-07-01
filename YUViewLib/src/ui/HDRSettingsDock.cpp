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
 *   You should have received a copy of the GNU General Public License
 *   along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "HDRSettingsDock.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QLabel>
#include <QSettings>
#include <QVBoxLayout>

#include <ui/views/SplitViewWidget.h>

HDRSettingsDock::HDRSettingsDock(QWidget *parent)
  : QWidget(parent)
{
  auto *mainLayout = new QVBoxLayout(this);
  mainLayout->setContentsMargins(4, 4, 4, 4);
  mainLayout->setSpacing(4);

  // ── HDR Rendering section ────────────────────────────────────────

  auto *labelHDRSection = new QLabel("HDR Rendering");
  QFont boldFont = labelHDRSection->font();
  boldFont.setBold(true);
  labelHDRSection->setFont(boldFont);
  mainLayout->addWidget(labelHDRSection);

  m_checkHDR = new QCheckBox("Enable HDR 10-bit Rendering");
  m_checkHDR->setToolTip("Enable HDR rendering. On Windows, uses DXGI native HDR when available.");
  mainLayout->addWidget(m_checkHDR);

#ifdef Q_OS_WIN
  m_checkDXGI = new QCheckBox("DXGI (Native HDR)");
  m_checkDXGI->setToolTip("Use DXGI/D3D11 native HDR rendering. When disabled, falls back to OpenGL.");
  mainLayout->addWidget(m_checkDXGI);
#endif

#ifdef Q_OS_MAC
  m_checkEDR = new QCheckBox("EDR (Metal Renderer)");
  m_checkEDR->setToolTip("Use macOS Metal renderer for EDR display. When enabled, uses CAMetalLayer "
                         "with Extended Linear Display P3 color space for reliable HDR output.");
  mainLayout->addWidget(m_checkEDR);
#endif

  m_checkDithering = new QCheckBox("Dithering");
  m_checkDithering->setToolTip("Enable Bayer dithering for HDR rendering to reduce banding on SDR displays.");
  mainLayout->addWidget(m_checkDithering);

  m_labelHDRInfo = new QLabel("HDR: --");
  m_labelHDRInfo->setWordWrap(true);
  mainLayout->addWidget(m_labelHDRInfo);

  // Separator
  auto *line1 = new QFrame();
  line1->setFrameShape(QFrame::HLine);
  mainLayout->addWidget(line1);

  // ── Color Processing section ─────────────────────────────────────

  auto *labelColorSection = new QLabel("Color Processing");
  labelColorSection->setFont(boldFont);
  mainLayout->addWidget(labelColorSection);

  auto *gridLayout = new QGridLayout();
  gridLayout->setHorizontalSpacing(8);
  gridLayout->setVerticalSpacing(4);

  // EOTF
  auto *labelEOTF = new QLabel("EOTF:");
  labelEOTF->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  m_comboEOTF = new QComboBox();
  m_comboEOTF->addItem("PQ (ST.2084)");
  m_comboEOTF->addItem("HLG");
  m_comboEOTF->addItem("Gamma");
  m_comboEOTF->addItem("sRGB");
  m_comboEOTF->setToolTip("Electro-Optical Transfer Function");
  gridLayout->addWidget(labelEOTF, 0, 0);
  gridLayout->addWidget(m_comboEOTF, 0, 1);

  // Gamma
  m_labelGamma = new QLabel("Gamma:");
  m_labelGamma->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  m_spinGamma = new QDoubleSpinBox();
  m_spinGamma->setRange(1.0, 3.0);
  m_spinGamma->setSingleStep(0.1);
  m_spinGamma->setValue(2.2);
  m_spinGamma->setToolTip("Pure gamma exponent. Only used when EOTF is Gamma.");
  gridLayout->addWidget(m_labelGamma, 1, 0);
  gridLayout->addWidget(m_spinGamma, 1, 1);

  // Gamut
  auto *labelGamut = new QLabel("Gamut:");
  labelGamut->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  m_comboGamut = new QComboBox();
  m_comboGamut->addItem("BT.2020");
  m_comboGamut->addItem("BT.709");
  m_comboGamut->addItem("DCI-P3");
  m_comboGamut->setToolTip("Source color gamut");
  gridLayout->addWidget(labelGamut, 2, 0);
  gridLayout->addWidget(m_comboGamut, 2, 1);

  // Diffuse White
  auto *labelDiffuse = new QLabel("Diffuse White:");
  labelDiffuse->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  m_spinDiffuseWhite = new QDoubleSpinBox();
  m_spinDiffuseWhite->setRange(100.0, 10000.0);
  m_spinDiffuseWhite->setSingleStep(1.0);
  m_spinDiffuseWhite->setValue(203.0);
  m_spinDiffuseWhite->setSuffix(" nits");
  m_spinDiffuseWhite->setToolTip("Reference diffuse white level in nits");
  gridLayout->addWidget(labelDiffuse, 3, 0);
  gridLayout->addWidget(m_spinDiffuseWhite, 3, 1);

  // HDR Brightness
  auto *labelBrightness = new QLabel("Brightness:");
  labelBrightness->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  m_spinBrightness = new QDoubleSpinBox();
  m_spinBrightness->setRange(0.1, 16.0);
  m_spinBrightness->setSingleStep(0.1);
  m_spinBrightness->setDecimals(1);
  m_spinBrightness->setValue(1.0);
  m_spinBrightness->setSuffix("x");
  m_spinBrightness->setToolTip("Global HDR brightness scaling");
  gridLayout->addWidget(labelBrightness, 4, 0);
  gridLayout->addWidget(m_spinBrightness, 4, 1);

  mainLayout->addLayout(gridLayout);
  mainLayout->addStretch();

  // ── Load settings ────────────────────────────────────────────────

  loadSettings();

  // ── Connect signals ──────────────────────────────────────────────

  connect(m_comboEOTF, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &HDRSettingsDock::onEOTFChanged);
  connect(m_comboEOTF, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &HDRSettingsDock::onAnySettingChanged);
  connect(m_comboGamut, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &HDRSettingsDock::onAnySettingChanged);
  connect(m_spinGamma, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
          this, &HDRSettingsDock::onAnySettingChanged);
  connect(m_spinDiffuseWhite, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
          this, &HDRSettingsDock::onAnySettingChanged);
  connect(m_spinBrightness, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
          this, &HDRSettingsDock::onAnySettingChanged);
  connect(m_checkHDR, &QCheckBox::toggled, this, &HDRSettingsDock::onHDRToggled);
#ifdef Q_OS_WIN
  connect(m_checkDXGI, &QCheckBox::toggled, this, &HDRSettingsDock::onDXGIToggled);
#endif
#ifdef Q_OS_MAC
  connect(m_checkEDR, &QCheckBox::toggled, this, &HDRSettingsDock::onEDRToggled);
#endif
  connect(m_checkDithering, &QCheckBox::toggled, this, &HDRSettingsDock::onAnySettingChanged);

  onEOTFChanged(m_comboEOTF->currentIndex());
}

HDRSettingsDock::~HDRSettingsDock() = default;

void HDRSettingsDock::setSplitViewWidget(splitViewWidget *splitView)
{
  m_splitView = splitView;
}

void HDRSettingsDock::loadSettings()
{
  QSettings settings;
  m_comboEOTF->setCurrentIndex(settings.value("View/EDR_EOTF", 3).toInt());
  m_comboGamut->setCurrentIndex(settings.value("View/EDR_ColorGamut", 1).toInt());
  m_spinGamma->setValue(settings.value("View/EDR_Gamma", 2.2).toDouble());
  m_spinDiffuseWhite->setValue(settings.value("View/EDR_DiffuseWhite", 203.0).toDouble());
  m_spinBrightness->setValue(settings.value("View/EDR_Brightness", 1.0).toDouble());
  m_checkHDR->setChecked(settings.value("View/HDRRendering", false).toBool());
  m_checkDithering->setChecked(settings.value("View/HDRDithering", false).toBool());
#ifdef Q_OS_WIN
  m_checkDXGI->setChecked(settings.value("View/UseDXGIMode", true).toBool());
  m_checkDXGI->setEnabled(m_checkHDR->isChecked());
#endif
#ifdef Q_OS_MAC
  m_checkEDR->setChecked(settings.value("View/EDRMode", true).toBool());
  m_checkEDR->setEnabled(m_checkHDR->isChecked());
#endif
  updateDitheringState();
}

void HDRSettingsDock::applySettings()
{
  if (!m_splitView)
    return;

  QSettings settings;
  settings.setValue("View/EDR_EOTF", m_comboEOTF->currentIndex());
  settings.setValue("View/EDR_ColorGamut", m_comboGamut->currentIndex());
  settings.setValue("View/EDR_Gamma", m_spinGamma->value());
  settings.setValue("View/EDR_DiffuseWhite", m_spinDiffuseWhite->value());
  settings.setValue("View/EDR_Brightness", m_spinBrightness->value());
  settings.setValue("View/HDRRendering", m_checkHDR->isChecked());
  settings.setValue("View/HDRDithering", m_checkDithering->isChecked());
#ifdef Q_OS_WIN
  settings.setValue("View/UseDXGIMode", m_checkDXGI->isChecked());
#endif
#ifdef Q_OS_MAC
  settings.setValue("View/EDRMode", m_checkEDR->isChecked());
#endif

  // Trigger SplitViewWidget to reload all settings (HDR mode + color params)
  m_splitView->updateSettings();
}

void HDRSettingsDock::onEOTFChanged(int index)
{
  bool gammaEnabled = (index == 2);
  m_spinGamma->setEnabled(gammaEnabled);
  m_labelGamma->setEnabled(gammaEnabled);
}

void HDRSettingsDock::onHDRToggled(bool checked)
{
#ifdef Q_OS_WIN
  m_checkDXGI->setEnabled(checked);
#endif
#ifdef Q_OS_MAC
  m_checkEDR->setEnabled(checked);
#endif
  updateDitheringState();
  applySettings();
}

void HDRSettingsDock::onDXGIToggled(bool)
{
  updateDitheringState();
  applySettings();
}

void HDRSettingsDock::onEDRToggled(bool)
{
  updateDitheringState();
  applySettings();
}

void HDRSettingsDock::onAnySettingChanged()
{
  applySettings();
}

void HDRSettingsDock::setHDRInfo(bool hdrActive, bool systemHandlesTonemapping,
                                 float maxNits, float sdrWhiteNits)
{
  QString info;
  if (hdrActive)
    info = QString("HDR: Active | Max: %1 nits | SDR White: %2 nits")
               .arg(maxNits, 0, 'f', 0)
               .arg(sdrWhiteNits, 0, 'f', 0);
  else if (systemHandlesTonemapping)
  {
    if (sdrWhiteNits > 0.0f)
      info = QString("SDR + ACM | System color mgmt | SDR White: %1 nits")
                 .arg(sdrWhiteNits, 0, 'f', 0);
    else
      info = "SDR + ACM | System color mgmt";
  }
  else
    info = "SDR (no ACM) | App tonemapping";
  m_labelHDRInfo->setText(info);
}

video::HDR10_EOTF HDRSettingsDock::eotf() const
{
  return static_cast<video::HDR10_EOTF>(m_comboEOTF->currentIndex());
}

video::HDR10_ColorGamut HDRSettingsDock::colorGamut() const
{
  return static_cast<video::HDR10_ColorGamut>(m_comboGamut->currentIndex());
}

float HDRSettingsDock::gammaValue() const
{
  return static_cast<float>(m_spinGamma->value());
}

float HDRSettingsDock::diffuseWhiteNits() const
{
  return static_cast<float>(m_spinDiffuseWhite->value());
}

float HDRSettingsDock::hdrBrightness() const
{
  return static_cast<float>(m_spinBrightness->value());
}

bool HDRSettingsDock::ditheringEnabled() const
{
  return m_checkDithering->isChecked();
}

void HDRSettingsDock::updateDitheringState()
{
  bool hdrOn = m_checkHDR->isChecked();
  bool dxgiOn = false;
#ifdef Q_OS_WIN
  dxgiOn = m_checkDXGI && m_checkDXGI->isChecked();
#endif
  bool edrOn = false;
#ifdef Q_OS_MAC
  edrOn = m_checkEDR && m_checkEDR->isChecked();
#endif
  // Dithering is only available when HDR is enabled and DXGI/EDR mode is off
  // (DXGI uses FP16 scRGB which doesn't need dithering; EDR uses Metal's native HDR)
  bool ditheringAvailable = hdrOn && !dxgiOn && !edrOn;
  m_checkDithering->setEnabled(ditheringAvailable);
  if (!ditheringAvailable)
    m_checkDithering->setChecked(false);
}
