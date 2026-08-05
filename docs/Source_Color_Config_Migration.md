# 源色彩配置迁移：从渲染器到图像层

**创建日期:** 2026-07-27  
**最后更新:** 2026-08-05  
**状态:** 已实施  
**统计:** 21 个文件, +675 / -183 行

---

## 1. 背景与动机

### 1.1 旧架构问题

YUView 的 EOTF（电光转换函数）、色域（ColorGamut）、Gamma 三项"源色彩属性"原本存储在 `splitViewWidget` 的全局成员变量中：

- `m_colorEOTF` / `m_colorGamut` / `m_colorGamma`
- 通过 `RendererSettingsDock` 的全局 dock 面板设置
- 所有播放列表项共享同一组配置

**问题：** 不同图像可能使用不同的色彩编码（例如一个 BT.709 sRGB 的 SDR 视频和一个 BT.2020 PQ 的 HDR 视频），但旧架构无法为每个图像独立配置。切换选中项时渲染器使用的是全局配置，而非当前图像的实际编码。

### 1.2 设计决策

| 属性类型 | 配置项 | 归属 | 理由 |
|---------|--------|------|------|
| **源属性** | EOTF, ColorGamut, Gamma | 图像（FrameHandler） | 描述像素编码方式，跟随源文件 |
| **显示属性** | DiffuseWhiteNits, HDRBrightness, Dithering | 渲染器（RendererSettingsDock） | 描述显示映射，与显示器/后端相关 |

---

## 2. 架构变更

### 2.1 配置归属变更

| 配置项 | 旧归属 | 新归属 | 理由 |
|--------|--------|--------|------|
| EOTF | `splitViewWidget` (全局) | `FrameHandler` (每图像) | 描述像素编码方式，跟随源文件 |
| ColorGamut | `splitViewWidget` (全局) | `FrameHandler` (每图像) | 同上 |
| Gamma | `splitViewWidget` (全局) | `FrameHandler` (每图像) | 同上 |
| DiffuseWhiteNits | `RendererSettingsDock` | 不变 | 描述显示映射，与显示器/后端相关 |
| HDRBrightness | `RendererSettingsDock` | 不变 | 同上 |
| Dithering | `RendererSettingsDock` | 不变 | 同上 |

### 2.2 数据流变更

**旧架构:**
```
RendererSettingsDock -> splitViewWidget (m_colorEOTF/Gamut/Gamma)
  -> applyColorSettingsToWidgets() -> renderer->setEOTF() ...
  (全局，所有图像共享)

paintEvent: frameHandler -> getCurrentFrameAsVideoFrame() -> renderer->setFrame()
```

**新架构:**
```
FrameHandler.m_sourceColorConfig (每图像独立)
  ← UI 控件 (FrameHandler.ui EOTF/Gamut/Gamma)
  ← playlist 序列化

VideoFrame 携带 SourceColorConfig (随帧传递)

paintEvent:
  item->drawItem() -> videoHandler::drawFrame() -> 更新 currentImage
  frameHandler -> getCurrentFrameAsVideoFrame() (含 SourceColorConfig) -> renderer->setFrame()
  setFrame() 检测到 SourceColorConfig 或帧数据变化 -> update() -> paintGL()

RendererSettingsDock -> splitViewWidget (m_colorDiffuseWhite/Brightness)
  -> applyColorSettingsToWidgets() -> renderer->setDiffuseWhiteNits()/setHDRBrightness()
  (仅显示属性，全局)
```

---

## 3. 实施方案

### 3.1 新增 `SourceColorConfig` 结构体

**文件:** `YUViewLib/src/common/ColorPipeline.h` / `.cpp`

```cpp
struct SourceColorConfig
{
  EOTF       eotf{EOTF::SRGB};
  ColorGamut sourceGamut{ColorGamut::BT709};
  float      gammaValue{2.2f};

  bool operator==(const SourceColorConfig &o) const;
  bool operator!=(const SourceColorConfig &o) const;
  QString toString() const;      // "3;1;2.2" 格式
  bool fromString(const QString &);
};
```

同时新增 CPU 端色彩处理函数 `applyColorTransformToImage()`，为 Software (QPainter) 路径实现完整的 EOTF -> 色域转换 -> Reinhard tonemapping -> sRGB OETF 管线，镜像 shader 逻辑。对于 identity 情况（sRGB + BT.709）直接跳过以保持性能。

