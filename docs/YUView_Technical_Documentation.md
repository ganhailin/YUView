# YUView 技术文档

`YUView` 是一款基于 Qt 的跨平台开源 YUV 播放器及分析工具，支持多种视频格式、像素格式 and 高级分析功能（如 HEVC 比特流解析、对比视图等）。

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
    5.  `SplitViewWidget` 调用 OpenGL Shader 将像素绘制到屏幕上，利用 GPU 进行插值 and 颜色映射。

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

### E. 交互逻辑与手势 (Interaction & Gestures)
`YUView` 为主视图提供了丰富的交互支持，特别是在 macOS 触控板环境下，通过对 Qt 手势事件的深度集成，实现了缩放、平移和快捷导航。

1. **核心类实现**:
    - **`MoveAndZoomableView`**: 交互基类，实现了缩放算法、平移逻辑以及 `QGestureEvent` 的基础分发。它通过 `grabGesture` 捕获 `SwipeGesture` 和 `PinchGesture`。
    - **`splitViewWidget`**: 业务层派生类，重写了 `onSwipe...` 系列虚函数，将物理轻扫手势映射为播放逻辑（如跳帧）。

2. **鼠标/触控板模式 (Mouse Modes)**:
    `YUView` 提供两种主要的鼠标操作模式，决定了“左键”和“右键”的语义。
    - **模式 A (MOUSE_RIGHT_MOVE)**: 左键画框缩放，右键平移。
    - **模式 B (MOUSE_LEFT_MOVE)**: 左键平移，右键画框缩放。
    - **配置指南**: 对于开启了 macOS **“三指拖移”** 的用户，建议切换到 **模式 B** (可在 Display Settings 或右键菜单的 "Mouse Mode" 中修改)，这样三指滑动即可直接平移画面。

3. **macOS 手势映射**:
    - **缩放 (Pinch)**: 触发 `Qt::PinchGesture`，实现以手指为中心的平滑缩放。
    - **双指滚动 (Wheel)**: 映射为 `QWheelEvent`，在 YUView 中默认用于快速缩放。
    - **轻扫导航 (Swipe)**: 三指/四指水平滑动切换帧，垂直滑动切换播放列表项。

4. **交互行为总结表**:

| 操作方式 | 默认功能 (模式 B - MOUSE_LEFT_MOVE) | macOS 系统设置影响 ("三指拖移") |
| :--- | :--- | :--- |
| **双指捏合** | 平滑缩放 | - |
| **双指滑动** | 快速缩放 (Wheel 模拟) | - |
| **单指按压滑动** | 平移画面 (Dragging) | - |
| **三指滑动** | 切换帧/文件 (Swipe) | 若系统开启“三指拖移”，则会被模拟为单指按压，直接触发**平移画面** |
| **四指轻扫** | 切换帧/文件 (Swipe) | 若三指被“三指拖移”占用，系统通常需要四指来触发 Swipe 事件 |

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

---

## 5. 支持的格式 (Supported Formats)

`YUView` 的强大之处在于其极其灵活的格式配置能力，涵盖了从原始像素数据到先进压缩比特流的广泛范围。

### A. 原始 YUV (Raw YUV)
支持几乎所有常见的 YUV 变体：
- **采样格式 (Subsampling)**: 4:0:0 (灰度), 4:2:0, 4:2:2, 4:4:4。
- **位深 (Bit Depth)**: 8, 10, 12, 14, 16 bit。
- **存储布局**:
    - **Planar (平面)**: Y, U, V 分开存储（如 I420, YV12）。
    - **Semi-Planar**: Y 平面独立，UV 交织存储（如 NV12, NV21）。
    - **Packed (打包)**: 像素在内存中连续排列（如 YUY2, UYVY）。
- **特殊预设格式**:
    - **V210**: 10-bit 4:2:2 Packed 格式（常用于专业领域）。
    - **NV15 / NV20 / NV30**: 针对特定平台优化的 10-bit 紧凑格式。

### B. 原始 RGB (Raw RGB)
支持自定义排列的 RGB 数据：
- **通道顺序**: RGB, BGR, GBR, ARGB, RGBA 等。
- **布局**: 支持 Packed 和 Planar 布局。
- **位深**: 8, 10, 12, 14, 16 bit（注：RGB565 目前主要通过 FFmpeg 间接支持）。

### C. 压缩视频比特流 (Annex B)
`YUView` 具备强大的比特流解析和解码能力，支持直接打开原始码流文件：
- **H.264 / AVC**
- **H.265 / HEVC**: 支持内部解码器（HM, Libde265）和 FFmpeg 解码。
- **H.266 / VVC**: 支持 VTM 参考软件 and VVDec 解码器。
- **AV1**: 支持 Dav1d and FFmpeg 解码。
- **MPEG-2**

### D. 容器与图像格式
- **视频容器**: 通过 FFmpeg 动态库支持 MP4, MKV, AVI, MOV 等。
- **图像格式**: 
    - 原生支持 **TGA (Targa)** 格式。
    - 通过 Qt 框架支持 PNG, JPG, BMP, WebP 等。

### E. 色彩空间 (Color Spaces)
内置多种色彩空间转换系数，支持 HDR/SDR 切换：
- **BT.601** (SD)
- **BT.709** (HD)
- **BT.2020** (UHD/HDR)
- 支持 Limited Range 和 Full Range 切换。
