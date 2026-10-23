#pragma once
#include <array>
#include <cstdint>

namespace gow3::renderer {
// Object copies 12/13 have no live planner consumer. Copy 11 is retained for
// missing-VS diagnostics unless the bridge has decoded this exact captured VS.
// This policy never applies to an active diagnostic capture or shader discovery.
struct LiveMetadataCapturePolicy {
    bool enabled=false,live=false,diagnostic_capture=false,validated_vertex_inputs=false;
    constexpr bool Omit(uint32_t kind) const noexcept {
        return enabled&&live&&!diagnostic_capture&&
            (kind==12||kind==13||(kind==11&&validated_vertex_inputs));
    }
};
struct LiveMetadataCaptureStatistics {
    uint64_t omitted_blocks=0,readable_copies_avoided=0,copy_bytes_avoided=0,unreadable_blocks=0;
    std::array<uint64_t,3> copies_by_kind{},bytes_by_kind{};
    // Probe the same virtual range as Block. Never count unreadable/empty
    // original copies as work avoided; no guest contents are read or invented.
    void Record(uint32_t kind,uint32_t bytes,bool readable) noexcept {
        if(kind<11||kind>13)return;
        ++omitted_blocks;
        if(!readable||!bytes){++unreadable_blocks;return;}
        ++readable_copies_avoided;copy_bytes_avoided+=bytes;
        ++copies_by_kind[kind-11];bytes_by_kind[kind-11]+=bytes;
    }
};
}
