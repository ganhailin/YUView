/*  This file is part of YUView - The YUV player with advanced analytics toolset
 *   <https://github.com/IENT/YUView>
 *   Copyright (C) 2015  Institut für Nachrichtentechnik, RWTH Aachen University, GERMANY
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   In addition, as a special exception, the copyright holders give
 *   permission to link the code of portions of this program with the
 *   OpenSSL library under certain conditions as described in each
 *   individual source file, and distribute linked combinations including
 *   the two.
 *
 *   You must obey the GNU General Public License in all respects for all
 *   of the code used other than OpenSSL. If you modify file(s) with this
 *   exception, you may extend this exception to your version of the
 *   file(s), but you are not obligated to do so. If you do not wish to do
 *   so, delete this exception statement from your version. If you delete
 *   this exception statement from all source files in the program, then
 *   also delete it here.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "ConversionRGB.h"

#include <video/LimitedRangeToFullRange.h>

namespace video::rgb
{

namespace
{

template <int bitDepth>
using UintValueType =
  typename std::conditional_t<bitDepth == 8,
                              uint8_t *,
                              std::conditional_t<bitDepth == 16, uint16_t *, uint32_t *>>;

template <int bitDepth, typename T> T swapBytesEndianess(const T &val)
{
  if (bitDepth <= 8)
    return val;
  if (bitDepth <= 16)
    return ((val & 0xff) << 8) | ((val & 0xff00) >> 8);
  if (bitDepth <= 32)
    return ((val & 0xff) << 24) | ((val & 0xff00) << 8) | ((val & 0xff0000) >> 8) |
           ((val & 0xff000000) >> 24);
};

int getOffsetToFirstByteOfComponent(const Channel         channel,
                                    const PixelFormatRGB &pixelFormat,
                                    const Size            frameSize)
{
  auto offset = pixelFormat.getChannelPosition(channel);
  if (pixelFormat.getDataLayout() == DataLayout::Planar)
    offset *= frameSize.width * frameSize.height;
  return offset;
}

// Convert the input format to the output RGBA format. Apply inversion, scaling,
// limited range conversion and alpha multiplication. The input can be any supported
// format. The output is always 8 bit ARGB little endian.
template <int bitDepth>
void convertRGBToARGB(const QByteArray     &sourceBuffer,
                      const PixelFormatRGB &srcPixelFormat,
                      unsigned char        *targetBuffer,
                      const Size            frameSize,
                      const bool            componentInvert[4],
                      const int             componentScale[4],
                      const bool            limitedRange,
                      const bool            outputHasAlpha,
                      const bool            premultiplyAlpha)
{
  const int  rightShift = bitDepth == 8 ? 0 : (srcPixelFormat.getBitsPerSample() - 8);
  const auto offsetToNextValue =
    srcPixelFormat.getDataLayout() == DataLayout::Planar ? 1 : srcPixelFormat.nrChannels();

  using InValueType   = UintValueType<bitDepth>;
  const auto setAlpha = outputHasAlpha && srcPixelFormat.hasAlpha();

  const auto rawData = (InValueType)sourceBuffer.data();

  auto srcR = rawData + getOffsetToFirstByteOfComponent(Channel::Red, srcPixelFormat, frameSize);
  auto srcG = rawData + getOffsetToFirstByteOfComponent(Channel::Green, srcPixelFormat, frameSize);
  auto srcB = rawData + getOffsetToFirstByteOfComponent(Channel::Blue, srcPixelFormat, frameSize);

  InValueType srcA = nullptr;
  if (setAlpha)
  {
    auto offsetA = srcPixelFormat.getChannelPosition(Channel::Alpha);
    if (srcPixelFormat.getDataLayout() == DataLayout::Planar)
      offsetA *= frameSize.width * frameSize.height;
    srcA = ((InValueType)sourceBuffer.data()) + offsetA;
  }

  for (unsigned i = 0; i < frameSize.width * frameSize.height; i++)
  {
    const auto isBigEndian = bitDepth > 8 && srcPixelFormat.getEndianess() == Endianness::Big;
    auto       convertValue =
      [&isBigEndian, &rightShift](const InValueType sourceData, const int scale, const bool invert)
    {
      auto value = static_cast<int64_t>(sourceData[0]);
      if (isBigEndian)
        value = swapBytesEndianess<bitDepth>(value);
      value = ((value * scale) >> rightShift);
      value = functions::clip(value, 0, 255);
      if (invert)
        value = 255 - value;
      return value;
    };

    auto valR = convertValue(srcR, componentScale[0], componentInvert[0]);
    auto valG = convertValue(srcG, componentScale[1], componentInvert[1]);
    auto valB = convertValue(srcB, componentScale[2], componentInvert[2]);

    if (limitedRange)
    {
      valR = LimitedRangeToFullRange.at(valR);
      valG = LimitedRangeToFullRange.at(valG);
      valB = LimitedRangeToFullRange.at(valB);
      // No limited range for alpha
    }

    int valA = 255;
    if (setAlpha)
    {
      valA = convertValue(srcA, componentScale[3], componentInvert[3]);
      srcA += offsetToNextValue;

      if (premultiplyAlpha)
      {
        valR = ((valR * 255) * valA) / (255 * 255);
        valG = ((valG * 255) * valA) / (255 * 255);
        valB = ((valB * 255) * valA) / (255 * 255);
      }
    }

    srcR += offsetToNextValue;
    srcG += offsetToNextValue;
    srcB += offsetToNextValue;

    targetBuffer[0] = valB;
    targetBuffer[1] = valG;
    targetBuffer[2] = valR;
    targetBuffer[3] = valA;

    targetBuffer += 4;
  }
}

// Convert one single plane of the input format to RGBA. This is used to visualize the individual
// components.
template <int bitDepth>
void convertRGBPlaneToARGB(const QByteArray     &sourceBuffer,
                           const PixelFormatRGB &srcPixelFormat,
                           unsigned char        *targetBuffer,
                           const Size            frameSize,
                           const Channel         displayChannel,
                           const int             scale,
                           const bool            invert,
                           const bool            limitedRange)
{
  const auto shiftTo8Bit = srcPixelFormat.getBitsPerSample() - 8;
  const auto offsetToNextValue =
    srcPixelFormat.getDataLayout() == DataLayout::Planar ? 1 : srcPixelFormat.nrChannels();

  using InValueType = UintValueType<bitDepth>;

  auto       src                    = (InValueType)sourceBuffer.data();
  const auto displayComponentOffset = srcPixelFormat.getChannelPosition(displayChannel);
  if (srcPixelFormat.getDataLayout() == DataLayout::Planar)
    src += displayComponentOffset * frameSize.width * frameSize.height;
  else
    src += displayComponentOffset;

  for (size_t i = 0; i < frameSize.width * frameSize.height; i++)
  {
    auto val = static_cast<int64_t>(src[0]);
    if (bitDepth > 8 && srcPixelFormat.getEndianess() == Endianness::Big)
      val = swapBytesEndianess<bitDepth>(val);
    val = (val * scale) >> shiftTo8Bit;
    val = functions::clip(val, 0, 255);
    if (invert)
      val = 255 - val;
    if (limitedRange)
      val = LimitedRangeToFullRange.at(val);

    targetBuffer[0] = val;
    targetBuffer[1] = val;
    targetBuffer[2] = val;
    targetBuffer[3] = 255;

    src += offsetToNextValue;
    targetBuffer += 4;
  }
}

template <int bitDepth>
rgba_t getPixelValue(const QByteArray     &sourceBuffer,
                     const PixelFormatRGB &srcPixelFormat,
                     const Size            frameSize,
                     const QPoint         &pixelPos)
{
  const auto offsetToNextValue =
    srcPixelFormat.getDataLayout() == DataLayout::Planar ? 1 : srcPixelFormat.nrChannels();
  const auto offsetPixelPos = frameSize.width * pixelPos.y() + pixelPos.x();

  using InValueType = UintValueType<bitDepth>;

  const auto rawData  = (InValueType)sourceBuffer.data();
  auto       srcPixel = rawData + offsetPixelPos * offsetToNextValue;

  rgba_t value{};
  for (auto channel : {Channel::Red, Channel::Green, Channel::Blue, Channel::Alpha})
  {
    if (channel == Channel::Alpha && !srcPixelFormat.hasAlpha())
      continue;

    const auto offset = getOffsetToFirstByteOfComponent(channel, srcPixelFormat, frameSize);

    auto src = srcPixel + offset;
    auto val = 0;
    if((src - rawData)*sizeof(rawData[0]) < sourceBuffer.size())
    {
      val = (unsigned)src[0];
    }
    if (bitDepth > 8 && srcPixelFormat.getEndianess() == Endianness::Big)
      val = swapBytesEndianess<bitDepth>(val);
    value[channel] = val;
  }

  return value;
}

} // namespace

// AB30 helper functions
namespace {

// Expand 2-bit alpha to 8-bit using replication (same as Python reference)
inline uint8_t expandAlpha2To8(uint8_t a2)
{
  if (a2 == 0)
    return 0;
  if (a2 == 3)
    return 255;
  // Replicate pattern: 0b01 -> 0b01010101 (0x55), 0b10 -> 0b10101010 (0xAA)
  return (a2 << 6) | (a2 << 4) | (a2 << 2) | a2;
}

// Expand 2-bit alpha to 16-bit using replication
inline uint16_t expandAlpha2To16(uint8_t a2)
{
  if (a2 == 0)
    return 0;
  if (a2 == 3)
    return 65535;
  // Replicate pattern to 16 bits
  uint16_t a8 = expandAlpha2To8(a2);
  return (a8 << 8) | a8;
}

} // namespace

// Forward declarations for AB30 conversion functions
void convertAB30ToARGB(const QByteArray &sourceBuffer,
                       unsigned char *   targetBuffer,
                       const Size        frameSize,
                       const bool        componentInvert[4],
                       const int         componentScale[4],
                       const bool        limitedRange,
                       const bool        outputHasAlpha,
                       const bool        premultiplyAlpha);

rgba_t getAB30PixelValue(const QByteArray &sourceBuffer,
                         const Size        frameSize,
                         const QPoint     &pixelPos);

void convertAB30To16BitRGBA(const QByteArray &sourceBuffer,
                            uint16_t *       targetBuffer,
                            const Size       frameSize,
                            const bool       componentInvert[4],
                            const int        componentScale[4],
                            const bool       limitedRange);

void convertInputRGBToARGB(const QByteArray     &sourceBuffer,
                           const PixelFormatRGB &srcPixelFormat,
                           unsigned char        *targetBuffer,
                           const Size            frameSize,
                           const bool            componentInvert[4],
                           const int             componentScale[4],
                           const bool            limitedRange,
                           const bool            outputHasAlpha,
                           const bool            premultiplyAlpha)
{
  // Check for predefined formats first
  if (srcPixelFormat.getPredefinedFormat())
  {
    if (auto handler = srcPixelFormat.getPredefinedHandler())
    {
      handler->convertToARGB(sourceBuffer,
                             targetBuffer,
                             frameSize,
                             componentInvert,
                             componentScale,
                             limitedRange,
                             premultiplyAlpha);
      return;
    }
  }

  const auto bitsPerSample = srcPixelFormat.getBitsPerSample();
  if (bitsPerSample < 8 || bitsPerSample > 32)
    throw std::invalid_argument("Invalid bit depth in pixel format for conversion");

  if (bitsPerSample == 8)
    convertRGBToARGB<8>(sourceBuffer,
                        srcPixelFormat,
                        targetBuffer,
                        frameSize,
                        componentInvert,
                        componentScale,
                        limitedRange,
                        outputHasAlpha,
                        premultiplyAlpha);
  else if (bitsPerSample <= 16)
    convertRGBToARGB<16>(sourceBuffer,
                         srcPixelFormat,
                         targetBuffer,
                         frameSize,
                         componentInvert,
                         componentScale,
                         limitedRange,
                         outputHasAlpha,
                         premultiplyAlpha);
  else
    convertRGBToARGB<32>(sourceBuffer,
                         srcPixelFormat,
                         targetBuffer,
                         frameSize,
                         componentInvert,
                         componentScale,
                         limitedRange,
                         outputHasAlpha,
                         premultiplyAlpha);
}

void convertSinglePlaneOfRGBToGreyscaleARGB(const QByteArray     &sourceBuffer,
                                            const PixelFormatRGB &srcPixelFormat,
                                            unsigned char        *targetBuffer,
                                            const Size            frameSize,
                                            const Channel         displayChannel,
                                            const int             scale,
                                            const bool            invert,
                                            const bool            limitedRange)
{
  const auto bitsPerSample = srcPixelFormat.getBitsPerSample();
  if (bitsPerSample < 8 || bitsPerSample > 32)
    throw std::invalid_argument("Invalid bit depth in pixel format for conversion");

  if (bitsPerSample == 8)
    convertRGBPlaneToARGB<8>(sourceBuffer,
                             srcPixelFormat,
                             targetBuffer,
                             frameSize,
                             displayChannel,
                             scale,
                             invert,
                             limitedRange);
  else if (bitsPerSample <= 16)
    convertRGBPlaneToARGB<16>(sourceBuffer,
                              srcPixelFormat,
                              targetBuffer,
                              frameSize,
                              displayChannel,
                              scale,
                              invert,
                              limitedRange);
  else
    convertRGBPlaneToARGB<32>(sourceBuffer,
                              srcPixelFormat,
                              targetBuffer,
                              frameSize,
                              displayChannel,
                              scale,
                              invert,
                              limitedRange);
}

rgba_t getPixelValueFromBuffer(const QByteArray     &sourceBuffer,
                               const PixelFormatRGB &srcPixelFormat,
                               const Size            frameSize,
                               const QPoint         &pixelPos)
{
  // Check for predefined formats first
  if (srcPixelFormat.getPredefinedFormat())
  {
    if (auto handler = srcPixelFormat.getPredefinedHandler())
    {
      return handler->getPixelValue(sourceBuffer, frameSize, pixelPos);
    }
  }

  const auto bitsPerSample = srcPixelFormat.getBitsPerSample();
  if (bitsPerSample < 8 || bitsPerSample > 32)
    throw std::invalid_argument("Invalid bit depth in pixel format for conversion");

  if (bitsPerSample == 8)
    return getPixelValue<8>(sourceBuffer, srcPixelFormat, frameSize, pixelPos);
  else if (bitsPerSample <= 16)
    return getPixelValue<16>(sourceBuffer, srcPixelFormat, frameSize, pixelPos);
  else
    return getPixelValue<32>(sourceBuffer, srcPixelFormat, frameSize, pixelPos);
}

// Convert input RGB data to 16-bit RGBA output for HDR rendering.
// Template parameter bitDepth is the bit depth of the input data (8, 16, or 32).
// Output is always 16-bit per channel in RGBA order.
template <int bitDepth>
void convertRGBTo16BitRGBAInternal(const QByteArray     &sourceBuffer,
                                   const PixelFormatRGB &srcPixelFormat,
                                   uint16_t             *targetBuffer,
                                   const Size            frameSize,
                                   const bool            componentInvert[4],
                                   const int             componentScale[4],
                                   const bool            limitedRange)
{
  // Calculate the shift needed to scale input values to 16-bit
  // For 8-bit: shift by 8 (<< 8) to get 16-bit
  // For 10-bit: shift by 6 (<< 6) to get 16-bit
  // For 12-bit: shift by 4 (<< 4) to get 16-bit
  // For 16-bit: shift by 0 (no shift)
  const auto inputBits = srcPixelFormat.getBitsPerSample();
  const auto shiftTo16 = 16 - inputBits;

  const auto offsetToNextValue =
      srcPixelFormat.getDataLayout() == DataLayout::Planar ? 1 : srcPixelFormat.nrChannels();

  using InValueType = UintValueType<bitDepth>;

  const auto rawData = (InValueType)sourceBuffer.data();

  auto srcR = rawData + getOffsetToFirstByteOfComponent(Channel::Red, srcPixelFormat, frameSize);
  auto srcG = rawData + getOffsetToFirstByteOfComponent(Channel::Green, srcPixelFormat, frameSize);
  auto srcB = rawData + getOffsetToFirstByteOfComponent(Channel::Blue, srcPixelFormat, frameSize);

  // Handle alpha channel
  const auto setAlpha = srcPixelFormat.hasAlpha();
  InValueType srcA    = nullptr;
  int         alphaPos;
  if (setAlpha)
  {
    alphaPos = srcPixelFormat.getChannelPosition(Channel::Alpha);
    if (srcPixelFormat.getDataLayout() == DataLayout::Planar)
      alphaPos *= frameSize.width * frameSize.height;
    srcA = ((InValueType)sourceBuffer.data()) + alphaPos;
  }

  for (unsigned i = 0; i < frameSize.width * frameSize.height; i++)
  {
    const auto isBigEndian = bitDepth > 8 && srcPixelFormat.getEndianess() == Endianness::Big;

    auto convertValue =
        [&isBigEndian, &shiftTo16](const InValueType sourceData, const int scale, const bool invert)
    {
      auto value = static_cast<int64_t>(sourceData[0]);
      if (isBigEndian)
        value = swapBytesEndianess<bitDepth>(value);

      // Apply scale first, then shift to 16-bit
      value = (value * scale) << shiftTo16;
      value = functions::clip(value, 0, 65535);

      if (invert)
        value = 65535 - value;
      return static_cast<uint16_t>(value);
    };

    auto valR = convertValue(srcR, componentScale[0], componentInvert[0]);
    auto valG = convertValue(srcG, componentScale[1], componentInvert[1]);
    auto valB = convertValue(srcB, componentScale[2], componentInvert[2]);

    if (limitedRange)
    {
      // Apply limited range to full range conversion for 16-bit values
      // Limited range for N-bit is [16 << (N-8), 235 << (N-8)]
      // For 16-bit: [4096, 60160]
      const auto limitedMin   = 4096;   // 16 << 8
      const auto limitedMax   = 60160;  // 235 << 8
      const auto fullRange    = 65535;
      const auto limitedRange = limitedMax - limitedMin;

      auto limitedToFull = [](uint16_t val, int min, int range, int full)
      {
        if (val <= min)
          return uint16_t(0);
        if (val >= min + range)
          return uint16_t(full);
        return uint16_t(((val - min) * full) / range);
      };

      valR = limitedToFull(valR, limitedMin, limitedRange, fullRange);
      valG = limitedToFull(valG, limitedMin, limitedRange, fullRange);
      valB = limitedToFull(valB, limitedMin, limitedRange, fullRange);
    }

    uint16_t valA = 65535;  // Default to fully opaque
    if (setAlpha)
    {
      valA = convertValue(srcA, componentScale[3], componentInvert[3]);
      srcA += offsetToNextValue;
    }

    srcR += offsetToNextValue;
    srcG += offsetToNextValue;
    srcB += offsetToNextValue;

    // Output in RGBA order
    targetBuffer[0] = valR;
    targetBuffer[1] = valG;
    targetBuffer[2] = valB;
    targetBuffer[3] = valA;

    targetBuffer += 4;
  }
}

void convertRGBTo16BitRGBA(const QByteArray     &sourceBuffer,
                           const PixelFormatRGB &srcPixelFormat,
                           uint16_t             *targetBuffer,
                           const Size            frameSize,
                           const bool            componentInvert[4],
                           const int             componentScale[4],
                           const bool            limitedRange)
{
  // Check for predefined formats first
  if (srcPixelFormat.getPredefinedFormat())
  {
    if (auto handler = srcPixelFormat.getPredefinedHandler())
    {
      handler->convertTo16BitRGBA(sourceBuffer,
                                  targetBuffer,
                                  frameSize,
                                  componentInvert,
                                  componentScale,
                                  limitedRange);
      return;
    }
  }

  const auto bitsPerSample = srcPixelFormat.getBitsPerSample();
  if (bitsPerSample < 8 || bitsPerSample > 32)
    throw std::invalid_argument("Invalid bit depth in pixel format for conversion");

  if (bitsPerSample == 8)
    convertRGBTo16BitRGBAInternal<8>(
        sourceBuffer, srcPixelFormat, targetBuffer, frameSize, componentInvert, componentScale, limitedRange);
  else if (bitsPerSample <= 16)
    convertRGBTo16BitRGBAInternal<16>(
        sourceBuffer, srcPixelFormat, targetBuffer, frameSize, componentInvert, componentScale, limitedRange);
  else
    convertRGBTo16BitRGBAInternal<32>(
        sourceBuffer, srcPixelFormat, targetBuffer, frameSize, componentInvert, componentScale, limitedRange);
}

void convertAB30ToARGB(const QByteArray &sourceBuffer,
                       unsigned char *   targetBuffer,
                       const Size        frameSize,
                       const bool        componentInvert[4],
                       const int         componentScale[4],
                       const bool        limitedRange,
                       const bool        outputHasAlpha,
                       const bool        premultiplyAlpha)
{
  const auto numPixels = frameSize.width * frameSize.height;
  const auto *src      = reinterpret_cast<const uint32_t *>(sourceBuffer.data());

  for (unsigned i = 0; i < numPixels; i++)
  {
    uint32_t value = src[i];

    // Extract AB30 components (little endian)
    // bits 31-30: Alpha[1:0]
    // bits 29-20: Blue[9:0]
    // bits 19-10: Green[9:0]
    // bits  9-0 : Red[9:0]
    uint8_t  a2  = (value >> 30) & 0x3;
    uint16_t b10 = (value >> 20) & 0x3FF;
    uint16_t g10 = (value >> 10) & 0x3FF;
    uint16_t r10 = value & 0x3FF;

    // Convert to 8-bit (shift right by 2 for RGB, expand alpha)
    uint8_t a8 = expandAlpha2To8(a2);
    uint8_t b8 = b10 >> 2;
    uint8_t g8 = g10 >> 2;
    uint8_t r8 = r10 >> 2;

    // Apply scale and inversion
    auto applyTransform = [](uint8_t val, int scale, bool invert) -> uint8_t
    {
      int v = (val * scale) >> 8;
      v     = functions::clip(v, 0, 255);
      if (invert)
        v = 255 - v;
      return static_cast<uint8_t>(v);
    };

    r8 = applyTransform(r8, componentScale[0], componentInvert[0]);
    g8 = applyTransform(g8, componentScale[1], componentInvert[1]);
    b8 = applyTransform(b8, componentScale[2], componentInvert[2]);
    a8 = applyTransform(a8, componentScale[3], componentInvert[3]);

    if (limitedRange)
    {
      r8 = LimitedRangeToFullRange.at(r8);
      g8 = LimitedRangeToFullRange.at(g8);
      b8 = LimitedRangeToFullRange.at(b8);
    }

    if (premultiplyAlpha && a8 != 255)
    {
      r8 = (r8 * a8) / 255;
      g8 = (g8 * a8) / 255;
      b8 = (b8 * a8) / 255;
    }

    // Output in BGRA order (QImage format)
    targetBuffer[0] = b8;
    targetBuffer[1] = g8;
    targetBuffer[2] = r8;
    targetBuffer[3] = a8;

    targetBuffer += 4;
  }
}

rgba_t getAB30PixelValue(const QByteArray &sourceBuffer,
                         const Size        frameSize,
                         const QPoint     &pixelPos)
{
  if (pixelPos.x() < 0 || pixelPos.x() >= static_cast<int>(frameSize.width) ||
      pixelPos.y() < 0 || pixelPos.y() >= static_cast<int>(frameSize.height))
    return {};

  const auto pixelOffset = pixelPos.y() * frameSize.width + pixelPos.x();
  const auto *src          = reinterpret_cast<const uint32_t *>(sourceBuffer.data());
  uint32_t   value         = src[pixelOffset];

  // Extract AB30 components
  uint8_t  a2  = (value >> 30) & 0x3;
  uint16_t b10 = (value >> 20) & 0x3FF;
  uint16_t g10 = (value >> 10) & 0x3FF;
  uint16_t r10 = value & 0x3FF;

  rgba_t result;
  result.A = a2;
  result.B = b10;
  result.G = g10;
  result.R = r10;

  return result;
}

void convertAB30To16BitRGBA(const QByteArray &sourceBuffer,
                            uint16_t *       targetBuffer,
                            const Size       frameSize,
                            const bool       componentInvert[4],
                            const int        componentScale[4],
                            const bool       limitedRange)
{
  const auto numPixels = frameSize.width * frameSize.height;
  const auto *src      = reinterpret_cast<const uint32_t *>(sourceBuffer.data());

  for (unsigned i = 0; i < numPixels; i++)
  {
    uint32_t value = src[i];

    // Extract AB30 components
    uint8_t  a2  = (value >> 30) & 0x3;
    uint16_t b10 = (value >> 20) & 0x3FF;
    uint16_t g10 = (value >> 10) & 0x3FF;
    uint16_t r10 = value & 0x3FF;

    // Convert to 16-bit (shift left by 6 for RGB, expand alpha)
    uint16_t a16 = expandAlpha2To16(a2);
    uint16_t b16 = b10 << 6;
    uint16_t g16 = g10 << 6;
    uint16_t r16 = r10 << 6;

    // Apply scale and inversion
    auto applyTransform = [](uint16_t val, int scale, bool invert) -> uint16_t
    {
      int64_t v = (static_cast<int64_t>(val) * scale);
      v         = functions::clip(v, 0, 65535);
      if (invert)
        v = 65535 - v;
      return static_cast<uint16_t>(v);
    };

    r16 = applyTransform(r16, componentScale[0], componentInvert[0]);
    g16 = applyTransform(g16, componentScale[1], componentInvert[1]);
    b16 = applyTransform(b16, componentScale[2], componentInvert[2]);
    a16 = applyTransform(a16, componentScale[3], componentInvert[3]);

    if (limitedRange)
    {
      // Apply limited range to full range conversion for 16-bit values
      const auto limitedMin = 4096;  // 16 << 8
      const auto limitedMax = 60160; // 235 << 8
      const auto fullRange  = 65535;
      const auto range      = limitedMax - limitedMin;

      auto limitedToFull = [](uint16_t val, int min, int range, int full) -> uint16_t
      {
        if (val <= static_cast<uint16_t>(min))
          return uint16_t(0);
        if (val >= static_cast<uint16_t>(min + range))
          return uint16_t(full);
        return uint16_t(((static_cast<int64_t>(val) - min) * full) / range);
      };

      r16 = limitedToFull(r16, limitedMin, range, fullRange);
      g16 = limitedToFull(g16, limitedMin, range, fullRange);
      b16 = limitedToFull(b16, limitedMin, range, fullRange);
    }

    // Output in RGBA order
    targetBuffer[0] = r16;
    targetBuffer[1] = g16;
    targetBuffer[2] = b16;
    targetBuffer[3] = a16;

    targetBuffer += 4;
  }
}

} // namespace video::rgb
