# Renderer 技术文档（OpenGLRenderer）

> **命名变更说明**
>
> 本文档原名 "HDR10Widget 技术文档"，已随 renderer 重命名（提交 `cf195e3d`，2026-07-05）更新为 "Renderer 技术文档"。renderer 相关类、枚举、文件已完成重命名，使其与 ComboBox 选项命名一致。主要映射如下（完整映射详见 `docs/Renderer_Rename_Plan.md`）：
>
> | 旧名 | 新名 |
> |------|------|
> | `HDR10Widget` | `OpenGLRenderer` |
> | `HDR10WidgetMacEDR` | `NativeEDRRenderer` |
> | `HDRSettingsDock` | `RendererSettingsDock` |
> | `EDRSettingsDialog` | 已删除（设置并入 `RendererSettingsDock`） |
> | `HDR10_EOTF` | `RendererEOTF` |
> | `HDR10_ColorGamut` | `RendererColorGamut` |
> | `HDRRenderingMode` | `RendererMode` |
> | `HDRRenderingMode::Disabled` | `RendererMode::Software` |
> | `HDRRenderingMode::GL` | `RendererMode::OpenGL` |
> | `HDRRenderingMode::EDR` | `RendererMode::NativeEDR` |
> | `hdr10_vertex.glsl` | `opengl_vertex.glsl` |
> | `hdr10_fragment.glsl` | `opengl_fragment.glsl` |
> | `hdr10_fragment_dither.glsl` | `opengl_fragment_dither.glsl` |
> | `hdr10_fragment_edr.glsl` | 已删除（EDR 由 Metal 路径处理） |
> | `hdr10Widget` | `glRenderer` |
> | `hdr10WidgetMacEDR` | `edrRenderer` |
> | `hdrRenderingMode` | `rendererMode` |
> | `setHDRRenderingMode` | `setRendererMode` |
> | `hdrStatusChanged` | `rendererStatusChanged` |
>
> 下方正文中的旧标识符已更新为新名称。涉及"原名"的对照说明保留为历史标注。

## 概述

`OpenGLRenderer`（原名 `HDR10Widget`）是 YUView 中用于 HDR（高动态范围）视频渲染的 OpenGL 控件。它继承自 `QOpenGLWidget`，支持 10-bit 或更高位深的视频渲染，提供了像素值显示、缩放平移、抖动处理等功能。

在 macOS 上，EDR（Extended Dynamic Range）HDR 显示由 Metal 路径的 `NativeEDRRenderer`（原名 `HDR10WidgetMacEDR`）处理，`OpenGLRenderer` 作为 SDR fallback。在其他平台上，`OpenGLRenderer` 可通过着色器输出 >1.0 浮点值（依赖 GPU 浮点 FBO 支持）。

**详细 EDR 设计文档:** `docs/macOS_EDR_Design_and_Implementation.md`

## 目录

