# RGB/YUV 16-bit HDR 数据通路实现计划

## 文档信息
- **创建日期**: 2026-05-08
- **更新日期**: 2026-07-06（更新 renderer 重命名后的类名/文件名）
- **目标**: 将 OpenGLRenderer 的16bit显示功能从"伪16bit"升级到真实10/16bit数据通路
- **状态**: ✅ 已完成 (RGB + YUV)

> 历史命名：文档创建时该类名为 `HDR10Widget`，现已重命名为 `OpenGLRenderer`；shader `hdr10_fragment.glsl`→`opengl_fragment.glsl`、`hdr10_fragment_dither.glsl`→`opengl_fragment_dither.glsl`。详见 `docs/Renderer_Rename_Plan.md`。

---

## 1. 项目概述

### 1.1 背景
OpenGLRenderer 已支持 16-bit OpenGL 渲染，但数据源存在问题：
- **RGB源**: 高 bit 深度数据被转换为 8-bit QImage，再扩展为"伪16bit"
- **YUV源**: 同样问题，YUV→8bit RGB→扩展16bit

### 1.2 目标
实现从源到 OpenGLRenderer 的**真实高 bit 深度数据通路**：
- 8-bit 源: 保持兼容 (r*257 扩展)
- 10/12/16-bit 源: 真实精度直通 OpenGLRenderer

---

## 2. 数据流设计

### 2.1 RGB 数据流 (✅ 已完成)

```
源文件 (10/16-bit RGB)
    ↓
currentFrameRawData (原始字节)
    ↓
├─→ convertRGBToImage() → QImage (8-bit) → QPainter 显示
│
└─→ convertRGBTo16BitRGBA() → 16-bit RGBA buffer (新功能)
    ↓
VideoFrame.set16bitBuffer() → currentVideoFrame
    ↓
OpenGLRenderer (真实16-bit 渲染)
```

### 2.2 YUV 数据流 (✅ 已完成)

```
YUV Source (10-bit 4:2:0等)
    ↓
currentFrameRawYUVData
    ↓
├─→ convertYUVToImage() → QImage (8-bit RGB) → QPainter
│
└─→ convertYUVTo16BitRGBA() → 16-bit RGBA buffer
    ├─ Planar: 直接 YUV→RGB 转换
    └─ Packed (V210/NV15/NV20/NV30): 先 unpack 再转换
    ↓
VideoFrame.set16bitBuffer() → currentVideoFrame
    ↓
OpenGLRenderer (真实16-bit 渲染)
```

---

## 3. 实现详情

### 3.1 RGB 实现 (✅)

**文件**: `YUViewLib/src/video/rgb/`

#### ConversionRGB.cpp
```cpp
void convertRGBTo16BitRGBA(
    const QByteArray &sourceBuffer,
    const PixelFormatRGB &srcPixelFormat,
    uint16_t *targetBuffer,
    const Size frameSize,
    const bool componentInvert[4],
    const int componentScale[4],
    const bool limitedRange
);
```

**支持格式**:
- 8/10/12/16-bit planar/packed RGB
- 大端/小端字节序

#### videoHandlerRGB.cpp
在 `loadFrame()` 中添加：
```cpp
if (srcPixelFormat.getBitsPerSample() > 8) {
    QVector<uint16_t> buffer16bit(w * h * 4);
    convertRGBTo16BitRGBA(..., buffer16bit.data(), ...);
    currentVideoFrame.set16bitBuffer(std::move(buffer16bit), w, h);
}
```

### 3.2 YUV 实现 (✅)

**文件**: `YUViewLib/src/video/yuv/videoHandlerYUV.cpp`

#### 新增函数
```cpp
template <int bitDepth>
void convertYUVTo16BitRGBAInternal(
    const QByteArray &sourceBuffer,
    uint16_t *targetBuffer,
    const Size curFrameSize,
    const PixelFormatYUV &yuvFormat,
    const ConversionSettings &conversionSettings
);

void convertYUVTo16BitRGBA(
    const QByteArray &sourceBuffer,
    uint16_t *targetBuffer,
    const Size frameSize,
    const PixelFormatYUV &yuvFormat,
    const ConversionSettings &conversionSettings
);
```

**支持格式**:
- Planar: YUV 4:2:0, 4:2:2, 4:4:4, 4:0:0 (monochrome)
- Packed: V210, NV15, NV20, NV30
- Bit深度: 8/10/12/16-bit
- 颜色空间: BT.601/BT.709/BT.2020 (Limited/Full Range)

**关键技术点**:
1. **指针算术**: 先计算字节偏移，再转换为类型指针
2. **Math处理**: 在原始值上应用 scale/offset/invert，再扩展到16bit
3. **颜色转换**: 使用标准 YUV→RGB 系数，结果存储为16bit RGBA
4. **字节序**: 支持大端/小端格式

### 3.3 OpenGLRenderer 适配 (✅)

**文件**: `YUViewLib/src/ui/views/OpenGLRenderer.cpp`

