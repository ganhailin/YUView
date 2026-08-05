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
 *   file(s), but you are not obligated to do so. If you do not wish to do
 *   so, delete this exception statement from your version. If you delete
 *   this exception statement from all source files in the program, then
 *   also delete it here.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "FrameHandler.h"

#include <QDebug>
#include <QPainter>
#include <QSettings>
#include <QColorSpace>
#include <QSurfaceFormat>

#include <common/FunctionsGui.h>
#include <common/ColorPipeline.h>
#include <decoder/decoderTarga.h>
#include <playlistitem/playlistItem.h>

using namespace std::string_view_literals;

namespace video
{

// Activate this if you want to know when which buffer is loaded/converted to image and so on.
#define FRAMEHANDLER_DEBUG_LOADING 0
#if FRAMEHANDLER_DEBUG_LOADING && !NDEBUG
#define DEBUG_FRAME qDebug
#else
#define DEBUG_FRAME(fmt, ...) ((void)0)
#endif

class FrameHandler::frameSizePresetList
{
public:
  // Constructor. Fill the names and sizes lists
  frameSizePresetList();
  // Get all presets in a displayable format ("Name (xxx,yyy)")
  QStringList getFormattedNames() const;
  // Return the index of a certain size (0 (Custom Size) if not found)
  int findSize(const Size &size)
  {
    int idx = sizes.indexOf(size);
    return (idx == -1) ? 0 : idx;
  }
  // Get the size with the given index.
  Size getSize(int index) { return sizes[index]; }

private:
  QList<QString> names;
  QList<Size>    sizes;
};

FrameHandler::frameSizePresetList::frameSizePresetList()
{
  names << "Custom Size"
        << "QCIF"
        << "QVGA"
        << "WQVGA"
        << "CIF"
        << "VGA"
        << "WVGA"
        << "4CIF"
        << "ITU R.BT601"
        << "720i/p"
        << "1080i/p"
        << "4k"
        << "XGA"
        << "XGA+";
  sizes << Size(0, 0) << Size(176, 144) << Size(320, 240) << Size(416, 240) << Size(352, 288)
        << Size(640, 480) << Size(832, 480) << Size(704, 576) << Size(720, 576) << Size(1280, 720)
        << Size(1920, 1080) << Size(3840, 2160) << Size(1024, 768) << Size(1280, 960);
}

/* Get all the names of the preset frame sizes in the form "Name (xxx,yyy)" in a QStringList.
 * This can be used to directly fill the combo box.
 */
QStringList FrameHandler::frameSizePresetList::getFormattedNames() const
{
  QStringList presetList;
  presetList.append("Custom Size");

  for (int i = 1; i < names.count(); i++)
  {
    auto str = QString("%1 (%2,%3)").arg(names[i]).arg(sizes[i].width).arg(sizes[i].height);
    presetList.append(str);
  }

  return presetList;
}

FrameHandler::frameSizePresetList FrameHandler::presetFrameSizes;

FrameHandler::FrameHandler()
{
  // Load default source color config from settings (per-image default)
  QSettings s;
  m_sourceColorConfig.eotf = static_cast<color::EOTF>(
      s.value("Frame/SourceEOTF", static_cast<int>(color::EOTF::SRGB)).toInt());
  m_sourceColorConfig.sourceGamut = static_cast<color::ColorGamut>(
      s.value("Frame/SourceGamut", static_cast<int>(color::ColorGamut::BT709)).toInt());
  m_sourceColorConfig.gammaValue =
      s.value("Frame/SourceGamma", 2.2).toFloat();
}

QLayout *FrameHandler::createFrameHandlerControls(bool isSizeFixed)
{
  // Absolutely always only call this function once!
  assert(!ui.created());

  ui.setupUi();

  // Set default values
  ui.widthSpinBox->setMaximum(100000);
  ui.widthSpinBox->setValue(frameSize.width);
  ui.widthSpinBox->setEnabled(!isSizeFixed);
  ui.heightSpinBox->setMaximum(100000);
  ui.heightSpinBox->setValue(frameSize.height);
  ui.heightSpinBox->setEnabled(!isSizeFixed);
  ui.frameSizeComboBox->addItems(presetFrameSizes.getFormattedNames());
  int idx = presetFrameSizes.findSize(frameSize);
  ui.frameSizeComboBox->setCurrentIndex(idx);
  ui.frameSizeComboBox->setEnabled(!isSizeFixed);
  ui.fbcFormatComboBox->addItems(functions::toQStringList(FBCFormatMapper.getNames()));
  ui.fbcFormatComboBox->setCurrentIndex(int(FBCFormatMapper.indexOf(this->fbcFormat)));
  ui.fbcFormatComboBox->setEnabled(!isSizeFixed);

  // AFBC custom options
  ui.afbcCustomOptionsCheckBox->setChecked(this->afbcCustomOptions);
  ui.afbcYuvTfCheckBox->setChecked(this->afbcYuvTf);
  ui.afbcSplitModeCheckBox->setChecked(this->afbcSplitMode);
  ui.afbcYoffsetSpinBox->setValue(this->afbcYoffset);
  ui.afbcLayoutComboBox->addItems(functions::toQStringList(AFBCLayoutMapper.getNames()));
  ui.afbcLayoutComboBox->setCurrentIndex(int(AFBCLayoutMapper.indexOf(this->afbcLayout)));
  updateAfbcOptionWidgetsEnabled();

  // ── Source color controls ────────────────────────────────────
  ui.eotfComboBox->addItem("PQ (ST.2084)");   // 0
  ui.eotfComboBox->addItem("HLG");             // 1
  ui.eotfComboBox->addItem("Gamma");           // 2
  ui.eotfComboBox->addItem("sRGB");            // 3
  ui.eotfComboBox->setCurrentIndex(static_cast<int>(m_sourceColorConfig.eotf));

  ui.gamutComboBox->addItem("BT.2020");       // 0
  ui.gamutComboBox->addItem("BT.709");         // 1
  ui.gamutComboBox->addItem("DCI-P3");         // 2
  ui.gamutComboBox->setCurrentIndex(static_cast<int>(m_sourceColorConfig.sourceGamut));

  ui.gammaSpinBox->setRange(1.0, 3.0);
  ui.gammaSpinBox->setSingleStep(0.1);
  ui.gammaSpinBox->setValue(m_sourceColorConfig.gammaValue);
  updateColorControlsEnabled();

  // Connect all the change signals from the controls to "connectWidgetSignals()"
  connect(ui.widthSpinBox,
          QOverload<int>::of(&QSpinBox::valueChanged),
          this,
          &FrameHandler::slotVideoControlChanged);
  connect(ui.heightSpinBox,
          QOverload<int>::of(&QSpinBox::valueChanged),
          this,
          &FrameHandler::slotVideoControlChanged);
  connect(ui.frameSizeComboBox,
          QOverload<int>::of(&QComboBox::currentIndexChanged),
          this,
          &FrameHandler::slotVideoControlChanged);
  connect(ui.fbcFormatComboBox,
          QOverload<int>::of(&QComboBox::currentIndexChanged),
          this,
          &FrameHandler::slotVideoControlChanged);
  connect(ui.afbcCustomOptionsCheckBox,
          &QCheckBox::stateChanged,
          this,
          &FrameHandler::slotVideoControlChanged);
  connect(ui.afbcYuvTfCheckBox,
          &QCheckBox::stateChanged,
          this,
          &FrameHandler::slotVideoControlChanged);
  connect(ui.afbcSplitModeCheckBox,
          &QCheckBox::stateChanged,
          this,
          &FrameHandler::slotVideoControlChanged);
  connect(ui.afbcYoffsetSpinBox,
          QOverload<int>::of(&QSpinBox::valueChanged),
          this,
          &FrameHandler::slotVideoControlChanged);
  connect(ui.afbcLayoutComboBox,
          QOverload<int>::of(&QComboBox::currentIndexChanged),
          this,
          &FrameHandler::slotVideoControlChanged);

  // Source color control signals
  connect(ui.eotfComboBox,
          QOverload<int>::of(&QComboBox::currentIndexChanged),
          this,
          &FrameHandler::slotVideoControlChanged);
  connect(ui.gamutComboBox,
          QOverload<int>::of(&QComboBox::currentIndexChanged),
          this,
          &FrameHandler::slotVideoControlChanged);
  connect(ui.gammaSpinBox,
          QOverload<double>::of(&QDoubleSpinBox::valueChanged),
          this,
          &FrameHandler::slotVideoControlChanged);

  return ui.frameHandlerLayout;
}

void FrameHandler::setFrameSize(Size newSize)
{
  if (newSize != this->frameSize)
  {
    // Set the new size
    DEBUG_FRAME("FrameHandler::setFrameSize %dx%d", newSize.width, newSize.height);
    this->frameSize = newSize;
  }
}

bool FrameHandler::loadCurrentImageFromFile(const QString &filePath)
{
  auto extension = QFileInfo(filePath).suffix().toLower();
  if (extension == "tga" || extension == "icb" || extension == "vda" || extension == "vst")
  {
    auto image = dec::Targa::loadTgaFromFile(filePath.toStdString());
    if (!image)
      return false;

    // Convert byte array to QImage
    this->setFrameSize(Size(image->size.width, image->size.height));
    auto qFrameSize    = QSize(image->size.width, image->size.height);
    this->currentImage = QImage(qFrameSize, QImage::Format_ARGB32);
    for (unsigned y = 0; y < image->size.height; y++)
    {
      auto bits = this->currentImage.scanLine(y);
      for (unsigned x = 0; x < image->size.width; x++)
      {
        auto idx = y * image->size.width * 4 + x * 4;
        // Src is RGBA and output it BGRA
        bits[2] = image->data.at(idx);
        bits[1] = image->data.at(idx + 1);
        bits[0] = image->data.at(idx + 2);
        bits[3] = image->data.at(idx + 3);
        bits += 4;
      }
    }
  }
  else
  {
    // Load the image and return if loading was successful
    this->currentImage = QImage(filePath);
    auto qFrameSize    = currentImage.size();
    this->setFrameSize(Size(qFrameSize.width(), qFrameSize.height()));
  }

  return (!this->currentImage.isNull());
}

QString FrameHandler::getAfbcModeSuffix() const
{
  if (!this->afbcCustomOptions)
    return {};

  return QString("_%1_%2_%3_%4")
      .arg(this->afbcYuvTf ? "1" : "0")
      .arg(this->afbcSplitMode ? "1" : "0")
      .arg(this->afbcYoffset)
      .arg(int(this->afbcLayout));
}

void FrameHandler::savePlaylist(YUViewDomElement &element) const
{
  // Append the video handler properties
  element.appendProperiteChild("width", QString::number(this->frameSize.width));
  element.appendProperiteChild("height", QString::number(this->frameSize.height));
  element.appendProperiteChild("fbcFormat",
                               QString::fromStdString(std::string(FBCFormatMapper.getName(this->fbcFormat))));
  element.appendProperiteChild("afbcCustomOptions", QString::number(this->afbcCustomOptions ? 1 : 0));
  element.appendProperiteChild("afbcYuvTf", QString::number(this->afbcYuvTf ? 1 : 0));
  element.appendProperiteChild("afbcSplitMode", QString::number(this->afbcSplitMode ? 1 : 0));
  element.appendProperiteChild("afbcYoffset", QString::number(this->afbcYoffset));
  element.appendProperiteChild("afbcLayout",
                               QString::fromStdString(std::string(AFBCLayoutMapper.getName(this->afbcLayout))));
  // Source color configuration (per-image)
  element.appendProperiteChild("sourceColorConfig", m_sourceColorConfig.toString());
}

void FrameHandler::loadPlaylist(const YUViewDomElement &root)
{
  auto width  = unsigned(root.findChildValue("width").toInt());
  auto height = unsigned(root.findChildValue("height").toInt());
  this->setFrameSize(Size(width, height));

  auto fbcFormatName = root.findChildValue("fbcFormat");
  if (!fbcFormatName.isEmpty())
  {
    if (auto newFormat = FBCFormatMapper.getValue(fbcFormatName.toStdString()))
      this->fbcFormat = *newFormat;
    else if (fbcFormatName == "AFBC 32x8" || fbcFormatName == "AFBC 16x16")
      this->fbcFormat = FBCFormat::AFBC;
  }

  auto afbcCustomOptionsVal = root.findChildValue("afbcCustomOptions");
  if (!afbcCustomOptionsVal.isEmpty())
    this->afbcCustomOptions = (afbcCustomOptionsVal == "1");

  auto afbcYuvTfVal = root.findChildValue("afbcYuvTf");
  if (!afbcYuvTfVal.isEmpty())
    this->afbcYuvTf = (afbcYuvTfVal == "1");

  auto afbcSplitModeVal = root.findChildValue("afbcSplitMode");
  if (!afbcSplitModeVal.isEmpty())
    this->afbcSplitMode = (afbcSplitModeVal == "1");

  auto afbcYoffsetVal = root.findChildValue("afbcYoffset");
  if (!afbcYoffsetVal.isEmpty())
    this->afbcYoffset = functions::clip(afbcYoffsetVal.toInt(), 0, 15);

  auto afbcLayoutName = root.findChildValue("afbcLayout");
  if (!afbcLayoutName.isEmpty())
    if (auto newLayout = AFBCLayoutMapper.getValue(afbcLayoutName.toStdString()))
      this->afbcLayout = *newLayout;

  // Source color configuration (per-image) - backward compatible: defaults if absent
  auto sccStr = root.findChildValue("sourceColorConfig");
  if (!sccStr.isEmpty())
    m_sourceColorConfig.fromString(sccStr);
}

void FrameHandler::slotVideoControlChanged()
{
  if (checkFbcFormatChanged())
    return;

  if (checkAfbcOptionsChanged())
    return;

  if (checkSourceColorChanged())
    return;

  // Update the controls and get the new selected size
  auto newSize = getNewSizeFromControls();
  DEBUG_FRAME(
      "FrameHandler::slotVideoControlChanged new size %dx%d", newSize.width, newSize.height);

  if (newSize != frameSize && newSize.isValid())
  {
    // Set the new size and update the controls.
    this->setFrameSize(newSize);
    // The frame size changed. We need to redraw/re-cache.
    emit signalHandlerChanged(true, RECACHE_CLEAR);
  }
}

void FrameHandler::setSourceColorConfig(const color::SourceColorConfig &config)
{
  if (m_sourceColorConfig != config)
  {
    m_sourceColorConfig = config;
    // Update UI controls if they exist
    if (ui.created())
    {
      ui.eotfComboBox->setCurrentIndex(static_cast<int>(config.eotf));
      ui.gamutComboBox->setCurrentIndex(static_cast<int>(config.sourceGamut));
      ui.gammaSpinBox->setValue(config.gammaValue);
      updateColorControlsEnabled();
    }
    emit signalHandlerChanged(true, RECACHE_NONE);
  }
}

void FrameHandler::updateColorControlsEnabled()
{
  if (!ui.created())
    return;
  // Gamma spin box is only relevant when EOTF == Gamma
  bool gammaEnabled = (m_sourceColorConfig.eotf == color::EOTF::Gamma);
  ui.gammaSpinBox->setEnabled(gammaEnabled);
  ui.labelGamma->setEnabled(gammaEnabled);
}

void FrameHandler::disableSourceColorControls()
{
  if (!ui.created())
    return;
  ui.eotfComboBox->setEnabled(false);
  ui.gamutComboBox->setEnabled(false);
  ui.gammaSpinBox->setEnabled(false);
  ui.labelEOTF->setEnabled(false);
  ui.labelGamut->setEnabled(false);
  ui.labelGamma->setEnabled(false);
  ui.labelColorSection->setEnabled(false);
}

bool FrameHandler::checkSourceColorChanged()
{
  auto sender = QObject::sender();

  if (sender == ui.eotfComboBox)
  {
    auto newEotf = static_cast<color::EOTF>(ui.eotfComboBox->currentIndex());
    if (newEotf != m_sourceColorConfig.eotf)
    {
      m_sourceColorConfig.eotf = newEotf;
      updateColorControlsEnabled();
      return true;
    }
    return true;
  }

  if (sender == ui.gamutComboBox)
  {
    auto newGamut = static_cast<color::ColorGamut>(ui.gamutComboBox->currentIndex());
    if (newGamut != m_sourceColorConfig.sourceGamut)
    {
      m_sourceColorConfig.sourceGamut = newGamut;
      return true;
    }
    return true;
  }

  if (sender == ui.gammaSpinBox)
  {
    auto newGamma = static_cast<float>(ui.gammaSpinBox->value());
    if (newGamma != m_sourceColorConfig.gammaValue)
    {
      m_sourceColorConfig.gammaValue = newGamma;
      return true;
    }
    return true;
  }

  return false;
}

bool FrameHandler::checkFbcFormatChanged()
{
  auto sender = QObject::sender();
  if (sender == ui.fbcFormatComboBox)
  {
    auto newFormat = FBCFormatMapper.getValueAt(ui.fbcFormatComboBox->currentIndex());
    if (newFormat && *newFormat != this->fbcFormat)
    {
      this->fbcFormat = *newFormat;
      this->onFbcFormatChanged();
      updateAfbcOptionWidgetsEnabled();
      return true;
    }
    // Same format selected, nothing to do
    return true;
  }
  return false;
}

bool FrameHandler::checkAfbcOptionsChanged()
{
  auto sender = QObject::sender();
  bool changed = false;

  if (sender == ui.afbcCustomOptionsCheckBox)
  {
    bool newVal = ui.afbcCustomOptionsCheckBox->isChecked();
    if (newVal != this->afbcCustomOptions)
    {
      this->afbcCustomOptions = newVal;
      changed = true;
    }
    updateAfbcOptionWidgetsEnabled();
    if (changed)
      this->onFbcFormatChanged();
    return true;
  }

  // All sub-options only matter when Custom Options is enabled
  if (sender == ui.afbcYuvTfCheckBox)
  {
    bool newVal = ui.afbcYuvTfCheckBox->isChecked();
    if (newVal != this->afbcYuvTf)
    {
      this->afbcYuvTf = newVal;
      changed = true;
    }
  }
  else if (sender == ui.afbcSplitModeCheckBox)
  {
    bool newVal = ui.afbcSplitModeCheckBox->isChecked();
    if (newVal != this->afbcSplitMode)
    {
      this->afbcSplitMode = newVal;
      changed = true;
    }
  }
  else if (sender == ui.afbcYoffsetSpinBox)
  {
    int newVal = ui.afbcYoffsetSpinBox->value();
    if (newVal != this->afbcYoffset)
    {
      this->afbcYoffset = newVal;
      changed = true;
    }
  }
  else if (sender == ui.afbcLayoutComboBox)
  {
    auto newLayout = AFBCLayoutMapper.getValueAt(ui.afbcLayoutComboBox->currentIndex());
    if (newLayout && *newLayout != this->afbcLayout)
    {
      this->afbcLayout = *newLayout;
      changed = true;
    }
  }
  else
  {
    return false;
  }

  if (changed)
    this->onFbcFormatChanged();
  return true;
}

void FrameHandler::updateAfbcOptionWidgetsEnabled()
{
  bool afbcActive = (this->fbcFormat != FBCFormat::Raster);
  ui.afbcCustomOptionsCheckBox->setEnabled(afbcActive);
  bool customEnabled = afbcActive && this->afbcCustomOptions;
  ui.afbcYuvTfCheckBox->setEnabled(customEnabled);
  ui.afbcSplitModeCheckBox->setEnabled(customEnabled);
  ui.afbcYoffsetSpinBox->setEnabled(customEnabled);
  ui.afbcLayoutComboBox->setEnabled(customEnabled);
}

Size FrameHandler::getNewSizeFromControls()
{
  // The control that caused the slot to be called
  auto sender = QObject::sender();

  if (sender == ui.widthSpinBox || sender == ui.heightSpinBox)
  {
    auto newSize = Size(ui.widthSpinBox->value(), ui.heightSpinBox->value());
    if (newSize != frameSize)
    {
      // Set the comboBox index without causing another signal to be emitted.
      const QSignalBlocker blocker(ui.frameSizeComboBox);
      int                  idx = presetFrameSizes.findSize(newSize);
      ui.frameSizeComboBox->setCurrentIndex(idx);
    }
    return newSize;
  }
  else if (sender == ui.frameSizeComboBox)
  {
    auto newSize = presetFrameSizes.getSize(ui.frameSizeComboBox->currentIndex());

    // Set the width/height spin boxes without emitting another signal.
    const QSignalBlocker blocker1(ui.widthSpinBox);
    const QSignalBlocker blocker2(ui.heightSpinBox);
    ui.widthSpinBox->setValue(int(newSize.width));
    ui.heightSpinBox->setValue(int(newSize.height));
    return newSize;
  }
  return {};
}

void FrameHandler::drawFrame(QPainter *painter, double zoomFactor, bool drawRawValues)
{
  // Create the video QRect with the size of the sequence and center it.
  QRect videoRect;
  videoRect.setSize(QSize(frameSize.width * zoomFactor, frameSize.height * zoomFactor));
  videoRect.moveCenter(QPoint(0, 0));

  // Draw the current image (currentFrame)
  // If premultiplied alpha is enabled, the source data is already premultiplied
  // but may be tagged as non-premultiplied (Format_ARGB32). Re-tag without converting.
  //
  // Color management: tag the image as sRGB, then convert to the display's
  // color space (Display P3 on most Macs). This matches the EDR Metal path
  // which converts BT.709 → Display P3 in the shader.
  //
  // When the surface is already sRGB and the display is also sRGB, the
  // QImage's sRGB-encoded values are drawn directly — no conversion needed.
  QImage imgToDraw = currentImage;

  // Apply source color processing (EOTF -> gamut -> tonemap -> sRGB OETF)
  // for the Software/QPainter path. This mirrors what the OpenGL/DXGI/EDR
  // shaders do. Skip for the identity case (sRGB + BT.709) for performance.
  color::applyColorTransformToImage(imgToDraw, m_sourceColorConfig);

  const bool surfaceIsSRGB =
      (QSurfaceFormat::defaultFormat().colorSpace() == QColorSpace::SRgb);
  if (!surfaceIsSRGB)
  {
    if (!imgToDraw.colorSpace().isValid())
      imgToDraw.setColorSpace(QColorSpace::SRgb);
    QColorSpace displayCS = functionsGui::getDisplayColorSpace();
    if (displayCS.isValid() && displayCS != QColorSpace::SRgb)
      imgToDraw = imgToDraw.convertedToColorSpace(displayCS);
  }


  // Premultiplied alpha handling.
  // On Windows, platformImageFormat returns Format_ARGB32_Premultiplied, so the
  // data is already premultiplied at creation time — draw directly.
  // On macOS, platformImageFormat returns Format_ARGB32 (non-premultiplied).
  // When the premultiplied setting is enabled, we re-tag the format to
  // Format_ARGB32_Premultiplied WITHOUT converting (the data is assumed to be
  // already premultiplied by the source). Using convertedTo() would double-premultiply.
  QSettings settings;
  bool premul = settings.value("View/PremultipliedAlpha", true).toBool();
  auto src_format = premul ? QImage::Format_ARGB32_Premultiplied : QImage::Format_ARGB32;
  if (imgToDraw.hasAlphaChannel() && src_format != imgToDraw.format())
  {
    // Re-tag as premultiplied (or unpremultiplied) without copying/converting data.
    QImage premulImage(imgToDraw.bits(), imgToDraw.width(), imgToDraw.height(),
                       imgToDraw.bytesPerLine(), src_format);
    painter->drawImage(videoRect, premulImage);
  }
  else
  {
    painter->drawImage(videoRect, imgToDraw);
  }

  if (drawRawValues && zoomFactor >= SPLITVIEW_DRAW_VALUES_ZOOMFACTOR)
  {
    // Draw the pixel values onto the pixels
    drawPixelValues(painter, 0, videoRect, zoomFactor);
  }
}

VideoFrame FrameHandler::getCurrentFrameAsVideoFrame() const
{
  // Always rebuild from currentImage. QImage is implicitly shared so this is cheap
  // (no pixel data copy). The previous caching caused stale frames in the OpenGL
  // renderer because QImage::cacheKey() doesn't change when currentImage is
  // reassigned to a different cache entry that shares the same underlying data.
  currentVideoFrame = VideoFrame(currentImage);
  currentVideoFrame.setSourceColorConfig(getSourceColorConfig());
  return currentVideoFrame;
}

void FrameHandler::drawPixelValues(QPainter *painter,
                                   const int,
                                   const QRect  &videoRect,
                                   const double  zoomFactor,
                                   FrameHandler *item2,
                                   const bool    markDifference,
                                   const int)
{
  // Draw the pixel values onto the pixels

  // TODO: Does this also work for sequences with width/height non divisible by 2? Not sure about
  // that.

  // First determine which pixels from this item are actually visible, because we only have to draw
  // the pixel values of the pixels that are actually visible
  auto viewport       = painter->viewport();
  auto worldTransform = painter->worldTransform();

  int xMin = (videoRect.width() / 2 - worldTransform.dx()) / zoomFactor;
  int yMin = (videoRect.height() / 2 - worldTransform.dy()) / zoomFactor;
  int xMax = (videoRect.width() / 2 - (worldTransform.dx() - viewport.width())) / zoomFactor;
  int yMax = (videoRect.height() / 2 - (worldTransform.dy() - viewport.height())) / zoomFactor;

  // functions::clip the min/max visible pixel values to the size of the item (no pixels outside of
  // the item have to be labeled)
  xMin = functions::clip(xMin, 0, int(frameSize.width) - 1);
  yMin = functions::clip(yMin, 0, int(frameSize.height) - 1);
  xMax = functions::clip(xMax, 0, int(frameSize.width) - 1);
  yMax = functions::clip(yMax, 0, int(frameSize.height) - 1);

  // The center point of the pixel (0,0).
  auto centerPointZero = (QPoint(-(int(frameSize.width)), -(int(frameSize.height))) * zoomFactor +
                          QPoint(zoomFactor, zoomFactor)) /
                         2;
  // This QRect has the size of one pixel and is moved on top of each pixel to draw the text
  QRect pixelRect;
  pixelRect.setSize(QSize(zoomFactor, zoomFactor));
  for (int x = xMin; x <= xMax; x++)
  {
    for (int y = yMin; y <= yMax; y++)
    {
      // Calculate the center point of the pixel. (Each pixel is of size (zoomFactor,zoomFactor))
      // and move the pixelRect to that point.
      QPoint pixCenter = centerPointZero + QPoint(x * zoomFactor, y * zoomFactor);
      pixelRect.moveCenter(pixCenter);

      // Get the text to show
      bool      drawWhite = false;
      QRgb      pixVal;
      QString   valText;
      const int formatBase = settings.value("ShowPixelValuesHex").toBool() ? 16 : 10;
      if (item2 != nullptr)
      {
        auto pixel1 = getPixelVal(x, y);
        auto pixel2 = item2->getPixelVal(x, y);

        int dR = int(qRed(pixel1)) - int(qRed(pixel2));
        int dG = int(qGreen(pixel1)) - int(qGreen(pixel2));
        int dB = int(qBlue(pixel1)) - int(qBlue(pixel2));

        const QString RString = ((dR < 0) ? "-" : "") + QString::number(std::abs(dR), formatBase);
        const QString GString = ((dG < 0) ? "-" : "") + QString::number(std::abs(dG), formatBase);
        const QString BString = ((dB < 0) ? "-" : "") + QString::number(std::abs(dB), formatBase);

        if (markDifference)
          drawWhite = (dR == 0 && dG == 0 && dB == 0);
        else
        {
          int r     = functions::clip(128 + dR, 0, 255);
          int g     = functions::clip(128 + dG, 0, 255);
          int b     = functions::clip(128 + dB, 0, 255);
          pixVal    = qRgb(r, g, b);
          drawWhite = (qRed(pixVal) < 128 && qGreen(pixVal) < 128 && qBlue(pixVal) < 128);
        }
        valText = QString("R%1\nG%2\nB%3").arg(RString, GString, BString);
      }
      else
      {
        pixVal    = getPixelVal(x, y);
        drawWhite = (qRed(pixVal) < 128 && qGreen(pixVal) < 128 && qBlue(pixVal) < 128);
        valText   = QString("R%1\nG%2\nB%3")
                      .arg(qRed(pixVal), 0, formatBase)
                      .arg(qGreen(pixVal), 0, formatBase)
                      .arg(qBlue(pixVal), 0, formatBase);
      }

      painter->setPen(drawWhite ? Qt::white : Qt::black);
      painter->drawText(pixelRect, Qt::AlignCenter, valText);
    }
  }
}

QImage FrameHandler::calculateDifference(FrameHandler *item2,
                                         const int,
                                         const int,
                                         QList<InfoItem> &differenceInfoList,
                                         const int        amplificationFactor,
                                         const bool       markDifference)
{
  auto width  = std::min(frameSize.width, item2->frameSize.width);
  auto height = std::min(frameSize.height, item2->frameSize.height);

  QImage diffImg(width, height, functionsGui::platformImageFormat(false));

  // Also calculate the MSE while we're at it (R,G,B)
  int64_t mseAdd[3] = {0, 0, 0};

  for (unsigned y = 0; y < height; y++)
  {
    for (unsigned x = 0; x < width; x++)
    {
      auto pixel1 = getPixelVal(x, y);
      auto pixel2 = item2->getPixelVal(x, y);

      int dR = int(qRed(pixel1)) - int(qRed(pixel2));
      int dG = int(qGreen(pixel1)) - int(qGreen(pixel2));
      int dB = int(qBlue(pixel1)) - int(qBlue(pixel2));

      int r, g, b;
      if (markDifference)
      {
        r = (dR != 0) ? 255 : 0;
        g = (dG != 0) ? 255 : 0;
        b = (dB != 0) ? 255 : 0;
      }
      else if (amplificationFactor != 1)
      {
        r = functions::clip(128 + dR * amplificationFactor, 0, 255);
        g = functions::clip(128 + dG * amplificationFactor, 0, 255);
        b = functions::clip(128 + dB * amplificationFactor, 0, 255);
      }
      else
      {
        r = functions::clip(128 + dR, 0, 255);
        g = functions::clip(128 + dG, 0, 255);
        b = functions::clip(128 + dB, 0, 255);
      }

      mseAdd[0] += dR * dR;
      mseAdd[1] += dG * dG;
      mseAdd[2] += dB * dB;

      auto val = qRgb(r, g, b);
      diffImg.setPixel(x, y, val);
    }
  }

  differenceInfoList.append(InfoItem("Difference Type"sv, "RGB"));

  double mse[4];
  mse[0] = double(mseAdd[0]) / (width * height);
  mse[1] = double(mseAdd[1]) / (width * height);
  mse[2] = double(mseAdd[2]) / (width * height);
  mse[3] = mse[0] + mse[1] + mse[2];
  differenceInfoList.append(InfoItem("MSE R", std::to_string(mse[0])));
  differenceInfoList.append(InfoItem("MSE G", std::to_string(mse[1])));
  differenceInfoList.append(InfoItem("MSE B", std::to_string(mse[2])));
  differenceInfoList.append(InfoItem("MSE All", std::to_string(mse[3])));

  return diffImg;
}

bool FrameHandler::isPixelDark(const QPoint &pixelPos)
{
  auto pixVal = getPixelVal(pixelPos);
  return (qRed(pixVal) < 128 && qGreen(pixVal) < 128 && qBlue(pixVal) < 128);
}

QStringPairList
FrameHandler::getPixelValues(const QPoint &pixelPos, int, FrameHandler *item2, const int)
{
  auto width  = (item2) ? std::min(frameSize.width, item2->frameSize.width) : frameSize.width;
  auto height = (item2) ? std::min(frameSize.height, item2->frameSize.height) : frameSize.height;

  if (pixelPos.x() < 0 || pixelPos.x() >= int(width) || pixelPos.y() < 0 ||
      pixelPos.y() >= int(height))
    return {};

  // Is the format (of both items) valid?
  if (!isFormatValid())
    return {};
  if (item2 && !item2->isFormatValid())
    return {};

  // Get the RGB values from the image
  QStringPairList values;

  if (item2)
  {
    // There is a second item. Return the difference values.
    auto pixel1 = getPixelVal(pixelPos);
    auto pixel2 = item2->getPixelVal(pixelPos);

    int r = int(qRed(pixel1)) - int(qRed(pixel2));
    int g = int(qGreen(pixel1)) - int(qGreen(pixel2));
    int b = int(qBlue(pixel1)) - int(qBlue(pixel2));

    values.append(QStringPair("R", QString::number(r)));
    values.append(QStringPair("G", QString::number(g)));
    values.append(QStringPair("B", QString::number(b)));
  }
  else
  {
    // No second item. Return the RGB values of this item.
    auto val = getPixelVal(pixelPos);
    values.append(QStringPair("R", QString::number(qRed(val))));
    values.append(QStringPair("G", QString::number(qGreen(val))));
    values.append(QStringPair("B", QString::number(qBlue(val))));
  }

  return values;
}

bool FrameHandler::setFormatFromString(QString format)
{
  auto split = format.split(";");
  if (split.length() != 2)
    return false;

  bool ok;
  auto newWidth = unsigned(split[0].toInt(&ok));
  if (!ok)
    return false;

  auto newHeight = unsigned(split[1].toInt(&ok));
  if (!ok)
    return false;

  this->setFrameSize(Size(newWidth, newHeight));
  return true;
}

} // namespace video
