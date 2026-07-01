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

#include <QCoreApplication>
#include <QSurfaceFormat>

#include <common/Typedef.h>
#include <ui/YUViewApplication.h>

#ifdef Q_OS_WIN
#include <windows.h>
#include <iostream>

// 动态创建控制台窗口 (Windows only)
void CreateMyConsole() {
    // 1. 分配一个控制台
    AllocConsole();
    
    // 2. 设置控制台标题
    SetConsoleTitle(L"YUView Debug Console");
    
    // 3. 重定向标准输出和标准错误到控制台
    FILE* fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
    
    // 4. 清除流状态
    std::cout.clear();
    std::cerr.clear();
}
#endif

int main(int argc, char *argv[])
{
#ifdef Q_OS_WIN
  CreateMyConsole(); // 创建控制台窗口
#endif

#if QT_VERSION >= QT_VERSION_CHECK(5, 6, 0) && QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
  QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling); // DPI support
  QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps); // DPI support
#endif
  QCoreApplication::setAttribute(Qt::AA_SynthesizeMouseForUnhandledTouchEvents,false);
  QCoreApplication::setAttribute(Qt::AA_SynthesizeTouchForUnhandledMouseEvents,false);

#ifdef Q_OS_MAC
  // On macOS, set the default OpenGL surface format before creating QApplication
  // This ensures the OpenGL context is created with the correct version
  QSurfaceFormat format;
  format.setRenderableType(QSurfaceFormat::OpenGL);
  format.setProfile(QSurfaceFormat::CoreProfile);
  format.setVersion(3, 3);
  QSurfaceFormat::setDefaultFormat(format);
#endif

  qRegisterMetaType<recacheIndicator>("recacheIndicator");
  
  YUViewApplication app(argc, argv);

  return app.returnCode;
}
