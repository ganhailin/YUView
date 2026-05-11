# YUView macOS EDR (Extended Dynamic Range) 实现文档

**文档日期:** 2026-05-10  
**版本:** 1.0  
**作者:** YUView 开发团队

---

## 1. 概述

本文档描述 YUView 在 macOS 平台上实现的 **EDR (Extended Dynamic Range)** 支持。EDR 是 Apple 提供的 HDR 渲染技术，允许应用程序输出超出传统 SDR (Standard Dynamic Range) 范围 (0.0-1.0) 的亮度值，最高可达显示器的最大 HDR 亮度能力。

### 1.1 什么是 EDR?

EDR 是 macOS 的 **扩展动态范围** 技术，它允许 OpenGL/Metal 应用程序：

- 输出 **线性光 (linear light)** 值，可以超过 1.0 (SDR 白点)
- 自动根据显示器能力进行 **色调映射 (tone mapping)**
- 支持从 SDR (100 nits) 到 HDR (1000+ nits) 的动态范围
- 根据环境光线和电源管理自动调整最大亮度

### 1.2 与传统 HDR10 的区别

| 特性 | 传统 HDR10 | macOS EDR |
|------|-----------|-----------|
| 色彩缓冲区 | 10-bit 整数 (0-1023) | 16-bit 浮点 (可超过 1.0) |
| 输出空间 | Gamma 编码 | 线性光 (linear) |
| 色调映射 | 应用程序负责 | 系统/显示器自动处理 |
| 显示器支持 | 需要 HDR 显示器 | 任何显示器 (自动降级到 SDR) |
| 亮度范围 | 固定 (通常 1000 nits) | 动态 (根据环境光调整) |

---

## 2. 架构设计

### 2.1 组件架构

```
┌─────────────────────────────────────────────────────────────────┐
│                        HDR10Widget                              │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │                    OpenGL 渲染管线                          │  │
│  │  ┌─────────────┐    ┌─────────────┐    ┌─────────────────┐ │  │
│  │  │  标准着色器  │    │  抖动着色器  │    │   EDR 着色器    │ │  │
│  │  │ (hdr10_     │    │ (hdr10_     │    │ (hdr10_         │ │  │
│  │  │  fragment)  │    │  fragment_  │    │  fragment_edr)  │ │  │
│  │  │             │    │  dither)    │    │                 │ │  │
│  │  │ 16-bit 输入  │    │ 16-bit 输入  │    │ 16-bit 输入      │ │  │
│  │  │ → 0-1.0 输出 │    │ → 抖动处理  │    │ → PQ/HLG 解码   │ │  │
│  │  │             │    │ → 0-1.0 输出 │    │ → 线性光输出    │ │  │
│  │  └─────────────┘    └─────────────┘    │   (>1.0 可能)   │ │  │
│  │                                        └─────────────────┘ │  │
│  │                          ▲                                  │  │
│  │                          │ (macOS + EDR 启用时使用)          │  │
│  └──────────────────────────┼───────────────────────────────────┘  │
│                             │                                    │
│  ┌──────────────────────────┴───────────────────────────────────┐│
│  │              YUViewMacOSEDRHelper (macOS 专用)                 ││
│  │  ┌─────────────────────────────────────────────────────────┐   ││
│  │  │  • 检测 EDR 支持 (isEDRSupported)                        │   ││
│  │  │  • 检测 HDR 显示器 (isEDRDisplayAvailable)              │   ││
│  │  │  • 获取最大亮度 (getMaxEDRBrightness)                    │   ││
│  │  │  • 启用 EDR (enableEDR)                                  │   ││
│  │  └─────────────────────────────────────────────────────────┘   ││
│  └────────────────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────────────┘
```

### 2.2 数据流

```
HDR 视频文件 (PQ/HLG 编码)
    ↓
VideoFrame (16-bit RGBA 缓冲区)
    ↓
OpenGL 纹理 (GL_RGBA16UI)
    ↓
EDR 着色器处理:
    ├─ PQ/HLG → 线性光转换 (pqToLinear/hlgToLinear)
    ├─ 色彩空间转换 (BT.2020 → RGB)
    └─ 输出线性值 (>1.0 用于 HDR 高亮)
    ↓
OpenGL 浮点帧缓冲区 (通过 EDR 启用)
    ↓
macOS EDR 系统合成器
    ↓
显示器 (自动色调映射到显示能力)
```

