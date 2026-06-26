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

QColorSpace getDisplayColorSpace()
{
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
    QByteArray iccBytes(reinterpret_cast<const char *>(iccData.bytes), int(iccData.length));
    auto cs = QColorSpace::fromIccProfile(iccBytes);
    if (cs.isValid())
      return cs;
  }

  return QColorSpace::SRgb;
}

} // namespace functionsGui

#else // Q_OS_MAC

#include <QColorSpace>

namespace functionsGui {

QColorSpace getDisplayColorSpace()
{
  return QColorSpace::SRgb;
}

} // namespace functionsGui

#endif // Q_OS_MAC
