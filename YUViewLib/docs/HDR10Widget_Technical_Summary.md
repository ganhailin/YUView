# HDR10Widget 技术开发详细总结

**文档生成日期:** 2026-05-10 (更新: 2025-06)  
**分析提交范围:** f4ea421a..d164e7ff (8个提交) + EDR 扩展提交  
**代码行数:** ~2,990行新增代码 + ~1,500行 EDR 扩展

---

## 1. 项目概述

本次开发为 YUView 添加了完整的 **HDR10 视频渲染支持**，实现了从 8-bit 到 16-bit 的高动态范围视频显示能力。核心创新是引入 `HDR10Widget` 作为基于 OpenGL 的专用渲染组件，替代传统的 8-bit QPainter 渲染路径。

**EDR 扩展**: 在 macOS 上，由于 `QOpenGLWidget` 的内部 FBO 为 8-bit RGBA（无法输出 >1.0 值），新增了 Metal 渲染路径通过 `HDR10WidgetMacEDR` + `MacEDRRenderer` 实现 EDR（Extended Dynamic Range）显示，使 HDR 视频能够在支持 EDR 的 Mac 显示器上呈现超过 SDR 白色的亮度值。

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
        │ ├─ HDR10Widget (OpenGL)│ ◄─── SDR fallback / 非 macOS
        │ └─ HDR10WidgetMacEDR  │ ◄─── macOS EDR (Metal) 新增
        └──────────┬─────────────┘
                   │
        ┌──────────┴──────────────┐
        │                         │
   ┌────▼───────┐          ┌──────▼─────────────┐
   │ HDR10Widget│          │ HDR10WidgetMacEDR   │ ◄── macOS EDR 新增
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

## 3. HDR10Widget - OpenGL 渲染引擎

### 3.1 类架构

**位置:** `YUViewLib/src/ui/views/HDR10Widget.h`

```cpp
class HDR10Widget : public QOpenGLWidget, protected QOpenGLFunctions {
    // OpenGL 资源
    QOpenGLShaderProgram *m_program;        // 标准着色器
    QOpenGLShaderProgram *m_programDither;  // 抖动着色器
    QOpenGLBuffer m_vbo;                    // 顶点缓冲
    QOpenGLVertexArrayObject m_vao;         // 顶点数组对象
    GLuint m_textureId{0};                    // 16-bit 纹理
    
    // 渲染状态
    VideoFrame m_currentFrame;
    int m_bitDepth{10};           // 显示位深 (OpenGL 格式检测)
    int m_sourceBitDepth{8};      // 源位深 (像素值显示用)
    bool m_ditheringEnabled{false};
    double m_zoom{1.0};
    QPointF m_moveOffset{0, 0};
    
    // 像素值叠加层
    class PixelOverlay : public QWidget {
        void paintEvent(QPaintEvent*) override; // QPainter 叠加绘制
    };
    PixelOverlay *m_pixelOverlay;
    
    // EDR 色彩处理参数
    HDR10_EOTF       m_eotf{HDR10_EOTF::SRGB};
    HDR10_ColorGamut m_colorGamut{HDR10_ColorGamut::BT709};
    float            m_gammaValue{2.2f};
    float            m_diffuseWhiteNits{203.0f};
    float            m_hdrBrightness{1.0f};
    
    // EDR 状态与着色器
    bool  m_edrSupported{false};
    float m_maxEDRValue{1.0f};
    QOpenGLShaderProgram *m_programEDR{nullptr};
};
```

**EDR 枚举类型:**

```cpp
enum class HDR10_EOTF {
    PQ    = 0,  // SMPTE ST 2084 Perceptual Quantizer
    HLG   = 1,  // ARIB STD-B67 Hybrid Log-Gamma
    Gamma = 2,  // 纯幂函数
    SRGB  = 3   // IEC 61966-2-1 sRGB 近似
};

enum class HDR10_ColorGamut {
    BT2020 = 0, // ITU-R BT.2020 宽色域
    BT709  = 1, // ITU-R BT.709 标准色域
    P3     = 2  // DCI-P3 色域
};
```

### 3.2 OpenGL 初始化与能力检测

```cpp
void HDR10Widget::initializeGL() {
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

#### 标准着色器 (`hdr10_fragment.glsl`)

```glsl
#version 330 core
uniform usampler2D texture16bit;  // 无符号整数 16-bit 纹理