---

## 3. 核心实现

### 3.1 YUViewMacOSEDRHelper (Objective-C++)

**文件:** `YUViewLib/src/ui/views/YUViewMacOSEDRHelper.h` / `.mm`

提供 C++ 接口访问 macOS 原生 EDR API：

```cpp
// 检测 EDR 支持
bool isEDRSupported();

// 检测当前窗口所在的显示器是否支持 HDR
bool isEDRDisplayAvailable(quintptr nativeWindowHandle);

// 获取显示器的 EDR 亮度上限 (1.0 = SDR, 4.0 = 400 nits, 10.0 = 1000 nits)
float getMaxEDRBrightness(quintptr nativeWindowHandle);

// 在指定视图上启用 EDR
bool enableEDR(quintptr nativeViewHandle);
```

#### 关键技术点

1. **NSOpenGLView 扩展属性:**
   ```objc
   [glView setWantsExtendedDynamicRangeOpenGLSurface:YES];
   ```

2. **显示器 HDR 能力检测:**
   ```objc
   NSScreen *screen = [window screen];
   float maxEDR = [screen maximumExtendedDynamicRangeColorComponentValue];
   ```

3. **浮点像素格式:**
   - 使用 `NSOpenGLPFAColorFloat` 请求浮点颜色缓冲区
   - 64-bit 颜色 (16-bit float 每通道)

### 3.2 HDR10Widget EDR 支持

**文件:** `YUViewLib/src/ui/views/HDR10Widget.h` / `.cpp`

#### 修改点

1. **构造函数中启用 EDR:**
   ```cpp
   #ifdef Q_OS_MAC
   if (YUViewMacOSEDRHelper::isEDRSupported())
   {
       format.setColorSpace(QSurfaceFormat::scRGBColorSpace);
   }
   #endif
   ```

2. **HDR 元数据结构:**
   ```cpp
   struct HDRMetadata {
       float maxContentLightLevel;      // MaxCLL (nits)
       float maxFrameAverageLightLevel; // MaxFALL (nits)
       TransferFunction transferFunction; // PQ/HLG/Linear
       ColorSpace colorSpace;           // BT.2020/P3/BT.709
       float edrHeadroom;               // EDR 亮度上限
   };
   ```

3. **运行时 EDR 检测:**
   ```cpp
   void HDR10Widget::checkEDRSupport()
   {
       #ifdef Q_OS_MAC
       m_edrAvailable = YUViewMacOSEDRHelper::isEDRSupported();
       if (m_edrAvailable)
       {
           m_edrHeadroom = YUViewMacOSEDRHelper::getMaxEDRBrightness(winId());
       }
       #endif
   }
   ```

### 3.3 EDR 着色器

**文件:** `YUViewLib/shaders/hdr10_fragment_edr.glsl`

#### 功能

1. **PQ (SMPTE ST 2084) 解码:**
   ```glsl
   float pqToLinear(float pq) {
       float np = pow(pq, 1.0 / PQ_M2);
       float linear = pow(max(np - PQ_C1, 0.0) / (PQ_C2 - PQ_C3 * np), 1.0 / PQ_M1);
       return linear * PQ_MAX_NITS;  // 转换为 nits
   }
   ```

2. **HLG (ARIB STD-B67) 解码:**
   ```glsl
   float hlgToLinear(float hlg) {
       if (hlg <= 0.5) {
           return hlg * hlg / 3.0 * HLG_MAX_NITS;
       } else {
           return (exp((hlg - HLG_C) / HLG_A) + HLG_B) / 12.0 * HLG_MAX_NITS;
       }
   }
   ```

3. **色彩空间转换:**
   - BT.2020 → RGB
   - P3-D65 → RGB
   - BT.709 (identity)

4. **EDR 输出:**
   ```glsl
   // 归一化到 SDR 白点 (100 nits)
   vec3 outputColor = linearColor / 100.0;
   
   // 应用 EDR headroom
   outputColor = clamp(outputColor, 0.0, edrHeadroom * 2.0);
   ```

#### Uniform 变量

