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

#include "YUViewMacOSEDRHelper.h"

#ifdef Q_OS_MAC

#import <Cocoa/Cocoa.h>
#import <OpenGL/OpenGL.h>
#import <AppKit/NSOpenGLView.h>
#import <AppKit/NSWindow.h>

namespace video
{

bool YUViewMacOSEDRHelper::isEDRSupported()
{
    // EDR is supported on macOS 10.11+ with compatible hardware
    // Check if NSOpenGLView responds to wantsExtendedDynamicRangeOpenGLSurface
    return [NSOpenGLView instancesRespondToSelector:@selector(setWantsExtendedDynamicRangeOpenGLSurface:)];
}

bool YUViewMacOSEDRHelper::isEDRDisplayAvailable(quintptr nativeWindowHandle)
{
    if (!isEDRSupported())
        return false;

    // Get the NSWindow from the native window handle
    NSWindow *window = nil;

    // The nativeWindowHandle on macOS is a WindowRef or NSWindow pointer
    // Qt's QWindow::winId() returns a native window handle
    if (nativeWindowHandle != 0)
    {
        // Try to get the window from the handle
        // On macOS, this could be a WindowRef or we need to find the window
        // by iterating through all windows
        for (NSWindow *w in [NSApp windows])
        {
            if ([w windowNumber] == nativeWindowHandle || (quintptr)w == nativeWindowHandle)
            {
                window = w;
                break;
            }
        }
    }

    if (!window)
        return false;

    // Check if the screen supports EDR
    NSScreen *screen = [window screen];
    if (!screen)
        return false;

    // Check for maximum extended dynamic range color component value
    // If > 1.0, the display supports EDR
    NSNumber *edrLimit = [screen valueForKey:@"maximumExtendedDynamicRangeColorComponentValue"];
    if (edrLimit)
    {
        float maxEDR = [edrLimit floatValue];
        return maxEDR > 1.0f;
    }

    // Fallback: check if the screen has HDR capabilities
    // macOS 10.15+ has additional HDR properties
    if ([screen respondsToSelector:@selector(maximumPotentialExtendedDynamicRangeColorComponentValue)])
    {
        float potentialEDR = [screen maximumPotentialExtendedDynamicRangeColorComponentValue];
        return potentialEDR > 1.0f;
    }

    return false;
}

float YUViewMacOSEDRHelper::getMaxEDRBrightness(quintptr nativeWindowHandle)
{
    if (!isEDRSupported())
        return 1.0f;  // SDR only

    // Find the window
    NSWindow *window = nil;
    if (nativeWindowHandle != 0)
    {
        for (NSWindow *w in [NSApp windows])
        {
            if ([w windowNumber] == nativeWindowHandle || (quintptr)w == nativeWindowHandle)
            {
                window = w;
                break;
            }
        }
    }

    if (!window)
        return 1.0f;

    NSScreen *screen = [window screen];
    if (!screen)
        return 1.0f;

    // Get the current maximum EDR value
    if ([screen respondsToSelector:@selector(maximumExtendedDynamicRangeColorComponentValue)])
    {
        // This is the current limit based on ambient conditions and power management
        NSNumber *currentLimit = [screen valueForKey:@"maximumExtendedDynamicRangeColorComponentValue"];
        if (currentLimit)
            return [currentLimit floatValue];
    }

    // Fallback to potential maximum
    if ([screen respondsToSelector:@selector(maximumPotentialExtendedDynamicRangeColorComponentValue)])
    {
        return [screen maximumPotentialExtendedDynamicRangeColorComponentValue];
    }

    return 1.0f;  // Default to SDR
}

bool YUViewMacOSEDRHelper::enableEDR(quintptr nativeViewHandle)
{
    if (!isEDRSupported())
        return false;

    // The nativeViewHandle should point to an NSView or NSOpenGLView
    // With Qt, this is typically obtained from QWindow::winId()

    NSView *view = nil;
    if (nativeViewHandle != 0)
    {
        // Try to get the view from the handle
        // This assumes the handle is a pointer to an NSView
        view = reinterpret_cast<NSView*>(nativeViewHandle);

        // Verify it's a valid NSView
        if (![view isKindOfClass:[NSView class]])
            view = nil;
    }

    if (!view)
        return false;

    // Enable EDR on the view
    if ([view respondsToSelector:@selector(setWantsExtendedDynamicRangeOpenGLSurface:)])
    {
        // Cast to NSOpenGLView if possible
        if ([view isKindOfClass:[NSOpenGLView class]])
        {
            NSOpenGLView *glView = (NSOpenGLView *)view;
            [glView setWantsExtendedDynamicRangeOpenGLSurface:YES];
            return true;
        }
        else
        {
            // For regular NSView, try to set the property anyway
            // This might work with Qt's QOpenGLWidget on newer macOS versions
            @try {
                [view setValue:@YES forKey:@"wantsExtendedDynamicRangeOpenGLSurface"];
                return true;
            }
            @catch (NSException *exception) {
                NSLog(@"Failed to enable EDR: %@", exception);
                return false;
            }
        }
    }

    return false;
}

int YUViewMacOSEDRHelper::getEDRPixelFormatAttributes(int *attributes, int maxAttributes)
{
    if (!isEDRSupported())
        return 0;

    // NSOpenGLPixelFormatAttribute is uint32_t on modern macOS
    // Define the minimum attributes needed for EDR
    int attribs[] = {
        (int)NSOpenGLPFAOpenGLProfile, (int)NSOpenGLProfileVersion4_1Core,
        (int)NSOpenGLPFADoubleBuffer,
        (int)NSOpenGLPFAColorFloat,      // Floating point color buffer
        (int)NSOpenGLPFAColorSize, 64,   // 64-bit total (16-bit per channel float)
        (int)NSOpenGLPFAAlphaSize, 16,   // 16-bit alpha
        (int)NSOpenGLPFAMultisample,
        (int)NSOpenGLPFASampleBuffers, 1,
        (int)NSOpenGLPFASamples, 4,
        0  // Terminator
    };

    int numAttribs = sizeof(attribs) / sizeof(attribs[0]);
    if (numAttribs > maxAttributes)
        numAttribs = maxAttributes;

    for (int i = 0; i < numAttribs; i++)
        attributes[i] = attribs[i];

    return numAttribs;
}

} // namespace video

#else  // Non-macOS platforms

namespace video
{

bool YUViewMacOSEDRHelper::isEDRSupported()
{
    return false;
}

bool YUViewMacOSEDRHelper::isEDRDisplayAvailable(quintptr /*nativeWindowHandle*/)
{
    return false;
}

float YUViewMacOSEDRHelper::getMaxEDRBrightness(quintptr /*nativeWindowHandle*/)
{
    return 1.0f;
}

bool YUViewMacOSEDRHelper::enableEDR(quintptr /*nativeViewHandle*/)
{
    return false;
}

int YUViewMacOSEDRHelper::getEDRPixelFormatAttributes(int */*attributes*/, int /*maxAttributes*/)
{
    return 0;
}

} // namespace video

#endif  // Q_OS_MAC