void main() {
    uvec4 raw = texture(texture16bit, vTexCoord);
    // 数据统一存储在 16-bit 范围 (0-65535)
    // 8-bit 源: ×257 扩展
    // 10-bit 源: <<6 扩展
    // 16-bit 源: 原生值
    vec3 color = vec3(raw.rgb) / 65535.0;
    fragColor = vec4(color, 1.0);
}
```

#### 抖动着色器 (`hdr10_fragment_dither.glsl`)

**目标:** 在 8-bit 显示器上显示 10/16-bit 内容时减少色带 (banding)

```glsl
// 4x4 Bayer 矩阵 (16级)
float matrix[16] = float[](
    -0.001953125, 0.000000000, ... // ±0.5/256 范围
);

void main() {
    uvec4 raw = texture(texture16bit, vTexCoord);
    vec3 color = vec3(raw.rgb) / 65535.0;
    
    // 添加空间抖动，分散量化误差
    float dither = bayerDither4x4(gl_FragCoord.xy);
    color = color + dither;
    
    fragColor = vec4(color, 1.0);
}
```

**算法说明:**
- Bayer 矩阵产生 16 级额外精度 (4-bit)
- 抖动值范围 ±0.5 LSB (8-bit)
- 通过空间分布模拟高位深效果

### 3.4 纹理管理策略

```cpp
void HDR10Widget::updateTexture() {
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

### 3.5 EDR 着色器 (`hdr10_fragment_edr.glsl`)

EDR 着色器实现完整的 HDR 色彩处理管线，将 16-bit 编码值转换为 >1.0 的线性光输出。

```glsl
#version 330 core
uniform usampler2D texture16bit;
uniform int   eotf;           // HDR10_EOTF 枚举值
uniform int   colorGamut;     // HDR10_ColorGamut 枚举值
uniform float gammaValue;     // Gamma 值 (仅 eotf=Gamma 时使用)
uniform float diffuseWhiteNits; // 漫射白参考亮度 (nits)
uniform float hdrBrightness;    // HDR 亮度倍率

// EOTF 转换: 编码值 → 线性光
vec3 applyEOTF(vec3 coded) {
    if (eotf == 0) return pqToLinear(coded);       // PQ (ST 2084)
    if (eotf == 1) return hlgToLinear(coded);      // HLG
    if (eotf == 2) return pow(coded, vec3(gammaValue)); // Gamma
    return sRGBToLinear(coded);                    // sRGB 近似
}

// 色域转换: 源色域 → 目标色域 (3×3 矩阵)
vec3 applyGamutConversion(vec3 linear) {
    mat3 gamutMatrix = getGamutMatrix(colorGamut);
    return gamutMatrix * linear;
}

void main() {
    uvec4 raw = texture(texture16bit, vTexCoord);
    vec3 coded = vec3(raw.rgb) / 65535.0;
    
    vec3 linear = applyEOTF(coded);
    linear = applyGamutConversion(linear);
    
    // HDR 亮度缩放: 线性光 × (hdrBrightness / diffuseWhiteNits)
    // hdrBrightness > 1.0 → 输出 >1.0 → 触发 EDR/HDR 显示
    vec3 output = linear * (hdrBrightness / diffuseWhiteNits) * (diffuseWhiteNits / 203.0);
    
    fragColor = vec4(output, 1.0);
}
```

**色彩处理管线:**

1. **EOTF 转换**: PQ → `((coded/10000)^m)^1/n`；HLG → OETF⁻¹ + 1/12 转换；Gamma → `pow(coded, γ)`；sRGB → 近似幂函数
2. **色域转换**: BT.2020→Display P3、BT.709→Display P3、P3→直接输出
3. **亮度缩放**: `output = linear × hdrBrightness / diffuseWhiteNits`，值 >1.0 在支持浮点 FBO 的 GPU 上触发 HDR 显示
4. **macOS 限制**: 此着色器在 macOS 上输出 >1.0 值会被 8-bit FBO 截断到 0-1 范围，因此 macOS EDR 必须通过 Metal 路径实现

---

## 4. 像素值显示系统

### 4.1 架构设计

HDR10Widget 采用 **OpenGL + QPainter 叠加** 的混合渲染模式：

```
┌─────────────────────────────────────┐
│ HDR10Widget (OpenGLWidget)          │
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
enum class HDRRenderingMode {
    Disabled,  // 传统 QPainter
    Enabled    // HDR10Widget OpenGL / HDR10WidgetMacEDR Metal (macOS)
};

void splitViewWidget::setHDRRenderingMode(HDRRenderingMode mode) {
    if (hdrRenderingMode == mode) return;
    
    if (mode == Enabled) {
        // macOS: 优先创建 Metal EDR Widget
#ifdef Q_OS_MAC
        if (!hdr10WidgetMacEDR) {
            hdr10WidgetMacEDR = std::make_unique<HDR10WidgetMacEDR>(this);
            // 应用 EDR 设置 (从 QSettings 加载)
            hdr10WidgetMacEDR->setEOTF(m_edrEOTF);
            hdr10WidgetMacEDR->setColorGamut(m_edrColorGamut);
            hdr10WidgetMacEDR->setGammaValue(m_edrGamma);
            hdr10WidgetMacEDR->setDiffuseWhiteNits(m_edrDiffuseWhite);
            hdr10WidgetMacEDR->setHDRBrightness(m_edrBrightness);
        }
        hdr10WidgetMacEDR->show();
#else
        // 非 macOS: 使用 OpenGL HDR10Widget
        if (!hdr10Widget) {
            hdr10Widget = std::make_unique<video::HDR10Widget>(this);
            hdr10Widget->setParent(this);
            hdr10Widget->setZoom(this->zoomFactor);
            hdr10Widget->setMoveOffset(this->moveOffset);
        }
        hdr10Widget->show();
#endif
    } else {
#ifdef Q_OS_MAC
        if (hdr10WidgetMacEDR) hdr10WidgetMacEDR->hide();
#else
        if (hdr10Widget) hdr10Widget->hide();
#endif
    }
}
```

### 6.2 渲染流程协调

```cpp
void splitViewWidget::paintEvent(QPaintEvent*) {
#ifdef Q_OS_MAC
    if (hdrRenderingMode == Enabled && hdr10WidgetMacEDR) {
        // macOS EDR 模式: Metal 处理 HDR 帧
        // 注意: Metal QWindow 是原生子窗口，覆盖在 Qt backing store 之上
        // QPainter 绘制的内容在 EDR 模式下不可见
        // 像素值等叠加内容通过 CALayer overlay 显示
        
        VideoFrame frame = frameHandler->getCurrentFrameAsVideoFrame();
        hdr10WidgetMacEDR->setFrame(frame);
        hdr10WidgetMacEDR->setFrameHandler(frameHandler);
        // QPainter 仅绘制网格、分割线等 (在 EDR 模式下可能不可见)
    }
#else
    if (hdrRenderingMode == Enabled && hdr10Widget) {
        // 非 macOS HDR 模式: OpenGL 处理视频帧
        VideoFrame frame = frameHandler->getCurrentFrameAsVideoFrame();
        hdr10Widget->setFrame(frame);
        hdr10Widget->setFrameHandler(frameHandler); // 像素值查询
        
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
    if (hdr10WidgetMacEDR) hdr10WidgetMacEDR->setZoom(zoom);
#else
    if (hdr10Widget) hdr10Widget->setZoom(zoom);
#endif
}

// EDR 设置同步
void splitViewWidget::setEDRSettings(HDR10_EOTF eotf, HDR10_ColorGamut gamut,
                                      float gamma, float diffuseWhite, float brightness) {
    m_edrEOTF = eotf;
    m_edrColorGamut = gamut;
    m_edrGamma = gamma;
    m_edrDiffuseWhite = diffuseWhite;
    m_edrBrightness = brightness;
    
#ifdef Q_OS_MAC
    if (hdr10WidgetMacEDR) {
        hdr10WidgetMacEDR->setEOTF(eotf);
        hdr10WidgetMacEDR->setColorGamut(gamut);
        hdr10WidgetMacEDR->setGammaValue(gamma);
        hdr10WidgetMacEDR->setDiffuseWhiteNits(diffuseWhite);
        hdr10WidgetMacEDR->setHDRBrightness(brightness);
    }
#else
    if (hdr10Widget) {
        hdr10Widget->setEOTF(eotf);
        hdr10Widget->setColorGamut(gamut);
        hdr10Widget->setGammaValue(gamma);
        hdr10Widget->setDiffuseWhiteNits(diffuseWhite);
        hdr10Widget->setHDRBrightness(brightness);
    }
#endif
}

// 抖动开关
void splitViewWidget::onHDRDitheringToggled(bool checked) {
    if (hdr10Widget) hdr10Widget->setDithering(checked);
}
```

### 6.4 EDR 设置持久化与初始化顺序

**关键修复**: EDR 设置必须在 widget 创建之前从 QSettings 加载，否则 widget 创建使用默认值 (sRGB=3, BT709=1) 而非保存的值 (PQ=0, BT2020=0)。

```cpp
void splitViewWidget::updateSettings() {
    // ⚠️ 正确顺序: 先加载 EDR 设置，再创建 widget
    QSettings settings;
    m_edrEOTF = static_cast<HDR10_EOTF>(settings.value("EDR/EOTF", 0).toInt());
    m_edrColorGamut = static_cast<HDR10_ColorGamut>(settings.value("EDR/ColorGamut", 0).toInt());
    m_edrGamma = settings.value("EDR/Gamma", 2.2).toFloat();
    m_edrDiffuseWhite = settings.value("EDR/DiffuseWhite", 203.0).toFloat();
    m_edrBrightness = settings.value("EDR/Brightness", 1.0).toFloat();
    
    // 然后才设置渲染模式 (触发 widget 创建)
    setHDRRenderingMode(hdrRenderingMode);
}
```

### 6.5 EDR 菜单集成

```cpp
void splitViewWidget::addMenuActions(QMenu* menu) {
    menu->addAction(&actionHDRRendering);   // 开关 HDR
    menu->addAction(&actionEDRMode);        // 开关 EDR (macOS)
    menu->addAction(&actionEDRSettings);    // EDR 设置对话框
}

void splitViewWidget::showEDRSettings() {
    EDRSettingsDialog dialog(this);
    dialog.setEOTF(m_edrEOTF);
    dialog.setColorGamut(m_edrColorGamut);
    dialog.setGammaValue(m_edrGamma);
    dialog.setDiffuseWhiteNits(m_edrDiffuseWhite);
    dialog.setHDRBrightness(m_edrBrightness);
    
    if (dialog.exec() == QDialog::Accepted) {
        setEDRSettings(dialog.eotf(), dialog.colorGamut(), dialog.gammaValue(),
                       dialog.diffuseWhiteNits(), dialog.hdrBrightness());
        // 保存到 QSettings
        QSettings settings;
        settings.setValue("EDR/EOTF", static_cast<int>(dialog.eotf()));
        settings.setValue("EDR/ColorGamut", static_cast<int>(dialog.colorGamut()));
        settings.setValue("EDR/Gamma", dialog.gammaValue());
        settings.setValue("EDR/DiffuseWhite", dialog.diffuseWhiteNits());
        settings.setValue("EDR/Brightness", dialog.hdrBrightness());
    }
}
```

---

## 6.6 macOS EDR Metal 路径详解

### MacEDRRenderer - Metal 渲染核心

**位置:** `YUViewLib/src/ui/views/MacEDRRenderer.h/mm`

`MacEDRRenderer` 是 Objective-C++ 实现的 Metal 渲染器，直接与 macOS Core Animation 和 Metal API 交互。

```objc
@interface MacEDRRenderer : NSObject <MTLMTLViewDelegate>
    @property (nonatomic, strong) MTLRenderPipelineState *pipelineState;
    @property (nonatomic, strong) MTLTexture *frameTexture;
@end

@implementation MacEDRRenderer

- (void)initRenderer:(void *)nativeWindow {
    // 获取 Metal 设备
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    
    // 创建 CAMetalLayer
    CAMetalLayer *metalLayer = [CAMetalLayer layer];
    metalLayer.device = device;
    metalLayer.pixelFormat = MTLPixelFormatRGBA16Float;  // ← 关键: 16-bit 浮点
    metalLayer.wantsExtendedDynamicRangeContent = YES;    // ← 触发 EDR
    metalLayer.framebufferOnly = YES;
    
    // 设置色彩空间 (根据 EDR 设置)
    switch (colorGamut) {
        case BT2020:
            metalLayer.colorspace = CGColorSpaceCreateWithName(kCGColorSpaceITUR_2020);
            break;
        case P3:
            metalLayer.colorspace = CGColorSpaceCreateWithName(kCGColorSpaceDisplayP3);
            break;
        default: // BT709/sRGB
            CGColorSpaceRef srgbSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            metalLayer.colorspace = srgbSpace;
            CGColorSpaceRelease(srgbSpace);  // ← 修复: 避免内存泄漏
            break;
    }
    
    // 添加 Metal 层到 NSView
    NSView *nsView = (__bridge NSView *)nativeWindow;
    nsView.layer = metalLayer;
    nsView.wantsLayer = YES;
}
@end
```

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

### HDR10WidgetMacEDR - macOS EDR 入口

**位置:** `YUViewLib/src/ui/views/HDR10WidgetMacEDR.h/cpp`

`HDR10WidgetMacEDR` 管理 Metal 渲染器生命周期和 CALayer 叠加层：

```cpp
class HDR10WidgetMacEDR : public QObject {
    std::unique_ptr<MacEDRRenderer> m_renderer;
    
    // EDR 色彩处理参数
    HDR10_EOTF       m_eotf{HDR10_EOTF::SRGB};
    HDR10_ColorGamut m_colorGamut{HDR10_ColorGamut::BT709};
    float            m_gammaValue{2.2f};
    float            m_diffuseWhiteNits{203.0f};
    float            m_hdrBrightness{1.0f};
    
    // 帧数据
    VideoFrame m_currentFrame;
    FrameHandler *m_frameHandler{nullptr};
    
    // 视图控制
    double m_zoom{1.0};
    QPointF m_moveOffset{0, 0};
};

void HDR10WidgetMacEDR::initializeRenderer() {
    // 创建 Metal 渲染器
    void *nativeView = getNativeNSView(parentWidget());
    m_renderer = std::make_unique<MacEDRRenderer>();
    m_renderer->initRenderer(nativeView);
    
    // 应用 EDR 设置 (从 QSettings 加载的值)
    m_renderer->setEOTF(m_eotf);
    m_renderer->setColorGamut(m_colorGamut);
    m_renderer->setGammaValue(m_gammaValue);
    m_renderer->setDiffuseWhiteNits(m_diffuseWhiteNits);
    m_renderer->setHDRBrightness(m_hdrBrightness);
    
    // 创建 CALayer 叠加层 (替代 QPainter 叠加)
    addCALayerOverlay(nativeView);
}
```

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
void HDR10Widget::paintGL() {
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
    // EDR 通过 CAMetalLayer (RGBA16Float) 实现
    hdr10WidgetMacEDR = std::make_unique<HDR10WidgetMacEDR>(this);
#else
    // 非 macOS: 使用 OpenGL EDR 着色器 (需要浮点 FBO 支持)
    hdr10Widget = std::make_unique<video::HDR10Widget>(this);
#endif

// 运行时检测回退
if (!m_supports10bit) {
    m_bitDepth = 8;
    // 降级到 8-bit 渲染
}
```

**关键平台差异:**

| 特性 | macOS | Windows/Linux |
|------|-------|---------------|
| HDR 渲染路径 | Metal (CAMetalLayer) | OpenGL (EDR 着色器) |
| FBO 位深 | 8-bit (QOpenGLWidget 限制) | 可请求 10-bit/浮点 |
| EDR 输出 >1.0 | ✅ CAMetalLayer RGBA16Float | ✅ 浮点 FBO (如支持) |
| 叠加层 | CALayer (原生) | PixelOverlay (QPainter) |
| EDR 检测 | MacEDRUtil (NSScreen API) | GPU FBO 格式检测 |

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
HDR10_EOTF eotf = static_cast<HDR10_EOTF>(settings.value("EDR/EOTF", 0).toInt());      // PQ=0
HDR10_ColorGamut gamut = static_cast<HDR10_ColorGamut>(settings.value("EDR/ColorGamut", 0).toInt()); // BT2020=0
float gamma = settings.value("EDR/Gamma", 2.2).toFloat();
float diffuseWhite = settings.value("EDR/DiffuseWhite", 203.0).toFloat();
float brightness = settings.value("EDR/Brightness", 1.0).toFloat();

// ⚠️ 重要: EDR 设置必须在 setHDRRenderingMode() 之前加载
// 否则 widget 创建将使用默认值 (sRGB=3, BT709=1) 而非保存的值
```

### 11.2 菜单集成

```cpp
void splitViewWidget::addMenuActions(QMenu* menu) {
    menu->addAction(&actionHDRRendering);   // 开关 HDR
    menu->addAction(&actionHDRDithering);   // 开关抖动
    menu->addAction(&actionEDRMode);        // 开关 EDR (macOS)
    menu->addAction(&actionEDRSettings);    // EDR 设置对话框
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
| HDR10Widget | 713 | HDR10Widget.cpp/h |
| VideoFrame | 192 | VideoFrame.cpp/h |
| RGB 16-bit 转换 | 488 | ConversionRGB.cpp/h |
| YUV 16-bit 转换 | 323 | videoHandlerYUV.cpp |
| SplitView 集成 | 152 | SplitViewWidget.cpp |
| EDR 着色器 | 70 | *.glsl |
| HDR10WidgetMacEDR | ~300 | HDR10WidgetMacEDR.h/cpp |
| MacEDRRenderer | ~500 | MacEDRRenderer.h/mm |
| MacEDRUtil | ~80 | MacEDRUtil.h/mm |
| EDR 设置对话框 | ~150 | edrSettingsDialog.ui/h/cpp |
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
- [ ] EDR 设置对话框 UI 无重叠
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