### 3.2 VideoFrame 携带 SourceColorConfig

**文件:** `YUViewLib/src/video/VideoFrame.h`

新增 `m_sourceColorConfig` 成员和 getter/setter。色彩配置随帧传递，使渲染器的 `setFrame()` 能检测到色彩参数变化（即使帧像素数据相同）。

### 3.3 FrameHandler 持有源色彩配置

**文件:** `YUViewLib/src/video/FrameHandler.h` / `.cpp`

- 新增成员: `color::SourceColorConfig m_sourceColorConfig`
- Virtual getter: `getSourceColorConfig()`（允许复合项 override）
- Setter: `setSourceColorConfig()` - 触发 `signalHandlerChanged` 重绘
- `checkSourceColorChanged()`: 用 `sender()` 匹配 EOTF/Gamut/Gamma 控件，与 `checkFbcFormatChanged()`/`checkAfbcOptionsChanged()` 同级
- `updateColorControlsEnabled()`: Gamma 输入框仅 EOTF=Gamma 时启用
- `disableSourceColorControls()`: 复合项禁用色彩控件
- `createFrameHandlerControls()`: 创建 EOTF/Gamut/Gamma 控件并连接信号
- `savePlaylist()` / `loadPlaylist()`: 序列化 `<sourceColorConfig>` 元素（向后兼容：无元素时用默认值）
- `drawFrame()`: Software 路径调用 `applyColorTransformToImage()` 应用色彩转换
- `getCurrentFrameAsVideoFrame()`: 每次从 `currentImage` 重建 VideoFrame（不缓存），确保帧切换时渲染器拿到新数据；同时填充 `SourceColorConfig`
- 构造函数从 QSettings `Frame/SourceEOTF` / `Frame/SourceGamut` / `Frame/SourceGamma` 读取默认值

### 3.4 videoHandler slotVideoControlChanged

**文件:** `YUViewLib/src/video/videoHandler.cpp`

`videoHandler` override 了 `FrameHandler::slotVideoControlChanged()`。在 override 版本中新增 `checkSourceColorChanged()` 调用，与 FBC/AFBC 同级处理。

### 3.5 UI 控件迁移到 FrameHandler 面板

**文件:** `YUViewLib/ui/FrameHandler.ui`

在现有控件（Width/Height/FrameSize/FBC）后新增 "Source Color" 区域（row 9-12）：
- EOTF 下拉框（PQ / HLG / Gamma / sRGB）
- Gamut 下拉框（BT.2020 / BT.709 / DCI-P3）
- Gamma 数值框（仅 EOTF=Gamma 时启用）

控件通过 `FrameHandler::createFrameHandlerControls()` 注入属性面板，随 `playlistItemWithVideo` 的 properties 面板显示。

### 3.6 videoHandlerRGB / videoHandlerYUV 始终创建 FrameHandler 控件

**文件:** `YUViewLib/src/video/rgb/videoHandlerRGB.cpp`, `YUViewLib/src/video/yuv/videoHandlerYUV.cpp`

`createVideoHandlerControls()` 始终调用 `FrameHandler::createFrameHandlerControls(isSizeFixed)`，不再因 `isSizeFixed=true` 跳过。`isSizeFixed` 仅控制 width/height 可编辑性，色彩控件与帧大小无关。

### 3.7 渲染管线从 VideoFrame 读取源属性

**文件:** `YUViewLib/src/ui/views/SplitViewWidget.cpp`, `OpenGLRenderer.cpp`, `NativeDXGIRenderer.cpp`, `NativeEDRRenderer.cpp`

- `SplitViewWidget::paintEvent()`: 从 `VideoFrame` 获取色彩配置（不再单独调用 `setEOTF` 等）；始终调用 `drawItem()` 更新 `currentImage`（即使 HDR 模式下），为渲染器提供正确帧数据
- 三个渲染器的 `setFrame()`: 检测 `SourceColorConfig` 变化，变化时触发 `update()`
- `OpenGLRenderer::paintGL()` / `NativeDXGIRenderer` / `NativeEDRRenderer`: EOTF/Gamut/Gamma 从帧的 `SourceColorConfig` 读取（不再从成员变量）

