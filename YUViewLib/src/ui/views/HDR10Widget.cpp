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
#include <video/yuv/videoHandlerYUV.h>
#include <common/FunctionsGui.h>

#include <QDebug>
#include <QMatrix3x3>
#include <QSurfaceFormat>
#include <QPainter>
#include <QSettings>
#include <QColorSpace>


namespace video
{

// Threshold for showing pixel values (same as SPLITVIEW_DRAW_VALUES_ZOOMFACTOR)
static const double SHOW_PIXEL_VALUES_ZOOM_THRESHOLD = 4.0;

HDR10Widget::HDR10Widget(QWidget *parent) : QOpenGLWidget(parent)
{
  // Disable auto-fill background to prevent Qt from clearing our OpenGL content
  setAutoFillBackground(false);
  setAttribute(Qt::WA_OpaquePaintEvent);

  QSurfaceFormat format;
  format.setProfile(QSurfaceFormat::CoreProfile);
  format.setVersion(3, 3);

#ifdef Q_OS_MAC
  // macOS: 8-bit color buffers (QOpenGLWidget FBO is always 8-bit on macOS)
  format.setRedBufferSize(8);
  format.setGreenBufferSize(8);
  format.setBlueBufferSize(8);
  format.setAlphaBufferSize(8);
#else
  // 10-bit color buffers on non-macOS platforms
  format.setRedBufferSize(10);
  format.setGreenBufferSize(10);
  format.setBlueBufferSize(10);
  format.setAlphaBufferSize(10);
#endif

  setFormat(format);

  // Create pixel overlay widget
  m_pixelOverlay = std::make_shared<PixelOverlay>(this);
  m_pixelOverlay->setGeometry(0, 0, width(), height());
  m_pixelOverlay->show();
}

HDR10Widget::~HDR10Widget()
{
  // Clean up the GL texture while the context is still valid.
  // shared_ptr members (m_program, m_programDither, m_pixelOverlay) are
  // automatically released by their destructors — no manual cleanup needed.
  auto *ctx = context();
  if (ctx && ctx->isValid())
  {
    makeCurrent();
    if (m_textureId != 0)
    {
      glDeleteTextures(1, &m_textureId);
      m_textureId = 0;
    }
    doneCurrent();
  }
  else
  {
    m_textureId = 0;
  }
}

void HDR10Widget::setFrame(const VideoFrame &frame)
{
  // Only update if frame data actually changed
  // Compare 16-bit buffer pointers to detect if it's the same frame data
  const uint16_t *newData = frame.getData16bit();
  const uint16_t *oldData =
    m_currentFrame.is16bitGenerateFrom8bit() ? nullptr : m_currentFrame.getData16bit();
  auto new_size_empty = frame.getSize().isEmpty();
  auto old_size_empty = m_frameSize.isEmpty();
  std::shared_ptr<QImage> newImage8 = frame.getImage8bit();
  std::shared_ptr<QImage> oldImage8 = m_currentFrame.getImage8bit();
  if (newData != oldData || (new_size_empty != old_size_empty && (frame.getSize() != m_frameSize)) ||
      newImage8 != oldImage8)
  {
    m_currentFrame = frame;
    if (!newData)
      m_currentFrame.clear16bitBuffer(); // Clear 16-bit buffer if new frame has no 16-bit data
    m_frameSize        = frame.getSize();
    m_frameNeedsUpdate = true;
    update();
  }
}

void HDR10Widget::setDithering(bool enable)
{
  m_ditheringEnabled = enable;
  updatePixelOverlay();
  update();
}

void HDR10Widget::setZoom(double zoom)
{
  m_zoom = zoom;
  update();             // Trigger OpenGL re-render (vertices changed)
  updatePixelOverlay(); // Update QPainter overlay
}

void HDR10Widget::setMoveOffset(QPointF offset)
{
  m_moveOffset = offset;
  update();             // Trigger OpenGL re-render (vertices changed)
  updatePixelOverlay(); // Update QPainter overlay
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
  // Note: On macOS, QOpenGLWidget uses an internal FBO. The default framebuffer
  // queries (GL_RED_BITS, etc.) may return garbage values if the FBO isn't properly
  // bound yet. We need to bind our FBO first to get reliable results.
  glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());

