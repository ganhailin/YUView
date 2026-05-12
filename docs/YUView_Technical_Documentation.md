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

工程采用模块化设计，核心逻辑封装在 `YUViewLib` 中，通过高度抽象的接口支持多种格式的扩展。

### A. 核心类层次结构 (Inheritance Hierarchy)
`YUView` 的视频处理逻辑建立在严谨的继承体系之上：
1.  **`FrameHandler`**: 基础逻辑类，定义了处理“单帧”图像的核心行为。
    - 负责管理基本的帧尺寸 (`frameSize`)、色彩空间信息。
    - 提供 `drawFrame` 基础接口和像素值查询 (`getPixelValues`)。
    - 它是实现“对比视图”的基础，能够计算两个 handler 之间的差异。
2.  **`videoHandler`**: 继承自 `FrameHandler`，增加了对“视频序列”的支持。
    - **缓存机制**: 引入了 `cacheFrame` 逻辑，支持后台异步读取和多帧缓存管理。
    - **格式嗅探**: 定义了 `guessAndSetPixelFormat` 接口，用于根据文件特征自动识别视频参数。
3.  **特化处理器 (Specialized Handlers)**:
    - **`videoHandlerYUV`**: 针对原始 YUV 数据的核心实现。处理平面格式、交织格式，支持各种采样率（4:2:0, 4:2:2, 4:4:4）和位深（8-16 bit）。
    - **`videoHandlerRGB`**: 处理原始 RGB/BGR 文件及序列。
    - **`videoHandlerFFmpeg`**: 通过封装 FFmpeg 库，支持 MP4, MKV, AVI 等主流容器格式的解码。

### B. 数据模型与渲染管线
- **`VideoFrame` (双缓冲模型)**:
    - 为了平衡兼容性与性能，`VideoFrame` 同时持有 8-bit 的 `QImage`（用于 QPainter 渲染 UI 组件）和可选的 16-bit 高位深 Buffer（用于 OpenGL 高性能渲染）。
    - 16-bit 缓冲区通常以 RGBA 64-bit (16-bit per channel) 存储，确保 HDR 和高位深素材不失真。
- **渲染流程**:
    1.  用户触发跳转，`videoHandler` 接收到 `frameIndex` 请求。
    2.  若不在缓存中，从 `IDataSource` (如 `DataSourceLocalFile`) 读取原始字节。
    3.  通过 `PixelFormatYUV/RGB` 类进行颜色空间转换。
    4.  生成的 `VideoFrame` 存入缓存。
    5.  `SplitViewWidget` 调用 OpenGL Shader 将像素绘制到屏幕上，利用 GPU 进行插值和颜色映射。

### C. 关键子系统原理
- **FFmpeg 动态加载**:
    - **解耦设计**: `YUView` 不直接链接 FFmpeg。`FFmpegLibraryFunctions` 类在运行时通过 `QLibrary::resolve` 查找系统路径下的 `libavcodec.59.dylib` 等。
    - **Wrapper 封装**: 对 FFmpeg 的 `AVPacket`, `AVFrame`, `AVCodecContext` 进行了 RAII 封装（如 `AVFrameWrapper`），有效防止了内存泄漏。
- **统计分析与叠加 (Statistics)**:
    - 这是一个解耦的覆盖层系统。`StatisticsHandler` 负责解析特定格式（如 HEVC 内部信息、外部 CSV 统计），并将数据映射到视频网格中。
    - 支持热力图显示、运动矢量箭头绘制等。
- **对比与差异计算**:
    - `videoHandlerDifference` 是一个特殊的处理器，它持有两个输入 handler，在读取时实时计算两者的差值（支持 YUV 分量差值或 RGB 色度差值），并可选地进行放大显示（Amplification）。

### D. 并行与性能
- **多线程解码**: 利用 `QtConcurrent` 和 `QThreadPool` 进行异步帧读取和转换，避免界面卡顿。
- **内存映射**: 对于超大原始 YUV 文件，底层 `DataSource` 能够利用文件指针定位，实现“秒开”大文件。

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
