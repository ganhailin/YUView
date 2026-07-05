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

#include "RendererSettingsDock.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QLabel>
#include <QSettings>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include <ui/views/SplitViewWidget.h>

RendererSettingsDock::RendererSettingsDock(QWidget *parent)
  : QWidget(parent)
{
  auto *mainLayout = new QVBoxLayout(this);
  mainLayout->setContentsMargins(4, 4, 4, 4);
  mainLayout->setSpacing(4);

  // ── Renderer section ────────────────────────────────────────

  auto *labelRendererSection = new QLabel("Renderer");
  QFont boldFont = labelRendererSection->font();
  boldFont.setBold(true);
  labelRendererSection->setFont(boldFont);
  mainLayout->addWidget(labelRendererSection);

  // Rendering backend selection (combobox — no dependency between options)
  auto *labelRenderer = new QLabel("Renderer:");
  labelRenderer->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  m_comboRenderingMode = new QComboBox();
  m_comboRenderingMode->addItem("QPainter");                  // index 0 (Software, always)
  m_comboRenderingMode->addItem("OpenGL");                    // index 1 (always)
#ifdef Q_OS_WIN
  m_comboRenderingMode->addItem("NativeDXGI");                // index 2 (Windows)
#endif
#ifdef Q_OS_MAC
  m_comboRenderingMode->addItem("NativeEDR");                 // index 2 (macOS)
#endif

  // Disable the OpenGL option if OpenGL 3.3 Core is not supported.
  // The combobox index stays stable (OpenGL is always index 1) so saved
  // settings remain valid — the user just can't select it.
  {
    QSettings settings;
    bool gl33Supported = settings.value("System/GL33Supported", true).toBool();
    if (!gl33Supported)
    {
      auto *model = qobject_cast<QStandardItemModel *>(m_comboRenderingMode->model());
      if (model)
      {
        auto *item = model->item(1); // OpenGL
        if (item)
          item->setEnabled(false);
      }
      m_comboRenderingMode->setItemText(1, "OpenGL — not supported");
    }
  }

  m_comboRenderingMode->setToolTip("Select renderer backend.");
  mainLayout->addWidget(labelRenderer);
  mainLayout->addWidget(m_comboRenderingMode);

  m_checkDithering = new QCheckBox("Dithering");
  m_checkDithering->setToolTip("Enable Bayer dithering for renderer to reduce banding on SDR displays.");
  mainLayout->addWidget(m_checkDithering);

  m_labelRendererInfo = new QLabel("HDR: --");
  m_labelRendererInfo->setWordWrap(true);
  mainLayout->addWidget(m_labelRendererInfo);

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
          this, &RendererSettingsDock::onEOTFChanged);
  connect(m_comboEOTF, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &RendererSettingsDock::onAnySettingChanged);
  connect(m_comboGamut, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &RendererSettingsDock::onAnySettingChanged);
  connect(m_spinGamma, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
          this, &RendererSettingsDock::onAnySettingChanged);
  connect(m_spinDiffuseWhite, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
          this, &RendererSettingsDock::onAnySettingChanged);
  connect(m_spinBrightness, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
          this, &RendererSettingsDock::onAnySettingChanged);
  connect(m_comboRenderingMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &RendererSettingsDock::onRenderingModeChanged);
  connect(m_checkDithering, &QCheckBox::toggled, this, &RendererSettingsDock::onAnySettingChanged);

  onEOTFChanged(m_comboEOTF->currentIndex());
}

RendererSettingsDock::~RendererSettingsDock() = default;

void RendererSettingsDock::setSplitViewWidget(splitViewWidget *splitView)
{
  m_splitView = splitView;
}

