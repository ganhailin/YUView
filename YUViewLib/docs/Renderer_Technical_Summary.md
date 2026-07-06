# Renderer 技术开发详细总结

**文档生成日期:** 2026-05-10 (更新: 2025-06, 重命名同步: 2026-07-06)  
**分析提交范围:** f4ea421a..d164e7ff (8个提交) + EDR 扩展提交  
**代码行数:** ~2,990行新增代码 + ~1,500行 EDR 扩展

> **命名变更说明**：本文档记录的开发工作发生在 renderer 重命名之前。文中出现的旧名称对应如下（详见 `docs/Renderer_Rename_Plan.md`，重命名提交 `cf195e3d`）：
> - `HDR10Widget` → `OpenGLRenderer`（`HDR10Widget.cpp/h` → `OpenGLRenderer.cpp/h`）
> - `HDR10WidgetMacEDR` → `NativeEDRRenderer`
> - `HDRSettingsDock` → `RendererSettingsDock`
> - `EDRSettingsDialog`（已删除，设置并入 `RendererSettingsDock`）
> - shader `hdr10_fragment.glsl` → `opengl_fragment.glsl`、`hdr10_fragment_dither.glsl` → `opengl_fragment_dither.glsl`、`hdr10_vertex.glsl` → `opengl_vertex.glsl`
> - `hdr10_fragment_edr.glsl`（已删除，EDR 统一由 Metal 路径处理）
> - 枚举 `HDR10_EOTF` → `RendererEOTF`、`HDR10_ColorGamut` → `RendererColorGamut`、`HDRRenderingMode` → `RendererMode`
> - 信号 `hdrStatusChanged` → `rendererStatusChanged`
> - 变量 `hdr10Widget` → `glRenderer`、`hdr10WidgetMacEDR` → `edrRenderer`、`hdrRenderingMode` → `rendererMode`
>
> 下方的代码示例与架构说明已同步为新名称；git 提交时间线中的提交消息保留历史原貌。

---

## 1. 项目概述

本次开发为 YUView 添加了完整的 **HDR10 视频渲染支持**，实现了从 8-bit 到 16-bit 的高动态范围视频显示能力。核心创新是引入 `OpenGLRenderer`（原名 `HDR10Widget`）作为基于 OpenGL 的专用渲染组件，替代传统的 8-bit QPainter 渲染路径。

**EDR 扩展**: 在 macOS 上，由于 `QOpenGLWidget` 的内部 FBO 为 8-bit RGBA（无法输出 >1.0 值），新增了 Metal 渲染路径通过 `NativeEDRRenderer`（原名 `HDR10WidgetMacEDR`）+ `MacEDRRenderer` 实现 EDR（Extended Dynamic Range）显示，使 HDR 视频能够在支持 EDR 的 Mac 显示器上呈现超过 SDR 白色的亮度值。

---

## 2. 核心技术架构

### 2.1 双通道数据流设计

```
┌─────────────────────────────────────────────────────────────────┐
│                        数据源 (YUV/RGB)                          │
│                    8-bit / 10-bit / 12-bit / 16-bit               │
└──────────────────────┬──────────────────────────────────────────┘
                       │
        ┌──────────────┴──────────────┐
        │                             │
   传统路径                      HDR 路径
   (8-bit QImage)               (16-bit OpenGL)
        │                             │
┌───────▼────────┐            ┌───────▼────────┐
│ videoHandler   │            │ videoHandler   │
│ (原有逻辑)     │            │ (扩展逻辑)     │
└───────┬────────┘            └───────┬────────┘
        │                             │
        │         ┌───────────────────┘
        │         │
        │    ┌────▼─────────────────┐
        │    │ VideoFrame 类        │ ◄─── 新增核心数据结构
        │    │ ├─ QImage (8-bit)    │      统一存储 8-bit 和 16-bit
        │    │ └─ QVector<uint16_t> │      支持移动语义优化内存
        │    └────┬─────────────────┘
        │         │
        │    ┌────▼─────────────────┐
        │    │ convertTo16BitRGBA │ ◄─── 格式转换层
        │    │ (YUV/RGB → RGBA16) │      支持位深扩展和色域转换
        │    └────┬─────────────────┘
        │         │
        └────┐    │
             │    │
        ┌────▼────▼──────────────┐
        │ SplitViewWidget        │
        │ ├─ 传统 paintEvent     │
        │ ├─ OpenGLRenderer (OpenGL)│ ◄─── SDR fallback / 非 macOS (原名 HDR10Widget)
        │ └─ NativeEDRRenderer  │ ◄─── macOS EDR (Metal) 新增 (原名 HDR10WidgetMacEDR)
        └──────────┬─────────────┘
                   │
        ┌──────────┴──────────────┐
        │                         │
   ┌────▼───────┐          ┌──────▼─────────────┐
   │ OpenGLRenderer│          │ NativeEDRRenderer   │ ◄── macOS EDR 新增 (原名 HDR10WidgetMacEDR)
   │ (OpenGL)   │          │ ├─ MacEDRRenderer   │    Metal 渲染核心
   │ ├─ 16UI纹理│          │ │ ├─ CAMetalLayer    │    RGBA16Float EDR 输出
   │ ├─ 着色器   │          │ │ └─ CALayer overlay │    像素值叠加层
   │ └─ 抖动    │          │ └─ MacEDRUtil        │    EDR 检测
   └────────────┘          └──────────────────────┘
```

### 2.2 VideoFrame - 核心数据结构

**位置:** `YUViewLib/src/video/VideoFrame.h/cpp`

```cpp
class VideoFrame {
    QImage image8bit;              // 8-bit QImage (QPainter 兼容)
    QVector<uint16_t> buffer16bit; // 16-bit RGBA 缓冲区 (HDR 渲染)
    
public:
    // 关键方法
    void generate16bitBuffer();    // 8-bit → 16-bit 扩展 (乘257)
    void set16bitBuffer(QVector<uint16_t>&& data, int w, int h); // 移动语义
    const uint16_t* getData16bit() const;
};
```

