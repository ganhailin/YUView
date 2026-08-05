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

#include "PixelFormatRGB.h"

#include <video/LimitedRangeToFullRange.h>

// Activate this if you want to know when which buffer is loaded/converted to image and so on.
#define RGBPIXELFORMAT_DEBUG 0
#if RGBPIXELFORMAT_DEBUG && !NDEBUG
#include <QDebug>
#define DEBUG_RGB_FORMAT qDebug
#else
#define DEBUG_RGB_FORMAT(fmt, ...) ((void)0)
#endif

namespace video::rgb
{

// AB30 Handler implementation
class AB30Handler : public PredefinedRGBFormatHandler
{
public:
  [[nodiscard]] std::string getName() const override { return "AB30"; }
  [[nodiscard]] unsigned    getBitsPerSampleForAlpha() const override { return 2; }
  [[nodiscard]] unsigned    getBitsPerSample() const override { return 10; }
  [[nodiscard]] bool        hasAlpha() const override { return true; }
  [[nodiscard]] unsigned    getNrChannels() const override { return 4; }

  [[nodiscard]] std::size_t bytesPerFrame(Size frameSize) const override
  {
    return std::size_t(frameSize.width) * std::size_t(frameSize.height) * 4;
  }

  [[nodiscard]] int getChannelPosition(Channel channel) const override
  {
    // AB30: ABGR order (Alpha, Blue, Green, Red)
    switch (channel)
    {
      case Channel::Alpha: return 0;
      case Channel::Blue:  return 1;
      case Channel::Green: return 2;
      case Channel::Red:   return 3;
    }
    return -1;
  }

  [[nodiscard]] Channel getChannelAtPosition(int position) const override
  {
    switch (position)
    {
      case 0: return Channel::Alpha;
      case 1: return Channel::Blue;
      case 2: return Channel::Green;
      case 3: return Channel::Red;
    }
    throw std::invalid_argument("Invalid position for AB30");
  }

private:
  // Expand 2-bit alpha to 8-bit using replication
  static inline uint8_t expandAlpha2To8(uint8_t a2)
  {
    if (a2 == 0) return 0;
    if (a2 == 3) return 255;
    return (a2 << 6) | (a2 << 4) | (a2 << 2) | a2;
  }