void RendererSettingsDock::loadSettings()
{
  QSettings settings;
  m_comboEOTF->setCurrentIndex(settings.value("View/EDR_EOTF", 3).toInt());
  m_comboGamut->setCurrentIndex(settings.value("View/EDR_ColorGamut", 1).toInt());
  m_spinGamma->setValue(settings.value("View/EDR_Gamma", 2.2).toDouble());
  m_spinDiffuseWhite->setValue(settings.value("View/EDR_DiffuseWhite", 203.0).toDouble());
  m_spinBrightness->setValue(settings.value("View/EDR_Brightness", 1.0).toDouble());
  m_checkDithering->setChecked(settings.value("View/HDRDithering", false).toBool());

  // Map saved settings to combobox index.
  // Legacy settings used separate bools: View/HDRRendering + View/UseDXGIMode + View/EDRMode.
  // New setting: View/HDRRenderer (int combobox index).
  int rendererIdx = settings.value("View/HDRRenderer", -1).toInt();
  if (rendererIdx < 0)
  {
    // Migrate from legacy settings
    bool hdrEnabled = settings.value("View/HDRRendering", false).toBool();
    if (!hdrEnabled)
      rendererIdx = 0; // Disabled
#ifdef Q_OS_WIN
    else if (settings.value("View/UseDXGIMode", true).toBool())
      rendererIdx = 2; // DXGI
    else
      rendererIdx = 1; // OpenGL
#elif defined(Q_OS_MAC)
    else if (settings.value("View/EDRMode", true).toBool())
      rendererIdx = 2; // EDR
    else
      rendererIdx = 1; // OpenGL
#else
    else
      rendererIdx = 1; // OpenGL
#endif
  }

  // If the saved renderer is OpenGL(1) but OpenGL 3.3 is not supported,
  // fall back to Disabled.
  if (rendererIdx == 1 && !settings.value("System/GL33Supported", true).toBool())
    rendererIdx = 0;

  m_comboRenderingMode->setCurrentIndex(rendererIdx);
  updateDitheringState();
}

void RendererSettingsDock::applySettings()
{
  if (!m_splitView)
    return;

  QSettings settings;
  settings.setValue("View/EDR_EOTF", m_comboEOTF->currentIndex());
  settings.setValue("View/EDR_ColorGamut", m_comboGamut->currentIndex());
  settings.setValue("View/EDR_Gamma", m_spinGamma->value());
  settings.setValue("View/EDR_DiffuseWhite", m_spinDiffuseWhite->value());
  settings.setValue("View/EDR_Brightness", m_spinBrightness->value());
  settings.setValue("View/HDRRenderer", m_comboRenderingMode->currentIndex());
  settings.setValue("View/HDRDithering", m_checkDithering->isChecked());

  // Trigger SplitViewWidget to reload all settings (HDR mode + color params)
  m_splitView->updateSettings();
}

void RendererSettingsDock::onEOTFChanged(int index)
{
  bool gammaEnabled = (index == 2);
  m_spinGamma->setEnabled(gammaEnabled);
  m_labelGamma->setEnabled(gammaEnabled);
}

void RendererSettingsDock::onRenderingModeChanged(int)
{
  updateDitheringState();
  applySettings();
}

void RendererSettingsDock::onAnySettingChanged()
{
  applySettings();
}

void RendererSettingsDock::setHDRInfo(bool hdrActive, bool systemHandlesTonemapping,
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
  m_labelRendererInfo->setText(info);
}

video::RendererEOTF RendererSettingsDock::eotf() const
{
  return static_cast<video::RendererEOTF>(m_comboEOTF->currentIndex());
}

video::RendererColorGamut RendererSettingsDock::colorGamut() const
{
  return static_cast<video::RendererColorGamut>(m_comboGamut->currentIndex());
}

float RendererSettingsDock::gammaValue() const
{
  return static_cast<float>(m_spinGamma->value());
}

float RendererSettingsDock::diffuseWhiteNits() const
{
  return static_cast<float>(m_spinDiffuseWhite->value());
}

float RendererSettingsDock::hdrBrightness() const
{
  return static_cast<float>(m_spinBrightness->value());
}

bool RendererSettingsDock::ditheringEnabled() const
{
  return m_checkDithering->isChecked();
}

void RendererSettingsDock::updateDitheringState()
{
  // Dithering is only useful for the OpenGL path (8-bit FBO on macOS).
  // DXGI uses FP16 scRGB (no banding), EDR uses Metal's native HDR.
  // ComboBox index 1 = OpenGL.
  bool isOpenGL = (m_comboRenderingMode->currentIndex() == 1);
  m_checkDithering->setEnabled(isOpenGL);
  if (!isOpenGL)
    m_checkDithering->setChecked(false);
}
