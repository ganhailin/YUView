# RGB 10bit/16bit 数据通路实现计划

## 文档信息
- **创建日期**: 2026-05-08
- **目标**: 将 HDR10Widget 的16bit显示功能从"伪16bit"升级到真实10/16bit数据通路
- **相关模块**: VideoFrame, FrameHandler, videoHandlerRGB, HDR10Widget

---

## 1. 当前问题分析

### 1.1 数据流现状（问题所在）

```
源文件 (10/16bit RGB) 
    ↓
currentFrameRawData (QByteArray 存储原始高bit数据)
    ↓
convertRGBToImage() [ConversionRGB.cpp:51行]
    ├─ 右移操作: rightShift = bitDepth - 8
    └─ 数据被裁剪到8bit → 信息丢失 ⚠️
    ↓
currentImage (QImage Format_ARGB32) - 只有8bit数据
    ↓
getCurrentFrameAsVideoFrame() [FrameHandler.cpp:282]
    └─ 从8bit QImage创建VideoFrame
    ↓
generate16bitBuffer() [VideoFrame.cpp:62-66]
    ├─ r * 257, g * 257, b * 257
    └─ "伪16bit" - 只是8bit值简单扩展 ⚠️
    ↓
HDR10Widget 显示 "假" 的16bit数据
```

### 1.2 关键问题代码

**问题1: ConversionRGB.cpp 第51行**
```cpp
const int rightShift = bitDepth == 8 ? 0 : (srcPixelFormat.getBitsPerSample() - 8);
// 高bit数据被右移裁剪到8bit
```

**问题2: VideoFrame.cpp 第62-66行**
```cpp
*dst++ = r * 257;  // 8bit值简单扩展到16bit范围 - 不是真实高bit数据
*dst++ = g * 257;
*dst++ = b * 257;
```

---

## 2. 目标数据流设计

### 2.1 理想数据流

```
源文件 (10/16bit RGB)
    ↓
currentFrameRawData (保持原始高bit数据)
    ↓ 分支
    ├─→ convertRGBToImage() → currentImage (8bit, 用于QPainter显示)
    │
    └─→ convertRGBTo16BitBuffer() → currentVideoFrame (真实16bit, 用于HDR10Widget)
                                        ↓
                                    HDR10Widget 显示真实高bit数据
```

### 2.2 双路径设计

| 路径 | 用途 | 数据格式 | 目标 |
|------|------|----------|------|
| 路径A | 传统QPainter渲染 | 8bit ARGB32 | currentImage |
| 路径B | HDR OpenGL渲染 | 16bit RGBA | currentVideoFrame (16bit buffer) |

---

## 3. 实现方案

### 3.1 方案概述

采用**条件双缓冲策略**：
- **8bit源**：保持现有通路，currentImage (8bit QImage) → VideoFrame，generate16bitBuffer() 生成扩展的16bit（兼容性保持）
- **10/12/16bit源**：新增通路，从 currentFrameRawData 直接生成真实16bit buffer

### 3.2 兼容性保证

| 源bit深度 | 8bit QImage | 16bit Buffer | 备注 |
|-----------|-------------|--------------|------|
| 8bit | ✓ 生成 | ✓ 扩展生成 (r*257) | **保持现有行为** |
| 10bit | ✓ 生成 (用于QPainter) | ✓ **真实数据** (新功能) | HDR10Widget获得真实数据 |
| 12bit | ✓ 生成 (用于QPainter) | ✓ **真实数据** (新功能) | HDR10Widget获得真实数据 |
| 16bit | ✓ 生成 (用于QPainter) | ✓ **真实数据** (新功能) | HDR10Widget获得真实数据 |

### 3.2 需要修改的文件

| 序号 | 文件路径 | 修改内容 |
|------|----------|----------|
| 1 | `YUViewLib/src/video/VideoFrame.h` | 添加设置16bit buffer的方法 |
| 2 | `YUViewLib/src/video/VideoFrame.cpp` | 实现16bit buffer设置 |
| 3 | `YUViewLib/src/video/FrameHandler.h` | 添加获取高bit VideoFrame的虚函数 |
| 4 | `YUViewLib/src/video/FrameHandler.cpp` | 默认实现（从8bit QImage转换） |
| 5 | `YUViewLib/src/video/rgb/videoHandlerRGB.h` | 重载高bit VideoFrame获取方法 |
| 6 | `YUViewLib/src/video/rgb/videoHandlerRGB.cpp` | 实现从raw data直接生成16bit buffer |
| 7 | `YUViewLib/src/video/rgb/ConversionRGB.h` | 添加16bit转换函数声明 |
| 8 | `YUViewLib/src/video/rgb/ConversionRGB.cpp` | 实现16bit转换函数 |
| 9 | `YUViewLib/src/ui/views/HDR10Widget.cpp` | 修改以使用新的VideoFrame获取方式 |