### 3.8 RendererSettingsDock 精简

**文件:** `YUViewLib/src/ui/RendererSettingsDock.h` / `.cpp`

- 移除: EOTF 下拉框、Gamut 下拉框、Gamma 数值框及其 getter
- 保留: Renderer 选择、Dithering、Diffuse White、HDR Brightness、渲染器状态信息
- 标题从 "Color Processing" 改为 "Display Color"

### 3.9 splitViewWidget 清理

**文件:** `YUViewLib/src/ui/views/SplitViewWidget.h` / `.cpp`

- 移除成员: `m_colorEOTF`, `m_colorGamut`, `m_colorGamma`
- `applyColorSettingsToWidgets()`: 仅推送显示属性（DiffuseWhite/Brightness/Dithering）
- `setRendererMode()`: 创建渲染器时不再设置 EOTF/Gamut/Gamma
- `loadSettings()`: 不再读取 `View/EDR_EOTF` / `View/EDR_ColorGamut` / `View/EDR_Gamma`

### 3.10 复合项继承子项配置（只读）

**文件:** `videoHandlerDifference.h/.cpp`, `videoHandlerResample.h/.cpp`

- Override `getSourceColorConfig()`: 返回第一个子项的配置
- Override `createFrameHandlerControls()`: 调用基类后 `disableSourceColorControls()` 禁用色彩控件

`playlistItemOverlay` 无需改动：它不持有 FrameHandler，渲染器路径不激活。

### 3.11 PropertiesWidget 支持滚动

**文件:** `YUViewLib/src/ui/widgets/PropertiesWidget.cpp`

属性面板包在 `QScrollArea` 中，内容过长时可上下滚动。`itemAboutToBeDeleted()` 正确释放 QScrollArea 包装。

---

## 4. QSettings 键变更

| 旧键 | 新键 | 说明 |
|------|------|------|
| `View/EDR_EOTF` | `Frame/SourceEOTF` | 从全局渲染器配置迁移到每图像默认值 |
| `View/EDR_ColorGamut` | `Frame/SourceGamut` | 同上 |
| `View/EDR_Gamma` | `Frame/SourceGamma` | 同上 |
| `View/EDR_DiffuseWhite` | 不变 | 保留在渲染器 |
| `View/EDR_Brightness` | 不变 | 保留在渲染器 |

旧键不再读取。每个图像的色彩配置通过 playlist 序列化持久化。

---

## 5. 调试过程中发现并修复的关键问题

### 5.1 `videoHandler::slotVideoControlChanged` override

`videoHandler` override 了 `FrameHandler::slotVideoControlChanged()`。直接 connect 到 `&FrameHandler::slotVideoControlChanged` 时，虚函数分派会调用 `videoHandler` 的版本，而该版本原本没有色彩控件处理逻辑。**修复:** 在 `videoHandler::slotVideoControlChanged()` 中新增 `checkSourceColorChanged()` 调用。

### 5.2 `createVideoHandlerControls` 跳过 FrameHandler 控件

`videoHandlerRGB` 和 `videoHandlerYUV` 的 `createVideoHandlerControls()` 在 `isSizeFixed=true` 时跳过 `FrameHandler::createFrameHandlerControls()`，导致压缩视频没有色彩控件。**修复:** 始终调用，`isSizeFixed` 仅控制 width/height 可编辑性。

### 5.3 `getCurrentFrameAsVideoFrame` 缓存导致帧不更新

缓存优化导致视频播放时 `currentVideoFrame` 持有旧帧的 QImage，`setFrame()` 判断"无变化"跳过纹理更新。QImage 隐式共享使 `cacheKey()` 也不变。**修复:** 去掉缓存，每次从 `currentImage` 重建 VideoFrame。QImage 隐式共享使此操作开销极小。

### 5.4 OpenGL 模式下 `drawItem` 不被调用

HDR/OpenGL 模式下 `paintEvent` 跳过 `drawItem()`，而 `drawItem` -> `videoHandler::drawFrame()` 负责从缓存加载当前帧到 `currentImage`。跳过后 `currentImage` 不更新。**修复:** 始终调用 `drawItem()`，其副作用（更新 `currentImage`）为渲染器提供正确帧数据。

---

## 6. 文件改动清单

### 核心数据模型

