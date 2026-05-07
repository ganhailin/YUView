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

#include <video/VideoFrame.h>

#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>

namespace video
{

class HDR10Widget : public QOpenGLWidget, protected QOpenGLFunctions
{
  Q_OBJECT

public:
  explicit HDR10Widget(QWidget *parent = nullptr);
  ~HDR10Widget() override;

  void setFrame(const VideoFrame &frame);
  void setBitDepth(int bits) { m_bitDepth = bits; }
  void setDithering(bool enable);
  void setZoom(double zoom) { m_zoom = zoom; }
  void setMoveOffset(QPointF offset) { m_moveOffset = offset; }
  bool supports10bit() const { return m_supports10bit; }
  QString getOpenGLInfo() const { return m_openglInfo; }

protected:
  void initializeGL() override;
  void resizeGL(int w, int h) override;
  void paintGL() override;

private:
  void initShaders();
  void initGeometry();
  void updateTexture();

  QOpenGLShaderProgram *m_program{nullptr};
  QOpenGLShaderProgram *m_programDither{nullptr};
  QOpenGLBuffer         m_vbo{QOpenGLBuffer::VertexBuffer};
  QOpenGLVertexArrayObject m_vao;

  GLuint m_textureId{0};

  VideoFrame m_currentFrame;
  bool       m_frameNeedsUpdate{false};
  QSize      m_frameSize;

  int  m_bitDepth{10};
  bool m_ditheringEnabled{false};
  bool m_initialized{false};
  bool m_supports10bit{false};

  QString m_openglInfo;

  int m_textureLoc{-1};
  int m_bitDepthLoc{-1};

  double m_zoom{1.0};
  QPointF m_moveOffset{0, 0};
};

} // namespace video
