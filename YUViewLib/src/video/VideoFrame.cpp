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

#include "VideoFrame.h"

#include <QtGlobal>

namespace video
{

VideoFrame::VideoFrame(const QImage &image) : image8bit(std::make_shared<QImage>(image))
{
}

void VideoFrame::clear()
{
  image8bit.reset();
  buffer16bit.reset();
}

void VideoFrame::generate16bitBuffer()
{
  if (!image8bit || image8bit->isNull())
    return;

  const int w = image8bit->width();
  const int h = image8bit->height();
  const int numPixels = w * h;
  const int numComponents = numPixels * 4; // RGBA

  buffer16bit = std::make_shared<QVector<uint16_t>>(numComponents);

  const uchar *srcBits = image8bit->constBits();
  const int   bytesPerLine = image8bit->bytesPerLine();

  uint16_t *dst = buffer16bit->data();

  for (int y = 0; y < h; y++)
  {
    const uchar *srcLine = srcBits + y * bytesPerLine;
    for (int x = 0; x < w; x++)
    {
      QRgb pixel = reinterpret_cast<const QRgb *>(srcLine)[x];
      int r = qRed(pixel);
      int g = qGreen(pixel);
      int b = qBlue(pixel);
      int a = qAlpha(pixel);

      *dst++ = r * 257;
      *dst++ = g * 257;
      *dst++ = b * 257;
      *dst++ = a * 257;
    }
  }
  is16bitGenerateFrom8bit_ = true;
}

void VideoFrame::set16bitBuffer(QVector<uint16_t> &&data, int width, int height)
{
  buffer16bit = std::make_shared<QVector<uint16_t>>(std::move(data));
  if (!image8bit || image8bit->size() != QSize(width, height))
    image8bit = std::make_shared<QImage>(width, height, QImage::Format_ARGB32);
}

} // namespace video