**设计亮点:**
- **双缓冲策略**: 同时维护 8-bit QImage (QPainter 向后兼容) 和 16-bit 缓冲区
- **移动语义**: `set16bitBuffer` 使用 `std::move` 避免拷贝，提升性能
- **延迟生成**: 8-bit 源仅在需要时才生成 16-bit 缓冲区

---

## 3. OpenGLRenderer - OpenGL 渲染引擎

### 3.1 类架构

**位置:** `YUViewLib/src/ui/views/OpenGLRenderer.h`（原名 `HDR10Widget.h`）

```cpp
class OpenGLRenderer : public QOpenGLWidget, protected QOpenGLFunctions {
    // OpenGL 资源 (shared_ptr 管理生命周期)
    std::shared_ptr<QOpenGLShaderProgram> m_program;        // 标准着色器
    std::shared_ptr<QOpenGLShaderProgram> m_programDither;  // 抖动着色器
    QOpenGLBuffer m_vbo{QOpenGLBuffer::VertexBuffer};       // 顶点缓冲
    QOpenGLVertexArrayObject m_vao;                          // 顶点数组对象
    GLuint m_textureId{0};                                   // 16-bit 纹理
    QSize  m_textureSize;       // 缓存纹理尺寸用于复用
    
    // 渲染状态
    VideoFrame m_currentFrame;
    bool m_frameNeedsUpdate{false};
    int m_bitDepth{10};           // 显示位深 (shader 归一化用)
    int m_sourceBitDepth{8};      // 源位深 (像素值显示用)
    bool m_ditheringEnabled{false};
    bool m_initialized{false};
    bool m_supports10bit{false};
    bool m_showRawData{false};
    double m_zoom{1.0};
    QPointF m_moveOffset{0, 0};
    
    // 像素值叠加层 (shared_ptr)
    class PixelOverlay : public QWidget {
        void paintEvent(QPaintEvent*) override; // QPainter 叠加绘制
    };
    std::shared_ptr<PixelOverlay> m_pixelOverlay;
    
    // 色彩处理参数 (标准/抖动着色器共用)
    RendererEOTF       m_eotf{RendererEOTF::SRGB};
    RendererColorGamut m_colorGamut{RendererColorGamut::BT709};
    float            m_gammaValue{2.2f};
    float            m_diffuseWhiteNits{203.0f};
    float            m_hdrBrightness{1.0f};
    bool             m_premultipliedAlpha{true};  // 预乘 alpha 开关
    
    // ACM 状态跟踪 (Windows)
    bool m_lastAcmActive{false};
    
    // 外部引用
    FrameHandler *m_frameHandler{nullptr};
};
```

> 注：实际代码中 `m_program`/`m_programDither`/`m_pixelOverlay` 使用 `std::shared_ptr`。不再有 `m_programEDR`（EDR 着色器已删除，色彩处理逻辑合并进标准/抖动着色器）。`m_edrSupported`/`m_maxEDRValue` 不在此类（位于 `NativeEDRRenderer`）。

**EDR 枚举类型（实际代码为 `color::EOTF` / `color::ColorGamut` 的别名）:**

```cpp
using RendererEOTF = color::EOTF;       // 原名 HDR10_EOTF
using RendererColorGamut = color::ColorGamut; // 原名 HDR10_ColorGamut

```cpp
enum class RendererEOTF {  // 原名 HDR10_EOTF
    PQ    = 0,  // SMPTE ST 2084 Perceptual Quantizer
    HLG   = 1,  // ARIB STD-B67 Hybrid Log-Gamma
    Gamma = 2,  // 纯幂函数
    SRGB  = 3   // IEC 61966-2-1 sRGB 近似
};

enum class RendererColorGamut {  // 原名 HDR10_ColorGamut
    BT2020 = 0, // ITU-R BT.2020 宽色域
    BT709  = 1, // ITU-R BT.709 标准色域
    P3     = 2  // DCI-P3 色域
};
```

### 3.2 OpenGL 初始化与能力检测

```cpp
void OpenGLRenderer::initializeGL() {
    // 请求 10-bit 帧缓冲区 (macOS 不支持)
    QSurfaceFormat format;
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setVersion(3, 3);
#ifndef Q_OS_MAC
    format.setRedBufferSize(10);
    format.setGreenBufferSize(10);
    format.setBlueBufferSize(10);
    format.setAlphaBufferSize(10);
#endif
    setFormat(format);
    
    // 运行时检测实际支持的位深
    GLint redBits, greenBits, blueBits;
    glGetIntegerv(GL_RED_BITS, &redBits);
    m_supports10bit = (redBits >= 10 && greenBits >= 10 && blueBits >= 10);
    
    if (!m_supports10bit) {
        m_bitDepth = 8;  // 回退到 8-bit
    }
}
```

**关键决策:** macOS 被排除在 10-bit 请求之外，因为平台驱动支持不稳定。

### 3.3 着色器系统

标准着色器和抖动着色器共享**统一的色彩处理管线**（与 Metal/HLSL 后端逻辑一致）：

```glsl
#version 330 core
uniform usampler2D texture16bit;
uniform int   eotfType;                 // 0=PQ, 1=HLG, 2=Gamma, 3=sRGB
uniform float gammaValue;
uniform float diffuseWhiteNits;
uniform float hdrBrightness;
uniform mat3  gamutMatrix;              // 3×3 色域转换矩阵
uniform float systemHandlesTonemapping; // 1.0=跳过 Reinhard
uniform float applySRGBOETF;            // 1.0=应用 sRGB OETF
uniform int   premultipliedAlpha;       // 1=源已预乘

// 管线步骤：
// 1. 采样 16-bit RGBA (0-65535)，归一化到 0-1
// 2. 预乘 alpha（编码域，若源非预乘）
// 3. EOTF 转换（PQ/HLG/Gamma/sRGB → 线性光）
// 4. 色域转换（源 → 显示器色域，3×3 矩阵）
// 5. HDR 亮度缩放（× hdrBrightness）
// 6. Reinhard tonemapping（仅 SDR 无系统 tonemapping 时）
// 7. sRGB OETF（线性 → sRGB 编码，SDR 输出）

