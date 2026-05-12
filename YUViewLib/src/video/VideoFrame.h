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

#include <QImage>
#include <QSize>
#include <QVector>
#include <cstdint>
#include <memory>

namespace video
{

/**
 * @brief VideoFrame wraps an 8-bit QImage and optionally a 16-bit buffer
 *
 * This class provides a unified frame representation that supports both
 * 8-bit QPainter rendering (via QImage) and 10/16-bit OpenGL rendering
 * (via the 16-bit buffer). The 16-bit buffer stores RGBA values with
 * 16 bits per channel (64 bits per pixel total).
 */
class VideoFrame
{
public:
  VideoFrame() = default;

  explicit VideoFrame(const QImage &image);
  explicit VideoFrame(const QSize &size);
  VideoFrame(int width, int height);

  ~VideoFrame() = default;

  VideoFrame(const VideoFrame &)            = default;
  VideoFrame &operator=(const VideoFrame &) = default;
  VideoFrame(VideoFrame &&)                 = default;
  VideoFrame &operator=(VideoFrame &&)      = default;

  const QImage &getImage8bit() const { return image8bit; }
  QImage &getImage8bit() { return image8bit; }

  const uint16_t *getData16bit() const { return buffer16bit ? buffer16bit->data() : nullptr; }
  uint16_t *getData16bit() { return buffer16bit ? buffer16bit->data() : nullptr; }

  bool has16bitBuffer() const { return buffer16bit && !buffer16bit->isEmpty(); }

  QSize getSize() const { return image8bit.size(); }
  int   width() const { return image8bit.width(); }
  int   height() const { return image8bit.height(); }
  bool  isValid() const { return !image8bit.isNull(); }

  void clear();

  void generate16bitBuffer();

  void set16bitBuffer(QVector<uint16_t> &&data, int width, int height);
  void clear16bitBuffer() { buffer16bit.reset(); }
  bool is16bitGenerateFrom8bit() const { return is16bitGenerateFrom8bit_; }

private:
  bool is16bitGenerateFrom8bit_ = false;
  QImage          image8bit;
  std::shared_ptr<QVector<uint16_t>> buffer16bit;
};

} // namespace video
