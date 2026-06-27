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

#include "FunctionsGui.h"

#include <QColorSpace>

// On macOS, this file compiles to an empty translation unit.
// The macOS implementation is in FunctionsGuiColorSpace.mm.
#ifndef Q_OS_MAC

#ifdef Q_OS_WIN
#include <QLibrary>
#include <QDebug>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace functionsGui {

QColorSpace getDisplayColorSpace()
{
#ifdef Q_OS_WIN
  // Query the primary monitor's ICC profile.
  // Try GetICMProfileW from gdi32 first, fall back to mscms.
  using GetICMProfileFunc = BOOL (WINAPI *)(HDC, LPDWORD, LPWSTR);
  static auto pGetICMProfile = []() -> GetICMProfileFunc {
    auto fn = reinterpret_cast<GetICMProfileFunc>(
        QLibrary::resolve(QStringLiteral("gdi32"), "GetICMProfileW"));
    if (!fn)
      fn = reinterpret_cast<GetICMProfileFunc>(
          QLibrary::resolve(QStringLiteral("mscms"), "GetICMProfileW"));
    return fn;
  }();
  if (!pGetICMProfile)
    return QColorSpace::SRgb;

  HDC hdc = GetDC(nullptr);
  if (!hdc)
    return QColorSpace::SRgb;

  WCHAR profilePath[MAX_PATH] = {};
  DWORD pathLen = MAX_PATH;
  BOOL ok = pGetICMProfile(hdc, &pathLen, profilePath);
  ReleaseDC(nullptr, hdc);
  if (!ok || pathLen == 0)
    return QColorSpace::SRgb;

  HANDLE hFile = CreateFileW(profilePath, GENERIC_READ, FILE_SHARE_READ,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hFile == INVALID_HANDLE_VALUE)
    return QColorSpace::SRgb;

  DWORD fileSize = GetFileSize(hFile, nullptr);
  if (fileSize == INVALID_FILE_SIZE || fileSize == 0)
  {
    CloseHandle(hFile);
    return QColorSpace::SRgb;
  }

  QByteArray iccBytes(int(fileSize), Qt::Uninitialized);
  DWORD bytesRead = 0;
  if (!ReadFile(hFile, iccBytes.data(), fileSize, &bytesRead, nullptr) || bytesRead != fileSize)
  {
    CloseHandle(hFile);
    return QColorSpace::SRgb;
  }
  CloseHandle(hFile);

  auto cs = QColorSpace::fromIccProfile(iccBytes);
  auto result = cs.isValid() ? cs : QColorSpace::SRgb;

  static bool logged = false;
  if (!logged)
  {
    logged = true;
    qDebug() << "[getDisplayColorSpace] Windows ICC profile path:"
             << QString::fromWCharArray(profilePath)
             << "\n  colorSpace valid:" << cs.isValid()
             << "description:" << (cs.isValid() ? cs.description() : QStringLiteral("N/A"))
             << "primaries:" << (cs.isValid() ? cs.primaries() : QColorSpace::Primaries::SRgb)
             << "gamma:" << (cs.isValid() ? cs.gamma() : 0.0f)
             << "\n  result:" << result.description();
  }

  return result;
#else
  return QColorSpace::SRgb;
#endif
}

} // namespace functionsGui

#endif // Q_OS_MAC