void main() {
    uvec4 raw = texture(texture16bit, vTexCoord);
    vec3 color = vec3(raw.rgb) / 65535.0;
    float alpha = float(raw.a) / 65535.0;
    if (premultipliedAlpha == 0) color *= alpha;
    vec3 output = processColor(color);  // 步骤 3-7
    fragColor = vec4(output, alpha);
}
```

#### 抖动着色器 (`opengl_fragment_dither.glsl`，原名 `hdr10_fragment_dither.glsl`)

与标准着色器共享相同管线，额外在 sRGB OETF **之前**（线性光域）应用 Bayer 4×4 抖动：

```glsl
// 抖动在线性光域应用（OETF 之前），数学上正确
linear = processColorWithoutOETF(color);  // EOTF + 色域 + 亮度 + tonemapping
linear += bayerDither4x4(gl_FragCoord.xy);  // ±0.5/256
linear = srgbOetf(linear);  // OETF
```

**算法说明:**
- Bayer 4×4 矩阵产生 16 级额外精度
- 抖动值范围 ±0.5 LSB (8-bit)，在线性光域应用
- 通过空间分布模拟高位深效果

> 注：旧版的标准着色器仅做 `/ 65535.0` 归一化，色彩处理在独立的 `hdr10_fragment_edr.glsl` 中。统一后色彩逻辑合并进标准/抖动着色器，EDR 着色器已删除。

### 3.4 纹理管理策略

```cpp
void OpenGLRenderer::updateTexture() {
    // 智能缓冲区检测
    if (!m_currentFrame.has16bitBuffer()) {
        // 8-bit 源: 实时生成 16-bit 扩展
        const_cast<VideoFrame&>(m_currentFrame).generate16bitBuffer();
        m_sourceBitDepth = 8;
    } else {
        // 高比特源: 直接使用
        m_sourceBitDepth = 10; // 或检测实际值
    }
    
    // 纹理重用策略
    if (m_textureId == 0 || sizeChanged) {
        // 创建新纹理
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16UI, w, h, 0,
                     GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, data);
    } else {
        // 仅更新数据 (性能优化)
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                        GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, data);
    }
}
```

**关键优化:** 使用 `glTexSubImage2D` 在尺寸不变时只更新像素数据，避免纹理重建开销。

### 3.5 EDR 着色器（已删除）

> 历史版本的 `hdr10_fragment_edr.glsl`（EDR 色彩处理着色器）已在提交 `09a06cac` 中删除。其 EOTF 转换、色域转换、HDR 亮度缩放等逻辑已**合并进标准/抖动着色器**（见 3.3）。macOS EDR 渲染统一由 Metal 路径（`NativeEDRRenderer` + `MacEDRRenderer`）处理。
>
> 关键变化：旧架构中标准着色器仅做归一化，EDR 着色器做色彩处理。新架构中所有着色器都包含完整色彩管线，通过 uniforms（`eotfType`、`gamutMatrix`、`systemHandlesTonemapping` 等）控制行为。

---

## 4. 像素值显示系统

### 4.1 架构设计

OpenGLRenderer 采用 **OpenGL + QPainter 叠加** 的混合渲染模式：

```
┌─────────────────────────────────────┐
│ OpenGLRenderer (OpenGLWidget)          │
│ ┌─────────────────────────────────┐ │
│ │ OpenGL 渲染 (10-bit HDR 画面)   │ │
│ └─────────────────────────────────┘ │
│ ┌─────────────────────────────────┐ │
│ │ PixelOverlay (QWidget 子窗口)   │ │ ← 透明背景叠加
│ │ ├─ 像素值文本绘制               │ │
│ │ ├─ 缩放指示器                   │ │
│ │ └─ 坐标标尺                     │ │
│ └─────────────────────────────────┘ │
└─────────────────────────────────────┘
```

### 4.2 智能字体颜色

**算法:** 基于像素实际亮度动态选择黑/白字体

```cpp
// 从 16-bit 缓冲区读取当前像素
int idx = (y * frameW + x) * 4;
int r = frameData[idx];
int g = frameData[idx + 1];
int b = frameData[idx + 2];

// ITU-R BT.601 亮度公式
int brightness = (299 * r + 587 * g + 114 * b) / 1000;

// 阈值判断
QColor textColor = (brightness < 32768) ? Qt::white : Qt::black;
```

### 4.3 YUV 色度子采样处理

```cpp
// 仅在实际色度采样位置显示 UV 值
if (label == "U" || label == "V") {
    if ((x - chromaOffsetFullX) % subsamplingX != 0 ||
        (y - chromaOffsetFullY) % subsamplingY != 0) {
        continue; // 跳过非采样位置
    }
}
```

---

## 5. 视频格式转换管道

### 5.1 RGB → 16-bit RGBA

**位置:** `YUViewLib/src/video/rgb/ConversionRGB.cpp`

```cpp
template <int bitDepth>
void convertRGBTo16BitRGBAInternal(...) {
    // 输入位深检测
    const auto shiftTo16 = 16 - inputBits; // 8→8, 10→6, 12→4, 16→0
    
    for (每个像素) {
        // 读取原始值
        auto value = static_cast<int64_t>(*src);
        
        // 大端字节序处理
        if (isBigEndian) value = swapBytesEndianess(value);
        
        // 应用缩放和扩展
        value = (value * scale) << shiftTo16;
        value = clip(value, 0, 65535);
        
        // 有限范围转换
        if (limitedRange) {
            // Limited: [16, 235] → Full: [0, 65535]
            value = ((value - 4096) * 65535) / 60160;
        }
        
        *target++ = static_cast<uint16_t>(value);
    }
}
```

### 5.2 YUV → 16-bit RGBA

**位置:** `YUViewLib/src/video/yuv/videoHandlerYUV.cpp` (2896行起)

```cpp
template <int bitDepth>
void convertYUVTo16BitRGBAInternal(...) {
    // 位深参数
    const auto shiftTo16 = 16 - bitDepth;
    const auto inputMax = (1 << bitDepth) - 1;
    
    // 色度子采样参数
    const auto chromaSubH = format.getSubsamplingHor(); // 1, 2, 4
    const auto chromaSubV = format.getSubsamplingVer(); // 1, 2, 4
    
    for (每个像素) {
        // 读取 Y (亮度)
        int Y_raw = readRaw(srcY + y * w + x);
        // 应用亮度数学运算 (scale, offset, invert)
        uint16_t Y = extendTo16(Y_raw) << shiftTo16;
        
        // 读取 UV (色度) 考虑子采样
        unsigned chromaX = x / chromaSubH;
        unsigned chromaY = y / chromaSubV;
        int U_raw = readRaw(srcU + chromaY * (w/chromaSubH) + chromaX);
        int V_raw = readRaw(srcV + chromaY * (w/chromaSubH) + chromaX);
        
        // YUV → RGB 矩阵转换
        // 使用 ITU-R BT.709/BT.601 系数
        int R = (Yc * cRY + (U-32768) * cRUV + (V-32768) * ... ) >> 8;
        
        *target++ = clip(R, 0, 65535);
    }
}
```

**支持的 YUV 格式:**
- 子采样: 4:4:4, 4:2:2, 4:2:0, 4:0:0 (单色)
- 位深: 8, 10, 12, 16 bit
- 布局: Planar (YUV/YVU), Semi-Planar (NV12/NV21), Packed

---

## 6. SplitViewWidget 集成

### 6.1 HDR 渲染模式切换

**位置:** `YUViewLib/src/ui/views/SplitViewWidget.h/cpp`

```cpp
enum class RendererMode {  // 原名 HDRRenderingMode
    Software,  // 传统 QPainter (原名 Disabled)
    OpenGL,    // OpenGLRenderer OpenGL (原名 GL/Enabled)
    NativeDXGI,// NativeDXGIRenderer Windows HDR
    NativeEDR  // NativeEDRRenderer Metal (macOS, 原名 EDR)
};

