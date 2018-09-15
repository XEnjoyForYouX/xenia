/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2018 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/d3d12/primitive_converter.h"

#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/platform.h"

namespace xe {
namespace gpu {
namespace d3d12 {

PrimitiveConverter::PrimitiveConverter(ui::d3d12::D3D12Context* context,
                                       RegisterFile* register_file,
                                       Memory* memory,
                                       SharedMemory* shared_memory)
    : context_(context),
      register_file_(register_file),
      memory_(memory),
      shared_memory_(shared_memory) {}

PrimitiveConverter::~PrimitiveConverter() { Shutdown(); }

void PrimitiveConverter::Initialize() {
  // There can be at most 65535 indices in a Xenos draw call, but they can be up
  // to 4 bytes large, and conversion can add more indices (almost triple the
  // count for triangle strips, for instance).
  buffer_pool_ =
      std::make_unique<ui::d3d12::UploadBufferPool>(context_, 4 * 1024 * 1024);
}

void PrimitiveConverter::Shutdown() { buffer_pool_.reset(); }

void PrimitiveConverter::ClearCache() { buffer_pool_->ClearCache(); }

void PrimitiveConverter::BeginFrame(ID3D12GraphicsCommandList* command_list) {
  buffer_pool_->BeginFrame();
  // TODO(Triang3l): Create the static index buffer for unindexed triangle fans.
}

void PrimitiveConverter::EndFrame() { buffer_pool_->EndFrame(); }

PrimitiveType PrimitiveConverter::GetReplacementPrimitiveType(
    PrimitiveType type) {
  if (type == PrimitiveType::kTriangleFan) {
    return PrimitiveType::kTriangleList;
  }
  return type;
}

PrimitiveConverter::ConversionResult PrimitiveConverter::ConvertPrimitives(
    PrimitiveType source_type, uint32_t address, uint32_t index_count,
    IndexFormat index_format, Endian index_endianness,
    D3D12_GPU_VIRTUAL_ADDRESS& gpu_address_out, uint32_t& index_count_out) {
  auto& regs = *register_file_;
  bool reset = (regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL].u32 & (1 << 21)) != 0;
  // Swap the reset index because we will be comparing unswapped values to it.
  uint32_t reset_index = xenos::GpuSwap(
      regs[XE_GPU_REG_VGT_MULTI_PRIM_IB_RESET_INDX].u32, index_endianness);
  // If the specified reset index is the same as the one used by Direct3D 12
  // (0xFFFF or 0xFFFFFFFF - in the pipeline cache, we use the former for
  // 16-bit and the latter for 32-bit indices), we can use the buffer directly.
  uint32_t reset_index_host =
      index_format == IndexFormat::kInt32 ? 0xFFFFFFFFu : 0xFFFFu;

  // Check if need to convert at all.
  if (source_type != PrimitiveType::kTriangleFan) {
    if (!reset || reset_index == reset_index_host) {
      return ConversionResult::kConversionNotNeeded;
    }
    if (source_type != PrimitiveType::kTriangleStrip ||
        source_type != PrimitiveType::kLineStrip) {
      return ConversionResult::kConversionNotNeeded;
    }
    // TODO(Triang3l): Write conversion for triangle and line strip reset index
    // and for indexed line loops.
    return ConversionResult::kConversionNotNeeded;
  }

  // Exit early for clearly empty draws, without even reading the memory.
  if (source_type == PrimitiveType::kTriangleFan ||
      source_type == PrimitiveType::kTriangleStrip) {
    if (index_count < 3) {
      return ConversionResult::kPrimitiveEmpty;
    }
  } else if (source_type == PrimitiveType::kLineStrip ||
             source_type == PrimitiveType::kLineLoop) {
    if (index_count < 2) {
      return ConversionResult::kPrimitiveEmpty;
    }
  }

  // TODO(Triang3l): Find the converted data in the cache.

  // Calculate the index count, and also check if there's nothing to convert in
  // the buffer (for instance, if not using primitive reset).
  uint32_t converted_index_count = 0;
  bool conversion_needed = false;
  bool simd = false;
  if (source_type == PrimitiveType::kTriangleFan) {
    // Triangle fans are not supported by Direct3D 12 at all.
    conversion_needed = true;
    if (reset) {
      // TODO(Triang3l): Triangle fans with primitive reset.
      return ConversionResult::kFailed;
    } else {
      converted_index_count = 3 * (index_count - 2);
    }
  }

  union {
    void* source;
    uint16_t* source_16;
    uint32_t* source_32;
  };
  source = memory_->TranslatePhysical(address);
  union {
    void* target;
    uint16_t* target_16;
    uint32_t* target_32;
  };
  D3D12_GPU_VIRTUAL_ADDRESS gpu_address;
  target = AllocateIndices(index_format, index_count, simd ? address & 15 : 0,
                           gpu_address);
  if (target == nullptr) {
    return ConversionResult::kFailed;
  }

  if (source_type == PrimitiveType::kTriangleFan) {
    // https://docs.microsoft.com/en-us/windows/desktop/direct3d9/triangle-fans
    // Ordered as (v1, v2, v0), (v2, v3, v0).
    if (reset) {
      // TODO(Triang3l): Triangle fans with primitive restart.
      return ConversionResult::kFailed;
    } else {
      if (index_format == IndexFormat::kInt32) {
        for (uint32_t i = 2; i < index_count; ++i) {
          *(target_32++) = source_32[i];
          *(target_32++) = source_32[i - 1];
          *(target_32++) = source_32[0];
        }
      } else {
        for (uint32_t i = 2; i < index_count; ++i) {
          *(target_16++) = source_16[i];
          *(target_16++) = source_16[i - 1];
          *(target_16++) = source_16[0];
        }
      }
    }
  }

  // TODO(Triang3l): Replace primitive reset index in triangle and line strips.
  // TODO(Triang3l): Line loops.

  gpu_address_out = gpu_address;
  index_count_out = converted_index_count;
  return ConversionResult::kConverted;
}

void* PrimitiveConverter::AllocateIndices(
    IndexFormat format, uint32_t count, uint32_t simd_offset,
    D3D12_GPU_VIRTUAL_ADDRESS& gpu_address_out) {
  if (count == 0) {
    return nullptr;
  }
  uint32_t size = count * (format == IndexFormat::kInt32 ? sizeof(uint32_t)
                                                         : sizeof(uint16_t));
  // 16-align all index data because SIMD is used to replace the reset index
  // (without that, 4-alignment would be required anyway to mix 16-bit and
  // 32-bit indices in one buffer page).
  size = xe::align(size, uint32_t(16));
  // Add some space to align SIMD register components the same way in the source
  // and the buffer.
  simd_offset &= 15;
  if (simd_offset != 0) {
    size += 16;
  }
  D3D12_GPU_VIRTUAL_ADDRESS gpu_address;
  uint8_t* mapping =
      buffer_pool_->RequestFull(size, nullptr, nullptr, &gpu_address);
  if (mapping == nullptr) {
    XELOGE("Failed to allocate space for %u converted %u-bit vertex indices",
           count, format == IndexFormat::kInt32 ? 32 : 16);
    return nullptr;
  }
  gpu_address_out = gpu_address + simd_offset;
  return mapping + simd_offset;
}

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe
