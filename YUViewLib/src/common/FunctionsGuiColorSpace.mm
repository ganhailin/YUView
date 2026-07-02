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
 *   You should have received a copy of the GNU General Public License
 *   along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

// Must include QtGlobal first for Q_OS_MAC
#include <QtGlobal>

#include "FunctionsGui.h"

#ifdef Q_OS_MAC

#include <QGuiApplication>
#include <QScreen>
#include <QColorSpace>

#include <qscreen_platform.h>

// Avoid MacTypes.h Size conflict with video::Size
#define Size MacSize
#import <Cocoa/Cocoa.h>
#undef Size

namespace functionsGui {

// Cache the ICC profile bytes from the last getDisplayColorSpace() call,
// so ColorPipeline can parse primaries from it without re-querying the OS.
static QByteArray s_cachedIccData;

QColorSpace getDisplayColorSpace()
{
  s_cachedIccData.clear();

  auto *screen = QGuiApplication::primaryScreen();
  if (!screen)
    return QColorSpace::SRgb;

  auto *cocoaScreen = screen->nativeInterface<QNativeInterface::QCocoaScreen>();
  if (!cocoaScreen)
    return QColorSpace::SRgb;

  NSScreen *nsScreen = cocoaScreen->nativeScreen();
  if (!nsScreen || !nsScreen.colorSpace)
    return QColorSpace::SRgb;

  // Try to get ICC profile data from the screen's color space
  NSData *iccData = [nsScreen.colorSpace ICCProfileData];
  if (iccData && iccData.length > 0)
  {
    s_cachedIccData = QByteArray(reinterpret_cast<const char *>(iccData.bytes), int(iccData.length));
    auto cs = QColorSpace::fromIccProfile(s_cachedIccData);
    if (cs.isValid())
      return cs;
  }

  return QColorSpace::SRgb;
}

const QByteArray &getCachedIccData()
{
  return s_cachedIccData;
}

bool isWindowsACMEnabled() { return false; }

} // namespace functionsGui

#endif // Q_OS_MAC