void splitViewWidget::setRendererMode(RendererMode mode) {  // 原名 setHDRRenderingMode
    if (rendererMode == mode) return;  // 原名 hdrRenderingMode
    
    if (mode == RendererMode::NativeEDR) {
        // macOS: 优先创建 Metal EDR Widget
#ifdef Q_OS_MAC
        if (!edrRenderer) {  // 原名 hdr10WidgetMacEDR
            edrRenderer = std::make_unique<NativeEDRRenderer>(this);  // 原名 HDR10WidgetMacEDR
            // 应用 EDR 设置 (从 QSettings 加载)
            edrRenderer->setEOTF(m_edrEOTF);
            edrRenderer->setColorGamut(m_edrColorGamut);
            edrRenderer->setGammaValue(m_edrGamma);
            edrRenderer->setDiffuseWhiteNits(m_edrDiffuseWhite);
            edrRenderer->setHDRBrightness(m_edrBrightness);
        }
        edrRenderer->show();
#else
        // 非 macOS: 使用 OpenGLRenderer
        if (!glRenderer) {  // 原名 hdr10Widget
            glRenderer = std::make_unique<video::OpenGLRenderer>(this);  // 原名 HDR10Widget
            glRenderer->setParent(this);
            glRenderer->setZoom(this->zoomFactor);
            glRenderer->setMoveOffset(this->moveOffset);
        }
        glRenderer->show();
#endif
    } else {
#ifdef Q_OS_MAC
        if (edrRenderer) edrRenderer->hide();
#else
        if (glRenderer) glRenderer->hide();
#endif
    }
}
```

### 6.2 渲染流程协调

```cpp
void splitViewWidget::paintEvent(QPaintEvent*) {
#ifdef Q_OS_MAC
    if (rendererMode == RendererMode::NativeEDR && edrRenderer) {  // 原名 hdrRenderingMode == Enabled && hdr10WidgetMacEDR
        // macOS EDR 模式: Metal 处理 HDR 帧
        // 注意: Metal QWindow 是原生子窗口，覆盖在 Qt backing store 之上
        // QPainter 绘制的内容在 EDR 模式下不可见
        // 像素值等叠加内容通过 CALayer overlay 显示
        
        VideoFrame frame = frameHandler->getCurrentFrameAsVideoFrame();
        edrRenderer->setFrame(frame);
        edrRenderer->setFrameHandler(frameHandler);
        // QPainter 仅绘制网格、分割线等 (在 EDR 模式下可能不可见)
    }
#else
    if (rendererMode == RendererMode::OpenGL && glRenderer) {  // 原名 hdrRenderingMode == Enabled && hdr10Widget
        // 非 macOS HDR 模式: OpenGL 处理视频帧
        VideoFrame frame = frameHandler->getCurrentFrameAsVideoFrame();
        glRenderer->setFrame(frame);
        glRenderer->setFrameHandler(frameHandler); // 像素值查询
        
        // OpenGL 已渲染，跳过 QPainter 视频绘制
    }
#endif
    else {
        // 传统模式: QPainter 完整渲染
        painter.drawImage(...);
    }
    
    // 公共叠加层 (QPainter)
    paintRegularGrid(&painter);
    paintSplitLine(&painter);
}
```

### 6.3 状态同步机制

```cpp
// 缩放同步
void splitViewWidget::setZoom(double zoom) {
    zoomFactor = zoom;
#ifdef Q_OS_MAC
    if (edrRenderer) edrRenderer->setZoom(zoom);  // 原名 hdr10WidgetMacEDR
#endif
#ifdef Q_OS_WIN
    if (dxgiRenderer) dxgiRenderer->setZoom(zoom);
#endif
    if (glRenderer) glRenderer->setZoom(zoom);  // 原名 hdr10Widget
}

