# macOS EDR (Extended Dynamic Range) 实现总结

## 概述

本文档总结了在 YUView 中尝试实现 macOS EDR HDR 渲染的过程、遇到的问题以及可能的解决方案。

---

## 问题定义

**目标**：在 macOS 上实现 HDR 视频渲染，利用 Apple Silicon MacBook Pro 的 XDR 显示屏（支持 1600 nits 峰值亮度）。

**关键发现**：
- Qt 的 `QOpenGLWidget` 无法配置浮点颜色缓冲区
- 8-bit 缓冲区只能存储 0-255 (0.0-1.0)，无法表示 HDR 亮度值 > 1.0
- 必须使用 Metal 或原生 `NSOpenGLView` 才能实现真正的 EDR

---

## 根本原因分析

### 1. Qt QOpenGLWidget 的限制

**苹果的 EDR 要求：**
```objc
NSOpenGLPixelFormatAttribute attribs[] = {
    NSOpenGLPFAColorFloat,      // 浮点颜色缓冲区（必需）
    NSOpenGLPFAColorSize, 64,   // 64-bit (16-bit float per channel)
    NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion4_1Core,
    0
};
```

**Qt 的问题：**
- `QSurfaceFormat` 没有 `setColorFloat()` 方法
- `setRedBufferSize(16)` 在 macOS 上会导致 OpenGL 上下文创建失败
- `QOpenGLWidget` 封装了 `NSOpenGLView`，无法直接配置 pixel format

### 2. 8-bit vs 16-bit 浮点缓冲区

| 特性 | 8-bit (当前) | 16-bit float (EDR 需要) |
|------|-------------|------------------------|
| 数值范围 | 0-255 (0.0-1.0) | 0.0 - 65504.0 |
| HDR 支持 | ❌ 只能表示 SDR | ✅ 可表示 HDR 亮度 |
| 色域 | sRGB | Display P3 / BT.2020 |

**关键问题**：
- EDR 需要输出值 > 1.0 才能触发 HDR 渲染
- 8-bit 缓冲区在 shader 输出 > 1.0 时会被钳位到 1.0
- 即使设置了 `wantsExtendedDynamicRangeOpenGLSurface`，没有浮点缓冲区也无法工作

### 3. 日志分析

```
# 成功的部分
[EDR] EDR enabled via setValue:forKey:                    ✅
[EDR] Extended-range colorspace applied to window          ✅
[EDR] maximumPotentialExtendedDynamicRangeColorComponentValue: 16.000000  ✅

# 失败的部分
HDR10Widget: 10-bit not supported, falling back to 8 bit  ❌
OpenGL buffer bits - R: 8 G: 8 B: 8 A: 8                  ❌
```

---

## 尝试过的方案

### 方案 1：Qt QOpenGLWidget + EDR Helper（失败）

**实现**：
- 创建 `YUViewMacOSEDRHelper` 类管理 EDR 设置
- 设置 `wantsExtendedDynamicRangeOpenGLSurface = YES`
- 配置 `CGColorSpaceCreateExtended` 扩展色域

**结果**：
- EDR 标志设置成功
- 显示器报告 headroom = 16x（正确）
- 但 8-bit 缓冲区无法输出 > 1.0 的值

**失败原因**：缺少浮点颜色缓冲区支持

### 方案 2：设置 QSurfaceFormat 16-bit（失败）

**实现**：
```cpp
format.setRedBufferSize(16);
format.setGreenBufferSize(16);
format.setBlueBufferSize(16);
format.setAlphaBufferSize(16);
```

**结果**：
- OpenGL 上下文创建失败
- `[qt.qpa.openglcontext] Failed to create NSOpenGLContext`

**失败原因**：Qt 在 macOS 上不支持这种设置

---

## 可行方案

### 方案 A：迁移到 Metal（推荐）

**参考实现**：`QtHDRDemo` 项目

**架构**：
```
QWindow (MetalSurface)
    ↓
CAMetalLayer (wantsExtendedDynamicRangeContent = YES)
    ↓
MTKView / MTLCommandBuffer
    ↓
MTLPixelFormatRGBA16Float (16-bit float per channel)
```

**优点**：
- ✅ 完整的 EDR 支持
- ✅ 苹果官方推荐方案
- ✅ 性能更好

**缺点**：
- ❌ 需要重写整个渲染层（OpenGL → Metal）
- ❌ 工作量大
- ❌ 仅支持 macOS 10.15+

**关键代码**：
```objc
// 配置 Metal Layer
CAMetalLayer *metalLayer = (CAMetalLayer *)view.layer;
metalLayer.wantsExtendedDynamicRangeContent = YES;
metalLayer.pixelFormat = MTLPixelFormatRGBA16Float;  // 关键：16-bit float
```

### 方案 B：原生 NSOpenGLView（中等工作量）

