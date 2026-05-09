# HDR10Widget 技术开发详细总结

**文档生成日期:** 2026-05-10  
**分析提交范围:** f4ea421a..d164e7ff (8个提交)  
**代码行数:** ~2,990行新增代码

---

## 1. 项目概述

本次开发为 YUView 添加了完整的 **HDR10 视频渲染支持**，实现了从 8-bit 到 16-bit 的高动态范围视频显示能力。核心创新是引入 `HDR10Widget` 作为基于 OpenGL 的专用渲染组件，替代传统的 8-bit QPainter 渲染路径。

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
        │ └─ HDR10Widget (OpenGL)│ ◄─── 新模式
        └──────────┬─────────────┘
                   │
        ┌──────────▼──────────────┐
        │ HDR10Widget             │ ◄─── OpenGL 10-bit 渲染核心
        │ ├─ GL_RGBA16UI 纹理     │
        │ ├─ 自定义着色器          │
        │ └─ Bayer 抖动算法       │
        └─────────────────────────┘
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
    Enabled    // HDR10Widget OpenGL
};

void splitViewWidget::setHDRRenderingMode(HDRRenderingMode mode) {
    if (mode == Enabled) {
        // 延迟创建 HDR10Widget
        if (!hdr10Widget) {
            hdr10Widget = std::make_unique<video::HDR10Widget>(this);
            hdr10Widget->setParent(this);
            
            // 同步状态
            hdr10Widget->setZoom(this->zoomFactor);
            hdr10Widget->setMoveOffset(this->moveOffset);
        }
        hdr10Widget->show();
    } else {
        if (hdr10Widget) hdr10Widget->hide();
    }
}
```

### 6.2 渲染流程协调

```cpp
void splitViewWidget::paintEvent(QPaintEvent*) {
    if (hdrRenderingMode == Enabled && hdr10Widget) {
        // HDR 模式: OpenGL 处理视频帧
        VideoFrame frame = frameHandler->getCurrentFrameAsVideoFrame();
        hdr10Widget->setFrame(frame);
        hdr10Widget->setFrameHandler(frameHandler); // 像素值查询
        
        // OpenGL 已渲染，跳过 QPainter 视频绘制
        // 但仍需绘制叠加层 (网格、分割线等)
    } else {
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
    if (hdr10Widget) hdr10Widget->setZoom(zoom);
}

// 平移同步
void splitViewWidget::setMoveOffset(QPointF offset) {
    moveOffset = offset;
    if (hdr10Widget) hdr10Widget->setMoveOffset(offset);
}

// 抖动开关
void splitViewWidget::onHDRDitheringToggled(bool checked) {
    if (hdr10Widget) hdr10Widget->setDithering(checked);
}
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
// macOS 排除 10-bit 请求
#ifndef Q_OS_MAC
    format.setRedBufferSize(10);
    // ...
#endif

// 运行时检测回退
if (!m_supports10bit) {
    m_bitDepth = 8;
    // 降级到 8-bit 渲染
}
```

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
```

### 11.2 菜单集成

```cpp
void splitViewWidget::addMenuActions(QMenu* menu) {
    menu->addAction(&actionHDRRendering);   // 开关 HDR
    menu->addAction(&actionHDRDithering);   // 开关抖动
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
| 着色器 | 70 | *.glsl |
| 文档 | 1,160 | *.md |

---

## 13. 潜在改进点

1. **色度插值优化:** YUV 转换目前使用最近邻，可添加双线性插值
2. **位深自动检测:** 当前硬编码为10-bit，可从文件元数据解析
3. **HDR 元数据:** 支持 PQ/HLG 传输函数和色域转换
4. **GPU 加速:** YUV 转换可移至着色器实时计算
5. **内存池:** VideoFrame 缓冲区可复用避免重复分配

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

- [ ] macOS (8-bit 回退路径)
- [ ] Windows (10-bit 路径)
- [ ] Linux (10-bit 路径)

### 14.3 边界测试

- [ ] 超大分辨率 (8K+)
- [ ] 非标准色度子采样
- [ ] 有限范围 vs 全范围
- [ ] 大端字节序格式
