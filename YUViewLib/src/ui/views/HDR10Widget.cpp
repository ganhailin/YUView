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

#include "HDR10Widget.h"

#include <video/FrameHandler.h>

#include <QDebug>
#include <QSurfaceFormat>
#include <QPainter>
#include <QSettings>

namespace video
{

// Threshold for showing pixel values (same as SPLITVIEW_DRAW_VALUES_ZOOMFACTOR)
static const double SHOW_PIXEL_VALUES_ZOOM_THRESHOLD = 4.0;

HDR10Widget::HDR10Widget(QWidget *parent) : QOpenGLWidget(parent)
{
  QSurfaceFormat format;
  format.setRedBufferSize(10);
  format.setGreenBufferSize(10);
  format.setBlueBufferSize(10);
  format.setAlphaBufferSize(10);
  format.setProfile(QSurfaceFormat::CoreProfile);
  format.setVersion(3, 3);
  setFormat(format);

  // Create pixel overlay widget
  m_pixelOverlay = new PixelOverlay(this);
  m_pixelOverlay->setGeometry(0, 0, width(), height());
  m_pixelOverlay->show();
}

HDR10Widget::~HDR10Widget()
{
  makeCurrent();
  if (m_textureId != 0)
    glDeleteTextures(1, &m_textureId);
  delete m_program;
  delete m_programDither;
  doneCurrent();
}

void HDR10Widget::setFrame(const VideoFrame &frame)
{
  m_currentFrame     = frame;
  m_frameSize        = frame.getSize();
  m_frameNeedsUpdate = true;
  update();
}

void HDR10Widget::setDithering(bool enable)
{
  m_ditheringEnabled = enable;
  update();
}

void HDR10Widget::setZoom(double zoom)
{
  m_zoom = zoom;
  updatePixelOverlay();
}

void HDR10Widget::setMoveOffset(QPointF offset)
{
  m_moveOffset = offset;
  updatePixelOverlay();
}

void HDR10Widget::updatePixelOverlay()
{
  if (m_pixelOverlay)
    m_pixelOverlay->update();
}

void HDR10Widget::initializeGL()
{
  initializeOpenGLFunctions();

  const char *version  = reinterpret_cast<const char *>(glGetString(GL_VERSION));
  const char *renderer = reinterpret_cast<const char *>(glGetString(GL_RENDERER));

  // Check actual buffer bit depth
  GLint redBits, greenBits, blueBits;
  glGetIntegerv(GL_RED_BITS, &redBits);
  glGetIntegerv(GL_GREEN_BITS, &greenBits);
  glGetIntegerv(GL_BLUE_BITS, &blueBits);

  m_supports10bit = (redBits >= 10 && greenBits >= 10 && blueBits >= 10);
  if (!m_supports10bit)
  {
    // Fall back to 8-bit rendering
    m_bitDepth = 8;
    qInfo() << "HDR10Widget: 10-bit not supported, falling back to" << m_bitDepth << "bit ("
            << redBits << "/" << greenBits << "/" << blueBits << "bits)";
  }

  m_openglInfo = QString("OpenGL %1, Renderer: %2, %3-bit").arg(version, renderer).arg(m_bitDepth);

  qInfo() << "HDR10Widget:" << m_openglInfo;

  initShaders();
  initGeometry();
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

  m_initialized = true;
}

void HDR10Widget::initShaders()
{
  m_program = new QOpenGLShaderProgram(this);
  m_program->addShaderFromSourceFile(QOpenGLShader::Vertex, ":/shaders/hdr10_vertex.glsl");
  m_program->addShaderFromSourceFile(QOpenGLShader::Fragment, ":/shaders/hdr10_fragment.glsl");
  if (!m_program->link())
    qWarning() << "HDR10Widget: Standard shader link error:" << m_program->log();

  m_programDither = new QOpenGLShaderProgram(this);
  m_programDither->addShaderFromSourceFile(QOpenGLShader::Vertex, ":/shaders/hdr10_vertex.glsl");
  m_programDither->addShaderFromSourceFile(QOpenGLShader::Fragment,
                                           ":/shaders/hdr10_fragment_dither.glsl");
  if (!m_programDither->link())
    qWarning() << "HDR10Widget: Dither shader link error:" << m_programDither->log();

  m_textureLoc  = m_program->uniformLocation("texture16bit");
  m_bitDepthLoc = m_program->uniformLocation("bitDepth");
}

void HDR10Widget::initGeometry()
{
  GLfloat vertices[] = {
    -1.0f,
    -1.0f,
    0.0f,
    0.0f,
    1.0f,
    -1.0f,
    1.0f,
    0.0f,
    -1.0f,
    1.0f,
    0.0f,
    1.0f,
    1.0f,
    1.0f,
    1.0f,
    1.0f,
  };

  m_vao.create();
  m_vao.bind();

  m_vbo.create();
  m_vbo.bind();
  m_vbo.allocate(vertices, sizeof(vertices));

  m_program->enableAttributeArray(0);
  m_program->enableAttributeArray(1);
  m_program->setAttributeBuffer(0, GL_FLOAT, 0, 2, 4 * sizeof(GLfloat));
  m_program->setAttributeBuffer(1, GL_FLOAT, 2 * sizeof(GLfloat), 2, 4 * sizeof(GLfloat));

  m_vao.release();
  m_vbo.release();
}

void HDR10Widget::updateTexture()
{
  if (!m_currentFrame.isValid())
    return;

  if (m_textureId != 0)
  {
    glDeleteTextures(1, &m_textureId);
    m_textureId = 0;
  }

  const int w = m_currentFrame.width();
  const int h = m_currentFrame.height();

  if (!m_currentFrame.has16bitBuffer())
  {
    const_cast<VideoFrame &>(m_currentFrame).generate16bitBuffer();
  }

  const uint16_t *data = m_currentFrame.getData16bit();
  if (!data)
    return;
  setBitDepth(16);

  glGenTextures(1, &m_textureId);
  glBindTexture(GL_TEXTURE_2D, m_textureId);

  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16UI, w, h, 0, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, data);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glBindTexture(GL_TEXTURE_2D, 0);
  m_frameNeedsUpdate = false;
}

