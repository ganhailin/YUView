# YUView WebAssembly 移植报告

本文档记录了将 YUView 移植到 WebAssembly (Wasm) 平台的完整过程，包括遇到的问题、解决方案和代码修改。

## 概述

YUView 是一个基于 Qt 6 的 YUV 视频分析工具，重度依赖多线程（`QThread`、`QThreadPool`、`QtConcurrent`）。移植目标是在浏览器中运行，使用 Qt for WebAssembly multithread 版本。

- **Qt 版本**: 6.11.1 (wasm_multithread)
- **Emscripten 版本**: 4.0.7
- **基础 Docker 镜像**: `anaselgarhy/qt-wasm-builder:multithread`
- **产物大小**: YUView.wasm ~23MB, YUView.js ~532KB

## 构建环境

### Docker 镜像

基础镜像只包含 wasm 目标 Qt，缺少宿主平台工具。通过 `Dockerfile.wasm` 构建自定义镜像：

1. 安装 Alpine 宿主 Qt6 工具（`qt6-qtbase-dev`、`qt6-qttools-dev`）
2. 重写 qmake 包装脚本（Alpine 的 qmake6 不支持 `TargetSpec`）
3. 建立宿主工具符号链接（moc、uic、rcc → `gcc_64/libexec/`）

### 构建脚本

`build-wasm.sh` 提供一键编译：

```bash
./build-wasm.sh          # 增量编译
./build-wasm.sh clean    # 清理
./build-wasm.sh rebuild  # 全量重编
```

关键 qmake 参数：

| 参数 | 说明 |
|------|------|
| `QMAKE_LFLAGS_RELEASE = -O0` | 跳过 wasm-opt（处理大文件会 SIGSEGV） |
| `PTHREAD_POOL_SIZE=64` | 足够线程数（VideoCache + workers） |
| `NO_DISABLE_EXCEPTION_CATCHING=1` | 启用 C++ 异常捕获 |

## 代码修改清单

### 1. 编译期问题

#### 1.1 wasm-opt SIGSEGV

**现象**: 链接阶段 `wasm-opt` 崩溃
**原因**: binaryen 已知 bug，处理大型 wasm 文件（>20MB）时 SIGSEGV
**解决**: `QMAKE_LFLAGS_RELEASE = -O0`，只降低链接优化级别，编译仍用 `-O2`

#### 1.2 线程池耗尽

**现象**: `Tried to spawn a new thread, but the thread pool is exhausted`
**原因**: YUView 的 VideoCache 创建大量线程，默认 `PTHREAD_POOL_SIZE=4` 不够
**解决**: `PTHREAD_POOL_SIZE=64`

#### 1.3 异常捕获被禁用

**现象**: `Assertion failed: Exception thrown, but exception catching is not enabled`
**原因**: `FileSource.cpp` 使用 `std::filesystem` 的 try/catch，Wasm 默认禁用异常
**解决**: `-s NO_DISABLE_EXCEPTION_CATCHING=1`

### 2. 平台适配

#### 2.1 平台宏缺失

**现象**: `is_Q_OS_LINUX` 在 Wasm 上为 false，导致多处代码走不到正确分支
**解决**: 在 `Typedef.h` 添加 `is_Q_OS_WASM`，并修复所有平台分支

| 文件 | 修改 |
|------|------|
| `common/Typedef.h` | 添加 `is_Q_OS_WASM` 定义 |
| `video/yuv/videoHandlerYUV.cpp` | 4 处 `is_Q_OS_LINUX` → `is_Q_OS_LINUX \|\| is_Q_OS_WASM` |
| `video/yuv/videoHandlerYUV.cpp` | `Format_RGB32` → `Format_ARGB32`（Wasm 上 buffer 不够） |

#### 2.2 QFileSystemWatcher 不可用

**现象**: 打开文件时 abort
**原因**: Wasm 不支持 inotify/kqueue
**解决**: `FileSource.cpp` 构造函数和 `updateFileWatchSetting()` 加 `#ifndef Q_OS_WASM`

#### 2.3 QProcess 不可用

**现象**: 编译错误 `call to deleted constructor of 'QProcess'`
**解决**: AFBC 解码相关代码加 `#ifndef Q_OS_WASM`

#### 2.4 QOpenGLContext 崩溃

**现象**: 启动时 abort
**原因**: Wasm 使用 WebGL，不支持桌面 OpenGL 3.3 Core Profile
**解决**: `YUViewApplication.cpp` 中 OpenGL 版本检查加 `#ifndef Q_OS_WASM`

### 3. 对话框适配（15 处）

Qt for WebAssembly 不支持 `QDialog::exec()`（阻塞主线程），所有模态对话框需改为异步。

