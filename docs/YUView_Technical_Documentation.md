# YUView 技术文档

`YUView` 是一款基于 Qt 的跨平台开源 YUV 播放器及分析工具，支持多种视频格式、像素格式和高级分析功能（如 HEVC 比特流解析、对比视图等）。

---

## 1. 编译指南 (macOS 重点)

`YUView` 采用 `QMake` 作为构建系统，对依赖项的处理非常简洁（核心逻辑不依赖外部二进制库，FFmpeg 为运行时动态加载）。

### 环境准备
1. **Xcode Command Line Tools**: 确保已安装编译器。
   ```bash
   xcode-select --install
   ```
2. **Qt 框架**: 支持 Qt 5.9+ 或 Qt 6.x。推荐通过 `Homebrew` 安装：
   ```bash
   brew install qt
   ```
   *注意：安装后需将 qt 的 bin 目录（如 `/opt/homebrew/opt/qt/bin`）添加到 PATH。*
3. **Git**: 用于克隆子模块（GoogleTest）。

### 编译步骤
1. **获取源码及子模块**:
   ```bash
   git clone https://github.com/IENT/YUView.git
   cd YUView
   git submodule update --init --recursive
   ```
2. **创建构建目录并编译**:
   ```bash
   mkdir build && cd build
   qmake ../YUView.pro
   make -j$(sysctl -n hw.ncpu)
   ```
3. **运行**:
   编译完成后，`YUView.app` 将生成在 `YUViewApp` 目录下。
   ```bash
   open YUViewApp/YUView.app
   ```

### 使用 Qt Creator (推荐)
- 直接打开根目录的 `YUView.pro`。
- 配置 Kit（选择对应的 Qt 版本）。
- 点击“运行”或“调试”按钮即可。

---

## 2. 组件原理与架构分析

工程主要分为三个模块：`YUViewLib` (核心库)、`YUViewApp` (壳程序) 和 `YUViewUnitTest` (测试)。

### A. YUViewLib (核心引擎)
这是项目的灵魂，包含了所有视频处理、解析和 UI 逻辑。
- **DataSource (数据源层)**:
    - 抽象类 `IDataSource` 定义了基础 IO 操作。
    - `DataSourceLocalFile` 实现了本地文件的随机访问。
- **Video Logic (视频逻辑层)**:
    - `VideoFrame`: 核心数据模型，存储原始像素数据。
    - `videoHandler`: 针对不同格式（YUV, RGB, 序列帧等）的处理器基类。它负责从数据源读取数据并转换为 `VideoFrame`。
    - `FrameHandler`: 协调层，管理多个 `videoHandler`，用于实现对比播放和同步。
- **FFmpeg Integration (动态加载)**:
    - 工程没有在编译时硬链接 FFmpeg。
    - 而是通过 `FFmpegLibraryFunctions` 利用 `QLibrary` 在运行时动态寻找并加载系统中的 FFmpeg 库（如 `libavcodec`, `libavformat`）。这种设计使得发布二进制包时不需要携带庞大的 FFmpeg 库。
- **UI & View (显示层)**:
    - `SplitViewWidget`: 核心渲染窗口，支持分屏对比。它继承自 `MoveAndZoomableView`，利用 OpenGL 实现高性能的缩放和平移。
    - `Statistics`: 统计分析模块，可在视频上叠加显示运动矢量、块划分等元数据。

### B. YUViewApp (应用程序)
- 这是一个非常薄的包装层。
- `main.cpp` 中负责设置高 DPI 支持、OpenGL 默认格式。
- 初始化 `YUViewApplication` 类，该类负责解析命令行参数并启动 `MainWindow`。

### C. YUViewUnitTest (质量保证)
- 基于 `GoogleTest`。
- 重点对 `common` 工具类、`video` 像素格式转换、`parser` 比特流解析等非 UI 核心逻辑进行覆盖测试。

---

## 3. 调试指南

### 源码级调试
1. **IDE 调试**: 建议使用 **Qt Creator**。在 `Debug` 模式下编译，可直接利用其集成的 GDB/LLDB 调试器进行断点调试、变量查看和堆栈回溯。
2. **控制台输出**:
    - 项目中广泛使用了 `qDebug()`。
    - `YUViewLib/src/ui/YUViewApplication.cpp` 中定义了 `APPLICATION_DEBUG` 宏。将其置为 `1` 可以开启更详细的启动日志。

### 常见调试场景
- **黑屏/渲染异常**: 重点检查 `YUViewLib/src/ui/views/SplitViewWidget.cpp` 中的 OpenGL 渲染代码。
- **格式解析失败**: 查看 `YUViewLib/src/video` 对应格式的 `videoHandler` 或是 `YUViewLib/src/parser` 中的解析逻辑。
- **FFmpeg 加载问题**: 检查 `YUViewLib/src/ffmpeg/FFmpegVersionHandler.cpp`，查看库查找路径是否正确。

### 运行测试
编译时如果开启了 `UNITTESTS` 配置（在 `YUView.pro` 中默认开启），可以运行生成的测试程序：
```bash
./YUViewUnitTest/YUViewUnitTest
```
这对于验证底层像素算法的修改是否引入 Regression 非常有效。

---

## 4. 技术特性总结
- **语言**: C++17
- **框架**: Qt 5/6
- **渲染**: OpenGL (Core Profile 3.3+)
- **扩展性**: 通过 `videoHandler` 接口可以轻松增加新的视频格式支持。
- **跨平台性**: 源码高度抽象，一份代码同时支持 Win/Mac/Linux。
