# Adding Predefined RGB Formats to YUView

This document explains how to add support for new predefined RGB formats (like AB30) to YUView using the handler pattern.

## Overview

Predefined RGB formats are non-standard packed formats that require special handling (e.g., AB30 with 2-bit alpha and 10-bit RGB). These formats are handled through a handler pattern that encapsulates all format-specific operations.

## Files Involved

1. `YUViewLib/src/video/rgb/PixelFormatRGB.h` - Header with handler interface and format enum
2. `YUViewLib/src/video/rgb/PixelFormatRGB.cpp` - Handler implementations and factory
3. `YUViewLib/src/video/rgb/ConversionRGB.cpp` - Conversion functions (uses handlers)
4. `YUViewLib/src/video/rgb/videoHandlerRGB.cpp` - Format preset list
5. `YUViewLib/src/video/rgb/PixelFormatRGBGuess.cpp` - File format detection
6. `YUViewLib/src/playlistitem/playlistItemRawFile.cpp` - File extension registration

## Step-by-Step Guide

### Step 1: Add Format to the Enum

In `PixelFormatRGB.h`, add your format to the `PredefinedRGBFormat` enum:

```cpp
enum class PredefinedRGBFormat
{
  AB30,
  YourNewFormat  // Add here
};
```

Update the `PredefinedRGBFormatMapper` to include the new format:

```cpp
constexpr EnumMapper<PredefinedRGBFormat, 2> PredefinedRGBFormatMapper = {
    std::make_pair(PredefinedRGBFormat::AB30, "AB30"),
    std::make_pair(PredefinedRGBFormat::YourNewFormat, "YourNewFormat")};
```

> **Note**: Update the template parameter `2` to match the number of formats.

### Step 2: Create the Handler Class

In `PixelFormatRGB.cpp`, create a handler class inheriting from `PredefinedRGBFormatHandler`:

```cpp
class YourNewFormatHandler : public PredefinedRGBFormatHandler
{
public:
  [[nodiscard]] std::string getName() const override { return "YourNewFormat"; }
  [[nodiscard]] unsigned    getBitsPerSample() const override { return 10; }
  [[nodiscard]] bool        hasAlpha() const override { return true; }
  [[nodiscard]] unsigned    getNrChannels() const override { return 4; }

  [[nodiscard]] std::size_t bytesPerFrame(Size frameSize) const override
  {
    return std::size_t(frameSize.width) * std::size_t(frameSize.height) * 4;
  }

  [[nodiscard]] int getChannelPosition(Channel channel) const override
  {
    // Define channel order (e.g., ARGB, RGBA, etc.)
    switch (channel)
    {
      case Channel::Alpha: return 0;
      case Channel::Red:   return 1;
      case Channel::Green: return 2;
      case Channel::Blue:  return 3;
    }
    return -1;
  }

  [[nodiscard]] Channel getChannelAtPosition(int position) const override
  {
    switch (position)
    {
      case 0: return Channel::Alpha;
      case 1: return Channel::Red;
      case 2: return Channel::Green;
      case 3: return Channel::Blue;
    }
    throw std::invalid_argument("Invalid position");
  }

  [[nodiscard]] rgba_t getPixelValue(const QByteArray &sourceBuffer,
                                      const Size        frameSize,
                                      const QPoint     &pixelPos) const override
  {
    // Extract pixel value at given position
    // Return as rgba_t with raw values (0-1023 for 10-bit)
  }

  void convertToARGB(const QByteArray &sourceBuffer,
                     unsigned char    *targetBuffer,
                     const Size        frameSize,
                     const bool        componentInvert[4],
                     const int         componentScale[4],
                     const bool        limitedRange,
                     const bool        premultiplyAlpha) const override
  {
    // Convert to 8-bit BGRA (QImage format)
    // Output order: B, G, R, A
  }

  void convertTo16BitRGBA(const QByteArray &sourceBuffer,
                          uint16_t         *targetBuffer,
                          const Size        frameSize,
                          const bool        componentInvert[4],
                          const int         componentScale[4],
                          const bool        limitedRange) const override
  {
    // Convert to 16-bit RGBA
    // Output order: R, G, B, A
  }
};
```