---

## 4. 详细实现步骤

### 步骤1: VideoFrame 类增强

**文件**: `YUViewLib/src/video/VideoFrame.h`

添加方法:
```cpp
// 从原始高bit数据创建VideoFrame（用于RGB源）
void createFromRawRGB16Bit(const QByteArray &rawData, 
                           const rgb::PixelFormatRGB &format,
                           int width, int height);
```

**文件**: `YUViewLib/src/video/VideoFrame.cpp`

实现从原始RGB数据直接生成16bit buffer，无需经过8bit QImage。

### 步骤2: FrameHandler 基类扩展

**文件**: `YUViewLib/src/video/FrameHandler.h`

添加虚函数:
```cpp
// 获取当前帧的VideoFrame，支持高bit深度
// 派生类可以重载以提供真实高bit数据
virtual VideoFrame getCurrentFrameAsVideoFrameHighBitDepth() const;
```

**文件**: `YUViewLib/src/video/FrameHandler.cpp`

默认实现调用 `getCurrentFrameAsVideoFrame()`（保持兼容性）。

### 步骤3: videoHandlerRGB 高bit支持（保持8bit兼容）

**文件**: `YUViewLib/src/video/rgb/videoHandlerRGB.h`

添加新方法（可选，或通过参数控制）:
```cpp
// 获取带真实高bit数据的VideoFrame
// 当源bit深度>8时，返回带真实16bit buffer的帧
// 当源bit深度<=8时，行为与getCurrentFrameAsVideoFrame相同
VideoFrame getCurrentFrameAsVideoFrameWithHighBitDepth() const;
```

**文件**: `YUViewLib/src/video/rgb/videoHandlerRGB.cpp`

修改 `loadFrame()` 方法，在加载高bit深度帧时**同时**设置16bit buffer:
```cpp
void videoHandlerRGB::loadFrame(int frameIndex, bool loadToDoubleBuffer) {
    // ... 现有代码 ...
    
    // 加载raw data和生成8bit QImage（保持现有行为）
    if (loadToDoubleBuffer) {
        QImage newImage;
        convertRGBToImage(currentFrameRawData, newImage);
        doubleBufferImage = newImage;
        doubleBufferImageFrameIndex = frameIndex;
        
        // 新增：为HDR渲染生成真实16bit buffer（仅高bit源）
        if (srcPixelFormat.getBitsPerSample() > 8) {
            QVector<uint16_t> buffer16bit;
            convertRGBTo16BitBuffer(currentFrameRawData, srcPixelFormat, 
                                   frameSize, buffer16bit);
            doubleBufferVideoFrame.set16bitBuffer(std::move(buffer16bit), 
                                                  frameSize.width, frameSize.height);
        }
    } else if (currentImageIndex != frameIndex) {
        QImage newImage;
        convertRGBToImage(currentFrameRawData, newImage);
        QMutexLocker writeLock(&currentImageSetMutex);
        currentImage = newImage;
        currentImageIndex = frameIndex;
        
        // 新增：为HDR渲染生成真实16bit buffer（仅高bit源）
        if (srcPixelFormat.getBitsPerSample() > 8) {
            QVector<uint16_t> buffer16bit;
            convertRGBTo16BitBuffer(currentFrameRawData, srcPixelFormat,
                                   frameSize, buffer16bit);
            currentVideoFrame.set16bitBuffer(std::move(buffer16bit),
                                            frameSize.width, frameSize.height);
        } else {
            // 8bit源：清除16bit buffer（如果需要）或使用generate16bitBuffer
            currentVideoFrame.clear16bitBuffer(); // 可选
        }
    }
}
```

### 步骤4: ConversionRGB 添加16bit转换

**文件**: `YUViewLib/src/video/rgb/ConversionRGB.h`

添加函数:
```cpp
// 将原始RGB数据转换为16bit RGBA buffer
// 支持8-16bit输入，输出统一为16bit
void convertRGBTo16BitRGBA(const QByteArray &sourceBuffer,
                           const PixelFormatRGB &srcPixelFormat,
                           uint16_t *targetBuffer,  // 输出16bit buffer
                           const Size frameSize,
                           const bool componentInvert[4],
                           const int componentScale[4],
                           const bool limitedRange);
```