void HDR10Widget::resizeGL(int w, int h)
{
  glViewport(0, 0, w, h);

  // Update pixel overlay geometry to match
  if (m_pixelOverlay)
    m_pixelOverlay->setGeometry(0, 0, w, h);
}

void HDR10Widget::paintGL()
{
  glClearColor(140.f/255.f, 140.f/255.f, 140.f/255.f, 0);
  glClear(GL_COLOR_BUFFER_BIT);

  if (!m_program || !m_programDither)
    return;

  if (m_frameNeedsUpdate)
    updateTexture();

  if (m_textureId == 0)
    return;

  // Calculate vertex positions that match SplitViewWidget's behavior
  // SplitViewWidget: videoRect.setSize(QSize(frameSize.width * zoom, frameSize.height * zoom))
  // The video is centered at (0,0) with the given size
  
  int widgetW = width();
  int widgetH = height();
  int frameW = m_frameSize.width();
  int frameH = m_frameSize.height();
  
  if (frameW <= 0 || frameH <= 0)
    return;
  
  // Calculate display size in pixels (same as SplitViewWidget)
  double displayW = frameW * m_zoom;
  double displayH = frameH * m_zoom;
  
  // Convert to NDC (Normalized Device Coordinates: -1 to 1)
  // NDC X = (pixelX / widgetW) * 2 - 1
  // NDC Y = 1 - (pixelY / widgetH) * 2 (flip Y because QPainter Y is down, OpenGL Y is up)
  //
  // The video rect in widget coordinates (centered at widget center + offset):
  // Left:   widgetW/2 + offsetX - displayW/2
  // Right:  widgetW/2 + offsetX + displayW/2
  // Top:    widgetH/2 + offsetY - displayH/2  (QPainter: y increases down)
  // Bottom: widgetH/2 + offsetY + displayH/2
  
  double ndcOffsetX = m_moveOffset.x() / (widgetW * 0.5);
  double ndcOffsetY = -m_moveOffset.y() / (widgetH * 0.5); // Flip Y for OpenGL
  
  double ndcW = displayW / (widgetW * 0.5);
  double ndcH = displayH / (widgetH * 0.5);
  
  float left   = static_cast<float>(ndcOffsetX - ndcW * 0.5);
  float right  = static_cast<float>(ndcOffsetX + ndcW * 0.5);
  float bottom = static_cast<float>(ndcOffsetY - ndcH * 0.5);
  float top    = static_cast<float>(ndcOffsetY + ndcH * 0.5);

  GLfloat vertices[] = {
    // Position          // Texture coords (flipped Y to fix upside-down)
    left,  bottom, 0.0f, 1.0f,  // Bottom-left
    right, bottom, 1.0f, 1.0f,  // Bottom-right
    left,  top,    0.0f, 0.0f,  // Top-left
    right, top,    1.0f, 0.0f,  // Top-right
  };

  m_vbo.bind();
  m_vbo.allocate(vertices, sizeof(vertices));
  m_vbo.release();

  QOpenGLShaderProgram *currentProgram = m_ditheringEnabled ? m_programDither : m_program;
  currentProgram->bind();
  m_vao.bind();

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_textureId);

  glUniform1i(currentProgram->uniformLocation("texture16bit"), 0);
  glUniform1i(currentProgram->uniformLocation("bitDepth"), m_bitDepth);

  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  m_vao.release();
  currentProgram->release();
}