// 色彩设置同步 (通过 RendererSettingsDock::settingsChanged 信号触发)
// 实际代码中不再有独立的 setEDRSettings 函数，而是直接设置成员变量并应用到各 widget。
// 成员变量名为 m_colorEOTF / m_colorGamut / m_colorGamma / m_colorDiffuseWhite / m_colorBrightness
// (原文档中 m_edrEOTF 等命名已过时)
void splitViewWidget::applyColorSettingsToWidgets() {
#ifdef Q_OS_MAC
    if (edrRenderer) {
        edrRenderer->setEOTF(m_colorEOTF);
        edrRenderer->setColorGamut(m_colorGamut);
        edrRenderer->setGammaValue(m_colorGamma);
        edrRenderer->setDiffuseWhiteNits(m_colorDiffuseWhite);
        edrRenderer->setHDRBrightness(m_colorBrightness);
    }
#endif
#ifdef Q_OS_WIN
    if (dxgiRenderer) { /* 同上 */ }
#endif
    if (glRenderer) {
        glRenderer->setEOTF(m_colorEOTF);
        glRenderer->setColorGamut(m_colorGamut);
        glRenderer->setGammaValue(m_colorGamma);
        glRenderer->setDiffuseWhiteNits(m_colorDiffuseWhite);
        glRenderer->setHDRBrightness(m_colorBrightness);
    }
}

// 抖动开关
void splitViewWidget::onHDRDitheringToggled(bool checked) {
    if (glRenderer) glRenderer->setDithering(checked);  // 原名 hdr10Widget
}
```

### 6.4 EDR 设置持久化与初始化顺序

**关键修复**: 色彩设置必须在 widget 创建之前从 QSettings 加载，否则 widget 创建使用默认值 (sRGB=3, BT709=1) 而非保存的值 (PQ=0, BT2020=0)。

```cpp
void splitViewWidget::updateSettings() {
    // ⚠️ 正确顺序: 先加载色彩设置，再创建 widget
    QSettings settings;
    // 实际成员变量名为 m_color* (原文档中 m_edr* 命名已过时)
    m_colorEOTF = static_cast<RendererEOTF>(settings.value("View/EDR_EOTF", 0).toInt());  // 原名 HDR10_EOTF
    m_colorGamut = static_cast<RendererColorGamut>(settings.value("View/EDR_ColorGamut", 0).toInt());  // 原名 HDR10_ColorGamut
    m_colorGamma = settings.value("View/EDR_Gamma", 2.2).toFloat();
    m_colorDiffuseWhite = settings.value("View/EDR_DiffuseWhite", 203.0).toFloat();
    m_colorBrightness = settings.value("View/EDR_Brightness", 1.0).toFloat();
    
    // 然后才设置渲染模式 (触发 widget 创建)
    setRendererMode(rendererMode);  // 原名 setHDRRenderingMode(hdrRenderingMode)
}
```

> 注：QSettings key 使用 `View/EDR_*` 前缀（保留旧名以兼容）。实际成员变量为 `m_colorEOTF`/`m_colorGamut`/`m_colorGamma`/`m_colorDiffuseWhite`/`m_colorBrightness`（跨平台共用，非仅 EDR）。

### 6.5 EDR 菜单集成

```cpp
void splitViewWidget::addMenuActions(QMenu* menu) {
    menu->addAction(&actionHDRRendering);   // 开关 HDR
    menu->addAction(&actionHDRDithering);   // 开关抖动
    // 注: 渲染模式选择和色彩设置现通过 RendererSettingsDock dock 面板（ComboBox + SpinBox）
    // 不再有独立的 EDR 设置对话框菜单项
}

// 色彩设置现通过 RendererSettingsDock::settingsChanged 信号触发
// splitViewWidget::applyColorSettingsToWidgets() 将 m_color* 成员应用到各渲染器
// QSettings key 保留旧名以兼容：
//   View/HDRRenderer    (int: RendererMode 枚举值)
//   View/HDRDithering   (bool)
//   View/PremultipliedAlpha (bool)
//   View/EDR_EOTF       (int: 0=PQ, 1=HLG, 2=Gamma, 3=sRGB)
//   View/EDR_ColorGamut (int)
//   View/EDR_Gamma      (float)
//   View/EDR_DiffuseWhite (float)
//   View/EDR_Brightness (float)
```

> 注：旧版使用独立的 `EDRSettingsDialog` 对话框和 `showEDRSettings()` 函数，现已删除。色彩设置集成在 `RendererSettingsDock` dock 面板中，通过 `settingsChanged` 信号通知 `splitViewWidget` 调用 `applyColorSettingsToWidgets()`。

---

## 6.6 macOS EDR Metal 路径详解

### MacEDRRenderer - Metal 渲染核心

**位置:** `YUViewLib/src/ui/views/MacEDRRenderer.h/mm`

`MacEDRRenderer` 是 Objective-C++ 实现的 Metal 渲染器，直接与 macOS Core Animation 和 Metal API 交互。

```objc
// MacEDRRenderer 是 Objective-C++ 类（实际为 C++ 类，成员用 void* 存储 ObjC 对象）
// initialize(QWindow *window) 方法：

- (bool)initialize:(QWindow *)window {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    
    // 创建 CAMetalLayer
    CAMetalLayer *metalLayer = [CAMetalLayer layer];
    metalLayer.device = device;
    metalLayer.pixelFormat = MTLPixelFormatRGBA16Float;  // 16-bit 浮点
    metalLayer.framebufferOnly = NO;
    
    // EDR 启用 (macOS 10.15+)
    if (@available(macOS 10.15, *)) {
        metalLayer.wantsExtendedDynamicRangeContent = YES;
        // 色彩空间固定为 Extended Linear Display P3
        // (不随色域设置切换——色域转换在 Metal 着色器内部完成)
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearDisplayP3);
        metalLayer.colorspace = cs;
        CGColorSpaceRelease(cs);
    }
    
    // 创建独立 NSView + addSubview (避免 Qt NSView 双重 sRGB decode)
    NSView *view = (__bridge NSView *)reinterpret_cast<void *>(window->winId());
    NSView *edrView = [[NSView alloc] initWithFrame:view.bounds];
    edrView.wantsLayer = YES;
    edrView.layer = metalLayer;
    [view addSubview:edrView];  // addSubview 隔离 Qt NSView 色彩管理
    
    // CALayer overlay 也添加到 edrView.layer
    return true;
}
```

> 注：实际代码中 `MacEDRRenderer` 是 C++ 类（非 NSObject 子类），ObjC 对象通过 `(__bridge_retained void*)` 存储为 `void*` 成员。色彩空间**始终**为 `kCGColorSpaceExtendedLinearDisplayP3`，色域转换在着色器中完成。

**ARC 内存管理要点:**

```objc
// void* 存储 Objective-C 对象时必须使用 __bridge_retained
// __bridge (无所有权转移) = 创建 dangling pointer
void *storedObj = (__bridge_retained void *)objcObj;  // 正确: 增加引用计数

