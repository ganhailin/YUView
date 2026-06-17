# AFBC Decoder Support — Implementation Summary

## Overview

为 YUView 添加 AFBC (Arm Frame Buffer Compression) 原始文件解码支持。用户在 raw YUV/RGB 的属性面板中选择 FBC Format：`Raster` 或 `AFBC`。当选择 `AFBC` 时，程序会调用外部 AFBC decoder 工具，把压缩数据解码为 raster 数据后再走原有 YUV/RGB 显示流程。

AFBC 解码器路径在 Settings 的 `RK Tools` 页面配置。

## 核心数据流

```text
文件原始数据
  → loadRawData（AFBC 模式读取整文件，压缩帧大小未知）
  → loadFrame
      → fbcFormat != Raster 时调用外部 AFBC decoder
      → decoder 输出替换 currentFrameRawData
      → 原有 YUV/RGB → RGB/QImage 转换流程
```

## UI 与参数

### FBC Format

基础帧控件 `FrameHandler.ui` 中新增 `fbcFormatComboBox`：

- `Raster`：普通未压缩 raster 原始数据
- `AFBC`：Arm Frame Buffer Compression 压缩数据

早期版本曾区分 `AFBC 32x8` / `AFBC 16x16`，现在不再区分，具体 layout 由 AFBC Layout 下拉框控制。playlist 加载时会把旧的 `AFBC 32x8` / `AFBC 16x16` 自动兼容为 `AFBC`。

### AFBC Custom Options

在 FBC Format 下方新增 AFBC 自定义选项：

- `Custom Options`：主开关。只有 FBC Format 为 `AFBC` 时可用。
- `YUV-TF`：布尔选项。
- `SplitMode`：布尔选项。
- `Yoffset`：整数，范围 `0-15`。
- `Layout`：下拉框，取值 0-6：
  - `0`: 16x16 块, 4:4:4 (无子采样)
  - `1`: 16x16 块, 4:2:0
  - `2`: 16x16 块, 4:2:2
  - `3`: 32x8 宽块, 4:4:4 (平坦)
  - `4`: 32x8 宽块, 4:4:4 (16bpp优化)
  - `5`: 32x8 宽块, 4:2:0
  - `6`: 32x8 宽块, 4:2:2

当 `Custom Options` 未勾选时，YUV-TF / SplitMode / Yoffset / Layout 控件禁用，decoder mode 字符串不追加这些选项。

## 外部解码器调用格式

基础格式：

```text
afbc_decoder -h <width>x<height>_<mode> -i <input.raw> -o <output.raw>
```

YUV mode：

```text
yuv<subsampling><bits>
```

例如：

```text
afbc_decoder -h 1920x1080_yuv4208 -i /tmp/afbc_xxxx.raw -o /tmp/raster_xxxx.raw
afbc_decoder -h 3840x2160_yuv42010 -i /tmp/afbc_xxxx.raw -o /tmp/raster_xxxx.raw
```

RGB mode：

```text
r<bits>g<bits>b<bits>[a<bits>]
```

例如：

```text
afbc_decoder -h 320x240_r8g8b8 -i /tmp/afbc_xxxx.raw -o /tmp/raster_xxxx.raw
afbc_decoder -h 320x240_r8g8b8a8 -i /tmp/afbc_xxxx.raw -o /tmp/raster_xxxx.raw
```

启用 `Custom Options` 时，mode 后追加：

```text
_<yuv-tf>_<split-mode>_<yoffset>_<layout>
```

例如：

```text
afbc_decoder -h 3840x2160_yuv42010_1_0_3_2 -i /tmp/afbc_xxxx.raw -o /tmp/raster_xxxx.raw
afbc_decoder -h 320x240_r8g8b8a8_1_0_3_2 -i /tmp/afbc_xxxx.raw -o /tmp/raster_xxxx.raw
```

## 关键文件

### FrameHandler 基础控件

- `YUViewLib/src/video/FrameHandler.h`
  - `FBCFormat`: `Raster`, `AFBC`
  - `AFBCLayout`: layout 0-6
  - AFBC 自定义选项状态：`afbcCustomOptions`, `afbcYuvTf`, `afbcSplitMode`, `afbcYoffset`, `afbcLayout`
  - `getAfbcModeSuffix()` 统一生成 mode 后缀
- `YUViewLib/src/video/FrameHandler.cpp`
  - 填充 FBC / Layout 下拉框
  - 处理 FBC 和 AFBC option 变化
  - 切换控件 enabled 状态
  - playlist 保存/加载 AFBC 选项
- `YUViewLib/ui/FrameHandler.ui`
  - FBC Format 下拉框
  - Custom Options / YUV-TF / SplitMode / Yoffset / Layout 控件

### YUV AFBC 解码

- `YUViewLib/src/video/yuv/videoHandlerYUV.cpp`
  - `loadFrame()` 中读取 raw 数据后调用 AFBC decoder
  - 根据 YUV subsampling 和 bit depth 生成 `yuv4208` / `yuv42210` 等 mode
  - 追加 `getAfbcModeSuffix()`
  - decoder 成功后用 raster 输出替换 `currentFrameRawData`
  - YUV 4:2:0 fast-path 对 `clip_buf` 索引做防御性裁剪，异常输入不会导致越界 crash
- `YUViewLib/src/video/yuv/videoHandlerYUV.h`
  - `onFbcFormatChanged()` 清 raw data cache，确保切换 FBC/AFBC option 后重新加载并解码

### RGB AFBC 解码

- `YUViewLib/src/video/rgb/videoHandlerRGB.cpp`
  - `loadFrame()` 中读取 raw 数据后调用 AFBC decoder
  - 根据 RGB bit depth / alpha 生成 `r8g8b8` / `r8g8b8a8` 等 mode
  - 追加 `getAfbcModeSuffix()`
  - decoder 成功后用 raster 输出替换 `currentFrameRawData`
- `YUViewLib/src/video/rgb/videoHandlerRGB.h`
  - `onFbcFormatChanged()` 清 raw data cache

### Raw 文件读取

- `YUViewLib/src/playlistitem/playlistItemRawFile.cpp`
  - Raster 模式保持原有按帧读取逻辑
  - AFBC 模式读取整文件，因为压缩帧大小未知
  - 最大读取限制：`8192 * 8192 * 4` bytes

### Settings

- `YUViewLib/ui/settingsDialog.ui`
- `YUViewLib/src/ui/SettingsDialog.h`
- `YUViewLib/src/ui/SettingsDialog.cpp`

Settings 中新增 `RK Tools` 分组：

- `AFBC Decoder Path`
- 选择 decoder 可执行文件按钮
- 清空路径按钮

路径保存在 `QSettings` 的 `RKTools/AFBCDecoderPath`。

## Debug 日志

AFBC 解码路径会打印：

- `[AFBC] Running:` decoder 路径和参数
- `[AFBC] Exit code:` 子进程退出码
- `[AFBC] stdout:` decoder 标准输出
- `[AFBC] stderr:` decoder 标准错误
- `[AFBC] Output raster size:` 解码输出大小

## 注意事项

- 外部 decoder 路径未配置时，AFBC decode 会被跳过，后续仍按当前 raw buffer 尝试显示。
- decoder 失败时不会主动中止显示流程；这可能导致画面错误，但不应导致程序崩溃。
- YUV 4:2:0 fast-path 已对 `clip_buf` 索引做裁剪，避免异常输入数据导致越界访问。
- AFBC 模式下整文件读取，超大文件会按上限截断。
- 临时输入/输出文件使用 `QTemporaryFile`。
