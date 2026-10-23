#pragma once
#include <algorithm>
#include <cstdint>

namespace gow3::renderer {
// Reserve bounded soft-trim space only for modules successfully admitted by
// the startup catalog. This is an immutable admission reservation, not live
// occupancy: hard pressure may still evict a module, without runtime reload.
// No cache scan, allocation, hard-limit change or candidate compile occurs here.
class NativeCatalogShaderHeadroom {
 public:
  static constexpr uint64_t kMaximumModules=128,kMaximumBytes=16u*1024*1024;
  struct Targets {uint64_t entries=0,bytes=0;};
  bool CanAdmit(uint64_t bytes) const noexcept {
    return bytes&&modules_<kMaximumModules&&bytes<=kMaximumBytes-bytes_;
  }
  bool Admit(uint64_t bytes) noexcept {
    if(!CanAdmit(bytes))return false;
    ++modules_;bytes_+=bytes;return true;
  }
  Targets SoftTargets(bool enabled,uint64_t hard_entries,uint64_t hard_bytes) const noexcept {
    // Exact floor(3*hard/4), including full-width arithmetic in the CPU fixture.
    const auto three_quarters=[](uint64_t value){return value/4*3+(value%4*3)/4;};
    Targets result{three_quarters(hard_entries),three_quarters(hard_bytes)};
    if(enabled) {
      result.entries+=std::min(modules_,hard_entries-result.entries);
      result.bytes+=std::min(bytes_,hard_bytes-result.bytes);
    }
    return result;
  }
  uint64_t modules() const noexcept {return modules_;}
  uint64_t bytes() const noexcept {return bytes_;}
 private:
  uint64_t modules_=0,bytes_=0;
};
} // namespace gow3::renderer
