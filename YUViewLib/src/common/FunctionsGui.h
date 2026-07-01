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

#pragma once

#include <common/Color.h>
#include <common/Typedef.h>

#include <QColor>
#include <QIcon>
#include <QImage>

/*
  This functions class is called "GUI" because you must link to the gui module of QT
  in order to use it. (E.g. QColor is part of the GUI module)
  Some tests we don't want to link against that module so here we can collect
  helpful functions that use the `gui` module.
 */

namespace functionsGui
{

QColor toQColor(const Color &color);
Color  toColor(const QColor &color);

// Get the display's color space for color management.
// On macOS, queries NSScreen.colorSpace (typically Display P3).
// Returns QColorSpace::SRgb as fallback if the display color space
// cannot be determined or on non-macOS platforms.
QColorSpace getDisplayColorSpace();

/// Returns the raw ICC profile bytes from the last successful getDisplayColorSpace() call.
/// Empty if ICC was not read (macOS or fallback). Used for Custom primaries parsing.
const QByteArray &getCachedIccData();

// An image format used internally by QPixmap. On a raster paint backend, the pixmap
// is backed by an image, and this returns the format of the internal QImage buffer.
// This will always return the same result as the platformImageFormat when the default
// raster backend is used.
// It is faster to call platformImageFormat instead. It will call this function as
// a fall back.
// This function is thread-safe.
QImage::Format pixmapImageFormat();

// Convert the QImage::Format to string
QString pixelFormatToString(QImage::Format f);

// The platform-specific screen-compatible image format. Using a QImage of this format
// is fast when drawing on a widget.
// This function is thread-safe.
inline QImage::Format platformImageFormat(bool needAlpha)
{
    return needAlpha ? QImage::Format_ARGB32 : QImage::Format_RGB32;
}

inline int bytesPerPixel(QPixelFormat format)
{
  auto const bits = format.bitsPerPixel();
  return (bits >= 1) ? ((bits + 7) / 8) : 0;
}

inline int bytesPerPixel(QImage::Format format)
{
  return bytesPerPixel(QImage::toPixelFormat(format));
}

void setupUi(void *ui, void (*setupUi)(void *ui, QWidget *widget));

// Return the icon/pixmap from the given file path (inverted if necessary)
QIcon   convertIcon(QString iconPath);
QPixmap convertPixmap(QString pixmapPath);

} // namespace functionsGui