| 文件 | 对话框 | 修复方式 |
|------|--------|----------|
| `ui/Mainwindow.cpp` | 构造函数 autosave 询问 | `#ifndef Q_OS_WASM` 跳过 |
| `ui/Mainwindow.cpp` | `showSettingsWindow()` | `new` + `open()` + `WA_DeleteOnClose` |
| `ui/Mainwindow.cpp` | `saveScreenshot()` | `#ifndef Q_OS_WASM` 跳过 |
| `ui/Mainwindow.cpp` | `performanceTest()` | `#ifdef Q_OS_WASM` 跳过 |
| `ui/Mainwindow.cpp` | `showFileOpenDialog()` | `getOpenFileContent()` + Emscripten VFS |
| `handler/UpdateHandler.cpp` | 3 处更新对话框 | `#ifndef Q_OS_WASM` 跳过 |
| `video/yuv/videoHandlerYUV.cpp` | Custom Format 对话框 | `new` + `open()` + 直接 `setSrcPixelFormat` |
| `video/rgb/videoHandlerRGB.cpp` | Custom Format 对话框 | `new` + `open()` + 直接 `setSrcPixelFormat` |
| `playlistitem/playlistItems.cpp` | 文件类型选择 | `new QInputDialog` + `open()` + 回调 |
| `playlistitem/playlistItemCompressedVideo.cpp` | FFmpeg 日志 | `show()` 非模态 |
| `ui/Statisticsstylecontrol.cpp` | 颜色映射编辑器 | `show()` 非模态 |
| `ui/widgets/PlaylistTreeWidget.cpp` | 加载播放列表询问 | `#ifndef Q_OS_WASM` 跳过 |

### 4. 文件 I/O 适配

#### 4.1 文件打开对话框

**问题**: `QFileDialog::exec()` 和 `QFileDialog::open()` 在 Wasm 上都不支持
**解决**: 使用 Qt 6 专为 Wasm 设计的 `QFileDialog::getOpenFileContent()`：
- 浏览器原生文件选择器（非阻塞）
- 回调返回文件名 + 内容
- 内容写入 Emscripten 虚拟 FS (`/data/`)，然后走正常加载流程

#### 4.2 文件路径检查

**问题**: `PlaylistTreeWidget::loadFiles()` 中 `QFile::exists()` 检查 VFS 路径
**解决**: Emscripten VFS 支持标准文件操作，无需额外修改

### 5. 网络/SSL 适配

**问题**: Qt for Wasm 不支持 `QNetworkAccessManager::sslErrors` 和 `connectToHostEncrypted`
**解决**: `UpdateHandler.cpp` 中 SSL 相关代码加 `#ifndef Q_OS_WASM`

## 部署

### 服务器要求

multithread 版本依赖 `SharedArrayBuffer`，服务器必须返回：

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

### 浏览器要求

- Chrome 92+ / Firefox 79+ / Edge 92+
- 支持 WebAssembly threads 和 SharedArrayBuffer

### 测试服务器

`build-wasm/YUViewApp/serve.py` 提供带正确响应头的测试服务器：

```bash
cd build-wasm/YUViewApp && python3 serve.py 8000
```

## 已知限制

1. **AFBC 解码不可用**: 依赖外部进程 `QProcess`，Wasm 不支持
2. **FFmpeg 外部库不可用**: Wasm 无法加载动态库（`.so`/`.dll`）
3. **自动更新不可用**: 更新对话框已禁用
4. **文件监视不可用**: `QFileSystemWatcher` 不支持
5. **性能测试不可用**: 依赖平台特定 API
6. **截图功能简化**: 跳过模式选择对话框，默认保存当前视图
7. **文件类型自动检测**: 无法检测时默认按 Raw YUV 打开
8. **wasm-opt 优化跳过**: 链接阶段未优化，wasm 体积较大（~23MB）

## 文件修改统计

| 文件 | 修改类型 |
|------|----------|
| `Dockerfile.wasm` | 新增 |
| `build-wasm.sh` | 新增 |
| `docs/BUILD_WASM.md` | 新增 |
| `docs/WASM_PORTING_REPORT.md` | 新增（本文档） |
| `common/Typedef.h` | 添加 `is_Q_OS_WASM` |
| `filesource/FileSource.cpp` | `QFileSystemWatcher` 保护 |
| `handler/UpdateHandler.cpp` | SSL + 对话框保护 |
| `ui/Mainwindow.cpp` | 文件对话框 + 5 处对话框保护 |
| `ui/YUViewApplication.cpp` | OpenGL 检查保护 |
| `ui/SettingsDialog.cpp` | 对话框保护 |
| `ui/Statisticsstylecontrol.cpp` | 对话框保护 |
| `ui/widgets/PlaylistTreeWidget.cpp` | 异步文件加载 + 对话框保护 |
| `video/yuv/videoHandlerYUV.cpp` | 平台分支 + QProcess + 对话框 |
| `video/rgb/videoHandlerRGB.cpp` | QProcess + 对话框 |
| `playlistitem/playlistItems.cpp` | 异步文件类型选择 |
| `playlistitem/playlistItems.h` | 异步函数声明 |
| `playlistitem/playlistItemCompressedVideo.cpp` | 对话框保护 |