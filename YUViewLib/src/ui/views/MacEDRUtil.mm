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

#include <QtGlobal>

#ifdef Q_OS_MAC

#include "MacEDRUtil.h"

#import <Cocoa/Cocoa.h>

MacEDRInfo MacEDRUtil::queryEDRSupport()
{
  MacEDRInfo info;

  if (@available(macOS 10.15, *))
  {
    NSScreen *screen = [NSScreen mainScreen];
    if (screen)
    {
      info.maxEDRValue = screen.maximumPotentialExtendedDynamicRangeColorComponentValue;
      info.edrSupported = (info.maxEDRValue > 1.0f);
    }
  }

  return info;
}

MacEDRInfo MacEDRUtil::queryEDRSupportForWindow(void *windowHandle)
{
  MacEDRInfo info;

  if (@available(macOS 10.15, *))
  {
    if (!windowHandle)
      return queryEDRSupport();  // Fallback to main screen

    NSView *view = (__bridge NSView *)windowHandle;
    NSScreen *screen = view.window.screen;
    if (!screen)
      screen = [NSScreen mainScreen];  // Fallback

    if (screen)
    {
      info.maxEDRValue = screen.maximumPotentialExtendedDynamicRangeColorComponentValue;
      info.edrSupported = (info.maxEDRValue > 1.0f);
    }
  }

  return info;
}

#endif // Q_OS_MAC