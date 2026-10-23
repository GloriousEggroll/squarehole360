#pragma once
#include <array>
#include <cstdint>
#include <utility>
#include <algorithm>
#include <cstring>
#include <rex/graphics/pipeline/texture/util.h>
#include "fetch_memo.h"

namespace gow3::renderer {
// Only immutable descriptor-derived numbers. No memory contents, access rights,
// byte owners or GPU state are retained. Serialize with the bridge capture lock.
struct CaptureTextureExtents {
    uint32_t base=0,mips=0,base_size=0,mip_size=0;
    bool operator==(const CaptureTextureExtents&)const=default;
};
inline CaptureTextureExtents DescribeCaptureTextureExtents(const std::array<uint32_t,6>& words) {
  using namespace rex::graphics;
  xenos::xe_gpu_texture_fetch_t fetch{};
  static_assert(sizeof(fetch)==sizeof(words));
  std::memcpy(&fetch,words.data(),sizeof(fetch));
  uint32_t base=0,mips=0,base_size=0,mip_size=0;
      const uint32_t dimension = uint32_t(fetch.dimension);
      // Reject invalid fetches before calling layout helpers with asserted
      // preconditions. CPU snapshots cannot stand in for GPU-only resolves.
      const bool valid = (dimension != 0 || (!fetch.tiled && !fetch.packed_mips && !fetch.stacked)) &&
                         (dimension != 2 || !fetch.stacked) &&
                         (dimension != 3 || (!fetch.stacked && fetch.size_2d.stack_depth == 5));
      const auto* format = FormatInfo::Get(fetch.format);
      if (valid && format->bytes_per_block() && format->block_width && format->block_height) {
        uint32_t w, h, d, min_mip, max_mip;
        texture_util::GetSubresourcesFromFetchConstant(fetch, &w, &h, &d, &base, &mips, &min_mip, &max_mip);
        // A diagnostic must not overflow the layout helper or attempt giant
        // physical reads when a stale binding is encountered.
        const uint64_t bound = uint64_t(std::max((w + 32) / 32, uint32_t(fetch.pitch))) * 32 *
                               ((h + 32) / 32) * 32 * (d + 1) * format->bytes_per_block();
        if (w < 8192 && h < 8192 && d < 64 && bound <= 128 * 1024 * 1024) {
          const auto layout = texture_util::GetGuestTextureLayout(fetch.dimension, fetch.pitch,
              w + 1, h + 1, d + 1, fetch.tiled, fetch.format, fetch.packed_mips, base != 0, max_mip);
          base_size = base ? layout.base.level_data_extent_bytes : 0;
          mip_size = mips ? layout.mips_total_extent_bytes : 0;
        }
      }
  return {base,mips,base_size,mip_size};
}
class CaptureTextureExtentCache {
public:
    using Fetch=std::array<uint32_t,6>;
    using Statistics=FetchMemo<CaptureTextureExtents>::Statistics;
    // Keyed by the exact fetch words; the slot number is not part of the key.
    template<class Describe>
    CaptureTextureExtents Get(uint32_t,const Fetch& fetch,Describe&& describe) {
        return memo_.Get(fetch,std::forward<Describe>(describe));
    }
    Statistics statistics()const noexcept{return memo_.statistics();}
    void Clear()noexcept{memo_.Clear();}
private:
    FetchMemo<CaptureTextureExtents> memo_;
};
}
