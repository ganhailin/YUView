# 色彩管线统一与 Windows ACM/HDR 动态检测方案

**创建日期:** 2026-06-23  
**状态:** 实施中

---

## 1. 背景与问题

### 1.1 三后端色彩处理不一致

YUView 有 3 个显示渲染后端，色彩处理代码重复三遍且存在不一致：

| 差异点 | OpenGL (`hdr10_fragment.glsl`) | DXGI (`HDR10WidgetWinDXGI.cpp`) | Metal (`MacEDRRenderer.mm`) |
|---|---|---|---|
| 色域转换目标 | Display P3 | BT.709 | Display P3 |
| Tonemapping | 无（sRGB OETF 硬裁剪） | Reinhard（仅 SDR 模式） | 无（系统 EDR 处理） |
| PQ 归一化 | ÷diffuseWhite → 相对 | ÷80 → scRGB 绝对 nits | ÷diffuseWhite → 相对 |
| HLG 缩放 | ×4（相对） | ×(sdrWhite×4/80)（scRGB） | ×4（相对） |
| 色域矩阵 | BT2020→P3 等 3 个 | BT2020→BT709 等 3 个 | 与 OpenGL 相同 |
| EOTF 函数 | 重复 | 重复（HLG b 常量写法略不同） | 重复 |
| 枚举类型 | `HDR10_EOTF` | 复用 `HDR10_EOTF` | 独立 `EOTF`/`ColorGamut` |

### 1.2 Windows ACM 未检测

Windows 11 ACM (Advanced Color Management) 在 SDR 模式下也会自动色彩管理和 tonemapping，但当前代码：
- 只检测 `hdrActive`（`IDXGIOutput6` 的 ColorSpace == PQ），不检测 ACM SDR 模式
- ACM SDR 模式下仍会执行 Reinhard，导致**双重 tonemapping**

### 1.3 无动态 HDR 切换监听

用户在 Windows 设置中切换 HDR/ACM 开关后，需要重启应用才能生效。缺少 `WM_DISPLAYCHANGE` 监听。

---

## 2. Tonemapping 策略（最终方案）

| 后端 | 条件 | Tonemapping | 原因 |
|---|---|---|---|
| **Metal** | 始终 | **无** | macOS 系统合成器是黑盒，EDR/SDR 都由系统处理 |
| **DXGI** | HDR 模式 | **无** | Windows 系统合成器处理 scRGB→display |
| **DXGI** | SDR + ACM 开启 | **无** | ACM 也自动色彩管理 + tonemapping |
| **DXGI** | SDR + ACM 关闭 | **Reinhard** | scRGB swap chain >1.0 硬裁剪，必须自己压缩 |
| **OpenGL** | 始终 SDR | **Reinhard** | QOpenGLWidget FBO 8-bit，sRGB OETF 会硬裁剪 |

---

## 3. 实施计划

### P0: Windows ACM 检测 + WM_DISPLAYCHANGE（最紧急）

#### 3.1 扩展 `DXGISwapChain::HDRCapabilities`

```cpp
struct HDRCapabilities {
    bool   hdrSupported{false};
    bool   hdrActive{false};
    bool   acmActive{false};                   // 新增：ACM SDR 模式
    bool   systemHandlesTonemapping{false};    // 新增：hdrActive || acmActive
    float  maxLuminance{0.0f};
    float  minLuminance{0.0f};
    float  maxFullFrameLuminance{0.0f};
    float  sdrWhiteNits{80.0f};
    QSize  displaySize;
};
```

#### 3.2 ACM 检测逻辑（`detectHDRCapabilities()`）

```cpp
// ACM SDR 检测：
// - HDR 模式：ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
//   → hdrActive = true, systemHandlesTonemapping = true
// - SDR + ACM：MaxLuminance > 80 且 ColorSpace 不是 HDR
//   → acmActive = true, systemHandlesTonemapping = true
//   （ACM 开启时 IDXGIOutput6 报告显示器真实峰值亮度，SDR 无 ACM 时为 0 或 80）
// - SDR 无 ACM：MaxLuminance <= 80 且非 HDR
//   → systemHandlesTonemapping = false, 需要 Reinhard

m_caps.acmActive = (!m_caps.hdrActive && m_caps.maxLuminance > 80.0f);
m_caps.systemHandlesTonemapping = m_caps.hdrActive || m_caps.acmActive;
```

