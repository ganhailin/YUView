# HDR10Widget 技术文档

## 概述

`HDR10Widget` 是 YUView 中用于 HDR（高动态范围）视频渲染的 OpenGL 控件。它继承自 `QOpenGLWidget`，支持 10-bit 或更高位深的视频渲染，提供了像素值显示、缩放平移、抖动处理等功能。

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
            └── HDR10Widget
```

### 内部组成

```
HDR10Widget (OpenGL 渲染层)
    ├── PixelOverlay (QPainter 覆盖层)
    │       ├── 像素值显示
    │       ├── 缩放倍率指示器
    │       └── 像素坐标标尺
    │
    ├── OpenGL 着色器
    │       ├── 标准渲染 (hdr10_fragment.glsl)
    │       └── 抖动渲染 (hdr10_fragment_dither.glsl)
    │
    └── 外部依赖
            ├── VideoFrame (帧数据)
            └── FrameHandler (像素值查询)
```

### 渲染流程

```
1. paintGL()              - OpenGL 渲染视频帧
2. PixelOverlay::paintEvent()  - QPainter 绘制覆盖层
   ├── drawPixelValues() - 像素值显示
   ├── drawZoomIndicator() - 缩放倍率
   └── drawPixelRulers() - 坐标标尺
```

---

## 类定义

### HDR10Widget

```cpp
namespace video {

class HDR10Widget : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT
public:
    explicit HDR10Widget(QWidget *parent = nullptr);
    ~HDR10Widget() override;

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
    class PixelOverlay : public QWidget;

    // OpenGL 资源
    QOpenGLShaderProgram *m_program;
    QOpenGLShaderProgram *m_programDither;
    QOpenGLBuffer m_vbo;
    QOpenGLVertexArrayObject m_vao;
    GLuint m_textureId;

    // 帧数据
    VideoFrame m_currentFrame;
    QSize m_frameSize;
    bool m_frameNeedsUpdate;

    // 渲染参数
    int m_bitDepth;           // 位深（默认10）
    bool m_ditheringEnabled;  // 抖动开关

    // OpenGL 状态
    bool m_initialized;
    bool m_supports10bit;
    QString m_openglInfo;

    // 视图控制
    double m_zoom;            // 缩放倍率
    QPointF m_moveOffset;     // 偏移量
    bool m_showRawData;       // 显示原始数据

    // 外部引用
    PixelOverlay *m_pixelOverlay;
    FrameHandler *m_frameHandler;
};

} // namespace video
```

### PixelOverlay (内部类)

透明 QWidget 覆盖层，用于在 OpenGL 渲染之上绘制像素值、标尺等 QPainter 内容。

---

## 接口说明

### 构造函数 / 析构函数

#### `HDR10Widget(QWidget *parent = nullptr)`

**功能**：构造函数，初始化 OpenGL 上下文和覆盖层

**行为**：
- 设置 OpenGL 3.3 Core Profile
- 请求 10-bit 颜色缓冲区（R/G/B/A 各10-bit）
- 创建 `PixelOverlay` 覆盖层

#### `~HDR10Widget()`

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
HDR10Widget::setFrame()
    ↓
updateTexture() → OpenGL Texture (GL_RGBA16UI)
    ↓
Shader (16-bit → 0-1.0 归一化)
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
    ├── 创建 HDR10Widget (setParent)
    ├── 传递 FrameHandler (setFrameHandler)
    ├── 每帧调用 (HDR模式时)
    │       ├── setFrame(videoFrame)     - 传递帧数据
    │       ├── setZoom(zoom)            - 同步缩放
    │       ├── setMoveOffset(offset)    - 同步偏移
    │       └── setShowRawData(drawRaw)  - 同步像素值开关
    └── 控制显示/隐藏 (show/hide)
```

**关键代码**（SplitViewWidget.cpp）：

```cpp
if (hdrRenderingMode == HDRRenderingMode::Enabled && hdr10Widget)
{
    hdr10Widget->setGeometry(0, 0, width(), height());
    
    if (auto frameHandler = item[0]->getFrameHandler())
    {
        video::VideoFrame videoFrame = frameHandler->getCurrentFrameAsVideoFrame();
        hdr10Widget->setFrame(videoFrame);
        hdr10Widget->setFrameHandler(frameHandler);  // 关键：传递 FrameHandler
    }
    
    hdr10Widget->setShowRawData(drawRawValues);
}
```

### 与 VideoFrame 的交互