1. [架构设计](#架构设计)
2. [类定义](#类定义)
3. [接口说明](#接口说明)
4. [数据交互](#数据交互)
5. [核心功能](#核心功能)
6. [使用示例](#使用示例)
7. [注意事项](#注意事项)

---

## 架构设计

### 继承关系

```
QOpenGLWidget
    └── QOpenGLFunctions (protected)
            └── OpenGLRenderer
```

### 双路径 HDR/EDR 架构（macOS）

```
splitViewWidget
    ├── NativeEDRRenderer (Metal 路径)  ← macOS EDR 首选
    │     ├── MacEDRRenderer (Metal 渲染核心)
    │     │     ├── CAMetalLayer (EDR 输出层, RGBA16Float)
    │     │     └── CALayer overlay (像素值/缩放叠加)
    │     └ MacEDRUtil (EDR 检测)
    │
    └── OpenGLRenderer (OpenGL 路径)       ← SDR fallback / 非 macOS
          ├── opengl_fragment.glsl (标准渲染)
          ├── opengl_fragment_dither.glsl (抖动渲染)
          └ PixelOverlay (像素值叠加)
```

> 注：旧版 `hdr10_fragment_edr.glsl`（EDR 着色器，非 macOS 路径）已在提交 `09a06cac` 中删除。EDR 渲染现统一由 macOS Metal 路径处理。

### 内部组成

```
OpenGLRenderer (OpenGL 渲染层)
    ├── PixelOverlay (QPainter 覆盖层)
    │       ├── 像素值显示
    │       ├── 缩放倍率指示器
    │       └── 像素坐标标尺
    │
    ├── OpenGL 着色器
    │       ├── 标准渲染 (opengl_fragment.glsl)
    │       └── 抖动渲染 (opengl_fragment_dither.glsl)
    │
    └── EDR 色彩处理参数
            ├── EOTF (PQ/HLG/Gamma/sRGB)
            ├── ColorGamut (BT.2020/BT.709/P3)
            ├── Gamma 值
            ├── Diffuse White (nits)
            └── HDR Brightness
```

> 注：旧版的 EDR 渲染着色器 (`hdr10_fragment_edr.glsl`) 已删除，EDR 统一由 macOS Metal 路径处理。

### 渲染流程

```
1. paintGL()              - OpenGL 渲染视频帧
   ├── 绑定 defaultFramebufferObject (QOpenGLWidget 内部 FBO)
   ├── 启用预乘 alpha 混合 (GL_ONE, GL_ONE_MINUS_SRC_ALPHA)
   ├── 选择着色器 (标准/抖动)
   ├── 设置色彩处理 uniforms (EOTF/色域矩阵/亮度/tonemapping)
   └── glDrawArrays(GL_TRIANGLE_STRIP)
   注: macOS 上 EDR 由 Metal 路径 (NativeEDRRenderer) 处理，不在此路径
2. PixelOverlay::paintEvent()  - QPainter 绘制覆盖层
   ├── drawPixelValues() - 像素值显示
   ├── drawZoomIndicator() - 缩放倍率
   └── drawPixelRulers() - 坐标标尺
```

---

## 类定义

### OpenGLRenderer

```cpp
namespace video {

class OpenGLRenderer : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT
public:
    explicit OpenGLRenderer(QWidget *parent = nullptr);
    ~OpenGLRenderer() override;

    // 数据设置
    void setFrame(const VideoFrame &frame);
    void setFrameHandler(FrameHandler *handler);
    void setBitDepth(int bits);

    // 渲染控制
    void setDithering(bool enable);
    void setZoom(double zoom);
    void setMoveOffset(QPointF offset);
    void setShowRawData(bool show);

    // 状态查询
    bool supports10bit() const;
    QString getOpenGLInfo() const;

protected:
    // OpenGL 生命周期
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

    // 覆盖层绘制
    void updatePixelOverlay();
    void drawPixelValues(QPainter *painter);
    void drawZoomIndicator(QPainter *painter);
    void drawPixelRulers(QPainter *painter);

private:
    // 内部类：透明覆盖层
    class PixelOverlay;

    // OpenGL 资源 (使用 shared_ptr 管理生命周期)
    std::shared_ptr<QOpenGLShaderProgram> m_program;        // 标准着色器
    std::shared_ptr<QOpenGLShaderProgram> m_programDither;  // 抖动着色器
    QOpenGLBuffer         m_vbo{QOpenGLBuffer::VertexBuffer};
    QOpenGLVertexArrayObject m_vao;
    GLuint m_textureId{0};
    QSize  m_textureSize;       // 缓存纹理尺寸用于复用

    // 帧数据
    VideoFrame m_currentFrame;
    bool       m_frameNeedsUpdate{false};
    QSize      m_frameSize;

    // 渲染参数
    int  m_bitDepth{10};           // 显示位深 (shader 归一化用)
    int  m_sourceBitDepth{8};      // 源位深 (像素值显示用)
    bool m_ditheringEnabled{false};
    bool m_initialized{false};
    bool m_supports10bit{false};
    bool m_showRawData{false};

    QString m_openglInfo;

    // 色彩处理参数 (标准/抖动着色器共用)
    RendererEOTF       m_eotf{RendererEOTF::SRGB};
    RendererColorGamut m_colorGamut{RendererColorGamut::BT709};
    float            m_gammaValue{2.2f};
    float            m_diffuseWhiteNits{203.0f};
    float            m_hdrBrightness{1.0f};
    bool             m_premultipliedAlpha{true};  // 预乘 alpha 开关

    // ACM 状态跟踪 (Windows，用于 dock 状态显示)
    bool             m_lastAcmActive{false};

    // 视图控制
    double m_zoom{1.0};
    QPointF m_moveOffset{0, 0};

    std::shared_ptr<PixelOverlay> m_pixelOverlay;
    FrameHandler *m_frameHandler{nullptr};
};

} // namespace video
```

> 注：实际代码中 `m_program`/`m_programDither`/`m_pixelOverlay` 使用 `std::shared_ptr` 而非裸指针；不再有独立的 `m_programEDR`（EDR 着色器已删除，色彩处理逻辑已合并进标准/抖动着色器）；`isEDRSupported()`/`getMaxEDRValue()` 不在此类（位于 `NativeEDRRenderer`）。

### PixelOverlay (内部类)

透明 QWidget 覆盖层，用于在 OpenGL 渲染之上绘制像素值、标尺等 QPainter 内容。

---

## EDR 色彩处理枚举

`OpenGLRenderer` 使用以下类型别名（定义在 `OpenGLRenderer.h` 中，底层枚举位于 `color` 命名空间）：

```cpp
using RendererEOTF       = color::EOTF;
using RendererColorGamut = color::ColorGamut;
```

### RendererEOTF (color::EOTF)

```cpp
enum class EOTF {
    PQ   = 0,  // SMPTE ST 2084 Perceptual Quantizer
    HLG  = 1,  // ARIB STD-B67 Hybrid Log-Gamma
    Gamma = 2, // 纯幂函数
    SRGB = 3   // IEC 61966-2-1 sRGB 近似
};
```

### RendererColorGamut (color::ColorGamut)

```cpp
enum class ColorGamut {
    BT2020 = 0, // ITU-R BT.2020 宽色域
    BT709  = 1, // ITU-R BT.709 标准色域
    P3     = 2  // DCI-P3 色域
};
```

### EDR 着色器色彩处理管线（已删除）

> 旧版的 `hdr10_fragment_edr.glsl` 着色器已在提交 `09a06cac` 中删除。该着色器原用于非 macOS 平台的 EDR 渲染，实现 EOTF 转换、色域转换、HDR 亮度缩放等。
>
> 现在的架构中，EDR 渲染统一由 macOS Metal 路径（`NativeEDRRenderer` + `MacEDRRenderer`）处理。`QOpenGLWidget` 内部 FBO 为 8-bit RGBA，无法输出 >1.0 值，因此 OpenGL 路径不再支持 EDR。

---

## 接口说明

### 构造函数 / 析构函数

#### `OpenGLRenderer(QWidget *parent = nullptr)`

**功能**：构造函数，初始化 OpenGL 上下文和覆盖层

**行为**：
- 设置 OpenGL 3.3 Core Profile
- 请求 10-bit 颜色缓冲区（R/G/B/A 各10-bit）
- 创建 `PixelOverlay` 覆盖层

#### `~OpenGLRenderer()`

**功能**：析构函数，释放 OpenGL 资源

**行为**：
- 删除纹理
- 删除着色器程序

---

### 数据设置接口

#### `void setFrame(const VideoFrame &frame)`

**功能**：设置要渲染的视频帧

**参数**：
- `frame` - 包含图像数据的 VideoFrame

**行为**：
- 更新帧数据
- 设置帧尺寸
- 标记需要更新纹理
- 触发重绘

**调用时机**：每帧渲染前调用

---

#### `void setFrameHandler(FrameHandler *handler)`

**功能**：设置 FrameHandler 用于查询原始像素值

**参数**：
- `handler` - 帧处理器指针

**重要性**：必须设置，否则像素值显示将使用 RGB 缓冲区（可能不准确）

**调用时机**：初始化 HDR 渲染时设置一次

---

#### `void setBitDepth(int bits)`

**功能**：设置渲染位深

**参数**：
- `bits` - 位深（8, 10, 12, 16等）

**默认值**：10

**影响**：
- 像素值显示的范围
- 文字颜色判断的阈值

---

### 渲染控制接口

#### `void setDithering(bool enable)`

**功能**：启用/禁用 Bayer 抖动

**参数**：
- `enable` - true 启用抖动，false 禁用

**用途**：在 8-bit 显示器上显示 10-bit 内容时减少色带

---

#### `void setZoom(double zoom)`

**功能**：设置缩放倍率

**参数**：
- `zoom` - 缩放值（1.0 = 原始大小）

**影响**：
- 视频渲染尺寸
- 像素值显示阈值（>=4.0 才显示）
- 标尺显示阈值（>=32.0 才显示）

**同步**：自动触发覆盖层更新

---

#### `void setMoveOffset(QPointF offset)`

**功能**：设置平移偏移量

**参数**：
- `offset` - 像素偏移（相对于中心）

**坐标系**：
- X 正方向：向右
- Y 正方向：向下
- (0,0) 位于窗口中心

**同步**：自动触发覆盖层更新

---

#### `void setShowRawData(bool show)`

**功能**：控制是否显示像素值

**参数**：
- `show` - true 显示像素值

**条件**：仅在缩放 >= 4.0 时实际显示

---

### 色彩处理接口

> 注：以下接口设置 shader 色彩处理参数。在 `OpenGLRenderer` 中，这些参数用于标准/抖动着色器的统一色彩管线（EOTF + 色域 + 亮度 + tonemapping + OETF），而非独立的 EDR 着色器。

#### `void setEOTF(RendererEOTF eotf)`

**功能**：设置 EOTF (Electro-Optical Transfer Function) 类型

**参数**：PQ, HLG, Gamma, SRGB

**影响**：着色器的 EOTF 转换步骤

---

#### `void setColorGamut(RendererColorGamut gamut)`

**功能**：设置源色域

**参数**：BT.2020, BT.709, DCI-P3

**影响**：着色器中色域转换矩阵的选择（源→显示器色域）。色域目标在运行时通过 `functionsGui::getDisplayColorSpace()` 动态获取。

---

#### `void setGammaValue(float gamma)`

**功能**：设置 Gamma 值（仅 EOTF=Gamma 时使用）

**参数**：1.0-3.0，默认 2.2

---

#### `void setDiffuseWhiteNits(float nits)`

**功能**：设置漫射白参考亮度

**参数**：100-10000 nits，默认 203.0

**用途**：PQ EOTF 归一化参考点

---

#### `void setHDRBrightness(float brightness)`

**功能**：设置全局 HDR 亮度倍率

**参数**：0.1-16.0x，默认 1.0

**影响**：线性光乘以亮度倍率。在 OpenGL 路径中受 8-bit FBO 限制，输出不会 >1.0。

---

#### `void setPremultipliedAlpha(bool enabled)`

**功能**：设置源纹理是否已预乘 alpha

**参数**：`true`（默认）表示源已是预乘格式，shader 不做转换；`false` 表示 shader 在编码域（EOTF 之前）执行预乘

**混合模式**：启用 `GL_BLEND` + `glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)`（预乘模式）

---

> 注：`isEDRSupported()` / `getMaxEDRValue()` 不在 `OpenGLRenderer` 中。这两个方法位于 `NativeEDRRenderer`（macOS Metal 路径），因为 OpenGL 路径受 `QOpenGLWidget` 8-bit FBO 限制无法输出 >1.0 值。

---

### 状态查询接口

#### `bool supports10bit() const`

**功能**：查询是否支持 10-bit 渲染

**返回值**：
- true - OpenGL 上下文实际获得了 10-bit 缓冲区
- false - 降级到 8-bit 渲染

**判断时机**：`initializeGL()` 中检测

---

#### `QString getOpenGLInfo() const`

**功能**：获取 OpenGL 信息字符串

**返回值**：包含版本、渲染器、位深信息的字符串

**用途**：调试和状态显示

---

## 数据交互

### 16-bit 数据通路（关键更新）

**数据流**（2025-05-08 更新）：

```
源数据 (8/10/12/16-bit RGB)
    ↓
videoHandlerRGB::loadFrame
    ↓
├─→ convertRGBToImage() → QImage (8-bit ARGB) 用于 QPainter
│
└─→ convertRGBTo16BitRGBA() → 16-bit RGBA buffer (真实高bit数据)
    ↓
VideoFrame.set16bitBuffer() → 存储在 VideoFrame
    ↓
OpenGLRenderer::setFrame()
    ↓
updateTexture() → OpenGL Texture (GL_RGBA16UI)
    ↓
Shader (16-bit 归一化 → EOTF → 色域转换 → 亮度 → tonemapping → sRGB OETF)
```

**关键改进**：
- 高 bit 源（10/12/16-bit）：直接生成真实 16-bit buffer，保持原始精度
- 8-bit 源：扩展为 16-bit（r*257），保持兼容性
- Shader 统一按 16-bit 归一化（/ 65535.0）

**兼容性**：
```cpp
if (m_currentFrame.has16bitBuffer()) {
    // 高bit源：使用真实数据
    m_sourceBitDepth = 10/12/16;
} else {
    // 8-bit源：生成扩展数据
    generate16bitBuffer(); // r*257
    m_sourceBitDepth = 8;
}
```

### 与 SplitViewWidget 的交互

```
SplitViewWidget
    ├── 创建 OpenGLRenderer (setParent)
    ├── 传递 FrameHandler (setFrameHandler)
    ├── 每帧调用 (rendererMode != Software 时)
    │       ├── setFrame(videoFrame)     - 传递帧数据
    │       ├── setZoom(zoom)            - 同步缩放
    │       ├── setMoveOffset(offset)    - 同步偏移
    │       └── setShowRawData(drawRaw)  - 同步像素值开关
    └── 控制显示/隐藏 (show/hide)
```

**关键代码**（SplitViewWidget.cpp）：

```cpp
if (rendererMode == RendererMode::OpenGL && glRenderer)
{
    glRenderer->setGeometry(0, 0, width(), height());
    
    if (auto frameHandler = item[0]->getFrameHandler())
    {
        video::VideoFrame videoFrame = frameHandler->getCurrentFrameAsVideoFrame();
        glRenderer->setFrame(videoFrame);
        glRenderer->setFrameHandler(frameHandler);  // 关键：传递 FrameHandler
    }
    
    glRenderer->setShowRawData(drawRawValues);
}
```

### 与 VideoFrame 的交互

```
VideoFrame
    ├── 提供 8-bit QImage (getImage8bit)
    ├── 提供 16-bit 缓冲区 (getData16bit) ← OpenGLRenderer 使用
    └── 自动生成 16-bit 缓冲区 (generate16bitBuffer)
```

### 与 FrameHandler 的交互

```
FrameHandler
    └── getPixelValues(QPoint, frameIdx) 
            └── 返回带标签的像素值 (YUV/RGB)
                    ├── YUV 源: [("Y", "128"), ("U", "64"), ("V", "32")]
                    └── RGB 源: [("R", "255"), ("G", "128"), ("B", "64")]
```

---

## 核心功能

### 1. OpenGL HDR 渲染

#### 流程

```
paintGL()
    ├── updateTexture() (如果需要)
    │       └── glTexImage2D(GL_RGBA16UI, ...)
    ├── 计算顶点坐标 (应用 zoom/offset)
    └── 渲染
            ├── macOS: 选择着色器 (标准/抖动) ← EDR 由 Metal 路径处理
            ├── 非 macOS: 选择着色器 (标准/抖动)
            ├── 绑定纹理
            ├── 设置 uniform (texture16bit, bitDepth, eotf, colorGamut, ...)
            └── glDrawArrays(GL_TRIANGLE_STRIP, 0, 4)
```

#### 坐标变换

```
输入: zoom, moveOffset, widgetSize, frameSize
输出: OpenGL NDC 顶点坐标 (-1.0 到 1.0)

displayW = frameW * zoom
displayH = frameH * zoom

videoLeft = widgetW/2 + moveOffset.x - displayW/2
videoTop = widgetH/2 + moveOffset.y - displayH/2

NDC 坐标 = (像素坐标 / widgetSize) * 2 - 1
```

### 2. 像素值显示

#### 触发条件

- 缩放 >= 4.0 (`SHOW_PIXEL_VALUES_ZOOM_THRESHOLD`)
- `setShowRawData(true)` 被调用

#### 显示内容

| 视频类型 | 显示格式 |
|---------|---------|
| YUV | Y\nU\nV (带标签) |
| RGB | R\nG\nB (带标签) |

#### 文字颜色判断

```cpp
// 从 16-bit RGB Buffer 获取实际显示颜色
int r = (r16 * maxVal) / 65535;
int g = (g16 * maxVal) / 65535;
int b = (b16 * maxVal) / 65535;

// ITU-R BT.601 加权亮度
int brightness = 0.299*r + 0.587*g + 0.114*b;
bool isDark = brightness < (maxVal / 2);
painter.setPen(isDark ? Qt::white : Qt::black);
```

### 3. 缩放倍率指示器

- **位置**：左上角 (10, 24)
- **字体**：helvetica 24pt
- **格式**："x2.0", "x16" 等
- **显示条件**：zoom != 1.0

### 4. 像素坐标标尺

#### 水平标尺（顶部）

- 显示 X 坐标
- 刻度线：白+黑双线
- 数值显示：每 5 个像素（zoom >= 128 时显示所有）

#### 垂直标尺（左侧）

- 显示 Y 坐标
- 刻度线：白+黑双线
- 数值显示：每 5 个像素（zoom >= 128 时显示所有）

#### 显示条件

- 缩放 >= 32.0
- 有有效的 FrameHandler

---

## 使用示例

### 基本使用流程

```cpp
// 1. 创建 Widget
auto glRenderer = new video::OpenGLRenderer(parent);
glRenderer->setGeometry(0, 0, width(), height());

// 2. 初始化设置（必须）
glRenderer->setFrameHandler(frameHandler);
glRenderer->setBitDepth(10);

// 3. 每帧更新
glRenderer->setFrame(videoFrame);
glRenderer->setZoom(zoomFactor);
glRenderer->setMoveOffset(offset);
glRenderer->setShowRawData(showRawData);
glRenderer->setDithering(enableDithering);

// 4. 显示
glRenderer->show();

// 5. 清理（析构时自动）
```

### 完整示例（SplitViewWidget 集成）

```cpp
void splitViewWidget::paintEvent(QPaintEvent *)
{
    // ... 其他绘制代码 ...
    
    if (rendererMode == RendererMode::OpenGL && glRenderer)
    {
        // 设置位置和大小
        glRenderer->setGeometry(0, 0, width(), height());
        
        // 传递帧数据
        if (auto frameHandler = item[0]->getFrameHandler())
        {
            video::VideoFrame videoFrame = frameHandler->getCurrentFrameAsVideoFrame();
            glRenderer->setFrame(videoFrame);
            glRenderer->setFrameHandler(frameHandler);
        }
        
        // 同步视图状态
        glRenderer->setZoom(this->zoomFactor);
        glRenderer->setMoveOffset(this->moveOffset);
        glRenderer->setShowRawData(showRawData() && !playing);
        
        glRenderer->show();
    }
    else
    {
        if (glRenderer)
            glRenderer->hide();
    }
    
    // ... QPainter 绘制其他内容 ...
}
```

---

## 注意事项

### 1. FrameHandler 必须设置

**问题**：如果不调用 `setFrameHandler()`，像素值显示将使用 RGB 缓冲区，对于 YUV 源可能不准确。

**解决**：始终在创建 OpenGLRenderer 后立即设置 FrameHandler。

### 2. OpenGL 上下文要求

**要求**：
- OpenGL 3.3 Core Profile
- 支持 10-bit 帧缓冲区（R/G/B/A 各10-bit）

**降级**：如果不支持 10-bit，自动降级到 8-bit，通过 `supports10bit()` 查询。

### 3. 线程安全

**限制**：OpenGL 操作必须在 GUI 线程执行。

**保证**：所有 public 接口都设计为在主线程调用。

### 4. 内存管理

**自动释放**：
- 纹理 (`m_textureId`) - 析构时检查 context 有效性后 `glDeleteTextures`
- 着色器程序 (`m_program`/`m_programDither`) - `std::shared_ptr` 自动释放
- PixelOverlay (`m_pixelOverlay`) - `std::shared_ptr` 自动释放
- 析构函数中先 `makeCurrent()` 再清理 GL 资源，避免基类析构后 context 失效崩溃

### 5. 性能考虑

**优化点**：
- 纹理只在帧变化时更新
- 像素值只绘制可见区域
- 覆盖层透明，不影响 OpenGL 性能

**注意**：
- 高缩放时像素值绘制可能耗时（每像素一个 drawText）
- 标尺在 zoom >= 32 时才显示，避免过多绘制

### 6. 坐标系

**Widget 坐标系**：
- 原点：左上角 (0,0)
- X：向右增加
- Y：向下增加

**OpenGL NDC 坐标系**：
- 范围：[-1.0, 1.0]
- 原点：中心
- X：向右增加
- Y：向上增加（已翻转处理）

**视频坐标系**：
- 原点：左上角 (0,0)
- 与 Widget 坐标系通过 zoom/offset 转换

---

## Shader 说明

### 顶点着色器 (opengl_vertex.glsl)

```glsl
#version 330 core
layout(location = 0) in vec2 position;
layout(location = 1) in vec2 texCoord;
out vec2 vTexCoord;

void main()
{
    gl_Position = vec4(position, 0.0, 1.0);
    vTexCoord = texCoord;
}
```

### 片段着色器 - 标准 (opengl_fragment.glsl)

标准着色器实现了**统一的色彩处理管线**（与 Metal/HLSL 后端共享逻辑），不仅仅是归一化：

```glsl
#version 330 core

in vec2 vTexCoord;
out vec4 fragColor;

uniform usampler2D texture16bit;

// 色彩处理 uniforms
uniform int   eotfType;                    // 0=PQ, 1=HLG, 2=Gamma, 3=sRGB
uniform float gammaValue;                  // Gamma 值 (仅 eotfType==2)
uniform float diffuseWhiteNits;            // 漫射白参考亮度 (默认 203)
uniform float hdrBrightness;               // HDR 亮度倍率 (默认 1.0)
uniform mat3  gamutMatrix;                 // 3×3 色域转换矩阵 (源→显示器)
uniform float systemHandlesTonemapping;    // 1.0=系统处理 tonemapping，跳过 Reinhard
uniform float applySRGBOETF;               // 1.0=应用 sRGB OETF (SDR 输出)
uniform int   premultipliedAlpha;          // 1=源已预乘，0=shader 预乘

// 管线步骤:
//   1. 采样 16-bit RGBA 纹理 (0-65535)
//   2. 归一化到 0-1
//   3. 预乘 alpha (编码域，若源非预乘)
//   4. EOTF 转换 (PQ/HLG/Gamma/sRGB → 线性光)
//   5. 色域转换 (3×3 矩阵: 源 → 显示器色域)
//   6. HDR 亮度缩放 (× hdrBrightness)
//   7. Reinhard tonemapping (仅 SDR 无系统 tonemapping 时)
//   8. sRGB OETF (线性 → sRGB 编码，SDR 输出)
//   9. 输出 RGBA8 (0-1 范围)

void main()
{
    uvec4 raw = texture(texture16bit, vTexCoord);
    vec3 color = vec3(raw.rgb) / 65535.0;
    float alpha = float(raw.a) / 65535.0;

    if (premultipliedAlpha == 0)
        color *= alpha;

    vec3 outputColor = processColor(color);  // EOTF + 色域 + 亮度 + tonemapping + OETF
    fragColor = vec4(outputColor, alpha);
}
```

> 注：上述为管线概要。`processColor()` 函数内含 PQ/HLG/Gamma/sRGB EOTF 实现、色域矩阵乘法、Reinhard tonemapping（条件执行）和 sRGB OETF。完整源码见 `YUViewLib/shaders/opengl_fragment.glsl`。

### 片段着色器 - 抖动 (opengl_fragment_dither.glsl)

抖动着色器与标准着色器共享相同的色彩处理管线，额外在 sRGB OETF **之前**（线性光域）应用 Bayer 4×4 抖动：

```glsl
// Bayer 4x4 抖动矩阵 (±0.5/256 范围)
float bayerDither4x4(vec2 position)
{
    int x = int(mod(position.x, 4.0));
    int y = int(mod(position.y, 4.0));
    float matrix[16] = float[](
        -0.001953125,  0.000000000, -0.001464844,  0.000488281,
         0.000976562, -0.000976562,  0.001464844, -0.000488281,
        -0.001220703,  0.000732422, -0.001708984,  0.000244141,
         0.001708984, -0.000244141,  0.001220703, -0.000732422
    );
    return matrix[x + y * 4];
}

void main()
{
    uvec4 raw = texture(texture16bit, vTexCoord);
    vec3 color = vec3(raw.rgb) / 65535.0;
    float alpha = float(raw.a) / 65535.0;

    if (premultipliedAlpha == 0)
        color *= alpha;

    // EOTF + 色域 + 亮度 + tonemapping (与标准着色器相同)
    vec3 linear = /* processColor steps */;

    // Bayer 抖动 (在线性光域，OETF 之前)
    linear += bayerDither4x4(gl_FragCoord.xy);

    // sRGB OETF
    linear = srgbOetf(linear);
    fragColor = vec4(linear, alpha);
}
```

**抖动原理**：
- 抖动幅度：±0.5/256（半个 8-bit 量化步长）
- 抖动在**线性光域**应用（OETF 之前），数学上正确
- 作用：在 8-bit 显示器上分布量化误差，视觉上模拟更高位深
- Bayer 4x4 矩阵提供 16 级空间分布，减少可察觉的色带

---

## 常见问题

### Q: 为什么像素值显示为 YUV 而不是 RGB？

A: 如果设置了 `FrameHandler` 且视频源是 YUV 格式，`getPixelValues()` 会返回 YUV 值。这是原始像素值，更准确。

### Q: 文字颜色如何确定？

A: 从 16-bit RGB 缓冲区读取实际显示颜色，计算感知亮度（加权 R/G/B），与中间灰度比较，暗背景用白字，亮背景用黑字。

### Q: 为什么标尺在 zoom < 32 时不显示？

A: 避免在缩放不够大时显示过多数字造成视觉混乱。32x 放大后像素足够大，可以容纳坐标标签。

### Q: 如何支持更高位深（如 12-bit）？

A: 数据通路已支持 8/10/12/16-bit 源（统一扩展为 16-bit RGBA 纹理）。`setBitDepth()` 用于像素值显示范围，不影响实际渲染精度（shader 统一除以 65535.0 归一化）。10-bit 显示需要非 macOS 平台且 GPU/驱动支持 10-bit 帧缓冲。

### Q: macOS 上为什么不使用 OpenGLRenderer 的 EDR 路径？

A: macOS 上 `QOpenGLWidget` 的内部 FBO 始终为 8-bit RGBA，即使 GL 着色器输出 >1.0 的浮点值，写入 FBO 时也会被截断到 0-1 范围。因此 macOS 的 EDR 必须通过 Metal 路径 (`NativeEDRRenderer` + `MacEDRRenderer`) 实现，使用 `CAMetalLayer` 的 `RGBA16Float` 格式输出 >1.0 值。

### Q: EDR 模式下像素值叠加层如何工作？

A: macOS EDR 模式下，Metal QWindow 是由 macOS 窗口服务器直接合成在 Qt backing store 上方的原生子窗口。`SplitViewWidget` 的 QPainter 绘制内容在 EDR 模式下不可见。解决方案是在 Metal 层的 NSView 上添加一个原生 CALayer 作为叠加层，用于显示像素值、缩放指示器等内容。

---

## macOS EDR 路径补充

### NativeEDRRenderer

`NativeEDRRenderer`（原名 `HDR10WidgetMacEDR`）继承自 `QWidget`（非 QObject），是 macOS 上 EDR HDR 渲染的入口组件，负责：

1. **QWindow 容器创建**: 创建 `MetalSurface` 类型的 QWindow，通过 `createWindowContainer` 嵌入 QWidget
2. **事件转发**: 通过 `eventFilter` 将 Metal QWindow NSView 拦截的鼠标/滚轮事件转发给父 `SplitViewWidget`
3. **Metal 渲染器创建**: 延迟创建 `MacEDRRenderer` 实例（在 `showEvent`/`paintEvent` 时初始化）
4. **EDR 参数配置**: 传递 EOTF、色域、亮度等参数到 Metal 渲染器
5. **CALayer 叠加**: 像素值/缩放/标尺通过 CALayer overlay 显示（Metal QWindow 是原生子窗口，QPainter 内容不可见）

> 注：`NativeEDRRenderer` 使用 `WA_TranslucentBackground` + `WA_NativeWindow` + `WA_NoSystemBackground`（不用 `WA_PaintOnScreen`，因为作为 SplitViewWidget 子 widget 时会导致白屏）。

### MacEDRRenderer

`MacEDRRenderer` 是 Metal 渲染核心（Objective-C++ 实现），负责：

1. **CAMetalLayer 创建**: 使用 `RGBA16Float` 格式，支持 >1.0 倍输出
2. **EDR 触发**: 设置 `CAMetalLayer.wantsExtendedDynamicRangeContent = YES`
3. **色彩空间**: **固定使用** `kCGColorSpaceExtendedLinearDisplayP3`（不随色域设置切换——色域转换在 Metal 着色器内部完成）
4. **NSView 隔离**: 创建独立 NSView + `addSubview` 托管 CAMetalLayer，避免 Qt createWindowContainer NSView 的双重 sRGB decode
5. **Metal 渲染管线**: 创建 MTLRenderPipelineState，着色器执行 EOTF + 色域转换 + HDR 亮度缩放（不做 tonemapping，由系统合成器处理）
6. **CALayer overlay**: 在 Metal 层之上添加 CALayer 子层，用于显示像素值/缩放/标尺
7. **ARC 内存管理**: 使用 `__bridge_retained` / `CFRelease()` 管理 Core Foundation 对象

> 注：Metal 着色器的色彩处理管线与 OpenGL 着色器类似（EOTF + 色域 + 亮度），但**不做 Reinhard tonemapping**也不做 sRGB OETF——输出为线性光 Extended P3，由 macOS 窗口服务器合成到显示器。

### EDR 设置

> 旧版的 `EDRSettingsDialog` 对话框及 `edrSettingsDialog.ui` 已删除。EDR 设置现已并入 `RendererSettingsDock` dock 面板，不再使用独立对话框。

EDR 设置包含：

- **EOTF 选择**: PQ / HLG / Gamma / sRGB
- **色域选择**: BT.2020 / BT.709 / DCI-P3
- **Gamma 值**: 1.0 - 3.0
- **漫射白**: 100 - 10000 nits
- **HDR 亮度**: 0.1 - 16.0x

设置通过 QSettings 持久化存储。

---

## 文件位置

| 文件 | 路径 |
|------|------|
| 头文件 | `YUViewLib/src/ui/views/OpenGLRenderer.h` |
| 实现 | `YUViewLib/src/ui/views/OpenGLRenderer.cpp` |
| 顶点着色器 | `YUViewLib/shaders/opengl_vertex.glsl` |
| 标准片段着色器 | `YUViewLib/shaders/opengl_fragment.glsl` |
| 抖动片段着色器 | `YUViewLib/shaders/opengl_fragment_dither.glsl` |
| macOS EDR Widget | `YUViewLib/src/ui/views/NativeEDRRenderer.h/cpp` |
| Metal 渲染器 | `YUViewLib/src/ui/views/MacEDRRenderer.h/mm` |
| EDR 工具类 | `YUViewLib/src/ui/views/MacEDRUtil.h/mm` |
| EDR 设计文档 | `docs/macOS_EDR_Design_and_Implementation.md` |

> 已删除的文件：
> - `hdr10_fragment_edr.glsl`（提交 `09a06cac` 删除，EDR 统一由 Metal 路径处理）
> - `EDRSettingsDialog.h/.cpp` 及 `edrSettingsDialog.ui`（已删除，设置并入 `RendererSettingsDock`）

---

## 作者和维护

- **作者**: YUView 开发团队
- **最近修改**: 2025-06
- **功能增强**: 添加像素值显示、标尺、缩放指示器、抖动控制、macOS EDR 支持