#### 3.3 WM_DISPLAYCHANGE 监听

在 `HDR10WidgetWinDXGI` 中：
- 新增 `nativeEvent()` override
- 监听 `WM_DISPLAYCHANGE` 消息
- 延迟 100ms 重新检测 HDR/ACM 能力（避免重入 DXGI）
- 发出 `hdrStatusChanged` 信号触发重渲染

#### 3.4 着色器 tonemapping 逻辑修改

DXGI pixel shader 的 constant buffer 新增 `systemHandlesTonemapping` 字段：
```hlsl
// 旧: if (hdrActive < 0.5f) { Reinhard }
// 新: if (systemHandlesTonemapping < 0.5f) { Reinhard }
```

#### 3.5 信号链扩展

```
HDR10WidgetWinDXGI::hdrStatusChanged(hdrActive, systemHandlesTonemapping, maxNits, sdrWhiteNits)
  → SplitViewWidget::hdrStatusChanged
    → HDRSettingsDock::setHDRInfo
```

### P1: 抽取 `ColorPipeline` 模块

#### 3.6 新建 `src/common/ColorPipeline.h/.cpp`

统一管理：
- 枚举：`EOTF`, `ColorGamut`, `Tonemapping`
- 配置结构体：`ColorConfig`, `DisplayInfo`
- 色域矩阵查找表（所有 source→target 组合）
- EOTF 常量（PQ/HLG/Gamma/sRGB）
- 着色器生成函数（GLSL/HLSL/MSL）

#### 3.7 逐个后端替换

- `HDR10Widget.cpp` → 删除本地矩阵/EOTF，改用 ColorPipeline
- `HDR10WidgetWinDXGI.cpp` → 删除内联 shader，改用生成函数
- `MacEDRRenderer.mm` → 删除内联 shader，改用生成函数（不做 tonemapping）
- 统一枚举，消除 `static_cast`

### P2: UI 更新

- HDRSettingsDock 显示 ACM/HDR 状态
- 文档更新（删除对不存在的 `hdr10_fragment_edr.glsl` 的引用）

---

## 4. 文件改动清单

| 文件 | 改动 | 优先级 |
|---|---|---|
| `src/ui/views/DXGISwapChain.h` | `HDRCapabilities` 增加 `acmActive`、`systemHandlesTonemapping` | P0 |
| `src/ui/views/DXGISwapChain.cpp` | `detectHDRCapabilities()` 增加 ACM 检测 | P0 |
| `src/ui/views/HDR10WidgetWinDXGI.h` | 新增 `nativeEvent()` override | P0 |
| `src/ui/views/HDR10WidgetWinDXGI.cpp` | 新增 `nativeEvent()`；shader tonemapping 改用 `systemHandlesTonemapping`；constant buffer 扩展 | P0 |
| `src/ui/views/SplitViewWidget.h` | `hdrStatusChanged` 信号扩展参数 | P0 |
| `src/ui/views/SplitViewWidget.cpp` | 信号转发适配 | P0 |
| `src/ui/HDRSettingsDock.h` | `setHDRInfo` 扩展参数 | P0 |
| `src/ui/HDRSettingsDock.cpp` | 显示 ACM 状态 | P0 |
| `src/common/ColorPipeline.h` | **新建** | P1 |
| `src/common/ColorPipeline.cpp` | **新建** | P1 |
| `src/ui/views/HDR10Widget.cpp` | 改用 ColorPipeline | P1 |
| `src/ui/views/HDR10Widget.h` | 枚举改为 typedef | P1 |
| `shaders/hdr10_fragment.glsl` | 添加 Reinhard | P1 |
| `shaders/hdr10_fragment_dither.glsl` | 添加 Reinhard | P1 |
| `src/ui/views/MacEDRRenderer.mm` | 改用 ColorPipeline | P1 |
| `src/ui/views/HDR10WidgetMacEDR.h` | 删除独立枚举 | P1 |