| Uniform | 类型 | 说明 |
|---------|------|------|
| `texture16bit` | usampler2D | 16-bit 输入纹理 |
| `maxDisplayBrightness` | float | 显示器最大亮度 (nits) |
| `maxContentBrightness` | float | 内容最大亮度 (MaxCLL) |
| `edrHeadroom` | float | EDR 亮度上限倍数 |
| `transferFunction` | int | 0=Linear, 1=PQ, 2=HLG |
| `colorSpace` | int | 0=BT.709, 1=BT.2020, 2=P3 |

---

## 4. 构建配置

### 4.1 YUViewLib.pro 修改

```qmake
# macOS: Link against Cocoa and OpenGL frameworks for EDR support
macx {
    LIBS += -framework Cocoa -framework OpenGL
    # Ensure .mm files are compiled as Objective-C++
    QMAKE_CXXFLAGS += -x objective-c++
}
```

### 4.2 资源文件更新

**shaders.qrc:**
```xml
<qresource prefix="/shaders">
    <file>hdr10_vertex.glsl</file>
    <file>hdr10_fragment.glsl</file>
    <file>hdr10_fragment_dither.glsl</file>
    <file>hdr10_fragment_edr.glsl</file>  <!-- 新增 -->
</qresource>
```

---

## 5. 使用指南

### 5.1 启用 EDR 渲染

```cpp
// 在 HDR10Widget 创建后启用 EDR
hdr10Widget->enableEDR(true);

// 设置 HDR 元数据 (从视频文件解析)
HDRMetadata metadata;
metadata.maxContentLightLevel = 1000.0f;  // 1000 nits
metadata.transferFunction = HDRMetadata::TransferPQ;
metadata.colorSpace = HDRMetadata::ColorBT2020;
hdr10Widget->setHDRMetadata(metadata);
```

### 5.2 查询 EDR 状态

```cpp
// 检查 EDR 是否可用
if (hdr10Widget->isEDRAvailable()) {
    float headroom = hdr10Widget->getEDRHeadroom();
    // headroom = 4.0 表示支持最高 400 nits (4x SDR)
}
```

### 5.3 自动切换

HDR10Widget 会自动根据以下条件选择着色器：

1. **EDR 启用 + HDR 内容** → 使用 EDR 着色器
2. **抖动启用 + 8-bit 显示** → 使用抖动着色器
3. **其他情况** → 使用标准着色器

---

## 6. 平台兼容性

### 6.1 系统要求

| 平台 | 最低版本 | 要求 |
|------|---------|------|
| macOS | 10.11+ | EDR API 可用性检测 |
| macOS | 10.15+ | 完整的 HDR 显示支持 |
| Qt | 5.10+ | `QSurfaceFormat::scRGBColorSpace` |

### 6.2 显示器支持

- **HDR 显示器 (XDR Display)**: 完整 EDR 支持，可达 1000+ nits
- **标准 HDR 显示器**: 支持，取决于显示器能力 (400-600 nits)
- **SDR 显示器**: 支持，自动色调映射到 SDR 范围

### 6.3 回退机制

如果 EDR 不可用，系统会自动回退到标准 10-bit 渲染：

```cpp
if (!m_edrAvailable) {
    // 回退到标准 10-bit 或 8-bit 渲染
    m_bitDepth = m_supports10bit ? 10 : 8;
}
```

---

## 7. 性能考虑

### 7.1 GPU 性能

- **PQ/HLG 解码**: 每像素额外 10-20 次浮点运算
- **色彩空间转换**: 3x3 矩阵乘法 (9 次乘加)
- **总体开销**: 相比标准着色器增加约 5-10% GPU 负载

### 7.2 内存使用

- **浮点帧缓冲区**: 比整数缓冲区增加 2x 显存使用 (16-bit float vs 8-bit)
- **HDR 元数据**: 可忽略不计 (< 1KB)

### 7.3 优化建议

1. **批量更新 HDR 元数据**: 不要在每帧更新元数据
2. **延迟 EDR 检测**: 在窗口显示后再检测 EDR 支持
3. **缓存 uniforms**: 元数据不变时避免重复设置

---

## 8. 调试与诊断

### 8.1 日志输出

HDR10Widget 启动时会输出 EDR 状态：

