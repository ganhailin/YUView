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

// Must include QtGlobal first to get Q_OS_MAC defined before the ifdef check
#include <QtGlobal>

#ifdef Q_OS_MAC

#include "NativeEDRRenderer.h"
#include "MacEDRRenderer.h"

#include <video/FrameHandler.h>
#include <video/yuv/videoHandlerYUV.h>

#include <QDebug>
#include <QPainter>
#include <QSettings>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QHoverEvent>
#include <QCoreApplication>
#include <QWindow>

namespace video
{

// Threshold for showing pixel values (same as OpenGLRenderer)
static const double SHOW_PIXEL_VALUES_ZOOM_THRESHOLD = 4.0;

// ========== PixelOverlayWidget ==========

// ========== NativeEDRRenderer ==========

NativeEDRRenderer::NativeEDRRenderer(QWidget *parent) : QWidget(parent)
{
  // NOTE: Do NOT use WA_PaintOnScreen here. When this widget is a child of
  // SplitViewWidget (which has its own backing store and paintEvent), WA_PaintOnScreen
  // causes "QWidget::paintEngine: Should no longer be called" warnings and can cause
  // Qt's backing store composition to fail, resulting in a white screen.
  // QtHDRDemo uses WA_PaintOnScreen because HDRWidget is a direct child of MainWindow,
  // but our widget hierarchy is different.
  //
  // Instead, we use WA_TranslucentBackground so the Metal rendering from the
  // embedded QWindow container shows through this widget's backing store.
  setAttribute(Qt::WA_NativeWindow);      // Needed for embedding QWindow
  setAttribute(Qt::WA_TranslucentBackground); // Let Metal rendering show through
  setAttribute(Qt::WA_NoSystemBackground);
  setAutoFillBackground(false);

  // Create layout
  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  // Create QWindow as Metal Surface container
  m_containerWindow = new QWindow();
  m_containerWindow->setSurfaceType(QWindow::MetalSurface);

  // Install event filter on the QWindow to forward mouse/wheel events
  // to the parent SplitViewWidget for zoom/pan/drag interaction.
  // WA_TransparentForMouseEvents on the container widget alone is not enough
  // because the Metal QWindow's native NSView intercepts events at macOS level.
  m_containerWindow->installEventFilter(this);

  // Embed QWindow into QWidget
  m_containerWidget = QWidget::createWindowContainer(m_containerWindow, this);
  m_containerWidget->setAttribute(Qt::WA_TransparentForMouseEvents);
  m_containerWidget->setFocusPolicy(Qt::NoFocus);
  layout->addWidget(m_containerWidget);

  // NOTE: Pixel overlay is handled via a native CALayer above the Metal layer
  // (see MacEDRRenderer::initialize). We cannot use a Qt Widget with
  // WA_NativeWindow as overlay because extra NSViews interfere with the
  // Metal QWindow's KVO chain, causing "method signature argument cannot be nil"
  // crashes. The overlay image is drawn via QPainter→QImage→CGImage→CALayer.
}

NativeEDRRenderer::~NativeEDRRenderer()
{
  m_renderer.reset();
}

bool NativeEDRRenderer::initializeRenderer()
{
  if (m_initialized)
    return true;

  if (!m_containerWindow)
  {
    qWarning() << "NativeEDRRenderer: container window not created";
    return false;
  }

  // Create Metal renderer
  m_renderer = std::make_unique<MacEDRRenderer>();

  if (!m_renderer->initialize(m_containerWindow))
  {
    qWarning() << "NativeEDRRenderer: failed to initialize Metal renderer";
    m_renderer.reset();
    return false;
  }

  m_initialized = true;
  m_edrSupported = m_renderer->isEDRSupported();
  m_maxEDRValue = m_renderer->getMaxEDRValue();

  // Sync initial zoom and moveOffset to renderer
  m_renderer->setZoom(m_zoom);
  m_renderer->setMoveOffset(m_moveOffset);

  // Apply EDR color processing settings that were loaded from QSettings
  // (these may have been set before the renderer was created)
  m_renderer->setEOTF(m_eotf);
  m_renderer->setColorGamut(m_colorGamut);
  m_renderer->setGammaValue(m_gammaValue);
  m_renderer->setDiffuseWhite(m_diffuseWhiteNits);
  m_renderer->setHDRBrightness(m_hdrBrightness);
  m_renderer->setPremultipliedAlpha(m_premultipliedAlpha);

  // Build renderer info string
  m_rendererInfo = QString("Metal EDR Renderer, EDR: %1, Max EDR: %2x")
                       .arg(m_edrSupported ? "supported" : "not supported")
                       .arg(m_maxEDRValue);

  qInfo() << "NativeEDRRenderer:" << m_rendererInfo;

  return true;
}

void NativeEDRRenderer::setFrame(const VideoFrame &frame)
{
  const uint16_t *newData = frame.getData16bit();
  const uint16_t *oldData =
      m_currentFrame.is16bitGenerateFrom8bit() ? nullptr : m_currentFrame.getData16bit();
  auto new_empty = frame.getSize().isEmpty();
  auto old_empty = m_frameSize.isEmpty();
  std::shared_ptr<QImage> newImage8 = frame.getImage8bit();
  std::shared_ptr<QImage> oldImage8 = m_currentFrame.getImage8bit();
  if (newData != oldData || (new_empty != old_empty && (frame.getSize() != m_frameSize)) ||
      newImage8 != oldImage8)
  {
    m_currentFrame = frame;
    if (!newData)
      m_currentFrame.clear16bitBuffer();
    m_frameSize = frame.getSize();
    m_frameNeedsUpdate = true;

    // Render immediately if initialized
    if (m_initialized && m_renderer)
    {
      m_renderer->loadFrame(m_currentFrame);
      m_renderer->render();
    }

    // Update pixel overlay (pixel values changed with new frame)
    updatePixelOverlay();
  }
}

void NativeEDRRenderer::setFrameHandler(FrameHandler *handler)
{
  m_frameHandler = handler;
}

void NativeEDRRenderer::setBitDepth(int bits)
{
  m_bitDepth = bits;
}

void NativeEDRRenderer::setDithering(bool enable)
{
  m_ditheringEnabled = enable;
  // Dithering is handled in shader - no immediate effect in EDR mode
  // (EDR displays can show the full range without dithering)
}

void NativeEDRRenderer::setShowRawData(bool show)
{
  if (m_showRawData == show) return;
  m_showRawData = show;
  updatePixelOverlay();
}

void NativeEDRRenderer::setZoom(double zoom)
{
  m_zoom = zoom;
  if (m_renderer)
    m_renderer->setZoom(zoom);
  updatePixelOverlay();
}

void NativeEDRRenderer::setMoveOffset(QPointF offset)
{
  m_moveOffset = offset;
  if (m_renderer)
    m_renderer->setMoveOffset(offset);
  updatePixelOverlay();
}

void NativeEDRRenderer::setEOTF(color::EOTF eotf)
{
  m_eotf = eotf;
  if (m_renderer)
  {
    m_renderer->setEOTF(eotf);
    m_renderer->render();
  }
}

void NativeEDRRenderer::setColorGamut(color::ColorGamut gamut)
{
  m_colorGamut = gamut;
  if (m_renderer)
  {
    m_renderer->setColorGamut(gamut);
    m_renderer->render();
  }
}

void NativeEDRRenderer::setGammaValue(float gamma)
{
  m_gammaValue = gamma;
  if (m_renderer)
  {
    m_renderer->setGammaValue(gamma);
    m_renderer->render();
  }
}

void NativeEDRRenderer::setDiffuseWhiteNits(float nits)
{
  m_diffuseWhiteNits = nits;
  if (m_renderer)
  {
    m_renderer->setDiffuseWhite(nits);
    m_renderer->render();
  }
}

void NativeEDRRenderer::setHDRBrightness(float brightness)
{
  m_hdrBrightness = brightness;
  if (m_renderer)
  {
    m_renderer->setHDRBrightness(brightness);
    m_renderer->render();
  }
}

void NativeEDRRenderer::setPremultipliedAlpha(bool enabled)
{
  m_premultipliedAlpha = enabled;
  if (m_renderer)
  {
    m_renderer->setPremultipliedAlpha(enabled);
    m_renderer->render();
  }
}

bool NativeEDRRenderer::isEDRSupported() const
{
  return m_edrSupported;
}

float NativeEDRRenderer::getMaxEDRValue() const
{
  return m_maxEDRValue;
}

QString NativeEDRRenderer::getRendererInfo() const
{
  return m_rendererInfo;
}

void NativeEDRRenderer::updatePixelOverlay()
{
  // Render pixel values/zoom/rulers into a QImage, then set it as the
  // overlay CALayer's contents (above the Metal layer in the NSView hierarchy).
  // This avoids creating extra NSViews that would interfere with KVO.
  if (!m_renderer || !m_initialized)
    return;

  int w = width();
  int h = height();
  if (w <= 0 || h <= 0)
    return;

  QImage overlayImage(w, h, QImage::Format_RGBA8888);
  overlayImage.fill(Qt::transparent);

  {
    QPainter painter(&overlayImage);
    drawPixelValues(&painter);
    drawZoomIndicator(&painter);
    drawPixelRulers(&painter);
  }

  m_renderer->setOverlayImage(overlayImage);
}

void NativeEDRRenderer::resizeEvent(QResizeEvent *event)
{
  QWidget::resizeEvent(event);

  // Resize Metal renderer
  if (m_renderer)
  {
    m_renderer->resize(width(), height());
    m_renderer->render();
  }

  // Update overlay after resize
  updatePixelOverlay();
}

void NativeEDRRenderer::paintEvent(QPaintEvent *event)
{
  Q_UNUSED(event)

  // Initialize renderer on first paint if needed
  if (!m_initialized)
    initializeRenderer();

  // Trigger Metal render (same as QtHDRDemo's HDRWidget::paintEvent → doRender())
  if (m_renderer)
    m_renderer->render();
}

void NativeEDRRenderer::showEvent(QShowEvent *event)
{
  QWidget::showEvent(event);

  // Initialize and render on first show
  if (!m_initialized)
    initializeRenderer();

  // Trigger initial render
  if (m_renderer)
    m_renderer->render();
}

bool NativeEDRRenderer::eventFilter(QObject *watched, QEvent *event)
{
  // Forward mouse and wheel events from the Metal QWindow to the parent
  // SplitViewWidget so that drag/zoom interactions work correctly.
  // The Metal QWindow's native NSView intercepts events at macOS level,
  // so WA_TransparentForMouseEvents on the container widget alone is not enough.
  // We intercept events here and re-send them to the parent widget.
  if (watched == m_containerWindow && parentWidget())
  {
    switch (event->type())
    {
      case QEvent::MouseButtonPress:
      case QEvent::MouseButtonRelease:
      case QEvent::MouseButtonDblClick:
      case QEvent::MouseMove:
      {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        // Map coordinates from QWindow to parent widget
        QPointF mappedPos = parentWidget()->mapFromGlobal(mouseEvent->globalPosition());
        QMouseEvent mappedEvent(mouseEvent->type(), mappedPos,
                                 mouseEvent->globalPosition(),
                                 mouseEvent->button(), mouseEvent->buttons(),
                                 mouseEvent->modifiers());
        QCoreApplication::sendEvent(parentWidget(), &mappedEvent);
        event->accept();
        return true;
      }
      case QEvent::Wheel:
      {
        auto *wheelEvent = static_cast<QWheelEvent *>(event);
        QPointF mappedPos = parentWidget()->mapFromGlobal(wheelEvent->globalPosition());
        QWheelEvent mappedEvent(mappedPos, wheelEvent->globalPosition(),
                                 wheelEvent->pixelDelta(), wheelEvent->angleDelta(),
                                 wheelEvent->buttons(), wheelEvent->modifiers(),
                                 wheelEvent->phase(), wheelEvent->inverted(),
                                 wheelEvent->source());
        QCoreApplication::sendEvent(parentWidget(), &mappedEvent);
        event->accept();
        return true;
      }
      case QEvent::HoverMove:
      case QEvent::HoverEnter:
      case QEvent::HoverLeave:
      {
        auto *hoverEvent = static_cast<QHoverEvent *>(event);
        QPointF mappedPos = parentWidget()->mapFromGlobal(mapToGlobal(hoverEvent->position()));
        QHoverEvent mappedEvent(hoverEvent->type(), mappedPos, hoverEvent->globalPosition(),
                                 hoverEvent->oldPos());
        QCoreApplication::sendEvent(parentWidget(), &mappedEvent);
        event->accept();
        return true;
      }
      default:
        break;
    }
  }
  return QWidget::eventFilter(watched, event);
}

// ========== Pixel value drawing (reused from OpenGLRenderer logic) ==========

void NativeEDRRenderer::drawPixelValues(QPainter *painter)
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

void NativeEDRRenderer::drawZoomIndicator(QPainter *painter)
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

void NativeEDRRenderer::drawPixelRulers(QPainter *painter)
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

// ========== PixelOverlay implementation ==========

} // namespace video

#endif // Q_OS_MAC