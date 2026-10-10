#pragma once
// CPU decoding of BC1/BC2/BC3 (DXT1/3/5) blocks to RGBA8, used when the GPU
// cannot sample block-compressed formats (ARM Mali exposes no
// textureCompressionBC). Decodes exactly as Vulkan defines the BC formats.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <vulkan/vulkan.h>

namespace rex::graphics::gta4_native {

inline bool IsNativeBcFormat(VkFormat format) {
  return format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK || format == VK_FORMAT_BC2_UNORM_BLOCK ||
         format == VK_FORMAT_BC3_UNORM_BLOCK;
}

inline uint32_t NativeBcBlockBytes(VkFormat format) {
  return format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ? 8u : 16u;
}

namespace bc_detail {
inline void Rgb565(uint16_t c, uint32_t out[3]) {
  const uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
  out[0] = (r << 3) | (r >> 2);
  out[1] = (g << 2) | (g >> 4);
  out[2] = (b << 3) | (b >> 2);
}

// Writes 16 RGBA texels (row-major 4x4) for a BC1-style color block.
inline void DecodeColor(const uint8_t* block, bool allow_punchthrough, uint8_t rgba[64]) {
  uint16_t c0, c1;
  uint32_t selectors;
  std::memcpy(&c0, block, 2);
  std::memcpy(&c1, block + 2, 2);
  std::memcpy(&selectors, block + 4, 4);
  uint32_t p[4][4];
  Rgb565(c0, p[0]);
  Rgb565(c1, p[1]);
  p[0][3] = p[1][3] = 255;
  if (c0 > c1 || !allow_punchthrough) {
    for (int k = 0; k < 3; ++k) {
      p[2][k] = (2 * p[0][k] + p[1][k] + 1) / 3;
      p[3][k] = (p[0][k] + 2 * p[1][k] + 1) / 3;
    }
    p[2][3] = p[3][3] = 255;
  } else {
    for (int k = 0; k < 3; ++k) {
      p[2][k] = (p[0][k] + p[1][k]) / 2;
      p[3][k] = 0;
    }
    p[2][3] = 255;
    p[3][3] = 0;
  }
  for (int i = 0; i < 16; ++i) {
    const uint32_t* c = p[(selectors >> (2 * i)) & 3];
    rgba[i * 4 + 0] = uint8_t(c[0]);
    rgba[i * 4 + 1] = uint8_t(c[1]);
    rgba[i * 4 + 2] = uint8_t(c[2]);
    rgba[i * 4 + 3] = uint8_t(c[3]);
  }
}

inline void DecodeBc3Alpha(const uint8_t* block, uint8_t rgba[64]) {
  const uint32_t a0 = block[0], a1 = block[1];
  uint32_t a[8] = {a0, a1};
  if (a0 > a1) {
    for (uint32_t i = 1; i < 7; ++i) a[i + 1] = ((7 - i) * a0 + i * a1 + 3) / 7;
  } else {
    for (uint32_t i = 1; i < 5; ++i) a[i + 1] = ((5 - i) * a0 + i * a1 + 2) / 5;
    a[6] = 0;
    a[7] = 255;
  }
  uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) bits |= uint64_t(block[2 + i]) << (8 * i);
  for (int i = 0; i < 16; ++i) rgba[i * 4 + 3] = uint8_t(a[(bits >> (3 * i)) & 7]);
}

inline void DecodeBc2Alpha(const uint8_t* block, uint8_t rgba[64]) {
  for (int i = 0; i < 16; ++i) {
    const uint32_t nibble = (block[i / 2] >> ((i & 1) * 4)) & 0xF;
    rgba[i * 4 + 3] = uint8_t(nibble * 17);
  }
}
}  // namespace bc_detail

// Decodes one 2D slice. row_length/image_height are in texels (multiples of
// 4, as in VkBufferImageCopy); destination is row_length*image_height RGBA8.
inline void DecodeNativeBcSlice(VkFormat format, const uint8_t* source, uint32_t row_length,
                                uint32_t image_height, uint8_t* destination) {
  const uint32_t blocks_x = (row_length + 3) / 4, blocks_y = (image_height + 3) / 4;
  const uint32_t block_bytes = NativeBcBlockBytes(format);
  const size_t dest_pitch = size_t(blocks_x) * 4 * 4;
  uint8_t texels[64];
  for (uint32_t by = 0; by < blocks_y; ++by) {
    for (uint32_t bx = 0; bx < blocks_x; ++bx) {
      const uint8_t* block = source + (size_t(by) * blocks_x + bx) * block_bytes;
      if (format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK) {
        bc_detail::DecodeColor(block, true, texels);
      } else if (format == VK_FORMAT_BC2_UNORM_BLOCK) {
        bc_detail::DecodeColor(block + 8, false, texels);
        bc_detail::DecodeBc2Alpha(block, texels);
      } else {
        bc_detail::DecodeColor(block + 8, false, texels);
        bc_detail::DecodeBc3Alpha(block, texels);
      }
      for (uint32_t y = 0; y < 4; ++y) {
        std::memcpy(destination + (size_t(by) * 4 + y) * dest_pitch + size_t(bx) * 16,
                    texels + y * 16, 16);
      }
    }
  }
}

}  // namespace rex::graphics::gta4_native