```cpp
void updateTexture() {
    if (!m_currentFrame.has16bitBuffer()) {
        // 8-bit源: 扩展为16bit (兼容)
        m_currentFrame.generate16bitBuffer();
        m_sourceBitDepth = 8;
    } else {
        // 高bit源: 使用真实数据
        m_sourceBitDepth = 10/12/16;
    }
    // 上传 GL_RGBA16UI 纹理
}
```

### 3.4 Shader 更新 (✅)

**标准 Shader** (`opengl_fragment.glsl`):
```glsl
void main() {
    uvec4 raw = texture(texture16bit, vTexCoord);
    vec3 color = vec3(raw.rgb) / 65535.0;  // 统一归一化
    fragColor = vec4(color, 1.0);
}
```

**抖动 Shader** (`opengl_fragment_dither.glsl`):
```glsl
// Bayer 4x4 抖动到 8-bit，预计算浮点值优化
float bayerDither4x4(vec2 position) {
    // 预计算: (value/16 - 0.5) / 256
    float matrix[16] = { -0.001953125, ... };
    return matrix[x + y * 4];
}
```

---

## 4. 兼容性

### 4.1 向后兼容
| 源类型 | 行为 |
|--------|------|
| 8-bit RGB/YUV | `has16bitBuffer()=false`，调用 `generate16bitBuffer()` (r*257) |
| 10/12/16-bit RGB/YUV | `has16bitBuffer()=true`，使用真实16bit数据 |

### 4.2 格式支持矩阵

| 格式 | 8-bit | 10-bit | 12-bit | 16-bit |
|------|-------|--------|--------|--------|
| RGB Planar | ✅ | ✅ | ✅ | ✅ |
| RGB Packed | ✅ | ✅ | ✅ | ✅ |
| YUV 4:2:0 Planar | ✅ | ✅ | ✅ | ✅ |
| YUV 4:2:2 Planar | ✅ | ✅ | ✅ | ✅ |
| YUV 4:4:4 Planar | ✅ | ✅ | ✅ | ✅ |
| V210 | - | ✅ | - | - |
| NV15 | - | ✅ | - | - |
| NV20 | - | ✅ | - | - |
| NV30 | - | ✅ | - | - |

---

## 5. 相关文件

### 核心文件
- `/home/hailin/YUView/YUViewLib/src/video/VideoFrame.h`
- `/home/hailin/YUView/YUViewLib/src/video/VideoFrame.cpp`
- `/home/hailin/YUView/YUViewLib/src/video/FrameHandler.h`

### RGB处理
- `/home/hailin/YUView/YUViewLib/src/video/rgb/ConversionRGB.h`
- `/home/hailin/YUView/YUViewLib/src/video/rgb/ConversionRGB.cpp`
- `/home/hailin/YUView/YUViewLib/src/video/rgb/videoHandlerRGB.cpp`

### YUV处理
- `/home/hailin/YUView/YUViewLib/src/video/yuv/videoHandlerYUV.cpp`

### 显示
- `/home/hailin/YUView/YUViewLib/src/ui/views/OpenGLRenderer.h`
- `/home/hailin/YUView/YUViewLib/src/ui/views/OpenGLRenderer.cpp`
- `/home/hailin/YUView/YUViewLib/shaders/opengl_fragment.glsl`
- `/home/hailin/YUView/YUViewLib/shaders/opengl_fragment_dither.glsl`

---

## 6. 实施状态

### 6.1 已完成 ✅

| 任务 | 状态 | 文件 |
|------|------|------|
| ConversionRGB 16-bit 转换函数 | ✅ | `ConversionRGB.h/cpp` |
| videoHandlerRGB 设置16-bit buffer | ✅ | `videoHandlerRGB.cpp` |
| videoHandlerYUV 16-bit 转换函数 | ✅ | `videoHandlerYUV.cpp` |
| videoHandlerYUV 设置16-bit buffer | ✅ | `videoHandlerYUV.cpp` |
| OpenGLRenderer 智能检测 | ✅ | `OpenGLRenderer.cpp` |
| Shader 简化归一化 | ✅ | `opengl_fragment.glsl` |
| Dithering shader 优化 | ✅ | `opengl_fragment_dither.glsl` |
| 8-bit 兼容性保证 | ✅ | 所有相关文件 |
| 编译验证 | ✅ | 通过 |

### 6.2 关键修复

1. **指针算术错误**: 
   - 问题: `rawData + nrBytesLuma` 当 `rawData` 是 `uint16_t*` 时偏移错误
   - 修复: 先用 `uint8_t*` 计算字节偏移，再转换类型

2. **Math 应用顺序**:
   - 问题: 在扩展后的16bit值上应用 scale/offset
   - 修复: 原始值 → Math → 扩展

3. **RGBConv 系数**:
   - 问题: 错误索引和除以256
   - 修复: 正确索引，除以65536

4. **Limited Range 偏移**:
   - 问题: 使用 `shiftTo16` 变量
   - 修复: 固定 `<< 8` (因为值已是16bit)

### 6.3 后续建议

- 性能优化: SIMD 加速 (SSE/AVX)
- GPU 加速: OpenGL Compute Shader
- 更多 YUV 格式: 如 YUV 4:1:1, 4:2:0 10-bit planar 等

---

*文档版本: 1.2*
*完成日期: 2026-05-08*
*作者: YUView Team*