**文件**: `YUViewLib/src/video/rgb/ConversionRGB.cpp`

实现模板函数，支持:
- 8bit输入: 左移8位扩展到16bit
- 10bit输入: 左移6位扩展到16bit
- 12bit输入: 左移4位扩展到16bit
- 16bit输入: 直接复制

### 步骤5: HDR10Widget 更新（兼容8bit/高bit）

**文件**: `YUViewLib/src/ui/views/HDR10Widget.cpp`

修改 `updateTexture()` 方法中的数据处理逻辑:
```cpp
void HDR10Widget::updateTexture() {
    if (!m_currentFrame.isValid()) return;
    
    // ... 纹理创建代码 ...
    
    // 检查是否已有16bit buffer（高bit源）
    if (m_currentFrame.has16bitBuffer()) {
        // 使用真实的16bit buffer
        const uint16_t *data = m_currentFrame.getData16bit();
        // ... 上传真实16bit数据到OpenGL ...
        // 根据源bit深度设置m_bitDepth
    } else {
        // 8bit源：生成扩展的16bit buffer（保持兼容）
        const_cast<VideoFrame&>(m_currentFrame).generate16bitBuffer();
        const uint16_t *data = m_currentFrame.getData16bit();
        // ... 上传扩展16bit数据 ...
        m_bitDepth = 8; // 标记为8bit源
    }
}
```

**或者**修改 `setFrame` 调用逻辑:
```cpp
// SplitViewWidget 中的修改（保持向后兼容）
video::VideoFrame videoFrame = frameHandler->getCurrentFrameAsVideoFrame();
// VideoFrame现在可能已经包含真实的16bit buffer（如果是高bit源）
// 或者需要调用generate16bitBuffer()（如果是8bit源）
hdr10Widget->setFrame(videoFrame);
```

**关键**：HDR10Widget的 `setFrame` 和 `updateTexture` 方法应该智能处理两种情况:
- 如果 `VideoFrame.has16bitBuffer()` 返回 true → 使用真实16bit数据
- 如果返回 false → 调用 `generate16bitBuffer()` 生成扩展数据（兼容8bit）

### 步骤6: SplitViewWidget 更新（如需要）

**文件**: `YUViewLib/src/ui/views/SplitViewWidget.cpp`

HDR模式下的帧获取逻辑（保持不变，依赖VideoFrame的has16bitBuffer检查）:
```cpp
if (hdrRenderingMode == HDRRenderingMode::Enabled && hdr10Widget) {
    hdr10Widget->setGeometry(0, 0, width(), height());
    
    if (auto frameHandler = item[0]->getFrameHandler()) {
        // 保持现有调用方式（向后兼容）
        video::VideoFrame videoFrame = frameHandler->getCurrentFrameAsVideoFrame();
        
        // HDR10Widget内部会检查has16bitBuffer():
        // - 如果有16bit buffer → 使用真实数据
        // - 如果没有 → 调用generate16bitBuffer()生成扩展数据
        hdr10Widget->setFrame(videoFrame);
        hdr10Widget->setFrameHandler(frameHandler);
    }
    // ...
}
```

**无需修改SplitViewWidget**，因为兼容性处理在VideoFrame和HDR10Widget内部完成。

---

## 5. 数据转换细节

### 5.1 不同bit深度的转换策略

| 源bit深度 | 存储方式 | 转换到16bit | 说明 |
|-----------|----------|-------------|------|
| 8bit | uint8_t | value << 8 | 扩展到16bit范围 |
| 10bit | uint16_t | value << 6 | 保留高位精度 |
| 12bit | uint16_t | value << 4 | 保留高位精度 |
| 16bit | uint16_t | value | 直接复制 |

### 5.2 Alpha通道处理

- 无Alpha的格式: Alpha通道填充为65535(全不透明)
- 有Alpha的格式: 根据源数据转换

### 5.3 字节序处理

- 小端序: 直接读取
- 大端序: 需要字节交换

---

## 6. 兼容性考虑

### 6.1 向后兼容

- `getCurrentFrameAsVideoFrame()` 保持现有行为（从8bit QImage创建）
- 新增 `getCurrentFrameAsVideoFrameHighBitDepth()` 专门用于HDR渲染

### 6.2 格式支持

**第一阶段**: 支持RGB格式（10bit, 12bit, 16bit）
**第二阶段**: 扩展到YUV格式（后续工作）

---

## 7. 验证计划

### 7.1 单元测试