  GLint redBits{8}, greenBits{8}, blueBits{8};
  glGetIntegerv(GL_RED_BITS, &redBits);
  glGetIntegerv(GL_GREEN_BITS, &greenBits);
  glGetIntegerv(GL_BLUE_BITS, &blueBits);

  // Validate results - on macOS, these may return garbage if FBO wasn't bound
  // If any value is negative or zero (except redBits which might be valid),
  // treat the entire query as unreliable and fall back to safe defaults
  if (greenBits < 0 || blueBits < 0 || greenBits == 0 && blueBits == 0)
  {
    qInfo() << "HDR10Widget: Framebuffer bit depth query returned unreliable values"
            << "(" << redBits << "/" << greenBits << "/" << blueBits << "bits),"
            << "assuming 8-bit default buffer";
    redBits = 8;
    greenBits = 8;
    blueBits = 8;
  }

  m_supports10bit = (redBits >= 10 && greenBits >= 10 && blueBits >= 10);
  if (!m_supports10bit)
  {
    // Fall back to 8-bit rendering
    m_bitDepth = 8;
    qInfo() << "HDR10Widget: 10-bit not supported, falling back to" << m_bitDepth << "bit ("
            << redBits << "/" << greenBits << "/" << blueBits << "bits)";
  }
  else
  {
    qInfo() << "HDR10Widget: 10-bit supported (" << redBits << "/" << greenBits << "/" << blueBits << "bits)";
  }


  m_openglInfo = QString("OpenGL %1, Renderer: %2, %3-bit")
                     .arg(version, renderer)
                     .arg(m_bitDepth);

  qInfo() << "HDR10Widget:" << m_openglInfo;

  initShaders();
  initGeometry();
  QSettings settings;
  QColor backgroundColor = settings.value("View/BackgroundColor", QColor(140, 140, 140)).value<QColor>();
  glClearColor(backgroundColor.redF(), backgroundColor.greenF(), backgroundColor.blueF(), 1.0f);

  m_initialized = true;
}