### Step 3: Register in Factory

In the `createPredefinedRGBFormatHandler` function, add a case for your format:

```cpp
std::unique_ptr<PredefinedRGBFormatHandler> createPredefinedRGBFormatHandler(
    PredefinedRGBFormat format)
{
  switch (format)
  {
    case PredefinedRGBFormat::AB30:
      return std::make_unique<AB30Handler>();
    case PredefinedRGBFormat::YourNewFormat:
      return std::make_unique<YourNewFormatHandler>();
  }
  return nullptr;
}
```

### Step 4: Add to Format Preset List

In `videoHandlerRGB.cpp`, add your format to the `formatPresetList`:

```cpp
const QStringList formatPresetList{
    // ... existing formats ...
    "AB30",
    "YourNewFormat"};
```

### Step 5: Add File Detection (Optional)

In `PixelFormatRGBGuess.cpp`, add detection logic in `checkSpecificFileExtensions`:

```cpp
std::optional<PixelFormatRGB> checkSpecificFileExtensions(
    const std::string &filename, const Size &frameSize, const std::optional<std::int64_t> &fileSize)
{
  // ... existing checks ...

  if (filename.find("yourformat") != std::string::npos ||
      filename.find("YOURFORMAT") != std::string::npos)
  {
    const auto format = PixelFormatRGB(PredefinedRGBFormat::YourNewFormat);
    if (doesPixelFormatMatchFileSize(format, frameSize, fileSize))
      return format;
  }

  return {};
}
```

### Step 6: Register File Extension (Optional)

In `playlistItemRawFile.cpp`, add the extension to `RGB_EXTENSIONS`:

```cpp
const auto RGB_EXTENSIONS = QStringList{
    // ... existing extensions ...
    "ab30",
    "yourformat"};
```

## Key Implementation Details

### Channel Order

- `getChannelPosition()`: Returns position (0-3) for each channel
- `getChannelAtPosition()`: Returns channel at given position
- Must be consistent with each other

### Pixel Value Extraction

- Return raw values in `rgba_t` (e.g., 10-bit values in range 0-1023)
- Used for pixel value display in the UI

### 8-bit Conversion (convertToARGB)

- Output format is **BGRA** (QImage::Format_ARGB32)
- Apply component scale, inversion, limited range, and alpha premultiply
- Use `LimitedRangeToFullRange` array for limited range conversion

### 16-bit Conversion (convertTo16BitRGBA)

- Output format is **RGBA** (different from 8-bit!)
- Scale values to full 16-bit range (0-65535)
- Apply component scale and inversion

### Alpha Handling

For non-standard alpha bit depths (like AB30's 2-bit alpha), use replication:

```cpp
// Expand 2-bit alpha to 8-bit
static inline uint8_t expandAlpha2To8(uint8_t a2)
{
  if (a2 == 0) return 0;
  if (a2 == 3) return 255;
  return (a2 << 6) | (a2 << 4) | (a2 << 2) | a2;  // 0b01 -> 0b01010101
}
```

## Testing

1. Create a small test file with known pixel values
2. Open in YUView and verify pixel values display correctly
3. Check that file size calculation matches actual file size
4. Verify conversion to 8-bit and 16-bit looks correct

## Example: AB30 Format

AB30 (DRM_FORMAT_ABGR2101010) is a 32-bit packed format:
- Bit layout: A[1:0]:B[9:0]:G[9:0]:R[9:0]
- Little endian: byte0=R[7:0], byte1=R[1:0]|G[5:0], etc.
- 2-bit alpha, 10-bit RGB
- See `AB30Handler` in `PixelFormatRGB.cpp` for full implementation.