1. 测试8bit RGB转换到16bit buffer
2. 测试10bit RGB转换到16bit buffer
3. 测试16bit RGB直接复制
4. 验证像素值正确性

### 7.2 集成测试

1. 加载10bit RGB文件，检查HDR10Widget显示
2. 对比8bit和10bit源文件的显示效果
3. 验证像素值显示正确

---

## 8. 时间线

| 阶段 | 任务 | 预估时间 |
|------|------|----------|
| 1 | VideoFrame类增强 | 30分钟 |
| 2 | FrameHandler基类扩展 | 20分钟 |
| 3 | ConversionRGB添加16bit转换 | 1小时 |
| 4 | videoHandlerRGB实现 | 45分钟 |
| 5 | HDR10Widget更新 | 20分钟 |
| 6 | 测试验证 | 30分钟 |
| **总计** | | **约3.5小时** |

---

## 9. 风险与缓解

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| 内存使用增加 | 中 | 仅在需要时生成16bit buffer，缓存机制保持 |
| 性能下降 | 低 | 仅在HDR模式下启用，普通模式无影响 |
| 格式兼容性问题 | 中 | 充分测试不同bit深度和字节序 |
| **8bit通路被破坏** | **高** | **明确区分处理：8bit源不预生成16bit buffer，保持现有generate16bitBuffer行为** |

### 9.1 8bit兼容性保证机制

1. **加载时判断**:
   ```cpp
   if (srcPixelFormat.getBitsPerSample() > 8) {
       // 高bit源：预生成真实16bit buffer
       generateAndSetReal16BitBuffer();
   } else {
       // 8bit源：不预生成，保持VideoFrame只有8bit QImage
       // HDR10Widget会调用generate16bitBuffer()生成扩展数据
   }
   ```

2. **HDR10Widget处理**:
   ```cpp
   if (videoFrame.has16bitBuffer()) {
       // 高bit源：使用真实数据
       useReal16BitData();
   } else {
       // 8bit源：调用generate16bitBuffer()（原有行为）
       videoFrame.generate16bitBuffer();
   }
   ```

3. **测试验证**:
   - 8bit RGB文件播放正常
   - 8bit源调用generate16bitBuffer()结果与修改前一致
   - 高bit源调用has16bitBuffer()返回true

---

## 10. 相关文件引用

### 核心文件
- `/home/hailin/YUView/YUViewLib/src/video/VideoFrame.h`
- `/home/hailin/YUView/YUViewLib/src/video/VideoFrame.cpp`
- `/home/hailin/YUView/YUViewLib/src/video/FrameHandler.h`
- `/home/hailin/YUView/YUViewLib/src/video/FrameHandler.cpp`

### RGB处理
- `/home/hailin/YUView/YUViewLib/src/video/rgb/videoHandlerRGB.h`
- `/home/hailin/YUView/YUViewLib/src/video/rgb/videoHandlerRGB.cpp`
- `/home/hailin/YUView/YUViewLib/src/video/rgb/ConversionRGB.h`
- `/home/hailin/YUView/YUViewLib/src/video/rgb/ConversionRGB.cpp`

### 显示
- `/home/hailin/YUView/YUViewLib/src/ui/views/HDR10Widget.h`
- `/home/hailin/YUView/YUViewLib/src/ui/views/HDR10Widget.cpp`
- `/home/hailin/YUView/YUViewLib/src/ui/views/SplitViewWidget.cpp`

---

## 11. 实施状态

### 11.1 已完成 ✅

| 任务 | 状态 | 文件 |
|------|------|------|
| ConversionRGB 16-bit 转换函数 | ✅ | `ConversionRGB.h/cpp` |
| videoHandlerRGB 设置16-bit buffer | ✅ | `videoHandlerRGB.cpp` |
| HDR10Widget 智能检测 | ✅ | `HDR10Widget.cpp` |
| Shader 简化归一化 | ✅ | `hdr10_fragment.glsl` |
| Dithering shader 优化 | ✅ | `hdr10_fragment_dither.glsl` |
| 8-bit 兼容性保证 | ✅ | 所有相关文件 |
| 编译验证 | ✅ | 通过 |

### 11.2 后续扩展建议

#### YUV源支持
- 类似地，为videoHandlerYUV添加高bit VideoFrame支持
- 实现YUV到16bit RGB的转换

#### 性能优化
- 考虑使用SIMD指令加速16bit转换
- GPU加速转换（未来）

---

*文档版本: 1.1*
*实施完成: 2026-05-08*
*作者: YUView Team with AI Assistant*
