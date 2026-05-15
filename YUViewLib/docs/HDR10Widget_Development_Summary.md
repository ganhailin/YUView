# HDR10Widget 功能开发总结

**文档生成日期:** 2026-05-10 (更新: 2025-06)

**基于提交范围:** f4ea421a..d164e7ff (9个提交) + EDR 扩展提交

---

## 1. 概述

本阶段开发主要围绕 YUView 的 HDR10Widget 组件展开，实现了对 10-bit/16-bit HDR 视频的高质量渲染支持。核心功能包括：OpenGL 10-bit 渲染管线、像素值显示、16-bit 数据处理通道以及 YUV/RGB 双格式支持。

**EDR 扩展**: 在 macOS 上，新增 Metal EDR 渲染路径，通过 `HDR10WidgetMacEDR` + `MacEDRRenderer` 实现 Extended Dynamic Range 显示，使 HDR 视频能够在支持 EDR 的 Mac 显示器上呈现 >1.0 SDR 白的亮度值。

---

## 2. 核心功能模块

### 2.1 OpenGL 10-bit 渲染支持 (86511ae3)

**主要改动:**
- 新增 HDR10 专用 OpenGL 着色器：
  - `hdr10_fragment.glsl` - 基础 HDR10 片段着色器
  - `hdr10_fragment_dither.glsl` - 带抖动的 HDR10 着色器
  - `hdr10_vertex.glsl` - 顶点着色器
- 新增 `VideoFrame` 类，支持 10-bit 帧数据存储和处理
- `FrameHandler` 扩展支持 HDR 帧处理
- `SplitViewWidget` 集成 HDR10Widget 显示能力

**文件影响:** 14个文件，+820/-3 行代码

---

### 2.2 像素值显示功能 (657ec8dc)

**主要改动:**
- `HDR10Widget` 新增像素值绘制功能
- 支持在画面上直接显示鼠标位置的像素数值
- `SplitViewWidget` 协调像素值显示状态

**文件影响:** 3个文件，+209/-2 行代码

---

### 2.3 自动字体颜色与坐标/缩放显示 (7aade9ab)

**主要改动:**
- 字体颜色自动切换（根据背景亮度自动选择黑/白）
- 新增坐标显示功能（鼠标位置坐标）
- 新增缩放比例指示器

**文件影响:** 2个文件，+134/-15 行代码

---

### 2.4 16-bit RGB 数据通道 (1208313f)

**主要改动:**
- 实现真正的 16-bit RGB 数据处理路径
- 新增 `ConversionRGB` 类处理 16-bit RGB 转换
- `videoHandlerRGB` 扩展支持 16-bit 格式
- 更新 HDR10 着色器以支持 16-bit 输入
- 添加详细文档：
  - `HDR10Widget_Documentation.md` (712行)
  - `RGB_16bit_Implementation_Plan.md` (448行)

**文件影响:** 9个文件，+1410/-34 行代码

---

### 2.5 YUV 16-bit HDR 渲染支持 (73f546f7)

**主要改动:**
- `videoHandlerYUV` 新增 YUV 16-bit HDR 渲染能力
- 实现 YUV 到 HDR 的完整转换链路

**文件影响:** 1个文件，+323 行代码

---

### 2.6 HDR10Widget 集成与缩放控制 (7e49948d)

**主要改动:**
- `YUViewApp` 主应用集成 HDR10Widget
- 新增缩放/平移控制功能
- `SplitViewWidget` 添加 HDR10Widget 管理支持

**文件影响:** 6个文件，+82/-3 行代码

---

### 2.7 Bug 修复 (d164e7ff)

**修复内容:**
- 缩放/平移更新问题
- 像素值显示功能修复
- HDR10Widget 可见性控制修复

**文件影响:** 4个文件，+146/-79 行代码

---

### 2.8 版本检查禁用 (65e9467a)

**改动:**
- `Typedef.h` 中禁用版本检查宏

**文件影响:** 1个文件，+1/-1 行代码

---

## 2.9 macOS EDR Metal 渲染路径 (EDR 扩展)

**主要改动:**
- 新增 `HDR10WidgetMacEDR` 类：macOS EDR HDR 渲染入口
- 新增 `MacEDRRenderer` 类：Metal 渲染核心（CAMetalLayer + MTLRenderPipeline）
- 新增 `MacEDRUtil` 类：EDR 屏幕能力检测
- 新增 `hdr10_fragment_edr.glsl`：EDR 色彩处理着色器（PQ/HLG/Gamma/sRGB EOTF + BT.2020/BT.709/P3 色域）
- 新增 `edrSettingsDialog.ui`：EDR 设置对话框（EOTF、色域、Gamma、漫射白、亮度）
- `SplitViewWidget` 添加 EDR 模式切换、菜单集成、设置持久化
- 解决 macOS QOpenGLWidget FBO 8-bit 限制：Metal 路径使用 RGBA16Float 输出 >1.0 值
- 解决 QPainter 叠加层在 Metal QWindow 下不可见问题：使用原生 CALayer 作为叠加层
- 修复 EDR 设置初始化顺序问题：QSettings 加载必须在 widget 创建之前
- 修复 UI 布局重叠：QGroupBox title 替换为 bold QLabel + Line 分隔符
- 修复 CGColorSpaceCreateWithName 内存泄漏：添加 CGColorSpaceRelease()

**文件影响:** 10+个文件，新增 ~1,500 行代码

---

## 3. 技术架构变化

### 3.1 新增组件