void HDR10Widget::initShaders()
{
  m_program = std::make_shared<QOpenGLShaderProgram>(this);
  m_program->addShaderFromSourceFile(QOpenGLShader::Vertex, ":/shaders/hdr10_vertex.glsl");
  m_program->addShaderFromSourceFile(QOpenGLShader::Fragment, ":/shaders/hdr10_fragment.glsl");
  if (!m_program->link())
    qWarning() << "HDR10Widget: Standard shader link error:" << m_program->log();

  m_programDither = std::make_shared<QOpenGLShaderProgram>(this);
  m_programDither->addShaderFromSourceFile(QOpenGLShader::Vertex, ":/shaders/hdr10_vertex.glsl");
  m_programDither->addShaderFromSourceFile(QOpenGLShader::Fragment,
                                           ":/shaders/hdr10_fragment_dither.glsl");
  if (!m_programDither->link())
    qWarning() << "HDR10Widget: Dither shader link error:" << m_programDither->log();

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

  const int w = m_currentFrame.width();
  const int h = m_currentFrame.height();
  const QSize newSize(w, h);

  // Check if VideoFrame already has a 16-bit buffer (from high bit-depth source like 10/12/16-bit RGB)
  // If not, generate it from the 8-bit QImage (for 8-bit sources, maintains backward compatibility)
  if (!m_currentFrame.has16bitBuffer())
  {
    // For 8-bit sources: generate expanded 16-bit buffer (r*257, etc.)
    // This maintains backward compatibility with existing 8-bit content
    const_cast<VideoFrame &>(m_currentFrame).generate16bitBuffer();
    // Source is 8-bit
    m_sourceBitDepth = 8;
  }
  else
  {
    // For high bit-depth sources: the data is already in 16-bit range
    // We store the actual source bit depth for pixel value display
    // Note: ideally we'd detect the actual source bit depth (10/12/16) from FrameHandler
    // For now, we assume high bit-depth sources are at least 10-bit
    m_sourceBitDepth = 10;  // Could be refined to detect actual bit depth
  }

  const uint16_t *data = m_currentFrame.getData16bit();
  if (!data)
    return;

  // Note: m_bitDepth remains as the DISPLAY bit depth (for shader normalization and dithering)
  // - Standard shader: normalizes by (2^m_bitDepth - 1)
  // - Dithering shader: dithers to m_bitDepth levels
  // Data is always uploaded as 16-bit, so shader divides by 65535.0 first

  const bool sizeChanged = (m_textureSize != newSize);

  if (m_textureId == 0 || sizeChanged)
  {
    // Create new texture (first time or size changed)
    if (m_textureId != 0)
    {
      glDeleteTextures(1, &m_textureId);
    }

    glGenTextures(1, &m_textureId);
    glBindTexture(GL_TEXTURE_2D, m_textureId);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16UI, w, h, 0, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, data);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    m_textureSize = newSize;
  }
  else
  {
    // Reuse existing texture - just update the data
    glBindTexture(GL_TEXTURE_2D, m_textureId);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, data);
  }

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
  // Bind the correct framebuffer (QOpenGLWidget uses an internal FBO)
  glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());

  // Disable depth testing for 2D rendering
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);

  // Enable alpha blending for transparent image regions.
  // Use premultiplied blending: the shader premultiplies in encoding domain.
  // Note: GL_FRAMEBUFFER_SRGB is NOT used because Qt's QOpenGLWidget NSView
  // backing store applies an extra sRGB decode, causing double-encode issues
  // (same root cause as the EDR createWindowContainer color bug).
  // The shader manually does sRGB OETF and gamut conversion to the display's
  // color space (queried at runtime via getDisplayColorSpace()).
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

  // Set viewport to full widget size
  glViewport(0, 0, width() * devicePixelRatio(), height() * devicePixelRatio());

  // Clear to the same background color as SplitViewWidget
  QSettings settings;
  QColor backgroundColor = settings.value("View/BackgroundColor", QColor(140, 140, 140)).value<QColor>();
  glClearColor(backgroundColor.redF(), backgroundColor.greenF(), backgroundColor.blueF(), 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  if (!m_program || !m_programDither)
    return;

  if (m_frameNeedsUpdate)
    updateTexture();

  if (m_textureId == 0)
  {
    glFinish();
    return;
  }

  // Calculate vertex positions that match SplitViewWidget's behavior
  // SplitViewWidget: videoRect.setSize(QSize(frameSize.width * zoom, frameSize.height * zoom))
  // The video is centered at (0,0) with the given size
  
  int widgetW = width();
  int widgetH = height();
  int frameW = m_frameSize.width();
  int frameH = m_frameSize.height();
  
  if (frameW <= 0 || frameH <= 0)
  {
    glFinish();
    return;
  }
  
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

  std::shared_ptr<QOpenGLShaderProgram> currentProgram = nullptr;

  // EDR output (>1.0 values) is NOT possible through QOpenGLWidget on macOS
  // (internal FBO is 8-bit RGBA). On macOS, EDR is handled by the Metal-based
  // HDR10WidgetMacEDR. This widget always uses the standard or dither shader.
  if (m_ditheringEnabled)
  {
    currentProgram = m_programDither;
  }
  else
  {
    currentProgram = m_program;
  }

  currentProgram->bind();
  m_vao.bind();

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_textureId);

  glUniform1i(currentProgram->uniformLocation("texture16bit"), 0);

  // Set color processing uniforms (EOTF, gamut, brightness)
  // These are used by both standard and dither shaders for SDR output
  glUniform1i(currentProgram->uniformLocation("eotfType"), static_cast<int>(m_eotf));
  glUniform1f(currentProgram->uniformLocation("gammaValue"), m_gammaValue);
  glUniform1f(currentProgram->uniformLocation("diffuseWhiteNits"), m_diffuseWhiteNits);
  glUniform1f(currentProgram->uniformLocation("hdrBrightness"), m_hdrBrightness);

  // Set gamut conversion matrix (3x3, column-major for OpenGL)
  // Gamut target is the display's color space (queried at runtime).
  // On macOS with Display P3 screens, this converts BT.709→Display P3.
  // The shader does sRGB OETF after gamut conversion, outputting sRGB-encoded
  // values in the display's gamut. This matches QPainter and EDR paths.
  //
  // When Windows ACM is enabled, the OS compositor handles gamut mapping
  // from sRGB to the display. We MUST output sRGB and let ACM do the work —
  // any in-shader gamut conversion would be double-applied, causing clamping
  // or oversaturation.
  //
  // When the surface is already sRGB, skip the display color space query and
  // use BT.709 as the gamut target directly — no conversion is needed.
  float customMatrix[9];
  const float *matrixData;
  const bool acmActive = functionsGui::isWindowsACMEnabled();

  // Emit status change if ACM state toggled (for dock display)
  if (acmActive != m_lastAcmActive)
  {
    m_lastAcmActive = acmActive;
    // OpenGL path: hdrActive=false, systemHandlesTonemapping reflects ACM
    emit hdrStatusChanged(false, acmActive, 0.0f, 0.0f);
  }
  const bool surfaceIsSRGB =
      (QSurfaceFormat::defaultFormat().colorSpace() == QColorSpace::SRgb);
  if (acmActive || surfaceIsSRGB)
  {
    matrixData = color::getGamutMatrix(m_colorGamut, color::ColorGamut::BT709);
  }
  else
  {
    auto displayCS = functionsGui::getDisplayColorSpace();
    if (displayCS.isValid())
    {
      matrixData = color::getGamutMatrixForDisplay(m_colorGamut, displayCS, customMatrix);
    }
    else
    {
      matrixData = color::getGamutMatrix(m_colorGamut, color::ColorGamut::BT709);
    }
  }
  QMatrix3x3 gamutMat;
  for (int row = 0; row < 3; ++row)
  {
    for (int col = 0; col < 3; ++col)
    {
      gamutMat.data()[col * 3 + row] = matrixData[row * 3 + col];
    }
  }
  glUniformMatrix3fv(currentProgram->uniformLocation("gamutMatrix"), 1, GL_FALSE, gamutMat.constData());

  // Set tonemapping and OETF uniforms.
  // Shader manually does sRGB OETF (GL_FRAMEBUFFER_SRGB is not used due to
  // Qt NSView backing store double-encode issue).
  // Reinhard tonemapping only for HDR content (PQ/HLG).
  bool isHDREOTF = (m_eotf == video::HDR10_EOTF::PQ || m_eotf == video::HDR10_EOTF::HLG);
  glUniform1f(currentProgram->uniformLocation("systemHandlesTonemapping"), isHDREOTF ? 0.0f : 1.0f);
  glUniform1f(currentProgram->uniformLocation("applySRGBOETF"), 1.0f);
  glUniform1i(currentProgram->uniformLocation("premultipliedAlpha"), m_premultipliedAlpha ? 1 : 0);

  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  m_vao.release();
  currentProgram->release();

  // Ensure rendering completes
  glFinish();
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

  // Cache common values
  const uint16_t *frameData = m_currentFrame.getData16bit();
  if (!frameData)
    return;

  QSettings settings;
  const bool showHex = settings.value("ShowPixelValuesHex", false).toBool();
  const int maxDisplayVal = (1 << m_sourceBitDepth) - 1;
  const int maxVal16 = 65535;
  const int halfMax16 = maxVal16 / 2;

  // Draw pixel values using FrameHandler if available, otherwise fallback to RGB buffer
  if (m_frameHandler)
  {
    // Check if this is a YUV source and get subsampling info
    auto *yuvHandler = dynamic_cast<video::yuv::videoHandlerYUV *>(m_frameHandler);
    const bool isYUV = (yuvHandler != nullptr);

    // Get YUV format info if available
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
      // The chroma offset in full luma pixels. This can range from 0 to 3.
      chromaOffsetFullX = format.getChromaOffset().x / 2;
      chromaOffsetFullY = format.getChromaOffset().y / 2;
      chromaPresent = (format.getSubsampling() != video::yuv::Subsampling::YUV_400);
    }

    // Pre-calculate row strides
    const int rowStride = frameW * 4;

    // Use FrameHandler to get properly labeled pixel values (YUV or RGB)
    for (int y = yMin; y <= yMax; ++y)
    {
      // Pre-calculate Y position
      double pxTop = videoTop + y * m_zoom;
      int baseIdx = y * rowStride;

      for (int x = xMin; x <= xMax; ++x)
      {
        // Calculate pixel rect in widget coordinates
        double pxLeft = videoLeft + x * m_zoom;
        QRect pixelRect(static_cast<int>(pxLeft), static_cast<int>(pxTop),
                        static_cast<int>(m_zoom), static_cast<int>(m_zoom));

        // Get pixel values from FrameHandler (this returns YUV or RGB with proper labels)
        auto pixelValues = m_frameHandler->getPixelValues(QPoint(x, y), 0);
        if (pixelValues.isEmpty())
          continue;

        // Format the values with their labels
        // For YUV sources, only show UV values on pixels where chroma is sampled
        QStringList lines;
        for (const auto &pair : pixelValues)
        {
          QString label = pair.first;
          // For YUV sources, check if we should show U/V values based on subsampling
          if (isYUV && chromaPresent && (label == "U" || label == "V"))
          {
            // Check if this pixel position should show chroma values
            // (same logic as videoHandlerYUV::drawPixelValues)
            if ((x - chromaOffsetFullX) % subsamplingX != 0 ||
                (y - chromaOffsetFullY) % subsamplingY != 0)
            {
              // Skip UV values for this pixel (not a chroma sampling position)
              continue;
            }
          }
          lines.append(label + pair.second);
        }

        // Determine text color based on actual RGB color of the pixel
        // Get RGB from the 16-bit buffer (this is what's actually displayed)
        int idx = baseIdx + x * 4;
        int r = frameData[idx];
        int g = frameData[idx + 1];
        int b = frameData[idx + 2];

        // Calculate perceived brightness using weighted RGB to Y conversion
        // Y = 0.299*R + 0.587*G + 0.114*B (ITU-R BT.601)
        int brightness = (299 * r + 587 * g + 114 * b) / 1000;
        painter->setPen(brightness < halfMax16 ? Qt::white : Qt::black);

        QString text = lines.join("\n");
        painter->drawText(pixelRect, Qt::AlignCenter, text);
      }
    }
  }
  else
  {
    // Fallback: use 16-bit RGB buffer (may not have correct YUV values)
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

        // Get pixel value from 16-bit buffer (RGBA)
        int idx = baseIdx + x * 4;
        uint16_t r = frameData[idx];
        uint16_t g = frameData[idx + 1];
        uint16_t b = frameData[idx + 2];

        // Convert from 16-bit storage range (0-65535) to source bit depth display range
        int rv = (r * maxDisplayVal) / maxVal16;
        int gv = (g * maxDisplayVal) / maxVal16;
        int bv = (b * maxDisplayVal) / maxVal16;

        // Determine text color based on perceived brightness (using display values)
        int brightness = (299 * rv + 587 * gv + 114 * bv) / 1000;
        painter->setPen(brightness < (maxDisplayVal / 2) ? Qt::white : Qt::black);

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

void HDR10Widget::drawZoomIndicator(QPainter *painter)
{
  if (m_zoom == 1.0)
    return;

  // Format zoom string (same as SplitViewWidget)
  QString zoomString = QString("x") + QString::number(m_zoom, 'g', (m_zoom < 0.5) ? 4 : 2);

  // Set up font
  QFont font("helvetica", 24);
  painter->setRenderHint(QPainter::TextAntialiasing);
  painter->setPen(QColor(Qt::black));
  painter->setFont(font);

  // Draw at top-left corner
  QPoint pos(10, QFontMetrics(font).height());
  painter->drawText(pos, zoomString);
}

void HDR10Widget::drawPixelRulers(QPainter *painter)
{
  if (!m_frameHandler || m_zoom < 32.0)
    return;

  if (!m_currentFrame.isValid() || m_frameSize.isEmpty())
    return;

  // Set up font for ruler values
  QFont valueFont("helvetica", 10);
  painter->setFont(valueFont);

  int widgetW = width();
  int widgetH = height();
  int frameW = m_frameSize.width();
  int frameH = m_frameSize.height();

  // Calculate video rect position (same as paintGL logic)
  double displayW = frameW * m_zoom;
  double displayH = frameH * m_zoom;
  double videoLeft = widgetW * 0.5 + m_moveOffset.x() - displayW * 0.5;
  double videoTop = widgetH * 0.5 + m_moveOffset.y() - displayH * 0.5;

  // Calculate visible pixel range for X (horizontal ruler on top)
  int xMin = static_cast<int>(std::max(0.0, -videoLeft / m_zoom));
  int xMax = static_cast<int>(std::min(static_cast<double>(frameW - 1), (widgetW - videoLeft) / m_zoom));

  // Draw X pixel indicators (horizontal ruler on top edge)
  for (int x = xMin; x <= xMax; ++x)
  {
    int xPosOnScreen = static_cast<int>(videoLeft + x * m_zoom);

    // Draw tick marks
    painter->setPen(QPen(Qt::white));
    painter->drawLine(xPosOnScreen, 0, xPosOnScreen, 5);
    painter->setPen(QPen(Qt::black));
    painter->drawLine(xPosOnScreen + 1, 0, xPosOnScreen + 1, 5);

    // Draw values (every 5th value, or all values for zoom >= 128)
    if ((m_zoom >= 128 || x % 5 == 0) && x != frameW)
    {
      QString numberText = QString::number(x);
      QFontMetrics metrics(valueFont);
      QSize rectSize = metrics.size(0, numberText) + QSize(4, 0);
      QPoint rectPosTopLeft(xPosOnScreen + static_cast<int>(m_zoom / 2) - rectSize.width() / 2, 2);
      QRect textRect(rectPosTopLeft, rectSize);

      // Draw white background rect and text
      painter->fillRect(textRect, Qt::white);
      painter->setPen(QPen(Qt::black));
      painter->drawText(textRect, Qt::AlignCenter, numberText);
    }
  }

  // Calculate visible pixel range for Y (vertical ruler on left)
  int yMin = static_cast<int>(std::max(0.0, -videoTop / m_zoom));
  int yMax = static_cast<int>(std::min(static_cast<double>(frameH - 1), (widgetH - videoTop) / m_zoom));

  // Draw Y pixel indicators (vertical ruler on left edge)
  for (int y = yMin; y <= yMax; ++y)
  {
    int yPosOnScreen = static_cast<int>(videoTop + y * m_zoom);

    // Draw tick marks
    painter->setPen(QPen(Qt::white));
    painter->drawLine(0, yPosOnScreen, 5, yPosOnScreen);
    painter->setPen(QPen(Qt::black));
    painter->drawLine(0, yPosOnScreen + 1, 5, yPosOnScreen + 1);

    // Draw values (every 5th value, or all values for zoom >= 128)
    if ((m_zoom >= 128 || y % 5 == 0) && y != frameH)
    {
      QString numberText = QString::number(y);
      QFontMetrics metrics(valueFont);
      QSize rectSize = metrics.size(0, numberText) + QSize(4, 0);
      QPoint rectPosTopLeft(2, yPosOnScreen + static_cast<int>(m_zoom / 2) - rectSize.height() / 2);
      QRect textRect(rectPosTopLeft, rectSize);

      // Draw white background rect and text
      painter->fillRect(textRect, Qt::white);
      painter->setPen(QPen(Qt::black));
      painter->drawText(textRect, Qt::AlignCenter, numberText);
    }
  }
}

// PixelOverlay implementation
HDR10Widget::PixelOverlay::PixelOverlay(HDR10Widget *parent)
  : QWidget(parent), hdrWidget(parent)
{
  setAttribute(Qt::WA_TransparentForMouseEvents);
  setAttribute(Qt::WA_TranslucentBackground);
  setAutoFillBackground(false);
}

void HDR10Widget::PixelOverlay::paintEvent(QPaintEvent *)
{
  if (!hdrWidget)
    return;

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, false);

  // Draw pixel values if enabled and zoom is high enough
  if (hdrWidget->m_showRawData && hdrWidget->m_zoom >= 4.0){
    hdrWidget->drawPixelValues(&painter);
  }

  // Draw zoom factor and pixel rulers (always draw if zoom != 1.0)
  hdrWidget->drawZoomIndicator(&painter);
  hdrWidget->drawPixelRulers(&painter);
}

} // namespace video
