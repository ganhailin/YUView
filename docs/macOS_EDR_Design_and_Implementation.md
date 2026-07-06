# YUView macOS EDR (Extended Dynamic Range) 设计与实现文档

**版本:** 1.0  
**日期:** 2026-05-15  
**适用范围:** YUView macOS 平台 HDR/EDR 视频渲染功能

---

## 目录

1. [概述](#1-概述)
2. [架构设计](#2-架构设计)
3. [组件详解](#3-组件详解)
4. [数据流](#4-数据流)
5. [EDR 设置系统](#5-edr-设置系统)
6. [构建配置](#6-构建配置)
7. [已知限制](#7-已知限制)
8. [文件清单](#8-文件清单)

---

## 1. 概述

### 1.1 什么是 EDR？

EDR (Extended Dynamic Range) 是 macOS 的 HDR 显示技术。在支持 HDR 的 Mac 显示器上（如 MacBook Pro Liquid Retina XDR、Pro Display XDR），像素值超过 1.0（SDR 白色）的部分会被系统自动映射到显示器的 HDR 亮度范围。这使得 HDR 视频内容可以在 SDR 和 HDR 混合的桌面环境中正确呈现，无需切换全屏 HDR 模式。

### 1.2 为什么需要 Metal 路径？

Qt 的 `QOpenGLWidget` 使用内部 FBO (Framebuffer Object)，其颜色缓冲区格式由 Qt 控制。在 macOS 上，Qt 的 `QOpenGLWidget` 内部 FBO 始终是 8-bit RGBA 格式（`GL_RGBA8`），无法输出超过 1.0 的浮点值。因此，通过 `QOpenGLWidget` 的 OpenGL 渲染路径无法触发 EDR。

Metal 的 `CAMetalLayer` 可以配置为 `RGBA16Float` 格式并设置 `wantsExtendedDynamicRangeContent = YES`，允许输出超过 1.0 的浮点值，从而正确触发 macOS EDR。

### 1.3 双路径设计

YUView 采用双路径渲染架构：

| 路径 | 技术 | 适用场景 | EDR 能力 |
|------|------|----------|----------|
| **Metal EDR** | `NativeEDRRenderer` + `MacEDRRenderer` | macOS HDR 显示 | ✅ 支持 EDR (>1.0) |
| **OpenGL HDR** | `OpenGLRenderer` + `opengl_fragment*.glsl` | 非 macOS / SDR fallback | ⚠️ macOS 不支持 EDR，其他平台依赖 GPU |

> 历史命名对照：`NativeEDRRenderer` 原名 `HDR10WidgetMacEDR`，`OpenGLRenderer` 原名 `HDR10Widget`，详见 `docs/Renderer_Rename_Plan.md`。

---

## 2. 架构设计

### 2.1 类层次关系

```
splitViewWidget (SplitViewWidget.h/cpp)
  ├── NativeEDRRenderer (NativeEDRRenderer.h/cpp)        [macOS Metal 路径]
  │     ├── MacEDRRenderer (MacEDRRenderer.h/mm)       [Metal 渲染器]
  │     │     ├── CAMetalLayer (Metal 渲染层)
  │     │     ├── MTLRenderPipelineState (Metal 着色管线)
  │     │     └── CALayer overlay (像素值/缩放叠加层)
  │     └ MacEDRUtil (MacEDRUtil.h/mm)                [EDR 检测工具]
  │
  ├── OpenGLRenderer (OpenGLRenderer.h/cpp)            [OpenGL 路径]
  │     ├── opengl_fragment.glsl                       [标准着色器]
  │     └ opengl_fragment_dither.glsl                   [抖动着色器]
  │
  └ RendererSettingsDock (RendererSettingsDock.h/cpp)  [设置 dock 面板]
```

> 注：早期版本存在独立的 `EDRSettingsDialog` 对话框，现已删除，其 EOTF/色域/Gamma/漫射白/亮度等设置已并入 `RendererSettingsDock` dock 面板。

### 2.2 组件职责

| 组件 | 职责 |
|------|------|
| `splitViewWidget` | 中央协调器：管理渲染模式切换、菜单集成、设置持久化 |
| `NativeEDRRenderer` | macOS Metal EDR 渲染容器：嵌入 QWindow、转发事件、管理 overlay |
| `MacEDRRenderer` | Metal 渲染核心：创建 Metal 设备/管线/纹理、执行 EDR 色彩处理 |
| `MacEDRUtil` | EDR 检测：查询屏幕 EDR 支持能力和最大 EDR 值 |
| `OpenGLRenderer` | OpenGL HDR 渲染：10-bit/16-bit 渲染、抖动 |
| `RendererSettingsDock` | 渲染器设置 dock 面板：渲染模式、EOTF、色域、Gamma、漫射白、HDR 亮度 |

---

## 3. 组件详解

### 3.1 MacEDRRenderer (Metal 渲染核心)

**文件:** `MacEDRRenderer.h`, `MacEDRRenderer.mm`

#### 关键配置

```objc
// CAMetalLayer EDR 配置
metalLayer.pixelFormat = MTLPixelFormatRGBA16Float;  // 16-bit 浮点
metalLayer.framebufferOnly = NO;  // 允许读取用于 overlay

// EDR 启用 (macOS 10.15+)
if (@available(macOS 10.15, *)) {
    metalLayer.wantsExtendedDynamicRangeContent = YES;
    // 色彩空间固定为 Extended Linear Display P3
    // (不随色域设置切换——色域转换在 Metal 着色器内部完成)
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearDisplayP3);
    metalLayer.colorspace = cs;
    CGColorSpaceRelease(cs);
}
```

> 注：`metalLayer.colorspace` **始终**为 `kCGColorSpaceExtendedLinearDisplayP3`。源色域（BT.2020/BT.709/P3）到 Display P3 的转换在 Metal 着色器中通过 3×3 矩阵完成，而非通过切换 layer 色彩空间。
```

#### EOTF 处理

| EOTF 类型 | Metal 着色器实现 | 说明 |
|-----------|-----------------|------|
| PQ (ST 2084) | `pqEotf(color, diffuseWhiteNits)` | 内部归一化：`linear × 10000 / diffuseWhite` |
| HLG (ARIB STD-B67) | `hlgEotf(color) × 4.0` | 标准输出后乘 4（峰值 = 4× 漫射白） |
| Gamma | `pow(color, gammaValue)` | 纯幂函数 |
| sRGB | `srgbEotf(color)` | IEC 61966-2-1 sRGB 分段函数 |

#### 色域转换

Metal 着色器使用 3×3 色域转换矩阵（由 `color::getGamutMatrix(source, target)` 提供，运行时计算）：

| 源色域 | 转换目标 | 说明 |
|--------|----------|------|
| BT.2020 | Display P3 | 宽色域压缩到显示器色域 |
| BT.709 | Display P3 | 色域扩展 |
| DCI-P3 | Display P3 | 1:1 映射（无转换） |

> 注：色域转换矩阵由 `MacEDRRenderer` 通过 uniform 传入着色器。`CAMetalLayer` 色彩空间固定为 `kCGColorSpaceExtendedLinearDisplayP3`，不随色域设置切换。

#### HDR 亮度处理

Metal 着色器中的亮度处理：

```
linear = EOTF(codeValue)   // PQ 内部已除以 diffuseWhiteNits
linear = gamutMatrix × linear  // 色域转换
linear = linear × hdrBrightness  // 全局亮度倍率
// 输出线性光值，>1.0 部分由 macOS 映射到 HDR
```

- `diffuseWhiteNits`: PQ EOTF 内部归一化参考（默认 203 nits）
- `hdrBrightness`: 全局亮度倍率（默认 1.0x）
- `maxEDRValue`: 显示器最大 EDR 能力（由 `MacEDRRenderer` 查询 `NSScreen.maximumPotentialExtendedDynamicRangeColorComponentValue`，用于信息显示，不直接参与 shader 计算）

> 注：Metal 路径**不做 Reinhard tonemapping**也不做 sRGB OETF——输出为线性光 Extended Display P3，值 >1.0 由 macOS 窗口服务器合成到显示器 HDR 能力。

#### CALayer Overlay

像素值显示、缩放指示器和标尺通过 CALayer overlay 实现：

```
QImage → QPainter 绘制 → CGImage → CALayer.contents → 显示在 Metal 层之上
```

原因：Metal QWindow 是原生子窗口，macOS 窗口服务器将其合成在 Qt 的 backing store 之上。QPainter 在 `SplitViewWidget` 上绘制的内容在 EDR 模式下不可见。CALayer 作为 Metal layer 的 sublayer 叠加在渲染结果之上。

#### 内存管理

在 ARC `.mm` 文件中，Objective-C 对象存储为 `void*` 需要显式所有权管理：

| 操作 | 桥接方式 | 说明 |
|------|----------|------|
| 存储 ObjC 对象 | `(__bridge_retained void*)` | 增加 CF 引用计数，C++ 拥有所有权 |
| 释放 ObjC 对象 | `CFRelease()` | 减少 CF 引用计数，ARC 兼容 |
| 读取 ObjC 对象 | `(__bridge id)` | 临时借用，不转移所有权 |
| 释放+转移 | `(__bridge_transfer)` | RAII 式释放，ARC 接管 |

### 3.2 NativeEDRRenderer (Metal EDR 容器)

**文件:** `NativeEDRRenderer.h`, `NativeEDRRenderer.cpp`

#### 关键设计决策

1. **不使用 `WA_NativeWindow` 创建 overlay widget**：额外的 NSView 会干扰 Metal QWindow 的 KVO 链，导致 "method signature argument cannot be nil" 崩溃。
2. **使用 `CALayer` 替代 Qt Widget overlay**：避免 NSView 层级冲突。
3. **延迟初始化 renderer**：Metal renderer 在首次 `paintEvent` 或 `showEvent` 时初始化，避免过早创建 Metal 资源。
4. **事件转发**：Metal QWindow 的 NSView 会拦截鼠标事件。通过 `eventFilter` 将鼠标/滚轮/悬浮事件转发给父 `SplitViewWidget`。

#### 初始化流程

```
构造函数: 创建 QWindow (MetalSurface), 设置容器窗口
showEvent/paintEvent: → initializeRenderer()
  → 创建 MacEDRRenderer
  → 初始化 Metal 设备、管线
  → 应用 EDR 设置 (EOTF/Gamut/Gamma/DiffuseWhite/Brightness)
  → 创建 CALayer overlay
```

#### 退出安全

析构函数中必须检查 OpenGL context 有效性：

```cpp
if (context() && context()->isValid()) {
    makeCurrent();
    glDeleteTextures(1, &m_textureId);
    // ...
} else {
    // Context 已失效，Qt 内部清理
    m_program = nullptr;
    // ...
}
```

### 3.3 OpenGLRenderer (OpenGL HDR 渲染)

**文件:** `OpenGLRenderer.h`, `OpenGLRenderer.cpp`

#### macOS 上的角色

在 macOS 上，`OpenGLRenderer` 作为 SDR fallback 路径：
- 当 EDR 模式关闭时使用 OpenGL 渲染
- 当 Metal 路径不可用时作为备用
- 不负责 EDR 输出（受 `QOpenGLWidget` FBO 8-bit 限制）

#### 非 macOS 上的角色

在其他平台上，`OpenGLRenderer` 负责高 bit 深度 OpenGL 渲染：
- 检测 GPU 是否支持浮点 FBO
- 支持 PQ/HLG/Gamma/sRGB EOTF 和色域转换
- （注：早期版本曾有独立的 `hdr10_fragment_edr.glsl` EDR 着色器，已在提交 `09a06cac` 中删除，EDR 渲染统一由 macOS Metal 路径处理）

### 3.4 MacEDRUtil (EDR 检测)

**文件:** `MacEDRUtil.h`, `MacEDRUtil.mm`

提供两个查询函数：

| 函数 | 说明 |
|------|------|
| `queryEDRSupport()` | 查询主屏幕是否支持 EDR |
| `queryEDRSupportForWindow(void*)` | 查询指定窗口所在屏幕的 EDR 能力 |

返回结构：
```cpp
struct EDRSupportInfo {
    bool supported;     // 是否支持 EDR
    float maxEDRValue;  // 最大 EDR 倍率 (如 16.0 表示 16x SDR)
};
```

---

## 4. 数据流

### 4.1 Metal EDR 渲染数据流

```
VideoFrame (16-bit YUV/RGB data)
  → playlistItem::getFrame()
  → splitViewWidget::paintEvent()
    → NativeEDRRenderer::setFrame(frame)
      → MacEDRRenderer::loadFrame(data)   // 上传到 RGBA16Uint Metal 纹理
      → MacEDRRenderer::render()           // Metal 着色器管线处理
        → 采样 16-bit 纹理，归一化到 0-1
        → 预乘 alpha (编码域，若源非预乘)
        → EOTF 转换 (PQ 内部÷diffuseWhite / HLG×4 / Gamma / sRGB → 线性)
        → 色域转换 (3×3 矩阵: BT.2020/BT.709/P3 → Display P3)
        → HDR 亮度缩放 (× hdrBrightness)
        → 输出到 CAMetalLayer (RGBA16Float, ExtendedLinearDisplayP3)
        → 值 >1.0 触发 macOS EDR 合成
  → macOS 窗口服务器合成 EDR 内容到显示器
```

> 注：Metal 路径不做 Reinhard tonemapping 也不做 sRGB OETF，输出线性光值。

### 4.2 Overlay 数据流

```
像素值/缩放/标尺信息
  → NativeEDRRenderer::updatePixelOverlay()
    → QPainter 在 QImage 上绘制文本和标尺
    → MacEDRRenderer::setOverlayImage(qimage)
      → QImage → CGImage 转换
      → CALayer.contents = CGImage
      → CALayer 显示在 Metal 层之上
```

### 4.3 设置持久化数据流

```
RendererSettingsDock (UI)
  → QSettings 读写:
    View/EDR_EOTF          (int: 0=PQ, 1=HLG, 2=Gamma, 3=sRGB)
    View/EDR_ColorGamut    (int: 0=BT2020, 1=BT709, 2=P3)
    View/EDR_Gamma         (float: 1.0-3.0, 默认 2.2)
    View/EDR_DiffuseWhite  (float: 100-10000, 默认 203.0 nits)
    View/EDR_Brightness    (float: 0.1-16.0, 默认 1.0x)
    View/HDRRenderer       (int: RendererMode 枚举值，渲染模式选择)
```

> 注：QSettings key 保留旧名（`View/HDRRenderer`、`View/EDR_*` 等）以兼容已有用户设置，未随重命名改动。早期版本曾有独立的 `EDRSettingsDialog` 对话框，现已删除，设置并入 `RendererSettingsDock`。

**关键时序**：EDR 设置必须在 `setRendererMode()` 之前加载，否则 widget 创建时会使用成员变量的默认值而非保存的值。

---

## 5. EDR 设置系统

### 5.1 设置面板

**文件:** `RendererSettingsDock.h`, `RendererSettingsDock.cpp`

UI 结构（使用 QGridLayout + 分区标题 QLabel + 分隔线，以 dock 面板形式集成在主窗口）：

```
┌─────────────────────────────────────────┐
│ ** Color Processing **                  │ (粗体分区标题)
│    EOTF:          [PQ (ST 2084)  ▼]     │
│    Gamma:         [2.2 ▲▼]              │ (仅 EOTF=Gamma 时启用)
│    Gamut:         [BT.2020     ▼]       │
│ ─────────────────────────────           │ (分隔线)
│ ** HDR Brightness **                    │ (粗体分区标题)
│    Diffuse White: [203.0 ▲▼] nits       │
│    HDR Brightness: [1.0 ▲▼] x           │
│    EDR: supported, Max: 16x             │ (信息标签)
│                                         │
│              [OK]  [Cancel]              │
└─────────────────────────────────────────┘
```

布局技术细节：
- 使用 QGridLayout 替代 QFormLayout（避免标签和输入控件重叠）
- `columnstretch="0,1"`：标签列固定宽度，输入列自动扩展
- 标签右对齐 + `minimumSize 110×24`，防止被压缩
- `horizontalSpacing=24`，标签和输入之间留出足够间距
- 分区标题用 QLabel + bold 字体替代 QGroupBox title（QGroupBox title 在 macOS 上容易和内容重叠）

### 5.2 设置应用流程

```
1. updateSettings(): 从 QSettings 加载 EDR 参数到成员变量
2. setRendererMode(): 创建 widget 并用成员变量设置 EDR 参数
3. initializeRenderer(): Metal renderer 初始化时应用 widget 的 EDR 参数
4. RendererSettingsDock: 用户修改 → 保存到 QSettings + 应用到 widget → renderer
```

---

## 6. 构建配置

### 6.1 YUViewLib.pro 添加

```qmake
macx {
    SOURCES += src/ui/views/MacEDRRenderer.mm \
               src/ui/views/MacEDRUtil.mm
    HEADERS += src/ui/views/MacEDRRenderer.h \
               src/ui/views/MacEDRUtil.h
    LIBS += -framework Metal -framework MetalKit \
            -framework QuartzCore -framework Cocoa
}
```

### 6.2 YUViewApp.pro 添加

```qmake
macx {
    LIBS += -framework Metal -framework MetalKit \
            -framework QuartzCore -framework Cocoa
}
```

### 6.3 shaders.qrc 添加

```xml
<qresource prefix="/shaders">
    <file>opengl_vertex.glsl</file>
    <file>opengl_fragment.glsl</file>
    <file>opengl_fragment_dither.glsl</file>
</qresource>
```

> 代码中通过 `:/shaders/opengl_vertex.glsl` 等路径引用（资源前缀为 `/shaders`）。历史名称 `hdr10_*` 已重命名为 `opengl_*`。

### 6.4 Objective-C++ 编译注意

- `.mm` 文件在 ARC (Automatic Reference Counting) 下编译
- ObjC 对象存储为 `void*` 时必须使用 `__bridge_retained`（增加引用计数）
- 释放时使用 `CFRelease()`（ARC 兼容方式）
- 初始化失败路径必须释放已 `__bridge_retained` 的对象，防止内存泄漏

---

## 7. 已知限制

1. **QOpenGLWidget FBO 限制**：macOS 上 `QOpenGLWidget` 内部 FBO 为 8-bit RGBA，无法输出 >1.0 值。EDR 必须通过 Metal 路径实现。
2. **CALayer overlay 性能**：每次 overlay 更新需要 QImage→QPainter→CGImage→CALayer 转换。对于频繁的像素值更新，可能产生性能开销。
3. **Metal QWindow 事件隔离**：Metal QWindow 的 NSView 拦截鼠标事件，需要 eventFilter 转发给父 widget。
4. **单屏 EDR 检测**：`MacEDRUtil` 默认查询主屏幕。多显示器场景下窗口可能在不同屏幕间移动。
5. **浮点纹理精度**：Metal 着色器使用 `half` (float16) 精度，极高亮度值可能存在精度损失。
6. **QSettings 默认值**：EOTF 默认为 sRGB (index=3)，Gamut 默认为 BT.709 (index=1)。用户首次使用需要手动设置 PQ/HLG 和 BT.2020 才能获得正确的 HDR 效果。

---

## 7.5 踩坑记录

### 7.5.1 createWindowContainer 导致 EDR 色彩双重 decode（已修复）

**日期:** 2026-06-24

**现象:** EDR 路径 sRGB 灰阶色彩异常，与非 EDR（QPainter / OpenGL）路径差距明显。Shader 输出经 dump 验证完全正确（sRGB EOTF 线性光值精确匹配理论值），但显示效果错误。

**根因:** `QWidget::createWindowContainer` 创建的 QWindow NSView 有自己的 backing store 和色彩管理系统。当直接将 `CAMetalLayer` 设为该 NSView 的 `view.layer` 时，Qt NSView 会对 EDR layer 的线性光输出施加额外的 sRGB 色彩转换（双重 decode），导致显示色彩异常。

**验证方法:** 创建独立 demo（`edr_linearity_test/`），并排对比两种 NSView 集成方式：
- **LEFT（正确）**: Pure NSView + `addSubview` — 独立 NSView 持有 CAMetalLayer，通过 `addSubview` 添加到父 NSView
- **RIGHT（错误）**: `createWindowContainer` + `view.layer = metalLayer` — 直接替换 QWindow NSView 的 layer

两者使用完全相同的 Metal shader、pipeline 和 EDR layer 配置。视觉对比确认 RIGHT 侧色彩异常，LEFT 侧正确。另通过 dump Metal 离屏渲染输出验证 shader 本身无问题。

**修复:** 在 `MacEDRRenderer::initialize()` 中，不再直接 `view.layer = metalLayer`，改为创建独立 NSView + `addSubview`：

```objc
NSView *edrView = [[NSView alloc] initWithFrame:view.bounds];
edrView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
edrView.wantsLayer = YES;
edrView.layer = metalLayer;
[view addSubview:edrView];  // addSubview 隔离 Qt NSView 色彩管理
m_edrView = (__bridge void *)edrView;
```

Overlay CALayer 也改为添加到 `edrView.layer`（而非 `view.layer`）。

**关键文件:** `MacEDRRenderer.mm`（`initialize()` 函数）、`MacEDRRenderer.h`（添加 `m_edrView` 成员）、`edr_linearity_test/`（验证 demo）

**教训:** `createWindowContainer` 创建的 Qt 管理 NSView 不适合直接替换 layer。需要通过 `addSubview` 创建独立的 NSView 子层来隔离 Qt 的色彩管理。

### 7.5.2 三条渲染路径的色域管理与一致性

**日期:** 2026-06-26

**背景:** YUView 有三条渲染路径——QPainter（默认）、OpenGL（`OpenGLRenderer`）、EDR/Metal（`NativeEDRRenderer`）。在 Display P3 显示器上，三条路径显示的彩色不一致：EDR 正确显示 sRGB 红（经 BT.709→P3 色域转换），QPainter 和 OpenGL 直接显示 P3 纯红（过饱和）。

**根因:** 三条路径对色域转换的处理不一致：
- **EDR**：shader 做 BT.709→Display P3 色域转换（正确）
- **QPainter**：QImage 无色域标记，macOS Core Graphics 按设备色域直接显示（不做转换）
- **OpenGL**：shader 做了色域转换，但 FBO 的 `QSurfaceFormat::sRGBColorSpace` 在 Qt 的 QOpenGLWidget NSView backing store 下不可靠

**修复方案:** 每条路径独立获取屏幕色域并自行转换：

1. **新增 `functionsGui::getDisplayColorSpace()`**（`FunctionsGuiColorSpace.mm`）：
   - macOS 上通过 `QNativeInterface::QCocoaScreen::nativeScreen()` 获取 `NSScreen`
   - 读取 `NSScreen.colorSpace.ICCProfileData`，用 `QColorSpace::fromIccProfile()` 解析
   - 非 macOS 返回 `QColorSpace::SRgb`

2. **QPainter 路径**（`videoHandler.cpp` + `FrameHandler.cpp`）：
   - 给 QImage 标记 `QColorSpace::SRgb`
   - `convertedToColorSpace(displayCS)` 转换到屏幕色域
   - 注意：`videoHandler::drawFrame` 是 YUV 视频的实际入口，`FrameHandler::drawFrame` 只用于图片文件

3. **OpenGL 路径**（`OpenGLRenderer.cpp`）：
   - `paintGL` 中调用 `getDisplayColorSpace()` 判断屏幕色域
   - 如果不是 sRGB（如 Display P3），色域目标设为 `ColorGamut::P3`
   - shader 手动做 sRGB OETF（不依赖 `GL_FRAMEBUFFER_SRGB`）
   - **不能使用 `GL_FRAMEBUFFER_SRGB`**：Qt 的 QOpenGLWidget NSView backing store 会施加额外的 sRGB decode，与 7.5.1 的 createWindowContainer 双重 decode 问题同源

4. **EDR/Metal 路径**：已正确使用 P3 色域目标，无需修改

### 7.5.3 QSurfaceFormat::sRGBColorSpace 与 GL_FRAMEBUFFER_SRGB 不可用

**日期:** 2026-06-26

**问题:** 尝试使用 `QSurfaceFormat::sRGBColorSpace` 和 `glEnable(GL_FRAMEBUFFER_SRGB)` 让 GPU 自动做线性光→sRGB 编码，配合 macOS 合成器自动做 sRGB→Display P3 色域转换。

**根因:** Qt 的 `QOpenGLWidget` 在 macOS 上通过内部 NSView 管理 framebuffer。这个 NSView 的 backing store 会对 FBO 输出施加**额外的色彩管理**：
- `sRGBColorSpace`：backing store 可能做 sRGB→线性光→sRGB 的双重转换
- `GL_FRAMEBUFFER_SRGB`：GPU 做 OETF 后，backing store 再做一次 EOTF+OETF

这与 7.5.1 的 createWindowContainer 双重 decode 问题**同源**——Qt 管理的 NSView 会干预 OpenGL 输出的色彩管理。

**修复:** 不使用 `QSurfaceFormat::sRGBColorSpace` 和 `GL_FRAMEBUFFER_SRGB`。shader 手动做完整的 EOTF + 色域转换 + sRGB OETF。色域目标通过 `getDisplayColorSpace()` 动态获取。

### 7.5.4 Alpha 混合与预乘模式

**日期:** 2026-06-26

**背景:** 原始渲染路径不支持图像透明区域——shader 硬编码 `alpha = 1.0`，且 Metal/OpenGL pipeline 未启用 blending。

**修复:**

1. **Metal/EDR pipeline**：启用预乘 alpha blending（`MTLBlendFactorOne / OneMinusSourceAlpha`）。shader 在 EOTF 前（编码域）做预乘：`if (!premultiplied) color *= alpha`。macOS 合成器要求 CAMetalLayer 输出预乘数据。

2. **OpenGL pipeline**：启用 `GL_BLEND` + `glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)`（预乘模式）。shader 同样在编码域做预乘。

3. **QPainter 路径**：通过重标记 QImage 格式为 `Format_ARGB32_Premultiplied` 实现（共享像素数据，不转换）。注意必须用 `QImage(bits, w, h, bpl, format)` 构造函数重标记，**不能用 `convertedTo()`**——后者会再次预乘已预乘的数据（双重预乘）。

4. **预乘位置**：预乘必须在 EOTF 之前（编码域）进行，不能在线性光域做。因为预乘是编码域操作，在 gamma/sRGB 曲线上做预乘才是数学正确的。

5. **设置开关**：Settings > General 中添加 "Source texture is premultiplied alpha (EDR)" checkbox。勾选时源数据已是预乘（shader 不做转换），取消时 shader 在编码域做预乘。默认勾选。

**关键文件:** `MacEDRRenderer.mm`、`opengl_fragment.glsl`、`opengl_fragment_dither.glsl`、`OpenGLRenderer.cpp`、`videoHandler.cpp`、`FrameHandler.cpp`、`settingsDialog.ui`、`SettingsDialog.cpp`

### 7.5.5 背景色需要 sRGB EOTF 转换

**日期:** 2026-06-26

**问题:** EDR 路径的 Metal clear color `(0.14, 0.14, 0.14, 1.0)` 被直接输出到 `ExtendedLinearDisplayP3` 层，0.14 被当作线性光解释（≈ sRGB 0.42 的亮度），导致背景过亮。

**修复:** 从 QSettings `View/BackgroundColor` 读取背景色，对 sRGB 值做 sRGB EOTF 转换为线性光后再用作 clear color。OpenGL 路径的 `glClearColor` 保持 sRGB 编码值（因为 shader 手动做 OETF，clear color 在 OETF 之前）。

---

## 8. 文件清单

### 新增文件

| 文件 | 类型 | 说明 |
|------|------|------|
| `YUViewLib/src/ui/views/MacEDRRenderer.h` | C++ 头文件 | Metal 渲染器接口 |
| `YUViewLib/src/ui/views/MacEDRRenderer.mm` | Objective-C++ | Metal 渲染器实现 |
| `YUViewLib/src/ui/views/MacEDRUtil.h` | C++ 头文件 | EDR 检测接口 |
| `YUViewLib/src/ui/views/MacEDRUtil.mm` | Objective-C++ | EDR 检测实现 |
| `YUViewLib/src/ui/views/NativeEDRRenderer.h` | C++ 头文件 | Metal EDR widget 接口（原名 `HDR10WidgetMacEDR.h`） |
| `YUViewLib/src/ui/views/NativeEDRRenderer.cpp` | C++ 源文件 | Metal EDR widget 实现（原名 `HDR10WidgetMacEDR.cpp`） |
| `YUViewLib/src/common/FunctionsGuiColorSpace.mm` | Objective-C++ | 屏幕色域获取（macOS NSScreen ICC profile） |
| `YUViewLib/src/ui/RendererSettingsDock.h` | C++ 头文件 | 渲染器设置 dock 面板接口（原名 `HDRSettingsDock.h`） |
| `YUViewLib/src/ui/RendererSettingsDock.cpp` | C++ 源文件 | 渲染器设置 dock 面板实现（原名 `HDRSettingsDock.cpp`） |

### 修改文件

| 文件 | 改动量 | 说明 |
|------|--------|------|
| `YUViewLib/src/ui/views/SplitViewWidget.cpp` | +300 行 | EDR 模式管理、菜单、设置持久化 |
| `YUViewLib/src/ui/views/SplitViewWidget.h` | +28 行 | EDR 成员变量、菜单 actions |
| `YUViewLib/src/ui/views/OpenGLRenderer.cpp` | +180 行 | EDR 着色器、色域矩阵、macOS 适配（原名 `HDR10Widget.cpp`） |
| `YUViewLib/src/ui/views/OpenGLRenderer.h` | +50 行 | EDR 枚举、成员变量（原名 `HDR10Widget.h`） |
| `YUViewLib/YUViewLib.pro` | +7 行 | Metal 源文件、框架链接 |
| `YUViewApp/YUViewApp.pro` | +4 行 | Metal 框架链接 |
| `YUViewLib/shaders/shaders.qrc` | +1 行 | EDR 着色器资源 |

> 历史命名对照详见 `docs/Renderer_Rename_Plan.md`。