**实现**：
- 创建原生的 `NSOpenGLView` 子类
- 配置正确的 `NSOpenGLPixelFormat`（`NSOpenGLPFAColorFloat` + 64-bit）
- 在 Qt 中通过 `QWidget::createWindowContainer()` 嵌入

**优点**：
- ✅ 保持 OpenGL API（不需要学习 Metal）
- ✅ 工作量适中

**缺点**：
- ❌ macOS 特有代码
- ❌ 混合 Qt/原生代码复杂

**关键步骤**：
```objc
// 创建原生 NSOpenGLView
NSOpenGLPixelFormatAttribute attribs[] = {
    NSOpenGLPFAColorFloat,    // 关键
    NSOpenGLPFAColorSize, 64,
    0
};
NSOpenGLPixelFormat *pf = [[NSOpenGLPixelFormat alloc] initWithAttributes:attribs];
NSOpenGLView *glView = [[NSOpenGLView alloc] initWithFrame:frame pixelFormat:pf];
glView.wantsExtendedDynamicRangeOpenGLSurface = YES;
```

### 方案 C：Qt RHI（Qt 6.4+）

**说明**：
- Qt 6 引入了 RHI（Rendering Hardware Interface）
- 可能支持 HDR 渲染
- 需要调研 Qt 6 的 RHI HDR 支持

**优点**：
- ✅ 跨平台
- ✅ 未来 Qt 标准方案

**缺点**：
- ❌ 需要升级到 Qt 6
- ❌ 不确定是否支持 macOS EDR

---

## 当前保留的代码

### 文件列表

| 文件 | 状态 | 说明 |
|------|------|------|
| `YUViewMacOSEDRHelper.h` | ✅ 保留 | EDR 辅助类头文件 |
| `YUViewMacOSEDRHelper.mm` | ✅ 保留 | EDR 实现（未使用） |
| `hdr10_fragment_edr.glsl` | ✅ 保留 | EDR 着色器（未使用） |
| `HDR10Widget.cpp` | ✅ 保留 | 已重置到原始状态 |

### 如何启用 EDR（如果未来实现）

1. 在 `HDR10Widget` 构造函数中调用 `enableEDR()`
2. 配置浮点颜色缓冲区（通过 Metal 或原生 NSOpenGLView）
3. 加载 `hdr10_fragment_edr.glsl` 着色器

---

## 技术参考

### 苹果官方 EDR 文档

- [Displaying HDR Content in Metal](https://developer.apple.com/documentation/metal/hdr_content/displaying_hdr_content_in_metal)
- [Extended Dynamic Range](https://developer.apple.com/documentation/metal/hdr_content/extended_dynamic_range)

### QtHDRDemo 关键配置

```swift
// Metal 渲染管线配置
let pipelineDescriptor = MTLRenderPipelineDescriptor()
pipelineDescriptor.colorAttachments[0].pixelFormat = .rgba16Float

// Layer 配置
metalLayer.wantsExtendedDynamicRangeContent = true
metalLayer.pixelFormat = .rgba16Float
```

### EDR 渲染流程

```
P010 数据 (10-bit YUV)
    ↓
纹理上传 (R16Unorm, RG16Unorm)
    ↓
YUV → RGB 转换 (BT.2020)
    ↓
EOTF (PQ/HLG 解码)
    ↓
漫射白归一化 (/ 203 nits)
    ↓
色域转换 (BT.2020 → Display P3)
    ↓
HDR 亮度调整
    ↓
输出到 16-bit float 渲染目标 (> 1.0 触发 HDR)
```

---

## 结论

**当前状态**：EDR 无法在 Qt QOpenGLWidget 上实现

**根本原因**：Qt 封装限制了浮点颜色缓冲区配置

**推荐方案**：
1. **短期**：保持当前 8-bit OpenGL 渲染（SDR 模式）
2. **中期**：实现原生 NSOpenGLView + 浮点缓冲区
3. **长期**：迁移到 Metal（参考 QtHDRDemo）

**相关项目**：
- QtHDRDemo：成功的 Metal EDR 实现参考
- 路径：`/Users/hailin/QtHDRDemo/`

---

## 附录：关键日志

### 成功的 EDR 初始化（但 8-bit 限制）
```
2026-05-13 03:40:21.577 [EDR] EDR enabled via setValue:forKey:
2026-05-13 03:40:21.577 [EDR] Extended-range colorspace applied to window
2026-05-13 03:40:21.577 [EDR] maximumPotentialExtendedDynamicRangeColorComponentValue: 16.000000
2026-05-13 03:40:21.577 [HDR10Widget] EDR enabled, headroom= 16
```

### 失败的缓冲区配置
```
2026-05-13 03:40:12.429 [qt.qpa.openglcontext] Failed to create NSOpenGLContext
2026-05-13 03:40:12.429 QOpenGLWidget: Failed to create context
```

---

*文档生成时间：2026-05-13*
*作者：Claude Code*
