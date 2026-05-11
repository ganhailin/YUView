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

#include <QtGlobal>

namespace video
{

/**
 * @brief macOS Extended Dynamic Range (EDR) helper class
 *
 * This class provides platform-specific functionality to enable HDR/EDR rendering
 * on macOS. EDR allows applications to render content with brightness values
 * exceeding the traditional SDR range (0.0-1.0), up to the display's
 * maximum HDR capability.
 *
 * On non-macOS platforms, this class is a no-op.
 */
class YUViewMacOSEDRHelper
{
public:
    /**
     * @brief Check if EDR is available on this system
     *
     * @return true if running on macOS with EDR support, false otherwise
     */
    static bool isEDRSupported();

    /**
     * @brief Check if the current display supports EDR
     *
     * This checks if the display connected to the given window supports
     * Extended Dynamic Range.
     *
     * @param nativeWindowHandle The native window handle (e.g., QWindow::winId())
     * @return true if the display supports EDR, false otherwise
     */
    static bool isEDRDisplayAvailable(quintptr nativeWindowHandle);

    /**
     * @brief Get the maximum EDR brightness for the current display
     *
     * Returns the maximum brightness level supported by the display in HDR mode.
     * For SDR displays, this will return 1.0 (100 nits).
     * For HDR displays, this can return values like 2.0 (200 nits),
     * 4.0 (400 nits), 10.0 (1000 nits), etc.
     *
     * @param nativeWindowHandle The native window handle
     * @return The maximum brightness multiplier (1.0 = 100 nits SDR)
     */
    static float getMaxEDRBrightness(quintptr nativeWindowHandle);

    /**
     * @brief Enable EDR on the given native view
     *
     * This should be called on the native NSView/NSOpenGLView to enable
     * Extended Dynamic Range rendering. This sets the
     * wantsExtendedDynamicRangeOpenGLSurface property.
     *
     * @param nativeViewHandle The native view handle (e.g., QWindow::winId())
     * @return true if EDR was successfully enabled, false otherwise
     */
    static bool enableEDR(quintptr nativeViewHandle);

    /**
     * @brief Get the OpenGL pixel format attributes for EDR rendering
     *
     * Returns the NSOpenGLPixelFormatAttribute array needed for EDR support:
     * - NSOpenGLPFAColorFloat (floating point color)
     * - NSOpenGLPFAColorSize, 64 (16-bit float per channel)
     *
     * Note: This is only used if creating a native NSOpenGLView.
     * With Qt's QOpenGLWidget, this is not directly applicable.
     *
     * @param attributes Output array to store the attributes (must be large enough)
     * @param maxAttributes Maximum number of attributes that can be stored
     * @return Number of attributes written, or 0 if EDR is not supported
     */
    static int getEDRPixelFormatAttributes(int *attributes, int maxAttributes);
};

} // namespace video