void HDR10Widget::drawPixelValues(QPainter *painter)
{
  if (!m_showRawData || m_zoom < SHOW_PIXEL_VALUES_ZOOM_THRESHOLD)
    return;

  if (!m_currentFrame.isValid() || m_frameSize.isEmpty())
    return;

  // Calculate display rect (same logic as paintGL)
  int widgetW = width();
  int widgetH = height();
  int frameW = m_frameSize.width();
  int frameH = m_frameSize.height();

  double displayW = frameW * m_zoom;
  double displayH = frameH * m_zoom;

  // Calculate the video rect in widget coordinates
  // Center of widget + offset - half display size
  double videoLeft = widgetW * 0.5 + m_moveOffset.x() - displayW * 0.5;
  double videoTop = widgetH * 0.5 + m_moveOffset.y() - displayH * 0.5;

  // Clip to widget bounds
  int xMin = static_cast<int>(std::max(0.0, -videoLeft / m_zoom));
  int yMin = static_cast<int>(std::max(0.0, -videoTop / m_zoom));
  int xMax = static_cast<int>(std::min(static_cast<double>(frameW - 1), (widgetW - videoLeft) / m_zoom));
  int yMax = static_cast<int>(std::min(static_cast<double>(frameH - 1), (widgetH - videoTop) / m_zoom));

  if (xMin > xMax || yMin > yMax)
    return;

  // Set up font (fixed size, same as SplitViewWidget)
  QFont font = painter->font();
  font.setPointSize(10);
  painter->setFont(font);

  // Draw pixel values using FrameHandler if available, otherwise fallback to RGB buffer
  if (m_frameHandler)
  {
    // Use FrameHandler to get properly labeled pixel values (YUV or RGB)
    for (int y = yMin; y <= yMax; ++y)
    {
      for (int x = xMin; x <= xMax; ++x)
      {
        // Calculate pixel rect in widget coordinates
        double pxLeft = videoLeft + x * m_zoom;
        double pxTop = videoTop + y * m_zoom;
        QRect pixelRect(static_cast<int>(pxLeft), static_cast<int>(pxTop),
                        static_cast<int>(m_zoom), static_cast<int>(m_zoom));

        // Get pixel values from FrameHandler (this returns YUV or RGB with proper labels)
        auto pixelValues = m_frameHandler->getPixelValues(QPoint(x, y), 0);
        if (pixelValues.isEmpty())
          continue;

        // Format the values with their labels
        QStringList lines;
        for (const auto &pair : pixelValues)
        {
          lines.append(pair.first + pair.second);
        }

        // Determine text color based on first value (assume luma/R)
        bool drawWhite = false;
        if (!pixelValues.isEmpty())
        {
          QString firstValue = pixelValues.first().second;
          // Parse the value (remove any +/- prefix)
          int value = std::abs(firstValue.toInt());
          drawWhite = value < 128;
        }
        painter->setPen(drawWhite ? Qt::white : Qt::black);

        QString text = lines.join("\n");
        painter->drawText(pixelRect, Qt::AlignCenter, text);
      }
    }
  }
  else
  {
    // Fallback: use 16-bit RGB buffer (may not have correct YUV values)
    const uint16_t *data = m_currentFrame.getData16bit();
    if (!data)
      return;

    QSettings settings;
    const bool showHex = settings.value("ShowPixelValuesHex", false).toBool();
    const int maxVal = (1 << m_bitDepth) - 1;

    for (int y = yMin; y <= yMax; ++y)
    {
      for (int x = xMin; x <= xMax; ++x)
      {
        // Calculate pixel rect in widget coordinates
        double pxLeft = videoLeft + x * m_zoom;
        double pxTop = videoTop + y * m_zoom;
        QRect pixelRect(static_cast<int>(pxLeft), static_cast<int>(pxTop),
                        static_cast<int>(m_zoom), static_cast<int>(m_zoom));

        // Get pixel value from 16-bit buffer (RGBA)
        int idx = (y * frameW + x) * 4;
        uint16_t r = data[idx];
        uint16_t g = data[idx + 1];
        uint16_t b = data[idx + 2];

        // Convert to display value based on bit depth
        int rv = (r * maxVal) / 65535;
        int gv = (g * maxVal) / 65535;
        int bv = (b * maxVal) / 65535;

        // Determine text color based on pixel brightness
        bool isDark = (rv + gv + bv) / 3 < (maxVal / 2);
        painter->setPen(isDark ? Qt::white : Qt::black);

        // Format text with R/G/B labels
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

// PixelOverlay implementation
HDR10Widget::PixelOverlay::PixelOverlay(HDR10Widget *parent)
  : QWidget(parent), hdrWidget(parent)
{
  setAttribute(Qt::WA_TransparentForMouseEvents);
  setAttribute(Qt::WA_TransparentForMouseEvents);
}

void HDR10Widget::PixelOverlay::paintEvent(QPaintEvent *)
{
  if (!hdrWidget || !hdrWidget->m_showRawData)
    return;

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, false);

  hdrWidget->drawPixelValues(&painter);
}

} // namespace video