```
YUViewLib/src/ui/views/HDR10Widget.cpp/h        # 核心 HDR10 显示组件 (OpenGL)
YUViewLib/src/ui/views/HDR10WidgetMacEDR.h/cpp   # macOS EDR 入口 (Metal)
YUViewLib/src/ui/views/MacEDRRenderer.h/mm        # Metal 渲染核心 (Objective-C++)
YUViewLib/src/ui/views/MacEDRUtil.h/mm            # EDR 屏幕检测 (Objective-C++)
YUViewLib/src/video/VideoFrame.cpp/h              # 10-bit 帧数据结构
YUViewLib/src/video/rgb/ConversionRGB.cpp/h       # 16-bit RGB 转换
```

### 3.2 着色器资源

```
YUViewLib/shaders/
├── hdr10_fragment.glsl           # 标准 HDR 渲染
├── hdr10_fragment_dither.glsl    # 抖动 HDR 渲染
├── hdr10_fragment_edr.glsl       # EDR 色彩处理渲染
├── hdr10_vertex.glsl             # 顶点着色器
└── shaders.qrc
```

### 3.3 UI 资源

```
YUViewLib/ui/
├── edrSettingsDialog.ui          # EDR 设置对话框
└── edrSettingsDialog.h/cpp       # 对话框逻辑 (自动生成/手动)
```

### 3.4 macOS 双路径架构

```
SplitViewWidget
    ├── HDR10WidgetMacEDR (Metal 路径)  ← macOS EDR 首选
    │     ├── MacEDRRenderer
    │     │     ├── CAMetalLayer (RGBA16Float, wantsExtendedDynamicRangeContent=YES)
    │     │     └── CALayer overlay (像素值/缩放叠加)
    │     └ MacEDRUtil (EDR 检测)
    │
    └── HDR10Widget (OpenGL 路径)       ← SDR fallback / 非 macOS
          ├── hdr10_fragment.glsl
          ├── hdr10_fragment_dither.glsl
          ├── hdr10_fragment_edr.glsl (非 macOS)
          └ PixelOverlay (QPainter 叠加)
```

**关键设计决策:**
- macOS 上 QOpenGLWidget 内部 FBO 为 8-bit RGBA，无法输出 >1.0 值 → 必须使用 Metal 路径
- Metal QWindow 是 macOS 窗口服务器直接合成的原生子窗口，覆盖在 Qt backing store 之上 → QPainter 内容不可见，需要原生 CALayer 叠加层

### 3.3 修改的现有组件

- `SplitViewWidget` - 集成 HDR10 显示支持
- `FrameHandler` - 扩展 HDR 帧处理能力
- `videoHandlerRGB` - 16-bit RGB 支持
- `videoHandlerYUV` - 16-bit YUV HDR 支持
- `MainWindow` - HDR10Widget 集成

---

## 4. 统计数据

| 指标 | 数值 |
|------|------|
| 新增文件 | 15+ |
| 修改文件 | 14+ |
| 新增代码行 | ~4,490 (含 EDR 扩展 ~1,500) |
| 删除代码行 | ~43 |
| 文档新增 | 4个 Markdown 文件 (含 EDR 设计文档) |

---

## 5. 提交时间线

```
f4ea421a  (基线) Allow buffer allocation for smaller frame sizes
    │
    ├── 86511ae3  Add support for 10-bit OpenGL rendering with HDR10Widget
    ├── 657ec8dc  Add pixel value drawing support in HDR10Widget
    ├── 7aade9ab  Add auto font color switching and coordinate/zoom display
    ├── 1208313f  feat: Implement true 16-bit RGB data pathway for HDR10Widget
    ├── 73f546f7  feat: Add YUV 16-bit HDR rendering support
    ├── 7e49948d  Update HDR10Widget with zoom controls and app integration
    ├── 65e9467a  Disable version check in Typedef.h
    └── d164e7ff  fix(HDR10Widget): Fix zoom/pan update, pixel value display...
    │
    └── [EDR 扩展提交]
        ├── MacEDRRenderer + HDR10WidgetMacEDR 实现
        ├── EDR 着色器 + 设置对话框
        ├── SplitViewWidget EDR 集成
        ├── EDR 设置持久化修复 (初始化顺序)
        ├── UI 布局重叠修复 (QGroupBox → QLabel)
        ├── CGColorSpace 泄漏修复
        └── 代码清理 (移除调试日志、废弃成员)
```

---

## 6. 功能演示要点

1. **HDR10 视频加载** - 支持 10-bit/16-bit HDR 内容
2. **高质量渲染** - OpenGL 着色器实现精确色彩映射
3. **像素值检查** - 实时显示鼠标位置像素值
4. **缩放与平移** - 流畅的交互式缩放/平移体验
5. **坐标显示** - 实时坐标与缩放比例指示
6. **自适应字体** - 根据背景自动切换黑/白字体颜色
7. **macOS EDR 显示** - Metal 路径实现 >1.0 SDR 白亮度输出
8. **EDR 设置对话框** - 可配置 EOTF、色域、Gamma、漫射白、亮度参数
9. **双路径渲染** - macOS Metal EDR + OpenGL SDR fallback

---

## 7. 后续建议

1. 考虑将 HDR10Widget 文档整合到主文档系统
2. 补充单元测试覆盖 16-bit 数据处理路径
3. 评估性能优化（大分辨率 HDR 内容）
4. 考虑支持更多 HDR 格式（HLG, Dolby Vision 等）
5. 考虑 EDR 路径的 GPU 加速 YUV 色域转换
6. 评估非 macOS 平台（Windows/Linux）的 HDR 显示器原生支持
