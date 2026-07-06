# Renderer 重命名计划

## 背景

将 HDR10Widget/HDRSettingsDock 等命名重构为与 ComboBox 选项一致的命名：
QPainter / OpenGL / NativeEDR / NativeDXGI

> **执行状态：已完成**（提交 `cf195e3d`，2026-07-05）
> 本文件记录映射关系，供查阅代码与历史文档对照。下方"实际结果"列标注了与原计划的差异。

## 映射表

### 类名

| 原名 | 计划新名 | 实际结果 |
|------|----------|----------|
| `HDR10Widget` | `OpenGLRenderer` | ✅ `OpenGLRenderer` |
| `HDR10WidgetMacEDR` | `NativeEDRRenderer` | ✅ `NativeEDRRenderer` |
| `HDR10WidgetWinDXGI` | `NativeDXGIRenderer` | ✅ `NativeDXGIRenderer` |
| `HDRSettingsDock` | `RendererSettingsDock` | ✅ `RendererSettingsDock` |
| `EDRSettingsDialog` | `NativeEDRSettingsDialog` | ❌ **已删除**（该对话框不再使用，设置并入 `RendererSettingsDock`） |

### 类型别名

| 原名 | 新名 | 实际结果 |
|------|------|----------|
| `HDR10_EOTF` | `RendererEOTF` | ✅ `RendererEOTF`（`using RendererEOTF = color::EOTF;`） |
| `HDR10_ColorGamut` | `RendererColorGamut` | ✅ `RendererColorGamut`（`using RendererColorGamut = color::ColorGamut;`） |

### 枚举

| 原名 | 计划新名 | 实际结果 |
|------|----------|----------|
| `HDRRenderingMode` | `RendererMode` | ✅ `RendererMode`（定义在 `SplitViewWidget.h`） |
| `HDRRenderingMode::Disabled` | `RendererMode::QPainter` | ⚠️ `RendererMode::Software`（注释说明对应 QPainter 渲染） |
| `HDRRenderingMode::GL` | `RendererMode::OpenGL` | ✅ `RendererMode::OpenGL` |
| `HDRRenderingMode::DXGI` | `RendererMode::NativeDXGI` | ✅ `RendererMode::NativeDXGI` |
| `HDRRenderingMode::EDR` | `RendererMode::NativeEDR` | ✅ `RendererMode::NativeEDR` |

### 文件名

| 原名 | 新名 | 实际结果 |
|------|------|----------|
| `HDR10Widget.h/.cpp` | `OpenGLRenderer.h/.cpp` | ✅ 已重命名 |
| `HDR10WidgetMacEDR.h/.cpp` | `NativeEDRRenderer.h/.cpp` | ✅ 已重命名 |
| `HDR10WidgetWinDXGI.h/.cpp` | `NativeDXGIRenderer.h/.cpp` | ✅ 已重命名 |
| `HDRSettingsDock.h/.cpp` | `RendererSettingsDock.h/.cpp` | ✅ 已重命名 |
| `EDRSettingsDialog.h/.cpp` | `NativeEDRSettingsDialog.h/.cpp` | ❌ **已删除**（不再使用） |
| `edrSettingsDialog.ui` | `nativeedrsettingsdialog.ui` | ❌ **已删除** |
| `hdr10_vertex.glsl` | `opengl_vertex.glsl` | ✅ 已重命名 |
| `hdr10_fragment.glsl` | `opengl_fragment.glsl` | ✅ 已重命名 |
| `hdr10_fragment_dither.glsl` | `opengl_fragment_dither.glsl` | ✅ 已重命名 |
| `hdr10_fragment_edr.glsl` | — | ❌ **已删除**（提交 `09a06cac`，EDR 改由 Metal 路径处理） |

> 注：`shaders.qrc` 改用 `<qresource prefix="/shaders">`，代码中通过 `:/shaders/opengl_*.glsl` 引用（无 `shaders/` 子目录别名）。

### 变量名 (SplitViewWidget)

| 原名 | 新名 | 实际结果 |
|------|------|----------|
| `hdr10Widget` | `glRenderer` | ✅ `glRenderer` |
| `hdr10WidgetMacEDR` | `edrRenderer` | ✅ `edrRenderer` |
| `hdr10WidgetWin` | `dxgiRenderer` | ✅ `dxgiRenderer` |
| `hdrRenderingMode` | `rendererMode` | ✅ `rendererMode`（默认 `RendererMode::Software`） |

### 函数名

| 原名 | 新名 | 实际结果 |
|------|------|----------|
| `setHDRRenderingMode` | `setRendererMode` | ✅ `setRendererMode(RendererMode mode, bool callUpdate = true)` |
| `isHDRSupported` | `isRendererSupported` | ✅ `isRendererSupported()` |
| `setHDRInfo`（dock） | — | ⚠️ **保留** `RendererSettingsDock::setHDRInfo(...)`（未改名） |

### 信号名

| 原名 | 新名 | 实际结果 |
|------|------|----------|
| `hdrStatusChanged` | `rendererStatusChanged` | ✅ `rendererStatusChanged(...)`（`OpenGLRenderer`、`NativeDXGIRenderer`、`splitViewWidget` 均已改名） |

### UI 变量 (HDRSettingsDock → RendererSettingsDock)

| 原名 | 新名 | 实际结果 |
|------|------|----------|
| `m_labelHDRInfo` | `m_labelRendererInfo` | ✅ `m_labelRendererInfo` |

### 不改的部分

- **QSettings key**：全部保留（`View/HDRRenderer`, `View/HDRDithering`, `View/EDR_*` 等），避免破坏用户设置和迁移逻辑
- **`color::EOTF` / `color::ColorGamut` 底层枚举**：色彩管线核心，不属于 renderer 层
- **`color::` 命名空间**：保留
- **`MacEDRRenderer` 类**（内部 Metal 渲染器）：EDR 是 macOS API 术语
- **`MacEDRUtil`**：同上，保留
- **`DXGISwapChain` 类**：DXGI 是 Windows API 术语
- **`m_color*` 变量**：色彩参数，非 renderer 特有
- **`m_checkDithering` / `m_comboRenderingMode`**：已改好
- **`RendererSettingsDock::setHDRInfo`**：函数名保留，仅改名了 dock 类本身和信号名

## 实际执行结果

- **提交**：`cf195e3d` "Rename renderer classes, files, and enums to match ComboBox naming"（2026-07-05）
- **统计**：11 个文件重命名，3 个文件删除（`EDRSettingsDialog.h/.cpp`、`edrSettingsDialog.ui`），共 27 个文件变更
- **与原计划的差异**：
  1. `RendererMode::Disabled` 实际改名为 `RendererMode::Software`（而非 `QPainter`），枚举值注释说明对应 QPainter 渲染
  2. `EDRSettingsDialog` 未重命名为 `NativeEDRSettingsDialog`，而是直接删除——EDR 设置已并入 `RendererSettingsDock` dock 面板，不再使用独立对话框
  3. `hdr10_fragment_edr.glsl` 此前已被删除（提交 `09a06cac`），EDR 渲染完全由 Metal 路径处理

## 执行步骤（已完成）

1. git mv 重命名文件（10 个源文件 + 3 个 shader）
2. 全局替换类名
3. 全局替换枚举名和枚举值
4. 全局替换变量名和函数名
5. 全局替换信号名
6. 更新 shader 路径引用（`:/shaders/hdr10_*` → `:/shaders/opengl_*`）
7. 更新 ui 文件引用
8. 更新注释和日志字符串
9. 构建验证
10. 拆分提交