// CF 对象必须手动释放 (ARC 不管理 Core Foundation)
CGColorSpaceRef space = CGColorSpaceCreateWithName(...);
metalLayer.colorspace = space;
CGColorSpaceRelease(space);  // 必须: 释放 CF 引用
```

### NativeEDRRenderer - macOS EDR 入口

**位置:** `YUViewLib/src/ui/views/NativeEDRRenderer.h/cpp`（原名 `HDR10WidgetMacEDR.h/cpp`）

`NativeEDRRenderer` 继承自 `QWidget`（非 QObject），管理 Metal 渲染器生命周期和 CALayer 叠加层：

```cpp
class NativeEDRRenderer : public QWidget {  // 原名 HDR10WidgetMacEDR
    std::unique_ptr<MacEDRRenderer> m_renderer;
    QWindow *m_containerWindow = nullptr;   // MetalSurface 容器
    QWidget *m_containerWidget = nullptr;   // createWindowContainer 包装
    
    // 色彩处理参数 (使用 color:: 枚举)
    color::EOTF       m_eotf{color::EOTF::SRGB};
    color::ColorGamut m_colorGamut{color::ColorGamut::BT709};
    float      m_gammaValue{2.2f};
    float      m_diffuseWhiteNits{203.0f};
    float      m_hdrBrightness{1.0f};
    bool       m_premultipliedAlpha{true};
    
    // EDR 状态
    bool  m_edrSupported{false};
    float m_maxEDRValue{1.0f};
    
    // 帧数据
    VideoFrame m_currentFrame;
    FrameHandler *m_frameHandler{nullptr};
    
    // 视图控制
    double m_zoom{1.0};
    QPointF m_moveOffset{0, 0};
};

bool NativeEDRRenderer::initializeRenderer() {  // 原名 HDR10WidgetMacEDR::initializeRenderer
    // 创建 Metal 渲染器
    m_renderer = std::make_unique<MacEDRRenderer>();
    if (!m_renderer->initialize(m_containerWindow))  // 传入 QWindow
        return false;
    
    m_edrSupported = m_renderer->isEDRSupported();
    m_maxEDRValue = m_renderer->getMaxEDRValue();
    
    // 应用色彩处理设置 (从 QSettings 加载的值)
    m_renderer->setEOTF(m_eotf);
    m_renderer->setColorGamut(m_colorGamut);
    m_renderer->setGammaValue(m_gammaValue);
    m_renderer->setDiffuseWhiteNits(m_diffuseWhiteNits);
    m_renderer->setHDRBrightness(m_hdrBrightness);
    // CALayer overlay 由 MacEDRRenderer::initialize 内部创建
    return true;
}
```

> 注：`NativeEDRRenderer` 通过 `eventFilter` 转发 Metal QWindow NSView 拦截的鼠标事件。使用 `WA_TranslucentBackground` + `WA_NativeWindow`（不用 `WA_PaintOnScreen`）。

### CALayer Overlay 叠加方案

**问题**: Metal QWindow 是 macOS 窗口服务器直接合成的原生子窗口，位于 Qt backing store 上方。`SplitViewWidget` 的 QPainter 内容在 EDR 模式下不可见。

**解决方案**: 在 Metal 层的 NSView 上添加原生 CALayer 作为叠加层：

```objc
// 在 MacEDRRenderer.mm 中
- (void)addOverlayLayer:(void *)nativeWindow {
    NSView *nsView = (__bridge NSView *)nativeWindow;
    
    // 创建叠加层 (用于像素值、缩放指示器等)
    CALayer *overlayLayer = [CALayer layer];
    overlayLayer.frame = nsView.bounds;
    overlayLayer.opaque = NO;
    
    // 添加为 Metal 层 NSView 的子层
    [nsView.layer addSublayer:overlayLayer];
    self.overlayLayer = overlayLayer;
}

- (void)drawOverlay:(NSDictionary *)overlayData {
    // 在叠加层上绘制像素值、标尺等
    // 使用 Core Graphics 绘图 API
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    
    // 清除旧内容
    self.overlayLayer.sublayers = nil;
    
    // 创建文字图层
    for (NSString *text in overlayData[@"texts"]) {
        CATextLayer *textLayer = [CATextLayer layer];
        textLayer.string = text;
        textLayer.fontSize = 12;
        textLayer.foregroundColor = ...;
        [self.overlayLayer addSublayer:textLayer];
    }
    
    [CATransaction commit];
}
```

### MacEDRUtil - EDR 屏幕检测

**位置:** `YUViewLib/src/ui/views/MacEDRUtil.h/mm`

```objc
@implementation MacEDRUtil

+ (BOOL)isEDRSupported {
    // 检查主屏幕是否支持 EDR
    NSScreen *mainScreen = [NSScreen mainScreen];
    return mainScreen.maximumExtendedDynamicRangeColorComponentValue > 1.0;
}

+ (float)maxEDRValue {
    NSScreen *mainScreen = [NSScreen mainScreen];
    return mainScreen.maximumExtendedDynamicRangeColorComponentValue;
    // 例如: 返回 16.0 表示 16x SDR 白色
}

