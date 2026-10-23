#include "lazy_device_state.h"
#include "content_hash.h"

namespace gow3::renderer {
std::string_view LazyDeviceState::Digest()const {
    std::call_once(once_,[&]{digest_=ContentHash(*bytes_);materialized_.store(true,std::memory_order_release);});
    return digest_;
}
} // namespace gow3::renderer