| 文件 | 改动 |
|------|------|
| `src/common/ColorPipeline.h` | 新增 `SourceColorConfig` 结构体、`applyColorTransformToImage()` 声明 |
| `src/common/ColorPipeline.cpp` | 新增 `SourceColorConfig` 序列化、CPU 端色彩处理函数 |
| `src/video/VideoFrame.h` | 新增 `m_sourceColorConfig` 成员、getter/setter |
| `src/video/FrameHandler.h` | 新增成员、virtual getter/setter、`checkSourceColorChanged()`、UI 辅助方法 |
| `src/video/FrameHandler.cpp` | 构造函数加载默认值、UI 控件/序列化/色彩转换/VideoFrame 重建 |
| `src/video/videoHandler.cpp` | `slotVideoControlChanged()` 新增 `checkSourceColorChanged()` |

### 复合项

| 文件 | 改动 |
|------|------|
| `src/video/videoHandlerDifference.h/.cpp` | override `getSourceColorConfig`/`createFrameHandlerControls` |
| `src/video/videoHandlerResample.h/.cpp` | 同上 |

### UI

| 文件 | 改动 |
|------|------|
| `ui/FrameHandler.ui` | 新增 EOTF/Gamut/Gamma 控件（row 9-12） |
| `src/video/rgb/videoHandlerRGB.cpp` | `createVideoHandlerControls()` 始终调用 `createFrameHandlerControls()` |
| `src/video/yuv/videoHandlerYUV.cpp` | 同上 |
| `src/ui/RendererSettingsDock.h/.cpp` | 移除 EOTF/Gamut/Gamma 控件和 getter |
| `src/ui/widgets/PropertiesWidget.cpp` | 属性面板包在 QScrollArea 中 |

### 渲染管线

| 文件 | 改动 |
|------|------|
| `src/ui/views/SplitViewWidget.h/.cpp` | 移除 `m_colorEOTF/Gamut/Gamma`；paintEvent 始终调用 `drawItem()`；精简 `applyColorSettingsToWidgets`/`setRendererMode`/`loadSettings` |
| `src/ui/views/OpenGLRenderer.cpp` | `setFrame()` 检测 config 变化；`paintGL()` 从帧读取 EOTF/Gamut/Gamma |
| `src/ui/views/NativeDXGIRenderer.cpp` | 同上 |
| `src/ui/views/NativeEDRRenderer.cpp` | 同上 |

---

## 7. 测试验证

- ✅ Raw RGB 文件：EOTF/Gamut/Gamma 控件可编辑，切换有视觉效果
- ✅ Raw YUV 文件：同上
- ✅ 压缩视频（MP4/HEVC）：控件可编辑，视频播放帧更新正常
- ✅ OpenGL 渲染模式：视频播放正常，EOTF 切换有效
- ✅ Software (QPainter) 模式：EOTF 切换有效（CPU 色彩转换）
- ✅ Gamma 输入框：EOTF=Gamma 时启用，其他时禁用
- ✅ 复合项（Difference/Resample）：色彩控件只读，继承子项配置
- ✅ 属性面板：内容过长时可上下滚动
- ✅ 187 个单元测试全部通过
- ✅ 编译无 error 无 warning（修改文件内）

---

## 8. 未来 TODO

### 8.1 FFmpeg 元数据自动检测

`AVCodecParametersWrapper` 已解析 `color_primaries` 和 `color_trc`，但未接入 `FrameHandler`。未来可在 `playlistItemCompressedVideo` 打开文件时自动映射：

- `AVColorPrimaries` -> `color::ColorGamut`（BT.709/BT.2020/DCI-P3）
- `AVColorTransferCharacteristic` -> `color::EOTF`（PQ/HLG/sRGB/Gamma）
- 处理 `AVCOL_PRI_UNSPECIFIED` / `AVCOL_TRC_UNSPECIFIED` 情况

### 8.2 Software 路径性能优化

Software 路径的 `applyColorTransformToImage` 对大图逐像素处理。当前对 identity 情况（sRGB + BT.709）已跳过，但非 identity 情况可考虑优化（如 SIMD 或缓存转换结果）。

### 8.3 SplitView 双图对比

当前 split 模式下渲染器不激活（两个 item 各自 `drawItem`）。若未来 split 模式使用渲染器，需处理两图不同 EOTF 的情况。