@end
```

---

## 7. FrameHandler 扩展

### 7.1 VideoFrame 集成

**位置:** `YUViewLib/src/video/FrameHandler.h/cpp`

```cpp
class FrameHandler {
    VideoFrame currentVideoFrame;  // 新增: 缓存高比特帧
    
public:
    // 新增方法: 获取统一帧接口
    VideoFrame getCurrentFrameAsVideoFrame() const {
        if (currentVideoFrame.isValid())
            return currentVideoFrame;
        return VideoFrame(currentImage); // 从 QImage 构造
    }
};
```

### 7.2 videoHandlerRGB 集成

```cpp
void videoHandlerRGB::loadFrame(int frameIndex) {
    // 原有 8-bit 转换
    currentImage = convertRGBToARGB(...);
    
    // 新增: 高比特源生成 16-bit 缓冲区
    if (srcPixelFormat.getBitsPerSample() > 8) {
        QVector<uint16_t> buffer16bit(numPixels * 4);
        convertRGBTo16BitRGBA(currentFrameRawData, ..., buffer16bit.data());
        currentVideoFrame.set16bitBuffer(std::move(buffer16bit), width, height);
    } else {
        currentVideoFrame.clear(); // 8-bit 源释放缓冲区
    }
}
```

### 7.3 videoHandlerYUV 集成

```cpp
void videoHandlerYUV::loadFrame(int frameIndex) {
    // 原有 8-bit 路径
    currentImage = convertYUVToARGB(...);
    
    // 新增 16-bit HDR 路径
    if (srcPixelFormat.getBitsPerSample() > 8) {
        QVector<uint16_t> buffer16bit(numPixels * 4);
        convertYUVTo16BitRGBA(currentFrameRawData, buffer16bit.data(), ...);
        currentVideoFrame.set16bitBuffer(std::move(buffer16bit), width, height);
    }
}
```

---

## 8. 像素值查询系统

### 8.1 FrameHandler 接口

```cpp
// 获取指定坐标的像素值 (带标签)
QList<QPair<QString, QString>> getPixelValues(QPoint pos, int frameIdx);

// RGB 实现示例
QList<QPair<QString, QString>> videoHandlerRGB::getPixelValues(...) {
    return {
        {"R", QString::number(r)},
        {"G", QString::number(g)},
        {"B", QString::number(b)}
    };
}

// YUV 实现示例
QList<QPair<QString, QString>> videoHandlerYUV::getPixelValues(...) {
    return {
        {"Y", QString::number(y)},
        {"U", QString::number(u)},
        {"V", QString::number(v)}
    };
}
```

---

## 9. 性能优化策略

### 9.1 内存优化

| 策略 | 实现 |
|------|------|
| 延迟分配 | 8-bit 源不分配 16-bit 缓冲区 |
| 移动语义 | `set16bitBuffer` 使用 `std::move` 转移所有权 |
| 缓冲区复用 | 纹理尺寸不变时 `glTexSubImage2D` 更新 |

### 9.2 渲染优化

```cpp
// paintGL 中的智能更新
void OpenGLRenderer::paintGL() {  // 原名 HDR10Widget::paintGL
    if (m_frameNeedsUpdate)
        updateTexture();  // 仅在帧变化时更新
    
    // 顶点数据重用
    if (!verticesChanged) {
        // 使用缓存的 VBO
    }
}
```

### 9.3 线程安全

```cpp
// FrameHandler 中的互斥锁
QMutexLocker writeLock(&currentImageSetMutex);
currentImage = newImage;
currentVideoFrame = newVideoFrame;
```

---

## 10. 平台兼容性

### 10.1 平台差异处理

```cpp
// macOS 排除 10-bit 请求 (QOpenGLWidget FBO 限制)
#ifndef Q_OS_MAC
    format.setRedBufferSize(10);
    // ...
#endif

// macOS EDR 路径: 使用 Metal 而非 OpenGL
#ifdef Q_OS_MAC
    // QOpenGLWidget 内部 FBO 为 8-bit RGBA, 无法输出 >1.0
    // EDR 通过 CAMetalLayer (RGBA16Float, ExtendedLinearDisplayP3) 实现
    edrRenderer = std::make_unique<NativeEDRRenderer>(this);  // 原名 hdr10WidgetMacEDR = std::make_unique<HDR10WidgetMacEDR>
#elif defined(Q_OS_WIN)
    // Windows HDR: DXGI scRGB swap chain (FP16, 系统处理 tonemapping)
    dxgiRenderer = std::make_unique<NativeDXGIRenderer>(this);
#else
    // Linux: 仅 OpenGL (SDR，不支持 >1.0 输出)
    glRenderer = std::make_unique<video::OpenGLRenderer>(this);
#endif

// 运行时检测回退
if (!m_supports10bit) {
    m_bitDepth = 8;
    // 降级到 8-bit 渲染
}
```

**关键平台差异:**

| 特性 | macOS | Windows | Linux |
|------|-------|---------|-------|
| HDR 渲染路径 | Metal (CAMetalLayer, `NativeEDRRenderer`) | DXGI scRGB swap chain (`NativeDXGIRenderer`) | 无（OpenGL SDR） |
| FBO/swap chain 位深 | 8-bit (QOpenGLWidget 限制) | FP16 scRGB | 8/10-bit (如支持) |
| EDR/HDR 输出 >1.0 | ✅ CAMetalLayer RGBA16Float | ✅ scRGB FP16 | ❌ |
| 系统处理 tonemapping | ✅ macOS 合成器 | ✅ (HDR 模式或 ACM 开启时) | ❌ |
| 叠加层 | CALayer (原生) | QPainter | PixelOverlay (QPainter) |
| EDR/HDR 检测 | `MacEDRUtil` (NSScreen API) | `DXGISwapChain` (IDXGIOutput6) | N/A |

> 注：旧架构中非 macOS 使用 OpenGL EDR 着色器（`hdr10_fragment_edr.glsl`），现已删除。Windows HDR 改由 `NativeDXGIRenderer` (DXGI/D3D11) 处理。OpenGL 路径仅用于 SDR 渲染。

### 10.2 OpenGL 版本要求

- **最低版本:** OpenGL 3.3 Core Profile
- **必需扩展:** `GL_RGBA16UI` 纹理格式
- **着色器版本:** GLSL 3.30 (`#version 330 core`)

---

## 11. 配置与状态管理

### 11.1 QSettings 集成