```
VideoFrame
    ├── 提供 8-bit QImage (getImage8bit)
    ├── 提供 16-bit 缓冲区 (getData16bit) ← HDR10Widget 使用
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
            ├── 选择着色器 (标准/抖动)
            ├── 绑定纹理
            ├── 设置 uniform (texture16bit, bitDepth)
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
auto hdrWidget = new video::HDR10Widget(parent);
hdrWidget->setGeometry(0, 0, width(), height());

// 2. 初始化设置（必须）
hdrWidget->setFrameHandler(frameHandler);
hdrWidget->setBitDepth(10);

// 3. 每帧更新
hdrWidget->setFrame(videoFrame);
hdrWidget->setZoom(zoomFactor);
hdrWidget->setMoveOffset(offset);
hdrWidget->setShowRawData(showRawData);
hdrWidget->setDithering(enableDithering);

// 4. 显示
hdrWidget->show();

// 5. 清理（析构时自动）
```

### 完整示例（SplitViewWidget 集成）

```cpp
void splitViewWidget::paintEvent(QPaintEvent *)
{
    // ... 其他绘制代码 ...
    
    if (hdrRenderingMode == HDRRenderingMode::Enabled && hdr10Widget)
    {
        // 设置位置和大小
        hdr10Widget->setGeometry(0, 0, width(), height());
        
        // 传递帧数据
        if (auto frameHandler = item[0]->getFrameHandler())
        {
            video::VideoFrame videoFrame = frameHandler->getCurrentFrameAsVideoFrame();
            hdr10Widget->setFrame(videoFrame);
            hdr10Widget->setFrameHandler(frameHandler);
        }
        
        // 同步视图状态
        hdr10Widget->setZoom(this->zoomFactor);
        hdr10Widget->setMoveOffset(this->moveOffset);
        hdr10Widget->setShowRawData(showRawData() && !playing);
        
        hdr10Widget->show();
    }
    else
    {
        if (hdr10Widget)
            hdr10Widget->hide();
    }
    
    // ... QPainter 绘制其他内容 ...
}
```

---

## 注意事项

### 1. FrameHandler 必须设置

**问题**：如果不调用 `setFrameHandler()`，像素值显示将使用 RGB 缓冲区，对于 YUV 源可能不准确。

**解决**：始终在创建 HDR10Widget 后立即设置 FrameHandler。

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
- 纹理 (`m_textureId`) - 析构时 `glDeleteTextures`
- 着色器程序 - 析构时 `delete`
- PixelOverlay - QObject 父子关系自动管理

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

### 顶点着色器 (hdr10_vertex.glsl)

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

### 片段着色器 - 标准 (hdr10_fragment.glsl)

```glsl
#version 330 core
in vec2 vTexCoord;
out vec4 fragColor;
uniform usampler2D texture16bit;

void main()
{
    uvec4 raw = texture(texture16bit, vTexCoord);
    // Data is always stored in 16-bit range (0-65535)
    // Normalize to 0.0-1.0 range for display
    vec3 color = vec3(raw.rgb) / 65535.0;
    fragColor = vec4(color, 1.0);
}
```

### 片段着色器 - 抖动 (hdr10_fragment_dither.glsl)

使用 Bayer 4x4 抖动矩阵，在 8-bit 显示器上模拟更高精度，减少色带。

```glsl
#version 330 core
in vec2 vTexCoord;
out vec4 fragColor;
uniform usampler2D texture16bit;

// Pre-computed Bayer matrix values (0-15 normalized to ±0.5/256)
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
    color = color + bayerDither4x4(gl_FragCoord.xy);
    fragColor = vec4(color, 1.0);
}
```

**抖动原理**：
- 抖动幅度：±0.5/256（半个 8-bit 量化步长）
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

A: 设置 `setBitDepth(12)`，渲染管线会自动适应。注意需要 OpenGL 和显示器支持。

---

## 文件位置

| 文件 | 路径 |
|------|------|
| 头文件 | `YUViewLib/src/ui/views/HDR10Widget.h` |
| 实现 | `YUViewLib/src/ui/views/HDR10Widget.cpp` |
| 顶点着色器 | `YUViewLib/shaders/hdr10_vertex.glsl` |
| 标准片段着色器 | `YUViewLib/shaders/hdr10_fragment.glsl` |
| 抖动片段着色器 | `YUViewLib/shaders/hdr10_fragment_dither.glsl` |

---

## 作者和维护

- **作者**: YUView 开发团队
- **最近修改**: 2025-05-08
- **功能增强**: 添加像素值显示、标尺、缩放指示器、抖动控制