```
HDR10Widget: EDR available, using Extended Dynamic Range rendering
HDR10Widget: OpenGL 4.1, Renderer: Apple M1, 10-bit, EDR headroom: 4x
```

### 8.2 OpenGL 信息

```cpp
QString info = hdr10Widget->getOpenGLInfo();
// "OpenGL 4.1, Renderer: Apple M1, 10-bit, EDR headroom: 4x"
```

### 8.3 故障排除

| 问题 | 可能原因 | 解决方案 |
|------|---------|---------|
| EDR 未启用 | 显示器不支持 HDR | 检查显示器 HDR 设置 |
| 颜色不正确 | HDR 元数据错误 | 验证 MaxCLL/transfer function |
| 性能问题 | GPU 负载过高 | 降低分辨率或禁用 EDR |
| 编译失败 | 缺少 Cocoa 框架 | 检查 YUViewLib.pro 配置 |

---

## 9. 未来改进

### 9.1 计划功能

1. **自动 HDR 元数据检测**: 从视频流解析 SEI 消息
2. **色调映射算法**: 提供更多选项 (Reinhard, ACES, Hable)
3. **亮度测量**: 实时计算 MaxCLL/MaxFALL
4. **多显示器支持**: 根据窗口位置自动选择 EDR headroom

### 9.2 扩展性

当前的 EDR 实现框架也可以支持：

- **Windows HDR**: 通过 DXGI HDR APIs
- **Linux HDR**: 通过 DRM/KMS 属性
- **Metal 后端**: 替代 OpenGL 的 Metal EDR 实现

---

## 10. 参考资源

### 10.1 Apple 文档

- [Extended Dynamic Range](https://developer.apple.com/documentation/metal/hdr_content/extended_dynamic_range)
- [wantsExtendedDynamicRangeOpenGLSurface](https://developer.apple.com/documentation/appkit/nsopenglview/2929880-wantsextendeddynamicrangeopenglsu)
- [NSScreen HDR Properties](https://developer.apple.com/documentation/appkit/nsscreen/2929877-maximumextendeddynamicrangecolorco)

### 10.2 HDR 标准

- [SMPTE ST 2084 (PQ)](https://ieeexplore.ieee.org/document/7291452)
- [ARIB STD-B67 (HLG)](https://www.arib.or.jp/english/html/overview/doc/2-STD-B67v1_0.pdf)
- [ITU-R BT.2100](https://www.itu.int/rec/R-REC-BT.2100)

### 10.3 OpenGL 扩展

- [EXT_color_buffer_float](https://www.khronos.org/registry/OpenGL/extensions/EXT/EXT_color_buffer_float.txt)
- [ARB_color_buffer_float](https://www.khronos.org/registry/OpenGL/extensions/ARB/ARB_color_buffer_float.txt)

---

## 11. 附录

### 11.1 术语表

| 术语 | 说明 |
|------|------|
| EDR | Extended Dynamic Range，Apple 的 HDR 渲染技术 |
| PQ | Perceptual Quantizer，感知量化器 (SMPTE ST 2084) |
| HLG | Hybrid Log-Gamma，混合对数伽马 (ARIB STD-B67) |
| MaxCLL | Maximum Content Light Level，内容最大亮度 |
| MaxFALL | Maximum Frame Average Light Level，帧平均最大亮度 |
| Nits | cd/m²，亮度单位 |
| Tone Mapping | 色调映射，将 HDR 映射到显示范围 |

### 11.2 常量定义

```cpp
// PQ 常数 (SMPTE ST 2084)
const float PQ_M1 = 2610.0 / 4096.0 / 4.0;    // 0.1593017578125
const float PQ_M2 = 2523.0 / 4096.0 * 128.0; // 78.84375
const float PQ_C1 = 3424.0 / 4096.0;         // 0.8359375
const float PQ_C2 = 2413.0 / 4096.0 * 32.0;   // 18.8515625
const float PQ_C3 = 2392.0 / 4096.0 * 32.0;   // 18.6875
const float PQ_MAX_NITS = 10000.0;

// HLG 常数 (ARIB STD-B67)
const float HLG_A = 0.17883277;
const float HLG_B = 0.28466892;
const float HLG_C = 0.55991073;
const float HLG_MAX_NITS = 1000.0;
```

---

**文档结束**