  // Expand 2-bit alpha to 16-bit using replication
  static inline uint16_t expandAlpha2To16(uint8_t a2)
  {
    if (a2 == 0) return 0;
    if (a2 == 3) return 65535;
    uint16_t a8 = expandAlpha2To8(a2);
    return (a8 << 8) | a8;
  }

public:
  [[nodiscard]] rgba_t getPixelValue(const QByteArray &sourceBuffer,
                                    const Size        frameSize,
                                    const QPoint     &pixelPos) const override
  {
    if (pixelPos.x() < 0 || pixelPos.x() >= static_cast<int>(frameSize.width) ||
        pixelPos.y() < 0 || pixelPos.y() >= static_cast<int>(frameSize.height))
      return {};

    const auto pixelOffset = pixelPos.y() * frameSize.width + pixelPos.x();
    const auto *src = reinterpret_cast<const uint32_t *>(sourceBuffer.data());
    uint32_t value = src[pixelOffset];

    // Extract AB30 components: A[1:0]:B[9:0]:G[9:0]:R[9:0]
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

  void convertToARGB(const QByteArray &sourceBuffer,
                     unsigned char    *targetBuffer,
                     const Size        frameSize,
                     const bool        componentInvert[4],
                     const int         componentScale[4],
                     const bool        limitedRange,
                     const bool        premultiplyAlpha) const override
  {
    const auto numPixels = frameSize.width * frameSize.height;
    const auto *src = reinterpret_cast<const uint32_t *>(sourceBuffer.data());

    for (unsigned i = 0; i < numPixels; i++)
    {
      uint32_t value = src[i];

      // Extract AB30 components
      uint8_t  a2  = (value >> 30) & 0x3;
      uint16_t b10 = (value >> 20) & 0x3FF;
      uint16_t g10 = (value >> 10) & 0x3FF;
      uint16_t r10 = value & 0x3FF;

      // Convert to 8-bit
      uint8_t a8 = expandAlpha2To8(a2);
      uint8_t b8 = b10 >> 2;
      uint8_t g8 = g10 >> 2;
      uint8_t r8 = r10 >> 2;

      // Apply scale and inversion
      auto applyTransform = [](uint8_t val, int scale, bool invert) -> uint8_t {
        int v = static_cast<int>(val) * scale;
        v = functions::clip(v, 0, 255);
        if (invert) v = 255 - v;
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

  void convertTo16BitRGBA(const QByteArray &sourceBuffer,
                          uint16_t         *targetBuffer,
                          const Size        frameSize,
                          const bool        componentInvert[4],
                          const int         componentScale[4],
                          const bool        limitedRange) const override
  {
    const auto numPixels = frameSize.width * frameSize.height;
    const auto *src = reinterpret_cast<const uint32_t *>(sourceBuffer.data());

    for (unsigned i = 0; i < numPixels; i++)
    {
      uint32_t value = src[i];

      // Extract AB30 components
      uint8_t  a2  = (value >> 30) & 0x3;
      uint16_t b10 = (value >> 20) & 0x3FF;
      uint16_t g10 = (value >> 10) & 0x3FF;
      uint16_t r10 = value & 0x3FF;

      // Convert to 16-bit
      uint16_t a16 = expandAlpha2To16(a2);
      uint16_t b16 = b10 << 6;
      uint16_t g16 = g10 << 6;
      uint16_t r16 = r10 << 6;

      // Apply scale and inversion
      auto applyTransform = [](uint16_t val, int scale, bool invert) -> uint16_t {
        int64_t v = (static_cast<int64_t>(val) * scale);
        v = functions::clip(v, 0, 65535);
        if (invert) v = 65535 - v;
        return static_cast<uint16_t>(v);
      };

      r16 = applyTransform(r16, componentScale[0], componentInvert[0]);
      g16 = applyTransform(g16, componentScale[1], componentInvert[1]);
      b16 = applyTransform(b16, componentScale[2], componentInvert[2]);
      a16 = applyTransform(a16, componentScale[3], componentInvert[3]);

      if (limitedRange)
      {
        const auto limitedMin = 4096;
        const auto limitedMax = 60160;
        const auto fullRange  = 65535;
        const auto range      = limitedMax - limitedMin;

        auto limitedToFull = [](uint16_t val, int min, int range, int full) -> uint16_t {
          if (val <= static_cast<uint16_t>(min)) return uint16_t(0);
          if (val >= static_cast<uint16_t>(min + range)) return uint16_t(full);
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

  void convertToGreyscaleARGB(const QByteArray &sourceBuffer,
                              unsigned char    *targetBuffer,
                              const Size        frameSize,
                              const Channel     displayChannel,
                              const int         scale,
                              const bool        invert,
                              const bool        limitedRange) const override
  {
    const auto numPixels = frameSize.width * frameSize.height;
    const auto *src = reinterpret_cast<const uint32_t *>(sourceBuffer.data());

    for (unsigned i = 0; i < numPixels; i++)
    {
      uint32_t value = src[i];

      // Extract AB30 components
      uint16_t componentVal = 0;
      if (displayChannel == Channel::Alpha)
        componentVal = expandAlpha2To8((value >> 30) & 0x3);
      else if (displayChannel == Channel::Blue)
        componentVal = (value >> 20) & 0x3FF;
      else if (displayChannel == Channel::Green)
        componentVal = (value >> 10) & 0x3FF;
      else if (displayChannel == Channel::Red)
        componentVal = value & 0x3FF;

      // Convert to 8-bit
      uint8_t val8 = 0;
      if (displayChannel == Channel::Alpha)
        val8 = static_cast<uint8_t>(componentVal);
      else
        val8 = componentVal >> 2;

      auto applyTransform = [](uint8_t val, int scale, bool invert) -> uint8_t {
        int v = (val * scale);
        v = functions::clip(v, 0, 255);
        if (invert) v = 255 - v;
        return static_cast<uint8_t>(v);
      };


      val8 = applyTransform(val8, scale, invert);

      if (limitedRange && displayChannel != Channel::Alpha)
      {
        val8 = LimitedRangeToFullRange.at(val8);
      }

      targetBuffer[0] = val8;
      targetBuffer[1] = val8;
      targetBuffer[2] = val8;
      targetBuffer[3] = 255;
      targetBuffer += 4;
    }
  }
};

// RGB565 Handler implementation
class RGB565Handler : public PredefinedRGBFormatHandler
{
public:
  RGB565Handler(Endianness endianness) : endianness(endianness) {}

  [[nodiscard]] std::string getName() const override
  {
    return endianness == Endianness::Big ? "RGB 565 BE" : "RGB 565";
  }
  [[nodiscard]] unsigned    getBitsPerSample() const override { return 16; }
  [[nodiscard]] bool        hasAlpha() const override { return false; }
  [[nodiscard]] unsigned    getNrChannels() const override { return 3; }

  [[nodiscard]] std::size_t bytesPerFrame(Size frameSize) const override
  {
    return std::size_t(frameSize.width) * std::size_t(frameSize.height) * 2;
  }

  [[nodiscard]] int getChannelPosition(Channel channel) const override
  {
    switch (channel)
    {
      case Channel::Red:   return 0;
      case Channel::Green: return 1;
      case Channel::Blue:  return 2;
      case Channel::Alpha: return -1;
    }
    return -1;
  }

  [[nodiscard]] Channel getChannelAtPosition(int position) const override
  {
    switch (position)
    {
      case 0: return Channel::Red;
      case 1: return Channel::Green;
      case 2: return Channel::Blue;
    }
    throw std::invalid_argument("Invalid position for RGB565");
  }

  [[nodiscard]] rgba_t getPixelValue(const QByteArray &sourceBuffer,
                                    const Size        frameSize,
                                    const QPoint     &pixelPos) const override
  {
    if (pixelPos.x() < 0 || pixelPos.x() >= static_cast<int>(frameSize.width) ||
        pixelPos.y() < 0 || pixelPos.y() >= static_cast<int>(frameSize.height))
      return {};

    const auto      pixelOffset = pixelPos.y() * frameSize.width + pixelPos.x();
    const uint16_t *src         = reinterpret_cast<const uint16_t *>(sourceBuffer.data());
    uint16_t        val         = src[pixelOffset];

    if (endianness == Endianness::Big)
      val = ((val & 0xff) << 8) | ((val & 0xff00) >> 8);

    rgba_t result;
    result.R = (val >> 11) & 0x1F;
    result.G = (val >> 5) & 0x3F;
    result.B = val & 0x1F;
    result.A = 0;

    return result;
  }

  void convertToARGB(const QByteArray &sourceBuffer,
                     unsigned char    *targetBuffer,
                     const Size        frameSize,
                     const bool        componentInvert[4],
                     const int         componentScale[4],
                     const bool        limitedRange,
                     const bool        premultiplyAlpha) const override
  {
    const auto      numPixels = frameSize.width * frameSize.height;
    const uint16_t *src       = reinterpret_cast<const uint16_t *>(sourceBuffer.data());

    for (unsigned i = 0; i < numPixels; i++)
    {
      uint16_t val = src[i];
      if (endianness == Endianness::Big)
        val = ((val & 0xff) << 8) | ((val & 0xff00) >> 8);

      int valR = (val >> 11) & 0x1F;
      int valG = (val >> 5) & 0x3F;
      int valB = val & 0x1F;

      valR = (valR * 255) / 31;
      valG = (valG * 255) / 63;
      valB = (valB * 255) / 31;

      auto applyTransform = [](int val, int scale, bool invert) -> uint8_t {
        int v = (val * scale);
        v     = functions::clip(v, 0, 255);
        if (invert)
          v = 255 - v;
        return static_cast<uint8_t>(v);
      };

      uint8_t r8 = applyTransform(valR, componentScale[0], componentInvert[0]);
      uint8_t g8 = applyTransform(valG, componentScale[1], componentInvert[1]);
      uint8_t b8 = applyTransform(valB, componentScale[2], componentInvert[2]);
      uint8_t a8 = 255;

      if (limitedRange)
      {
        r8 = LimitedRangeToFullRange.at(r8);
        g8 = LimitedRangeToFullRange.at(g8);
        b8 = LimitedRangeToFullRange.at(b8);
      }

      // Output in BGRA order (QImage format)
      targetBuffer[0] = b8;
      targetBuffer[1] = g8;
      targetBuffer[2] = r8;
      targetBuffer[3] = a8;
      targetBuffer += 4;
    }
  }

  void convertTo16BitRGBA(const QByteArray &sourceBuffer,
                          uint16_t         *targetBuffer,
                          const Size        frameSize,
                          const bool        componentInvert[4],
                          const int         componentScale[4],
                          const bool        limitedRange) const override
  {
    const auto      numPixels = frameSize.width * frameSize.height;
    const uint16_t *src       = reinterpret_cast<const uint16_t *>(sourceBuffer.data());

    for (unsigned i = 0; i < numPixels; i++)
    {
      uint16_t val = src[i];
      if (endianness == Endianness::Big)
        val = ((val & 0xff) << 8) | ((val & 0xff00) >> 8);

      int valR = (val >> 11) & 0x1F;
      int valG = (val >> 5) & 0x3F;
      int valB = val & 0x1F;

      valR = (valR * 65535) / 31;
      valG = (valG * 65535) / 63;
      valB = (valB * 65535) / 31;

      auto applyTransform = [](int val, int scale, bool invert) -> uint16_t {
        int64_t v = (static_cast<int64_t>(val) * scale);
        v         = functions::clip(v, 0, 65535);
        if (invert)
          v = 65535 - v;
        return static_cast<uint16_t>(v);
      };

      uint16_t r16 = applyTransform(valR, componentScale[0], componentInvert[0]);
      uint16_t g16 = applyTransform(valG, componentScale[1], componentInvert[1]);
      uint16_t b16 = applyTransform(valB, componentScale[2], componentInvert[2]);
      uint16_t a16 = 65535;

      if (limitedRange)
      {
        const auto limitedMin = 4096;
        const auto limitedMax = 60160;
        const auto fullRange  = 65535;
        const auto range      = limitedMax - limitedMin;

        auto limitedToFull = [](uint16_t val, int min, int range, int full) -> uint16_t {
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

  void convertToGreyscaleARGB(const QByteArray &sourceBuffer,
                              unsigned char    *targetBuffer,
                              const Size        frameSize,
                              const Channel     displayChannel,
                              const int         scale,
                              const bool        invert,
                              const bool        limitedRange) const override
  {
    const auto      numPixels = frameSize.width * frameSize.height;
    const uint16_t *src       = reinterpret_cast<const uint16_t *>(sourceBuffer.data());

    for (unsigned i = 0; i < numPixels; i++)
    {
      uint16_t val = src[i];
      if (endianness == Endianness::Big)
        val = ((val & 0xff) << 8) | ((val & 0xff00) >> 8);

      int componentVal = 0;
      if (displayChannel == Channel::Red)
        componentVal = (val >> 11) & 0x1F;
      else if (displayChannel == Channel::Green)
        componentVal = (val >> 5) & 0x3F;
      else if (displayChannel == Channel::Blue)
        componentVal = val & 0x1F;

      if (displayChannel == Channel::Green)
        componentVal = (componentVal * 255) / 63;
      else
        componentVal = (componentVal * 255) / 31;

      componentVal = (componentVal * scale);
      componentVal = functions::clip(componentVal, 0, 255);
      if (invert)
        componentVal = 255 - componentVal;
      if (limitedRange)
        componentVal = LimitedRangeToFullRange.at(componentVal);

      targetBuffer[0] = componentVal;
      targetBuffer[1] = componentVal;
      targetBuffer[2] = componentVal;
      targetBuffer[3] = 255;

      targetBuffer += 4;
    }
  }

private:
  Endianness endianness;
};

// Factory function implementation
std::unique_ptr<PredefinedRGBFormatHandler> createPredefinedRGBFormatHandler(
    PredefinedRGBFormat format, Endianness endianness)
{
  switch (format)
  {
    case PredefinedRGBFormat::AB30:
      return std::make_unique<AB30Handler>();
    case PredefinedRGBFormat::RGB565:
      return std::make_unique<RGB565Handler>(endianness);
  }
  return nullptr;
}

std::unique_ptr<PredefinedRGBFormatHandler> PixelFormatRGB::getPredefinedHandler() const
{
  if (this->predefinedFormat)
    return createPredefinedRGBFormatHandler(*this->predefinedFormat, this->endianness);
  return nullptr;
}

PixelFormatRGB::PixelFormatRGB(unsigned     bitsPerSample,
                               DataLayout   dataLayout,
                               ChannelOrder channelOrder,
                               AlphaMode    alphaMode,
                               Endianness   endianness)
    : bitsPerSample(bitsPerSample), dataLayout(dataLayout), channelOrder(channelOrder),
      alphaMode(alphaMode), endianness(endianness)
{
}

PixelFormatRGB PixelFormatRGB::rgb565(Endianness endianness)
{
  PixelFormatRGB fmt(PredefinedRGBFormat::RGB565);
  fmt.endianness = endianness;
  return fmt;
}

PixelFormatRGB::PixelFormatRGB(const std::string &name)
{
  // Check for predefined formats first
  if (auto predefinedFormat = PredefinedRGBFormatMapper.getValue(name))
  {
    this->predefinedFormat = predefinedFormat;
    return;
  }

  if (name == "RGB 565" || name == "RGB 565 BE")
  {
    *this = rgb565(name.find("BE") != std::string::npos ? Endianness::Big : Endianness::Little);
    return;
  }

  if (name != "Unknown Pixel Format")
  {
    auto channelOrderString = name.substr(0, 3);
    if (name[0] == 'a' || name[0] == 'A')
    {
      this->alphaMode    = AlphaMode::First;
      channelOrderString = name.substr(1, 3);
    }
    else if (name[3] == 'a' || name[3] == 'A')
    {
      this->alphaMode    = AlphaMode::Last;
      channelOrderString = name.substr(0, 3);
    }
    auto order = ChannelOrderMapper.getValue(channelOrderString);
    if (order)
      this->channelOrder = *order;

    auto bitIdx = name.find("bit");
    if (bitIdx != std::string::npos)
      this->bitsPerSample = std::stoi(name.substr(bitIdx - 2, 2), nullptr);
    if (name.find("planar") != std::string::npos)
      this->dataLayout = DataLayout::Planar;
    if (this->bitsPerSample > 8 && name.find("BE") != std::string::npos)
      this->endianness = Endianness::Big;
  }
}

PixelFormatRGB::PixelFormatRGB(PredefinedRGBFormat predefinedFormat)
    : predefinedFormat(predefinedFormat)
{
}

std::optional<PredefinedRGBFormat> PixelFormatRGB::getPredefinedFormat() const
{
  return this->predefinedFormat;
}

bool PixelFormatRGB::isValid() const
{
  if (this->predefinedFormat.has_value())
    return createPredefinedRGBFormatHandler(*this->predefinedFormat, this->endianness) != nullptr;
  return this->bitsPerSample >= 8 && this->bitsPerSample <= 32;
}

unsigned PixelFormatRGB::nrChannels() const
{
  if (this->predefinedFormat)
  {
    if (auto handler = getPredefinedHandler())
      return handler->getNrChannels();
  }
  return this->alphaMode != AlphaMode::None ? 4 : 3;
}

bool PixelFormatRGB::hasAlpha() const
{
  if (this->predefinedFormat)
  {
    if (auto handler = getPredefinedHandler())
      return handler->hasAlpha();
  }
  return this->alphaMode != AlphaMode::None;
}

unsigned PixelFormatRGB::getBitsPerSample() const
{
  if (this->predefinedFormat)
  {
    if (auto handler = getPredefinedHandler())
      return handler->getBitsPerSample();
  }
  return this->bitsPerSample;
}

std::string PixelFormatRGB::getName() const
{
  if (!this->isValid())
    return "Unknown Pixel Format";

  if (this->predefinedFormat)
  {
    if (auto handler = getPredefinedHandler())
      return handler->getName();
    return "Unknown Pixel Format";
  }

  std::string name;
  if (this->alphaMode == AlphaMode::First)
    name += "A";
  name += ChannelOrderMapper.getName(this->channelOrder);
  if (this->alphaMode == AlphaMode::Last)
    name += "A";

  name += " " + std::to_string(this->bitsPerSample) + "bit";
  if (this->dataLayout == DataLayout::Planar)
    name += " planar";
  if (this->bitsPerSample > 8 && this->endianness == Endianness::Big)
    name += " BE";

  return name;
}

/* Get the number of bytes for a frame with this RGB format and the given size
 */
std::size_t PixelFormatRGB::bytesPerFrame(Size frameSize) const
{
  if (this->predefinedFormat)
  {
    if (auto handler = getPredefinedHandler())
      return handler->bytesPerFrame(frameSize);
    return 0;
  }

  if (!this->isValid() || !frameSize.isValid())
    return 0;

  auto numSamples = std::size_t(frameSize.height) * std::size_t(frameSize.width);
  auto nrBytes = numSamples * this->nrChannels() * ((this->bitsPerSample + 7) / 8);
  DEBUG_RGB_FORMAT("PixelFormatRGB::bytesPerFrame samples %d channels %d bytes %d",
                   int(numSamples),
                   this->nrChannels(),
                   nrBytes);
  return nrBytes;
}

int PixelFormatRGB::getChannelPosition(Channel channel) const
{
  if (this->predefinedFormat)
  {
    if (auto handler = getPredefinedHandler())
      return handler->getChannelPosition(channel);
    return -1;
  }

  if (channel == Channel::Alpha)
  {
    switch (this->alphaMode)
    {
    case AlphaMode::First:
      return 0;
    case AlphaMode::Last:
      return 3;
    default:
      return -1;
    }
  }

  auto rgbIdx = 0;
  if (channel == Channel::Red)
  {
    if (this->channelOrder == ChannelOrder::RGB || this->channelOrder == ChannelOrder::RBG)
      rgbIdx = 0;
    if (this->channelOrder == ChannelOrder::GRB || this->channelOrder == ChannelOrder::BRG)
      rgbIdx = 1;
    if (this->channelOrder == ChannelOrder::GBR || this->channelOrder == ChannelOrder::BGR)
      rgbIdx = 2;
  }
  else if (channel == Channel::Green)
  {
    if (this->channelOrder == ChannelOrder::GRB || this->channelOrder == ChannelOrder::GBR)
      rgbIdx = 0;
    if (this->channelOrder == ChannelOrder::RGB || this->channelOrder == ChannelOrder::BGR)
      rgbIdx = 1;
    if (this->channelOrder == ChannelOrder::RBG || this->channelOrder == ChannelOrder::BRG)
      rgbIdx = 2;
  }
  else if (channel == Channel::Blue)
  {
    if (this->channelOrder == ChannelOrder::BGR || this->channelOrder == ChannelOrder::BRG)
      rgbIdx = 0;
    if (this->channelOrder == ChannelOrder::RBG || this->channelOrder == ChannelOrder::GBR)
      rgbIdx = 1;
    if (this->channelOrder == ChannelOrder::RGB || this->channelOrder == ChannelOrder::GRB)
      rgbIdx = 2;
  }

  if (this->alphaMode == AlphaMode::First)
    return rgbIdx + 1;
  return rgbIdx;
}

Channel PixelFormatRGB::getChannelAtPosition(int position) const
{
  if (this->predefinedFormat)
  {
    if (auto handler = getPredefinedHandler())
      return handler->getChannelAtPosition(position);
    throw std::invalid_argument("Invalid predefined format");
  }

  if (this->hasAlpha())
  {
    if (position == 0 && this->alphaMode == AlphaMode::First)
      return Channel::Alpha;
    if (position == 3 && this->alphaMode == AlphaMode::Last)
      return Channel::Alpha;

    if (this->alphaMode == AlphaMode::First)
      position--;
  }

  if (position == 0)
  {
    if (this->channelOrder == ChannelOrder::RGB || this->channelOrder == ChannelOrder::RBG)
      return Channel::Red;
    if (this->channelOrder == ChannelOrder::GRB || this->channelOrder == ChannelOrder::GBR)
      return Channel::Green;
    if (this->channelOrder == ChannelOrder::BGR || this->channelOrder == ChannelOrder::BRG)
      return Channel::Blue;
  }
  else if (position == 1)
  {
    if (this->channelOrder == ChannelOrder::GRB || this->channelOrder == ChannelOrder::BRG)
      return Channel::Red;
    if (this->channelOrder == ChannelOrder::RGB || this->channelOrder == ChannelOrder::BGR)
      return Channel::Green;
    if (this->channelOrder == ChannelOrder::RBG || this->channelOrder == ChannelOrder::GBR)
      return Channel::Blue;
  }
  else if (position == 2)
  {
    if (this->channelOrder == ChannelOrder::GBR || this->channelOrder == ChannelOrder::BGR)
      return Channel::Red;
    if (this->channelOrder == ChannelOrder::RBG || this->channelOrder == ChannelOrder::BRG)
      return Channel::Green;
    if (this->channelOrder == ChannelOrder::RGB || this->channelOrder == ChannelOrder::GRB)
      return Channel::Blue;
  }

  throw std::invalid_argument("Invalid argument for channel position");
}

} // namespace video::rgb