```cpp
// 背景色
QColor bg = settings.value("View/BackgroundColor", QColor(140,140,140)).value<QColor>();

// 抖动设置
bool dithering = settings.value("View/HDRDithering", false).toBool();

// 像素值十六进制显示
bool showHex = settings.value("ShowPixelValuesHex", false).toBool();

// EDR 色彩处理设置 (新增)
RendererEOTF eotf = static_cast<RendererEOTF>(settings.value("View/EDR_EOTF", 0).toInt());      // PQ=0 (原名 HDR10_EOTF)
RendererColorGamut gamut = static_cast<RendererColorGamut>(settings.value("View/EDR_ColorGamut", 0).toInt()); // BT2020=0 (原名 HDR10_ColorGamut)
float gamma = settings.value("View/EDR_Gamma", 2.2).toFloat();
float diffuseWhite = settings.value("View/EDR_DiffuseWhite", 203.0).toFloat();
float brightness = settings.value("View/EDR_Brightness", 1.0).toFloat();

// ⚠️ 重要: 色彩设置必须在 setRendererMode() 之前加载 (原名 setHDRRenderingMode)
// 否则 widget 创建将使用默认值 (sRGB=3, BT709=1) 而非保存的值
```

> 注：QSettings key 为 `View/EDR_*` 前缀（保留旧名以兼容），成员变量为 `m_color*`。

### 11.2 菜单集成

```cpp
void splitViewWidget::addMenuActions(QMenu* menu) {
    menu->addAction(&actionHDRRendering);   // 开关 HDR
    menu->addAction(&actionHDRDithering);   // 开关抖动
    // 渲染模式选择和色彩设置通过 RendererSettingsDock dock 面板
}
```

---

## 12. 提交详细分析

### 12.1 提交依赖关系

```
f4ea421a  Allow buffer allocation for smaller frame sizes (基线)
    │
    ├── 86511ae3  Add support for 10-bit OpenGL rendering with HDR10Widget
    │   └── 核心: HDR10Widget 类创建、OpenGL 初始化、基础着色器
    │
    ├── 657ec8dc  Add pixel value drawing support in HDR10Widget
    │   └── PixelOverlay 子窗口、基础像素值显示
    │
    ├── 7aade9ab  Add auto font color switching and coordinate/zoom display
    │   └── 智能字体颜色、坐标标尺、缩放指示器
    │
    ├── 1208313f  feat: Implement true 16-bit RGB data pathway
    │   └── VideoFrame 类、ConversionRGB 16-bit 转换、文档
    │
    ├── 73f546f7  feat: Add YUV 16-bit HDR rendering support
    │   └── videoHandlerYUV::convertYUVTo16BitRGBA
    │
    ├── 7e49948d  Update HDR10Widget with zoom controls and app integration
    │   └── SplitViewWidget 集成、主应用菜单、状态同步
    │
    ├── 65e9467a  Disable version check in Typedef.h
    │   └── 开发调试便利
    │
    └── d164e7ff  fix(HDR10Widget): Fix zoom/pan update, pixel value display...
        └── 修复同步问题、像素值格式、可见性控制
```

### 12.2 代码行分布

| 组件 | 新增代码行 | 核心文件 |
|------|--------|---------|
| OpenGLRenderer | 713 | OpenGLRenderer.cpp/h (原名 HDR10Widget.cpp/h) |
| VideoFrame | 192 | VideoFrame.cpp/h |
| RGB 16-bit 转换 | 488 | ConversionRGB.cpp/h |
| YUV 16-bit 转换 | 323 | videoHandlerYUV.cpp |
| SplitView 集成 | 152 | SplitViewWidget.cpp |
| EDR 着色器 | 70 | *.glsl (已删除 hdr10_fragment_edr.glsl) |
| NativeEDRRenderer | ~300 | NativeEDRRenderer.h/cpp (原名 HDR10WidgetMacEDR) |
| MacEDRRenderer | ~500 | MacEDRRenderer.h/mm |
| MacEDRUtil | ~80 | MacEDRUtil.h/mm |
| EDR 设置 | ~150 | 已删除 edrSettingsDialog.ui/h/cpp，并入 RendererSettingsDock |
| 文档 | 1,160 | *.md |

---

## 13. 潜在改进点

1. **色度插值优化:** YUV 转换目前使用最近邻，可添加双线性插值
2. **位深自动检测:** 当前硬编码为10-bit，可从文件元数据解析
3. **HDR 元数据:** 支持 PQ/HLG 传输函数和色域转换（EDR 着色器已部分实现）
4. **GPU 加速:** YUV 转换可移至着色器实时计算
5. **内存池:** VideoFrame 缓冲区可复用避免重复分配
6. **EDR 动态切换:** 运行时检测屏幕 EDR 能力变化（外接显示器切换）
7. **Windows HDR 支持:** 利用 DXGI HDR 元数据 API 实现原生 HDR 显示器输出
8. **CALayer overlay 性能优化:** 减少 CATransaction 开销，考虑异步绘制

---

## 14. 测试建议

### 14.1 功能测试

- [ ] 8-bit 视频 HDR 模式显示 (扩展路径)
- [ ] 10-bit RGB 视频正确渲染
- [ ] 10-bit YUV 4:2:0 视频正确渲染
- [ ] 像素值显示与实际文件一致
- [ ] 缩放/平移操作流畅
- [ ] 抖动功能可见效果

### 14.2 平台测试

- [ ] macOS EDR 路径 (Metal + CAMetalLayer)
- [ ] macOS SDR fallback (OpenGL)
- [ ] Windows (10-bit 路径)
- [ ] Linux (10-bit 路径)

### 14.3 EDR 功能测试

- [ ] EDR 模式开关正常工作
- [ ] RendererSettingsDock 设置面板 UI 无重叠（原 EDRSettingsDialog 已删除）
- [ ] EDR 设置持久化 (重启后恢复)
- [ ] PQ/HLG/Gamma/sRGB EOTF 转换正确
- [ ] BT.2020/BT.709/P3 色域转换正确
- [ ] 漫射白/亮度参数生效
- [ ] CALayer overlay 像素值显示正确
- [ ] EDR 检测 (支持/不支持 EDR 的屏幕)

### 14.4 边界测试

- [ ] 超大分辨率 (8K+)
- [ ] 非标准色度子采样
- [ ] 有限范围 vs 全范围
- [ ] 大端字节序格式
