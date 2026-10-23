#include "renderer/game_profile.h"
#include "live_plan.h"
#include "texture_data.h"
#include "footprint_ranges.h"
#include "memory_export_copy.h"
#include "resolve_state.h"
#include "shader_bindings.h"
#include "shader_byte_segments.h"
#include "fast_shader_fingerprint.h"
#include "shader_requirement_masks.h"
#include "fetch_memo.h"
#include "renderer/native/content_hash.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <xxhash.h>

namespace gow3::renderer {
#ifdef GOW3_NATIVE_VERIFY_PREFIX_DEPENDENCIES
namespace test {
size_t live_prefix_dependency_checks=0;
size_t live_flat_prefixes=0,live_map_prefixes=0;
size_t live_shader_fingerprint_mask=~size_t{0};
size_t live_payload_fingerprint_mask=~size_t{0};
size_t live_shader_snapshot_allocations=0;
size_t live_state_fingerprint_mask=~size_t{0};
size_t live_snapshot_bytes_limit=512u*1024*1024;
}
#endif
namespace {
using Json = nlohmann::json;
using Bytes = std::vector<uint8_t>;
using Ranges = std::vector<std::pair<uint32_t, uint32_t>>;
size_t SnapshotLimit() {
#ifdef GOW3_NATIVE_VERIFY_PREFIX_DEPENDENCIES
    return test::live_snapshot_bytes_limit;
#else
    return 512u*1024*1024;
#endif
}
constexpr uint32_t kPresent = profile::d3d::kPresent;
constexpr const char* kRectangle = "9d8adccbaa369d9fa2106a03a54a997ec417e12fcbdb4bed7e67c36dfadf8a01";
uint32_t Word(std::span<const uint8_t> bytes, size_t offset) {
    if (offset + 4 > bytes.size()) throw std::runtime_error("Truncated live D3D state");
    return uint32_t(bytes[offset]) << 24 | uint32_t(bytes[offset+1]) << 16 |
           uint32_t(bytes[offset+2]) << 8 | bytes[offset+3];
}
float Float(std::span<const uint8_t> bytes, size_t offset) {
    return std::bit_cast<float>(Word(bytes, offset));
}
std::string Hex(uint32_t word) {
    char text[9]; std::snprintf(text, sizeof(text), "%08x", word); return text;
}
uint32_t Physical(uint32_t address) {
    return (address & 0x1fffffffu) + (((address >> 20) + 0x200) & 0x1000);
}
std::array<uint32_t, 6> Fetch(std::span<const uint8_t> data, size_t offset) {
    std::array<uint32_t, 6> result;
    for (unsigned i = 0; i < 6; ++i) result[i] = Word(data, offset + i*4);
    return result;
}
TypedTextureIdentity TypedIdentity(std::array<uint32_t,6> f,bool resource=false) {
    if(resource)f[1]=(f[1]&4095)|Physical(f[1]&~4095u);
    if(((f[5]>>9)&3)!=1)throw std::runtime_error("Native resolve requires a 2D resource");
    auto format=f[1]&63;
    switch(format){case 27:format=30;break;case 28:format=31;break;case 29:format=32;break;
        case 54:format=7;break;case 55:format=16;break;case 56:format=17;break;}
    return {f[1]&~4095u,format,(f[2]&8191)+1,((f[2]>>13)&8191)+1,
        ((f[0]>>22)&511)*32,(f[1]>>6)&3,1,(f[1]&1024)?(f[2]>>26)+1:1,
        bool(f[0]>>31),bool(f[5]&2048)};
}
Json Identity(std::array<uint32_t,6> f,bool resource=false) {return ToJson(TypedIdentity(f,resource));}
std::array<int32_t, 4> Rectangle(std::span<const uint8_t> bytes) {
    if (bytes.size() != 16) throw std::runtime_error("Invalid live rectangle");
    return {int32_t(Word(bytes,0)), int32_t(Word(bytes,4)), int32_t(Word(bytes,8)), int32_t(Word(bytes,12))};
}
const char* FunctionName(uint32_t function) {
    switch (function) {
        case profile::d3d::kDrawVertices: return "DrawPrimitive";
        case profile::d3d::kDrawIndexedVertices: return "DrawIndexedPrimitive";
        case profile::d3d::kBeginVertices: return "BeginVertices";
        case profile::d3d::kBeginIndexedVertices: return "BeginIndexedVertices";
        case profile::d3d::kClearRect: return "ClearRect";
        case profile::d3d::kResolve: return "Resolve";
        case kPresent: return "Present";
        case profile::d3d::kOcclusionQueryReport: return "OcclusionQuery";
        default: return nullptr;
    }
}

// Exact base-level touched bytes, including packed/stacked views and endian
// sub-dword aliases. Adjacent bytes are merged before residency comparisons.
Ranges Footprint(std::array<uint32_t, 6> fetch, std::optional<std::array<int32_t, 4>> rectangle = {}) {
    // Resolve residency is base-only regardless of the sampler's mip range.
    fetch[4] &= ~((15u << 2) | (15u << 6));
    auto layout=DescribeTexture(fetch);
    const auto r=rectangle.value_or(std::array<int32_t,4>{0,0,int32_t(layout.width),int32_t(layout.height)});
    if (r[0]<0 || r[1]<0 || r[2]<=r[0] || r[3]<=r[1] || uint32_t(r[2])>layout.width || uint32_t(r[3])>layout.height)
        throw std::runtime_error("Invalid live resolve rectangle");
    Ranges ranges;
    const auto bw=layout.block.block_width, bh=layout.block.block_height, bytes=layout.block.bytes;
    const unsigned endian_xor[]={0,1,3,2};
    for (const auto& level : layout.levels) {
        if (level.mip || level.mips) continue;
        const auto append_row = [&](uint32_t first_x,uint32_t last_x,uint32_t y) {
            for (uint32_t x=first_x; x<last_x; ++x) {
                uint64_t offset=uint64_t(layout.base_address)+TextureBlockOffset(layout,level,x,y,0);
                if (bytes<4) offset^=endian_xor[layout.endian]&~(bytes-1u);
                if (offset+bytes>512u*1024*1024) throw std::runtime_error("Live texture exceeds physical memory");
                AppendFootprintRange(ranges,uint32_t(offset),uint32_t(offset+bytes));
            }
        };
        // Narrow admission: ordinary 2D, one unpacked/unstacked base level,
        // uncompressed four-byte blocks, and bounded aligned storage. Keep
        // every unsupported layout on the original point enumeration below.
        const bool tiled_base_4=layout.tiled && layout.dimension==1 &&
            bw==1 && bh==1 && bytes==4 && layout.depth==1 && layout.layers==1 &&
            layout.levels.size()==1 && !layout.min_mip && !layout.max_mip &&
            !(fetch[1]&1024u) && !(fetch[5]&2048u) &&
            !level.layer && !level.x && !level.y && !level.z && !level.offset &&
            level.depth==1 && level.pitch_blocks && level.pitch_blocks<=16352 &&
            !(level.pitch_blocks&31u) && layout.width<=8192 && layout.height<=8192;
        if(tiled_base_4) {
            const uint32_t left=uint32_t(r[0]),top=uint32_t(r[1]);
            const uint32_t right=uint32_t(r[2]),bottom=uint32_t(r[3]);
            const uint32_t tile_left=(left+31u)&~31u,tile_right=right&~31u;
            const uint32_t tile_top=(top+15u)&~15u,tile_bottom=bottom&~15u;
            if(tile_left<tile_right && tile_top<tile_bottom) {
                for(uint32_t y=top;y<bottom;++y) {
                    if(y<tile_top || y>=tile_bottom) {
                        append_row(left,right,y);
                        continue;
                    }
                    append_row(left,tile_left,y);
                    if(!(y&15u))for(uint32_t x=tile_left;x<tile_right;x+=32) {
                        // Tiled2D at an aligned 32x16 origin is the minimum
                        // of exactly 512 four-byte blocks. Its address-bit
                        // permutation covers this entire 2048-byte interval
                        // bijectively, including either half of a 32x32 tile.
                        const uint64_t first=uint64_t(layout.base_address)+TextureBlockOffset(layout,level,x,y,0);
                        if(first+2048u>512u*1024*1024) throw std::runtime_error("Live texture exceeds physical memory");
                        AppendFootprintRange(ranges,uint32_t(first),uint32_t(first+2048u));
                    }
                    append_row(tile_right,right,y);
                }
                continue;
            }
        }
        for (uint32_t y=uint32_t(r[1])/bh; y<(uint32_t(r[3])+bh-1)/bh; ++y)
            append_row(uint32_t(r[0])/bw,(uint32_t(r[2])+bw-1)/bw,y);
    }
    std::sort(ranges.begin(),ranges.end());
    Ranges merged;
    for (auto [first,last] : ranges) {
        if (!merged.empty() && first<=merged.back().second) merged.back().second=std::max(last,merged.back().second);
        else merged.emplace_back(first,last);
    }
    return merged;
}
// GPU byte ownership in API order. The planner and the guest-thread replica
// used by the asynchronous planner run exactly this code on the same inputs.
struct OwnershipRanges {
    std::map<uint32_t,uint32_t> gpu_ranges;
    std::function<void(uint32_t,uint32_t)> watch_memory;
    using FootprintKey=std::pair<std::array<uint32_t,6>,std::optional<std::array<int32_t,4>>>;
    std::map<FootprintKey,Ranges> texture_footprints;
    FetchMemo<bool> gpu_only_shapes;
    void Mark(const Ranges& ranges) {
        for (auto [first,last] : ranges) {
            if(watch_memory)watch_memory(first,last-first);
            auto it=gpu_ranges.lower_bound(first);
            if (it!=gpu_ranges.begin()) {
                auto previous=std::prev(it);
                if (previous->second>=first) { first=previous->first; last=std::max(last,previous->second); it=gpu_ranges.erase(previous); }
            }
            while (it!=gpu_ranges.end() && it->first<=last) { last=std::max(last,it->second); it=gpu_ranges.erase(it); }
            gpu_ranges.emplace(first,last);
        }
    }
    void Invalidate(uint32_t address,uint32_t bytes) {
        auto& ranges=gpu_ranges;const uint32_t last=address+bytes;
        auto it=ranges.lower_bound(address);
        if(it!=ranges.begin()&&std::prev(it)->second>address)--it;
        while(it!=ranges.end()&&it->first<last) {
            const auto [first,end]=*it;it=ranges.erase(it);
            if(first<address)ranges.emplace(first,address);
            if(end>last){ranges.emplace(last,end);break;}
        }
    }
    const Ranges& TextureFootprint(const std::array<uint32_t,6>& fetch,
                                  std::optional<std::array<int32_t,4>> rectangle={}) {
        const FootprintKey key{fetch,rectangle};
        auto it=texture_footprints.find(key);
        if (it==texture_footprints.end()) {
            if (texture_footprints.size()>=2048) texture_footprints.clear();
            it=texture_footprints.emplace(key,Footprint(fetch,rectangle)).first;
        }
        return it->second;
    }
    bool GPUOnlyTexture(const std::array<uint32_t,6>& fetch) {
        if (gpu_ranges.empty()) return false;
        // The GPU-only image loader currently handles complete uncompressed
        // base levels. Mixed CPU/GPU bytes or sampled mip chains must retain
        // their CPU payloads so the executor can overlay exact defined GPU
        // blocks without fabricating the remaining texels.
        const bool shape=gpu_only_shapes.Get(fetch,[](const auto& f) {
            const auto layout=DescribeTexture(f);
            return !(layout.dimension!=1||layout.min_mip||layout.max_mip||
                layout.block.block_width!=1||layout.block.block_height!=1||layout.levels.size()!=layout.layers);
        });
        if(!shape)return false;
        const auto& footprint=TextureFootprint(fetch);
        for (const auto& [first,last] : footprint) {
            auto next=gpu_ranges.upper_bound(first);
            if(next==gpu_ranges.begin()||std::prev(next)->second<last)return false;
        }
        return !footprint.empty();
    }
    bool GPUBytes(uint32_t first,uint32_t count) const {
        const uint64_t last=uint64_t(first)+count;
        auto it=gpu_ranges.lower_bound(first);
        return (it!=gpu_ranges.end()&&it->first<last)||
            (it!=gpu_ranges.begin()&&std::prev(it)->second>first);
    }
    Ranges GPUByteRanges(uint32_t first,uint32_t count) const {
        Ranges result;const uint32_t last=first+count;
        auto it=gpu_ranges.lower_bound(first);
        if(it!=gpu_ranges.begin()&&std::prev(it)->second>first)--it;
        for(;it!=gpu_ranges.end()&&it->first<last;++it)
            result.emplace_back(std::max(first,it->first),std::min(last,it->second));
        return result;
    }
};
// The resolve destination's base-level bytes, exactly as Push marks them.
const Ranges& ResolveDestinationFootprint(OwnershipRanges& ownership,std::span<const uint8_t> header,
        std::span<const uint8_t> source_rectangle,std::span<const uint8_t> destination_xy) {
    auto fetch=Fetch(header,28);const auto identity=TypedIdentity(fetch,true);
    const auto rect=source_rectangle.empty()?std::array<int32_t,4>{0,0,int32_t(identity.width),int32_t(identity.height)}:
        Rectangle(source_rectangle);
    const int32_t x=destination_xy.empty()?0:int32_t(Word(destination_xy,0)),
                  y=destination_xy.empty()?0:int32_t(Word(destination_xy,4));
    const std::array<int32_t,4> dest{x,y,x+rect[2]-rect[0],y+rect[3]-rect[1]};
    fetch[1]=(fetch[1]&4095)|Physical(fetch[1]&~4095u);
    return ownership.TextureFootprint(fetch,dest);
}
} // namespace

struct GearsLivePlan::State {
    std::shared_ptr<LiveRenderFrame> frame;
    bool query_prefixes=false,lazy_states=false,direct_resource_refs=false,fast_fingerprints=false;
    std::atomic<uint64_t> direct_resource_handoffs{0};
    std::atomic<uint64_t> device_state_collisions{0},device_state_calls{0},device_state_current_hits{0},device_state_previous_hits{0},device_state_created{0};
    std::atomic<uint8_t> lazy_state_mode{0};
    std::array<std::array<std::atomic<uint64_t>,4>,4> payload_calls{},payload_bytes{};
    unsigned payload_class=3;
    // Push is single-threaded, so these statistics have one writer. A plain
    // relaxed load/store avoids a locked RMW per payload; readers on other
    // threads still see whole values.
    static void Count(std::atomic<uint64_t>& counter,uint64_t amount=1) {
        counter.store(counter.load(std::memory_order_relaxed)+amount,std::memory_order_relaxed);
    }
    void CountPayload(unsigned outcome,size_t bytes) {
        Count(payload_calls[payload_class][outcome]);
        Count(payload_bytes[payload_class][outcome],bytes);
    }
    std::shared_ptr<LazyPayloadTable> previous_lazy_payloads;
    using StateIndex=std::unordered_multimap<uint64_t,LazyDeviceStateRef>;
    StateIndex lazy_states_current,lazy_states_previous;
    struct SameSizeResource {LiveResourceBytes bytes;bool shader;};
    using SameSizeIndex=std::unordered_multimap<uint64_t,SameSizeResource>;
    SameSizeIndex same_size_current,same_size_previous;
    size_t prefix_operation=0;
    std::optional<LiveQueryPrefix> ready_prefix;
    std::optional<LivePacketPrefix> ready_packet_prefix;
    std::optional<GearsLivePlan::MemoryCopy> ready_memory_copy;
    // API sequence -> operation end. Sequences arrive in increasing order, so
    // this stays sorted by appending; an out-of-order event is inserted.
    std::vector<std::pair<uint64_t,size_t>> operation_ends;
    void RecordOperationEnd(uint64_t sequence,size_t end) {
        if(operation_ends.empty()||operation_ends.back().first<sequence) {operation_ends.emplace_back(sequence,end);return;}
        const auto it=std::lower_bound(operation_ends.begin(),operation_ends.end(),std::pair<uint64_t,size_t>{sequence,0});
        if(it!=operation_ends.end()&&it->first==sequence)it->second=end;
        else operation_ends.insert(it,{sequence,end});
    }
    uint64_t query_generation=0;
    struct ActiveQuery {uint32_t handle,address;uint64_t generation;};
    std::optional<ActiveQuery> active_query;
    std::optional<uint32_t> previous_frame;
    OwnershipRanges ownership;
    struct CpuWrite {uint32_t address,bytes;std::vector<uint8_t> data;uint64_t coherence_epoch;};
    std::vector<CpuWrite> cpu_writes;
    std::unordered_map<std::string,ShaderRequirementMasks> requirements;
    // The digest is the persistent payload identity, but unchanged bindings
    // recur hundreds of times per frame. Check a cheap fingerprint and exact
    // bytes before recomputing SHA-256. Never trust a fingerprint collision or
    // a reused guest address as proof that an immutable snapshot is unchanged.
    std::array<std::unordered_multimap<size_t,std::string>,2> fingerprints;
    // Keep at most the previous frame's immutable payloads. Static textures
    // need not be hashed with SHA-256 again each frame; content is still
    // compared in full, including after CPU writes and address reuse.
    std::array<decltype(LiveRenderFrame::payloads),2> previous_snapshots;
    std::array<std::unordered_multimap<size_t,std::string>,2> previous_fingerprints;
    std::unordered_map<const Bytes*,std::pair<std::weak_ptr<const Bytes>,std::string>> snapshot_identities;
    // Draw state changes almost every call. Its canonical digest is cheaper
    // than a failed full-buffer fingerprint followed by that same digest.
    // Mark its owners so generic payloads retain previous-frame sharing even
    // though these states do not enter the generic fingerprint indexes.
    std::unordered_set<const Bytes*> device_states,previous_device_states;
    std::weak_ptr<const Bytes> last_device_state;
    std::string last_device_state_digest;

    // Only hash-valued operation fields can name a payload. Record them while
    // constructing the operation rather than walking all of its JSON at every
    // packet boundary. Every key is anchored in that operation's immutable
    // digest storage, so a detached prefix may borrow it while retaining only
    // its own LiveOperations slice. Mutable producer map keys never escape.
    struct DependencyRef {
        std::string_view digest;
        LazyDeviceStateRef state;
        std::string_view Key()const{return state?state->Digest():digest;}
    };
    std::vector<DependencyRef> operation_dependencies;
    std::vector<size_t> operation_dependency_ends;
    struct PendingDependencies {
        State& state;
        ~PendingDependencies() {
            // A failed operation may have interned inputs before validation
            // failed. Keep dependencies only for successfully emitted ops.
            state.operation_dependencies.resize(state.operation_dependency_ends.empty()?0:
                state.operation_dependency_ends.back());
        }
    };
    void Dependency(std::string_view digest) {
        operation_dependencies.push_back({digest,{}});
    }
    void AssignDependency(Json& field,std::string digest) {
        field=std::move(digest);
        Dependency(field.get_ref<const std::string&>());
    }
    void Emit(Json operation,const char* diagnostic_hash_field=nullptr) {
        if(diagnostic_hash_field)
            Dependency(operation.at(diagnostic_hash_field).get_ref<const std::string&>());
        // Moving JSON transfers its owned string storage. References recorded
        // above remain valid until the complete frame releases the operation.
        frame->operations.push_back(std::move(operation));
        operation_dependency_ends.push_back(operation_dependencies.size());
#ifdef GOW3_NATIVE_VERIFY_PREFIX_DEPENDENCIES
        // Captures need not contain command spans. Check every emitted op too,
        // without repeatedly constructing an ever-growing full-frame prefix.
        Prefix(frame->operations.size()-1,frame->operations.size());
#endif
    }

    CanonicalResourceRef Resource(std::string_view digest,bool shader=false) const {
        const auto& values=shader?frame->shaders:frame->payloads;
        const auto found=values.find(digest);
        if(found==values.end())throw std::out_of_range("unordered_map::at");
        return {CanonicalDigest(digest),found->second};
    }
    void Emit(TypedLiveQuery operation) {
        // A query control references no payload/shader digest.
        frame->operations.push_back(std::move(operation));
        operation_dependency_ends.push_back(operation_dependencies.size());
#ifdef GOW3_NATIVE_VERIFY_PREFIX_DEPENDENCIES
        Prefix(frame->operations.size()-1,frame->operations.size());
#endif
    }
    void Emit(TypedLiveDraw operation) {
        // Inline digests cannot be borrowed until their envelope has reached
        // its final heap address. Prefixes own only the selected envelopes.
        const size_t needed=operation_dependencies.size()+3+operation.geometry.size()+
            operation.up_geometry.size()+operation.textures.size()*2;
        if(needed>operation_dependencies.capacity())
            operation_dependencies.reserve(std::max(needed,operation_dependencies.capacity()*2));
        frame->operations.push_back(std::move(operation));
        const auto& draw=*frame->operations.back().Draw();
        if(draw.lazy_device_state)operation_dependencies.push_back({{},draw.lazy_device_state});
        else Dependency(draw.device_state.hash.View());
        for(const auto* shader:{&draw.vertex_shader,&draw.pixel_shader})
            if(*shader)Dependency((*shader)->hash.View());
        for(const auto* geometry:{&draw.geometry,&draw.up_geometry})
            for(const auto& item:*geometry)Dependency(item.payload.hash.View());
        for(const auto& texture:draw.textures) {
            if(texture.base_payload)Dependency(texture.base_payload->hash.View());
            if(texture.mip_payload)Dependency(texture.mip_payload->hash.View());
        }
        operation_dependency_ends.push_back(operation_dependencies.size());
#ifdef GOW3_NATIVE_VERIFY_PREFIX_DEPENDENCIES
        Prefix(frame->operations.size()-1,frame->operations.size());
#endif
    }

    std::shared_ptr<LiveRenderFrame> Prefix(size_t first,size_t end,size_t limit=LivePrefixResources::kMaximumDependencies) {
        if(!frame||end<first||end>frame->operations.size()||
           operation_dependency_ends.size()!=frame->operations.size())
            throw std::runtime_error("Invalid live packet operation range");
        auto batch=std::make_shared<LiveRenderFrame>();batch->frame=frame->frame;
        batch->operations=frame->operations.Slice(first,end);
        const auto dependency_first=first?operation_dependency_ends[first-1]:0;
        const auto dependency_end=end?operation_dependency_ends[end-1]:0;
        const size_t dependencies=dependency_end-dependency_first;
        // A wide prefix needs canonical map storage only for its own detached
        // slice. Keep the producer lazy: later bounded prefixes and frames
        // must not inherit this storage choice as a permanent identity mode.
        const bool lazy_prefix=lazy_states&&dependencies<=limit&&
            std::any_of(operation_dependencies.begin()+dependency_first,
            operation_dependencies.begin()+dependency_end,[](const auto& dependency){return bool(dependency.state);});
        if(lazy_prefix)batch->lazy_payloads=std::make_shared<LazyPayloadTable>(dependencies,true);
        if(dependencies<=limit)
            batch->prefix_resources.emplace(dependencies,limit);
#ifdef GOW3_NATIVE_VERIFY_PREFIX_DEPENDENCIES
        if(batch->prefix_resources)++test::live_flat_prefixes;else ++test::live_map_prefixes;
        LiveResourceMap old_payloads,old_shaders;
        const auto oracle_payloads=frame->lazy_payloads?frame->lazy_payloads->DiagnosticSnapshot():frame->payloads;
#endif
        for(size_t i=dependency_first;i<dependency_end;++i) {
            const auto& dependency=operation_dependencies[i];
            if(lazy_prefix&&dependency.state) {
                batch->lazy_payloads->Append(dependency.state);
#ifdef GOW3_NATIVE_VERIFY_PREFIX_DEPENDENCIES
                old_payloads.emplace(dependency.state->Digest(),dependency.state->Owner());
#endif
                continue;
            }
            const auto key=dependency.Key();
            // Preserve the legacy collector's two namespaces, including an
            // identical blob interned in the other namespace after this op.
            // A diagnostic memory-copy shader hash can also acquire a match
            // only later, before a detached prefix is requested.
            for(bool shader:{false,true}) {
                const auto& from=shader?frame->shaders:frame->payloads;
                const auto it=from.find(key);
                const auto* owner=it==from.end()?nullptr:&it->second;
                // Lazy states intentionally aren't in frame->payloads. A
                // canonical wide prefix owns their exact digest/byte owner
                // directly, while preserving an existing canonical entry's
                // precedence after a genuine cross-kind alias fallback.
                if(!owner&&!shader&&dependency.state)owner=&dependency.state->Owner();
                if(!owner)continue;
#ifdef GOW3_NATIVE_VERIFY_PREFIX_DEPENDENCIES
                (shader?old_shaders:old_payloads).emplace(key,*owner);
#endif
                if(lazy_prefix&&!shader)batch->lazy_payloads->Append(key,*owner);
                else if(batch->prefix_resources)batch->prefix_resources->Append(key,*owner,shader);
                else {
                    auto& to=shader?batch->shaders:batch->payloads;
                    if(to.emplace(key,*owner).second)batch->retained_bytes+=(*owner)->size();
                }
            }
        }
        if(batch->prefix_resources)batch->retained_bytes=batch->prefix_resources->Seal();
        if(batch->lazy_payloads) {batch->lazy_payloads->Seal();batch->retained_bytes+=batch->lazy_payloads->retained_bytes();}
#ifdef GOW3_NATIVE_VERIFY_PREFIX_DEPENDENCIES
        for(bool shader:{false,true}) {
            const auto& expected=shader?old_shaders:old_payloads;
            const auto actual=batch->MaterializeResources(shader);
            if(actual.bucket_count()!=expected.bucket_count()||actual.size()!=expected.size()||
               !std::equal(actual.begin(),actual.end(),expected.begin()))
                throw std::runtime_error("Flat prefix changed legacy diagnostic iteration order");
        }
        LiveRenderFrame legacy;legacy.frame=frame->frame;
        legacy.operations=frame->operations.Slice(first,end);
        auto retain=[&](auto&& self,const Json& value)->void {
            if(value.is_string()) {
                const auto& key=value.get_ref<const std::string&>();
                for(bool shader:{false,true}) {
                    const auto& from=shader?frame->shaders:oracle_payloads;
                    auto& to=shader?legacy.shaders:legacy.payloads;
                    const auto it=from.find(key);
                    if(it!=from.end()&&to.emplace(key,it->second).second)legacy.retained_bytes+=it->second->size();
                }
            } else if(value.is_structured())for(const auto& child:value)self(self,child);
        };
        for(const auto& operation:legacy.operations)retain(retain,operation.ToJson());
        // Identical immutable owners prove the keys and contents are exact,
        // without redundantly comparing hundreds of MiB of identical storage.
        bool resources_equal=true;
        for(bool shader:{false,true}) {
            const auto& expected=shader?legacy.shaders:legacy.payloads;
            resources_equal&=batch->ResourceCount(shader)==expected.size();
            for(const auto& [hash,owner]:expected) {
                const auto* actual=batch->FindResource(hash,shader);
                resources_equal&=actual&&*actual==owner;
            }
        }
        if(batch->frame!=legacy.frame||!resources_equal||
           batch->retained_bytes!=legacy.retained_bytes||batch->operations.size()!=legacy.operations.size())
            throw std::runtime_error("Recorded live prefix dependencies differ from legacy collector");
        for(size_t i=0;i<legacy.operations.size();++i)
            if(&batch->operations[i]!=&legacy.operations[i]||batch->operations[i].ToJson()!=legacy.operations[i].ToJson())
                throw std::runtime_error("Recorded live prefix changed operation identity or contents");
        ++test::live_prefix_dependency_checks;
#endif
        return batch;
    }
    std::shared_ptr<LiveRenderFrame> Prefix(size_t end) {return Prefix(prefix_operation,end);}
    void PacketPrefix(GraphicsCommandSpan span) {
        if(ready_packet_prefix)throw std::runtime_error("Live packet prefix was not consumed");
        const auto it=std::lower_bound(operation_ends.begin(),operation_ends.end(),std::pair<uint64_t,size_t>{span.api_sequence,0});
        if(it==operation_ends.end()||it->first!=span.api_sequence)throw std::runtime_error("Packet span has no matching API operation");
        const size_t end=std::max(prefix_operation,it->second);
        ready_packet_prefix=LivePacketPrefix{Prefix(end),prefix_operation,end,std::move(span)};
        prefix_operation=end;
    }

    void RecordPayload(const LiveResourceMap::value_type& value,bool shader) {
        if(!shader&&lazy_states)frame->lazy_payloads->Append(value.first,value.second,true);
    }
    void StopLazyStates(uint8_t reason) {
        if(!lazy_states)return;
        // Consumers own sealed detached tables. Canonicalizing these producer
        // banks cannot alter an already published prefix or its entry set.
        if(frame&&frame->lazy_payloads) {
            frame->lazy_payloads->Seal();frame->payloads=frame->lazy_payloads->Canonical();
            frame->lazy_payloads.reset();
        }
        if(previous_lazy_payloads)previous_snapshots[0]=previous_lazy_payloads->Canonical();
        previous_lazy_payloads.reset();
        lazy_states_current.clear();lazy_states_previous.clear();
        same_size_current.clear();same_size_previous.clear();
        if(const auto last=last_device_state.lock())last_device_state_digest=ContentHash(*last);
        lazy_states=false;lazy_state_mode.store(reason,std::memory_order_relaxed);
    }
    uint64_t StateFingerprint(std::span<const uint8_t> bytes)const {
        // Index key only: every candidate is confirmed by a full compare.
        // Captures change ~4 of 216 64-byte chunks per draw, nearly always in
        // the command words and the 0x480..0x880 render-state shadow, so
        // hashing those 1088 bytes keeps buckets near-unique (<=4 distinct
        // states per key on both reference captures) at 8% of the hash cost.
        auto fingerprint=bytes.size()==LazyDeviceState::kBytes
            ? XXH3_64bits_withSeed(bytes.data()+0x480,0x400,XXH3_64bits(bytes.data(),0x40))
            : XXH3_64bits(bytes.data(),bytes.size());
#ifdef GOW3_NATIVE_VERIFY_PREFIX_DEPENDENCIES
        fingerprint&=test::live_state_fingerprint_mask;
#endif
        return fingerprint;
    }
    bool MatchesLazyState(std::span<const uint8_t> bytes,uint64_t fingerprint)const {
        for(const auto* index:{&lazy_states_current,&lazy_states_previous}) {
            const auto [first,last]=index->equal_range(fingerprint);
            for(auto it=first;it!=last;++it)
                if(std::equal(bytes.begin(),bytes.end(),it->second->Owner()->begin()))return true;
        }
        return false;
    }
    void TrackSameSizeResource(uint64_t fingerprint,const LiveResourceBytes& bytes,bool shader) {
        if(!lazy_states)return;
        const auto [first,last]=same_size_current.equal_range(fingerprint);
        for(auto it=first;it!=last;++it)if(it->second.bytes==bytes&&it->second.shader==shader)return;
        // Owners already belong to the charged current canonical bank; this
        // bounded lookup index never extends retention beyond two frames.
        same_size_current.emplace(fingerprint,SameSizeResource{bytes,shader});
    }
    LazyDeviceStateRef InternLazyDeviceState(GraphicsBoundaryBlock& block) {
        const auto bytes=block.data();
        if(bytes.size()!=LazyDeviceState::kBytes)throw std::runtime_error("Missing live device state");
        const auto fingerprint=StateFingerprint(bytes);
        for(const auto* index:{&same_size_current,&same_size_previous}) {
            const auto [first,last]=index->equal_range(fingerprint);
            for(auto it=first;it!=last;++it)if(std::equal(bytes.begin(),bytes.end(),it->second.bytes->begin())) {
                const auto reason=it->second.shader?3:2;
                StopLazyStates(reason);return {};
            }
        }
        const auto find=[&](const StateIndex& index)->LazyDeviceStateRef {
            const auto [first,last]=index.equal_range(fingerprint);
            for(auto it=first;it!=last;++it) {
                if(std::equal(bytes.begin(),bytes.end(),it->second->Owner()->begin()))return it->second;
                Count(device_state_collisions);
            }
            return {};
        };
        Count(device_state_calls);
        if(auto current=find(lazy_states_current)) {
            Count(device_state_current_hits);last_device_state=current->Owner();return current;
        }
        if(frame->retained_bytes+bytes.size()>SnapshotLimit())
            throw std::runtime_error("Live frame exceeds its snapshot memory limit");
        auto state=find(lazy_states_previous);
        if(state)Count(device_state_previous_hits);
        else Count(device_state_created);
        if(!state) {
            auto owner=block.shared_bytes;
            if(!owner) {owner=DeviceStateBufferPool::Adopt(std::move(block.bytes));block.shared_bytes=owner;}
            state=std::make_shared<const LazyDeviceState>(std::move(owner));
        }
        device_states.insert(state->Owner().get());
        const auto inserted=lazy_states_current.emplace(fingerprint,state);
        try {frame->lazy_payloads->Append(state,true);}
        catch(...) {lazy_states_current.erase(inserted);throw;}
        frame->retained_bytes+=bytes.size();
        last_device_state=state->Owner();return state;
    }

    std::string Intern(std::span<const uint8_t> bytes, bool shader=false,std::shared_ptr<const Bytes> snapshot={}) {
        const bool same_size=lazy_states&&bytes.size()==LazyDeviceState::kBytes;
        uint64_t state_fingerprint=0;
        if(same_size) {
            state_fingerprint=StateFingerprint(bytes);
            if(MatchesLazyState(bytes,state_fingerprint))StopLazyStates(shader?3:2);
        }
        auto digest=InternCanonical(bytes,shader,std::move(snapshot));
        if(same_size&&lazy_states)TrackSameSizeResource(state_fingerprint,
            (shader?frame->shaders:frame->payloads).at(digest),shader);
        return digest;
    }
    const LiveResourceMap::value_type& InternCanonicalEntry(std::span<const uint8_t> bytes,bool shader,std::shared_ptr<const Bytes> snapshot) {
        auto& values=shader?frame->shaders:frame->payloads;
        auto& index=fingerprints[shader?1:0];
        if(snapshot) {
            if(shader||bytes.data()!=snapshot->data()||bytes.size()!=snapshot->size())
                throw std::runtime_error("Invalid shared physical snapshot");
            const auto found=snapshot_identities.find(snapshot.get());
            // Same control block as the live snapshot: the identity is current
            // (a reused address has an expired, different block). No lock().
            const auto same_owner=[&](const auto& weak) {
                return !weak.owner_before(snapshot)&&!snapshot.owner_before(weak);
            };
            if(found!=snapshot_identities.end()&&same_owner(found->second.first)) {
                const auto& digest=found->second.second;
                auto stored=values.find(digest);
                if(stored==values.end()) {
                    if(frame->retained_bytes+bytes.size()>SnapshotLimit())
                        throw std::runtime_error("Live frame exceeds its snapshot memory limit");
                    stored=values.emplace(digest,snapshot).first;frame->retained_bytes+=bytes.size();
                    RecordPayload(*stored,shader);
                }
                if(!shader)CountPayload(0,bytes.size());
                return *stored;
            }
        }
        // This is only a bucket hint. Every candidate still requires the
        // complete immutable byte comparison below; persistent identities stay SHA-256.
        auto fingerprint=fast_fingerprints?size_t(XXH3_64bits(bytes.data(),bytes.size())):
            std::hash<std::string_view>{}({reinterpret_cast<const char*>(bytes.data()),bytes.size()});
#ifdef GOW3_NATIVE_VERIFY_PREFIX_DEPENDENCIES
        fingerprint&=test::live_payload_fingerprint_mask;
#endif
        const auto [first,last]=index.equal_range(fingerprint);
        for(auto it=first;it!=last;++it) {
            const auto stored=values.find(it->second);
            const auto& previous=*stored->second;
            if(previous.size()==bytes.size()&&std::equal(previous.begin(),previous.end(),bytes.begin())) {
                if(snapshot)snapshot_identities[snapshot.get()]={snapshot,it->second};
                if(!shader)CountPayload(1,bytes.size());
                return *stored;
            }
        }
        const auto& old_values=previous_snapshots[shader?1:0];
        const auto& old_index=previous_fingerprints[shader?1:0];
        const auto [old_first,old_last]=old_index.equal_range(fingerprint);
        for(auto it=old_first;it!=old_last;++it) {
            const auto& previous=old_values.at(it->second);
            if(previous->size()!=bytes.size()||!std::equal(previous->begin(),previous->end(),bytes.begin()))continue;
            auto stored=values.find(it->second);
            if(stored==values.end()) {
                if(frame->retained_bytes+bytes.size()>SnapshotLimit())
                    throw std::runtime_error("Live frame exceeds its snapshot memory limit");
                stored=values.emplace(it->second,previous).first;
                frame->retained_bytes+=bytes.size();RecordPayload(*stored,shader);
            }
            index.emplace(fingerprint,it->second);
            if(snapshot)snapshot_identities[snapshot.get()]={snapshot,it->second};
            if(!shader)CountPayload(2,bytes.size());
            return *stored;
        }
        const auto digest=ContentHash(bytes);
        auto stored=values.find(digest);
        if (stored==values.end()) {
            if (frame->retained_bytes+bytes.size()>SnapshotLimit())
                throw std::runtime_error("Live frame exceeds its snapshot memory limit");
            // A device state intentionally has no full-buffer fingerprint.
            // A later generic payload may still alias that previous frame's
            // exact bytes, and must retain the same immutable owner.
            auto old=old_values.find(digest);
            if(!shader&&old!=old_values.end()&&previous_device_states.contains(old->second.get())&&
               old->second->size()==bytes.size()&&std::equal(old->second->begin(),old->second->end(),bytes.begin()))
                stored=values.emplace(digest,old->second).first;
            else
                stored=values.emplace(digest,snapshot?snapshot:std::make_shared<const Bytes>(bytes.begin(),bytes.end())).first;
            frame->retained_bytes+=bytes.size();RecordPayload(*stored,shader);
            index.emplace(fingerprint,digest);
        }
        if(snapshot)snapshot_identities[snapshot.get()]={snapshot,digest};
        if(!shader)CountPayload(3,bytes.size());
        return *stored;
    }
    std::string InternCanonical(std::span<const uint8_t> bytes,bool shader,std::shared_ptr<const Bytes> snapshot) {
        return InternCanonicalEntry(bytes,shader,std::move(snapshot)).first;
    }
    const LiveResourceMap::value_type& InternEntry(std::span<const uint8_t> bytes,bool shader,std::shared_ptr<const Bytes> snapshot={}) {
        const bool same_size=lazy_states&&bytes.size()==LazyDeviceState::kBytes;
        uint64_t fingerprint=0;
        if(same_size) {
            fingerprint=StateFingerprint(bytes);
            if(MatchesLazyState(bytes,fingerprint))StopLazyStates(shader?3:2);
        }
        const auto& entry=InternCanonicalEntry(bytes,shader,std::move(snapshot));
        if(same_size&&lazy_states)TrackSameSizeResource(fingerprint,entry.second,shader);
        return entry;
    }
    const LiveResourceMap::value_type& InternDeviceStateEntry(GraphicsBoundaryBlock& block) {
        const auto bytes=block.data();
        const auto last=last_device_state.lock();
        std::string calculated_digest;
        const std::string* digest=&last_device_state_digest;
        if(!last||last->size()!=bytes.size()||!std::equal(last->begin(),last->end(),bytes.begin())) {
            calculated_digest=ContentHash(bytes);digest=&calculated_digest;
        }
        auto& values=frame->payloads;
        auto stored=values.find(*digest);
        if(stored==values.end()) {
            if(frame->retained_bytes+bytes.size()>SnapshotLimit())
                throw std::runtime_error("Live frame exceeds its snapshot memory limit");
            const auto& previous=previous_snapshots[0];
            const auto old=previous.find(*digest);
            std::shared_ptr<const Bytes> snapshot;
            // Shared texture identity hits need not populate fingerprints.
            // Reuse a generic previous owner only if the legacy state path
            // could find it; dedicated state owners have the marker instead.
            const bool reusable=old!=previous.end()&&
                (previous_device_states.contains(old->second.get())||
                 std::any_of(previous_fingerprints[0].begin(),previous_fingerprints[0].end(),
                             [&](const auto& entry){return entry.second==*digest;}));
            if(reusable&&old->second->size()==bytes.size()&&
               std::equal(old->second->begin(),old->second->end(),bytes.begin()))
                snapshot=old->second;
            else if(block.shared_bytes)
                snapshot=block.shared_bytes;
            else {
                snapshot=std::make_shared<const Bytes>(std::move(block.bytes));
                // SaveFailure may inspect the event after a later allocation
                // or draw validation fails. Preserve its immutable state too.
                block.shared_bytes=snapshot;
            }
            // Moving the vector preserves the state span used by the rest of
            // this draw's validation. The frame owns it before further reads.
            stored=values.emplace(*digest,std::move(snapshot)).first;
            frame->retained_bytes+=bytes.size();
        }
        device_states.insert(stored->second.get());
        last_device_state_digest=stored->first;
        last_device_state=stored->second;
        return *stored;
    }
    std::string InternDeviceState(GraphicsBoundaryBlock& block) {return InternDeviceStateEntry(block).first;}
    std::string InternShader(std::span<const uint8_t> header,std::span<const uint8_t> code) {
        const ShaderByteSegments bytes(header,code);
        const bool same_size=lazy_states&&bytes.size()==LazyDeviceState::kBytes;
        auto digest=InternShaderCanonical(header,code);
        if(same_size&&lazy_states) {
            // Shaders have a separate charged namespace, so their normal
            // intern may finish first. Inspect its existing immutable owner:
            // no concatenation allocation on a segmented cache hit.
            const auto& owner=frame->shaders.at(digest);
            const auto fingerprint=StateFingerprint(*owner);
            if(MatchesLazyState(*owner,fingerprint))StopLazyStates(3);
            else TrackSameSizeResource(fingerprint,owner,true);
        }
        return digest;
    }
    const LiveResourceMap::value_type& InternShaderCanonicalEntry(std::span<const uint8_t> header,std::span<const uint8_t> code) {
        const ShaderByteSegments bytes(header,code);
        size_t fingerprint=RuntimeShaderInputFingerprint{fast_fingerprints}(bytes);
#ifdef GOW3_NATIVE_VERIFY_PREFIX_DEPENDENCIES
        // Test-only collision injection exercises the real current/previous
        // shader caches without changing the persistent content identity.
        fingerprint&=test::live_shader_fingerprint_mask;
#endif
        auto& values=frame->shaders;auto& index=fingerprints[1];
        const auto [first,last]=index.equal_range(fingerprint);
        for(auto it=first;it!=last;++it) {
            const auto stored=values.find(it->second);
            if(bytes.Matches(*stored->second))return *stored;
        }
        const auto& old_values=previous_snapshots[1];
        const auto [old_first,old_last]=previous_fingerprints[1].equal_range(fingerprint);
        for(auto it=old_first;it!=old_last;++it) {
            const auto& previous=old_values.at(it->second);
            if(!bytes.Matches(*previous))continue;
            auto stored=values.find(it->second);
            if(stored==values.end()) {
                if(frame->retained_bytes+bytes.size()>SnapshotLimit())
                    throw std::runtime_error("Live frame exceeds its snapshot memory limit");
                stored=values.emplace(it->second,previous).first;
                frame->retained_bytes+=bytes.size();
            }
            index.emplace(fingerprint,it->second);
            return *stored;
        }
        // Allocate concatenated bytes only for a cache miss. Cache hits above
        // still compare every captured byte; no guest address or notification
        // is treated as proof that shader contents are unchanged.
        auto snapshot=bytes.Snapshot();
#ifdef GOW3_NATIVE_VERIFY_PREFIX_DEPENDENCIES
        ++test::live_shader_snapshot_allocations;
#endif
        const auto digest=ContentHash(*snapshot);
        auto stored=values.find(digest);
        if(stored==values.end()) {
            if(frame->retained_bytes+bytes.size()>SnapshotLimit())
                throw std::runtime_error("Live frame exceeds its snapshot memory limit");
            // A different segment split may give the same canonical bytes a
            // different fingerprint. Retain its previous immutable owner too.
            if(const auto old=old_values.find(digest);old!=old_values.end()&&bytes.Matches(*old->second))
                snapshot=old->second;
            stored=values.emplace(digest,std::move(snapshot)).first;
            frame->retained_bytes+=bytes.size();
        }
        index.emplace(fingerprint,digest);
        return *stored;
    }
    std::string InternShaderCanonical(std::span<const uint8_t> header,std::span<const uint8_t> code) {
        return InternShaderCanonicalEntry(header,code).first;
    }
    const LiveResourceMap::value_type& InternShaderEntry(std::span<const uint8_t> header,std::span<const uint8_t> code) {
        const ShaderByteSegments bytes(header,code);
        const bool same_size=lazy_states&&bytes.size()==LazyDeviceState::kBytes;
        const auto& entry=InternShaderCanonicalEntry(header,code);
        if(same_size&&lazy_states) {
            const auto fingerprint=StateFingerprint(*entry.second);
            if(MatchesLazyState(*entry.second,fingerprint))StopLazyStates(3);
            else TrackSameSizeResource(fingerprint,entry.second,true);
        }
        return entry;
    }
    // Copy both immutable published fields now. No canonical-bank reference
    // may escape this immediate producer handoff or survive later interning.
    CanonicalResourceRef ResourceDirect(const LiveResourceMap::value_type& entry)const {
        return {CanonicalDigest(entry.first),entry.second};
    }
    void Mark(const Ranges& ranges) {ownership.Mark(ranges);}
    const Ranges& TextureFootprint(const std::array<uint32_t,6>& fetch,
                                  std::optional<std::array<int32_t,4>> rectangle={}) {
        return ownership.TextureFootprint(fetch,rectangle);
    }
    bool GPUOnlyTexture(const std::array<uint32_t,6>& fetch) {return ownership.GPUOnlyTexture(fetch);}
    bool GPUBytes(uint32_t first,uint32_t count) const {return ownership.GPUBytes(first,count);}
    Ranges GPUByteRanges(uint32_t first,uint32_t count) const {return ownership.GPUByteRanges(first,count);}
};

GearsLivePlan::GearsLivePlan(std::function<void(uint32_t,uint32_t)> watch_memory,bool query_prefixes,bool lazy_device_states,bool direct_resource_refs,bool fast_fingerprints) : state_(std::make_unique<State>()) {
    state_->ownership.watch_memory=std::move(watch_memory);
    state_->query_prefixes=query_prefixes;state_->lazy_states=lazy_device_states;state_->direct_resource_refs=direct_resource_refs;state_->fast_fingerprints=fast_fingerprints;
    state_->lazy_state_mode.store(lazy_device_states?1:0,std::memory_order_relaxed);
}
GearsLivePlan::~GearsLivePlan() = default;
const char* GearsLivePlan::LazyStateMode()const noexcept {
    static constexpr const char* names[]{"disabled","enabled","payload_size_alias","shader_size_alias","wide_prefix"};
    return names[state_->lazy_state_mode.load(std::memory_order_relaxed)];
}
GearsLivePlan::DeviceStateStatistics GearsLivePlan::DeviceStateStats()const noexcept {
    const auto& s=*state_;constexpr auto r=std::memory_order_relaxed;
    return {s.device_state_calls.load(r),s.device_state_current_hits.load(r),s.device_state_previous_hits.load(r),s.device_state_created.load(r),s.device_state_collisions.load(r)};
}
GearsLivePlan::PayloadInternStatistics GearsLivePlan::PayloadInternStats()const noexcept {
    PayloadInternStatistics result;
    for(size_t c=0;c<4;++c)for(size_t o=0;o<4;++o) {
        result.calls[c][o]=state_->payload_calls[c][o].load(std::memory_order_relaxed);
        result.bytes[c][o]=state_->payload_bytes[c][o].load(std::memory_order_relaxed);
    }
    return result;
}
GearsLivePlan::ResourceHandoffStatistics GearsLivePlan::ResourceHandoffStats()const noexcept {
    return {state_->direct_resource_refs,state_->direct_resource_handoffs.load(std::memory_order_relaxed)};
}
std::optional<LiveQueryPrefix> GearsLivePlan::TakeQueryPrefix() {
    return std::exchange(state_->ready_prefix,{});
}
bool dependency_bounded_direct_prefixes=true;
std::optional<LiveQueryPrefix> GearsLivePlan::TakeDirectPrefix(size_t min_operations) {
    auto& s=*state_;
    if(!s.frame||s.ready_prefix||s.ready_packet_prefix)return {};
    const size_t end=s.frame->operations.size();
    if(end<=s.prefix_operation||s.operation_dependency_ends.size()!=end)return {};
    // Stay within the flat prefix bound: a wider prefix falls back to canonical
    // map storage, which digests every lazy device state (SHA-256 per draw).
    const size_t first=s.prefix_operation?s.operation_dependency_ends[s.prefix_operation-1]:0;
    const size_t dependencies=s.operation_dependency_ends.back()-first;
    const bool near_bound=dependency_bounded_direct_prefixes&&dependencies+32>LivePrefixResources::kMaximumDirectDependencies;
    if(end<s.prefix_operation+std::max<size_t>(min_operations,1)&&!near_bound)return {};
    auto batch=s.Prefix(s.prefix_operation,end,LivePrefixResources::kMaximumDirectDependencies);
    LiveQueryPrefix prefix{std::move(batch),s.prefix_operation,end,0,0};
    s.prefix_operation=end;
    return prefix;
}
std::optional<GearsLivePlan::MemoryCopy> GearsLivePlan::TakeMemoryCopy() {
    return std::exchange(state_->ready_memory_copy,{});
}
std::optional<LivePacketPrefix> GearsLivePlan::TakePacketPrefix() {
    return std::exchange(state_->ready_packet_prefix,{});
}
void GearsLivePlan::InvalidateMemory(uint32_t address,uint32_t bytes,std::vector<uint8_t> data,uint64_t coherence_epoch) {
    if(!bytes||uint64_t(address)+bytes>0x20000000ull||(!data.empty()&&data.size()!=bytes))
        throw std::runtime_error("Invalid CPU memory invalidation");
    state_->ownership.Invalidate(address,bytes);
    state_->cpu_writes.push_back({address,bytes,std::move(data),coherence_epoch});
}
std::shared_ptr<LiveRenderFrame> GearsLivePlan::Push(GraphicsBoundaryEvent&& event) {
    auto& s=*state_;
    if(!event.capture_error.empty())throw std::runtime_error(event.capture_error);
    if(event.span_only) {
        if(!event.command_span||!s.frame||s.frame->frame!=event.frame)
            throw std::runtime_error("Invalid detached command span");
        s.PacketPrefix(std::move(*event.command_span));
        return {};
    }
    if (event.function==profile::d3d::kSpecializedDraw)
        throw std::runtime_error("Specialized D3D draw requires native ABI support");
    const char* name=FunctionName(event.function);
    const bool draw=(event.phase==1 && (event.function==profile::d3d::kDrawVertices || event.function==profile::d3d::kDrawIndexedVertices)) ||
        (event.phase==2 && (event.function==profile::d3d::kBeginVertices || event.function==profile::d3d::kBeginIndexedVertices));
    if (!name || (!draw && event.phase!=0) ||
        (!draw && event.function!=kPresent && event.function!=profile::d3d::kClearRect && event.function!=profile::d3d::kResolve && event.function!=profile::d3d::kOcclusionQueryReport)) return {};
    if (!s.frame) {
        if (s.previous_frame && event.frame!=*s.previous_frame+1) throw std::runtime_error("Live frame sequence gap");
        s.frame=std::make_shared<LiveRenderFrame>(); s.frame->frame=event.frame;
        if(s.lazy_states)s.frame->lazy_payloads=std::make_shared<LazyPayloadTable>();
    }
    if (s.frame->frame!=event.frame) throw std::runtime_error("Live frame changed without Present");
    State::PendingDependencies pending_dependencies{s};
    for(const auto& write:s.cpu_writes) {
        Json op=Json::object();op["event"]=event.sequence;op["frame"]=event.frame;op["type"]="cpu_memory_write";
        op["address"]=write.address;op["bytes"]=write.bytes;op["payload"]=nullptr;op["coherence_epoch"]=write.coherence_epoch;
        if(!write.data.empty())s.AssignDependency(op["payload"],s.Intern(write.data));
        s.Emit(std::move(op));
    }
    s.cpu_writes.clear();
    std::array<GraphicsBoundaryBlock*,105> blocks{};
    for (auto& block : event.blocks) {
        if (!block.kind || block.kind>=blocks.size() || blocks[block.kind]) throw std::runtime_error("Invalid live block kind");
        blocks[block.kind]=&block;
    }
    auto data=[&](unsigned kind)->std::span<const uint8_t> {
        return blocks[kind]?blocks[kind]->data():std::span<const uint8_t>();
    };
    auto intern_block=[&](unsigned kind) {
        return s.Intern(data(kind),false,blocks[kind]?blocks[kind]->shared_bytes:nullptr);
    };
    auto materialize_texture=[&](unsigned kind) {
        auto* block=blocks[kind];
        if(!block||!block->deferred_physical_bytes)return;
        if(!event.materialize_texture)throw std::runtime_error("Missing deferred texture snapshot capability");
        event.materialize_texture(*block);
        if(block->deferred_physical_bytes)
            throw std::runtime_error("Deferred texture snapshot was not completed");
    };
    const auto state=data(1);
    if (state.size()!=0x3600) throw std::runtime_error("Missing live device state");
    std::array<std::optional<TypedSurface>,5> draw_surfaces;
    for (unsigned kind=6;kind<=10;++kind) if (!data(kind).empty()) {
        auto b=data(kind); if (b.size()!=64) throw std::runtime_error("Invalid live surface header");
        const auto info=Word(b,24), format=Word(b,28), dimensions=Word(b,36);
        draw_surfaces[kind-6]=TypedSurface{blocks[kind]->address,((dimensions>>18)&0x3fff)+1,
            ((dimensions>>3)&0x7fff)+1,info,format,format&2047,(format>>16)&(kind==10?1u:15u),kind==10};
    }
    const bool ordinary_draw=draw&&(data(2).empty()||!(Word(data(2),0)&0x10));
    // Query controls are typed; their ToJson rebuilds the same envelope.
    const bool json_control=!ordinary_draw&&event.function!=profile::d3d::kOcclusionQueryReport;
    Json surfaces,op;
    if(json_control) {
        surfaces=Json::object();
        for(unsigned slot=0;slot<draw_surfaces.size();++slot)
            if(draw_surfaces[slot])surfaces[std::to_string(slot+6)]=ToJson(*draw_surfaces[slot]);
        op=Json::object();op["event"]=event.sequence;op["frame"]=event.frame;
    }
    const char* diagnostic_hash_field=nullptr;
    const auto& a=event.args;
    if (event.function==profile::d3d::kOcclusionQueryReport) {
        const auto query=data(104);
        if (query.size()!=0x98 || a[2]>1 || Word(query,0x94)>1)
            throw std::runtime_error("Native occlusion requires a single report allocation");
        const uint64_t report=uint64_t(Word(query,0x1c))+uint64_t(a[2])*32;
        if (!Word(query,0x1c) || report>UINT32_MAX) throw std::runtime_error("Invalid native query report address");
        const uint32_t address=Physical(uint32_t(report));
        if ((address&31) || bool(address&32)!=bool(a[2])) throw std::runtime_error("Misaligned native query report");
        if(a[2]) {
            if(s.active_query)throw std::runtime_error("Nested live occlusion query");
            const uint32_t end_address=address-32;
            s.active_query=State::ActiveQuery{a[1],end_address,++s.query_generation};
        } else if(!s.active_query||s.active_query->handle!=a[1]||s.active_query->address!=address)
            throw std::runtime_error("Unmatched live occlusion query end");
        TypedLiveQuery typed;typed.event=event.sequence;typed.frame=event.frame;
        typed.begin=a[2];typed.handle=a[1];typed.report_address=address;
        typed.generation=s.active_query->generation;typed.surfaces=draw_surfaces;
        if(!a[2])s.active_query.reset();
        s.Emit(std::move(typed));
    } else if (event.function==profile::d3d::kClearRect) {
        auto rect=Rectangle(data(98));
        std::array<int32_t,4> viewport;
        for (unsigned i=0;i<4;++i) {
            const auto v=Float(state,profile::device::kClearViewport+i*4);
            if (!std::isfinite(v) || double(v)<INT32_MIN || double(v)>INT32_MAX) throw std::runtime_error("Invalid clear viewport");
            viewport[i]=int32_t(v);
        }
        rect={std::max(rect[0],viewport[0]),std::max(rect[1],viewport[1]),
              std::min(rect[2],viewport[0]+viewport[2]),std::min(rect[3],viewport[1]+viewport[3])};
        if (Word(state,profile::device::kScissorEnable)) {
            rect[0]=std::max(rect[0],int32_t(Word(state,profile::device::kScissorRect))); rect[1]=std::max(rect[1],int32_t(Word(state,profile::device::kScissorRect+4)));
            rect[2]=std::min(rect[2],int32_t(Word(state,profile::device::kScissorRect+8))); rect[3]=std::min(rect[3],int32_t(Word(state,profile::device::kScissorRect+12)));
        }
        const auto extra=data(102);
        const double depth=std::bit_cast<double>((uint64_t(Word(extra,0))<<32)|Word(extra,4));
        Json color=nullptr;
        if (!data(99).empty()) { color=Json::array(); for (unsigned i=0;i<4;++i) color.push_back(Float(data(99),i*4)); }
        op["type"]="clearrect";op["flags"]=a[1];op["rectangle"]=rect;
        op["depth"]=depth;op["stencil"]=a[4];op["color"]=std::move(color);
    } else if (event.function==profile::d3d::kResolve) {
        auto fetch=Fetch(data(100),28); auto identity=Identity(fetch,true);
        auto rect=data(98).empty()?std::array<int32_t,4>{0,0,identity.at("width"),identity.at("height")}:Rectangle(data(98));
        const int32_t x=data(101).empty()?0:int32_t(Word(data(101),0)), y=data(101).empty()?0:int32_t(Word(data(101),4));
        const std::array<int32_t,4> dest{x,y,x+rect[2]-rect[0],y+rect[3]-rect[1]};
        const auto slot=a[1]&7;
        if (slot>4 || !surfaces.contains(std::to_string(6+slot))) throw std::runtime_error("Live resolve has no source surface");
        op["type"]="resolve";op["source_slot"]=slot;op["flags"]=a[1];op["source_rectangle"]=rect;
        op["destination_rectangle"]=dest;op["destination"]=std::move(identity);op["level"]=a[5];
        op["copy_dest_swap"]=ResolveDestinationSwap(fetch,slot);
        op["destination_fetch"]=fetch;op["exponent_bias"]=int32_t(((a[1]>>26)^32))-32;
        // This is an address-layout calculation, independent of current image
        // contents or ownership. Reuse exact rectangle footprints rather than
        // sorting hundreds of thousands of texel ranges at every resolve.
        s.Mark(ResolveDestinationFootprint(s.ownership,data(100),data(98),data(101)));
    } else if (event.function==kPresent) {
        auto fetch=Fetch(data(103),28); auto identity=Identity(fetch,true);
        fetch[1]=(fetch[1]&4095)|Physical(fetch[1]&~4095u);
        op["type"]="present";op["frontbuffer"]=std::move(identity);
        op["frontbuffer_fetch"]=fetch;op["provenance"]="captured_present_resource";
    } else if (draw&&!data(2).empty()&&(Word(data(2),0)&0x10)) {
        Bytes shader(data(2).begin(),data(2).end());shader.insert(shader.end(),data(3).begin(),data(3).end());
        if(event.function!=profile::d3d::kDrawVertices)throw std::runtime_error("Unsupported native memory-export draw ABI");
        const auto copy=DecodeMemoryExportCopy(shader,state,a);
        op["type"]="memory_copy";op["source"]=copy.source;op["destination"]=copy.destination;
        const auto gpu_ranges=s.GPUByteRanges(copy.source,copy.bytes);
        const bool fully_gpu=gpu_ranges.size()==1&&gpu_ranges[0].first==copy.source&&
            gpu_ranges[0].second==copy.source+copy.bytes;
        op["bytes"]=copy.bytes;op["source_kind"]=fully_gpu?"gpu_memory":gpu_ranges.empty()?"cpu_snapshot":"mixed_memory";
        // Preserve the exact ownership at capture, including sub-word holes
        // made by CPU writes. A partial overlap is not a full GPU buffer.
        if(!fully_gpu&&!gpu_ranges.empty())op["gpu_ranges"]=gpu_ranges;
        op["payload"]=nullptr;op["shader"]=ContentHash(shader);op["published_to_guest"]=false;
        diagnostic_hash_field="shader";
        if(!fully_gpu) {
            const auto bytes=data(14);
            if(!blocks[14]||blocks[14]->address!=copy.source_binding||
                uint64_t(copy.source_offset)+copy.bytes>bytes.size())throw std::runtime_error("Missing native memcpy source bytes");
            s.AssignDependency(op["payload"],s.Intern(bytes.subspan(copy.source_offset,copy.bytes)));
        }
        s.Mark({{copy.destination,copy.destination+copy.bytes}});
        s.ready_memory_copy=GearsLivePlan::MemoryCopy{copy.source,copy.destination,copy.bytes,gpu_ranges.empty()};
    } else if (draw) {
        TypedLiveDraw typed;typed.event=event.sequence;typed.frame=event.frame;typed.args=a;
        uint64_t handoffs=0;
        const auto direct=[&](const LiveResourceMap::value_type& entry) {
            auto ref=s.ResourceDirect(entry);++handoffs;return ref;
        };
        const auto typed_block=[&](unsigned kind) {
            s.payload_class=kind<=31?0:kind>=96?1:2;
            struct Reset {unsigned& value;~Reset(){value=3;}} reset{s.payload_class};
            if(!s.direct_resource_refs)return s.Resource(intern_block(kind));
            return direct(s.InternEntry(data(kind),false,blocks[kind]?blocks[kind]->shared_bytes:nullptr));
        };
        switch(event.function) {
            case profile::d3d::kDrawVertices:typed.function=DrawFunction::DrawPrimitive;break;
            case profile::d3d::kDrawIndexedVertices:typed.function=DrawFunction::DrawIndexedPrimitive;break;
            case profile::d3d::kBeginVertices:typed.function=DrawFunction::BeginVertices;break;
            case profile::d3d::kBeginIndexedVertices:typed.function=DrawFunction::BeginIndexedVertices;break;
        }
        typed.surfaces=std::move(draw_surfaces);
        if(s.lazy_states)typed.lazy_device_state=s.InternLazyDeviceState(*blocks[1]);
        if(!typed.lazy_device_state) {
            if(s.direct_resource_refs)typed.device_state=direct(s.InternDeviceStateEntry(*blocks[1]));
            else typed.device_state=s.Resource(s.InternDeviceState(*blocks[1]));
        }
        ShaderRequirementMasks required;
        for (unsigned stage=0;stage<2;++stage) {
            const auto v=data(2+2*stage), p=data(3+2*stage);
            auto& shader=stage?typed.pixel_shader:typed.vertex_shader;
            if (v.empty() || p.empty()) {
                if (!stage) {
                    Json context={{"function",Hex(event.function)},{"phase",event.phase},{"event",event.sequence},
                        {"frame",event.frame},{"result",Hex(event.result)},{"args",a},
                        {"bound_vertex_shader",Hex(Word(state,profile::device::kVertexShader))},{"blocks",Json::array()}};
                    for (unsigned kind : {2,3,11,96,97}) if(blocks[kind])
                        context["blocks"].push_back({{"kind",kind},{"address",Hex(blocks[kind]->address)},
                            {"bytes",data(kind).size()}});
                    if(data(11).size()>=0x38c) {
                        auto object=data(11);
                        context["shader_header"]={Hex(Word(object,0x368)),Word(object,0x36c),
                            Word(object,0x370),Hex(Word(object,0x20))};
                    }
                    throw std::runtime_error("Missing live vertex shader: "+context.dump());
                }
                continue;
            }
            const auto flags=Word(v,0), allowed=stage?0u:0x61u;
            if ((flags&~allowed)!=0x102A1100u || bool(flags&1)==bool(stage) || Word(v,4)!=v.size() || Word(v,8)!=p.size())
                throw std::runtime_error("Invalid live shader container");
            std::string legacy_hash;const std::string* hash;
            if(s.direct_resource_refs) {
                const auto& entry=s.InternShaderEntry(v,p);shader=direct(entry);hash=&entry.first;
            } else {legacy_hash=s.InternShader(v,p);shader=s.Resource(legacy_hash,true);hash=&legacy_hash;}
            if (!stage && *hash==kRectangle && event.function==profile::d3d::kBeginVertices && event.phase==2 && a[1]==8 && a[2]==3 && a[3]==16)
                typed.rectangle_list=true;
            else {
                auto it=s.requirements.find(*hash);
                if (it==s.requirements.end()) it=s.requirements.emplace(*hash,ShaderFetchRequirements(*s.frame->shaders.at(*hash))).first;
                required.Merge(it->second);
            }
        }
        auto& vertex_slots=typed.vertex_fetch_slots;vertex_slots.reserve(required.vertex_count());
        required.ForEachVertex([&](unsigned slot){vertex_slots.push_back(slot);});
        for (unsigned kind=14;kind<=31;++kind) if (!data(kind).empty()) {
            TypedGeometry geometry;geometry.kind=kind;geometry.address=blocks[kind]->address;
            geometry.bytes=data(kind).size();geometry.payload=typed_block(kind);
            if(s.GPUBytes(blocks[kind]->address,uint32_t(data(kind).size())))geometry.gpu_memory=true;
            if (kind<=29) { const auto slot=95-(kind-14); geometry.fetch_slot=slot; geometry.endian=Word(state,0x480+slot*8+4)&3; }
            typed.geometry.push_back(std::move(geometry));
        }
        if (event.phase==2) for (unsigned kind : {96,97}) if (!data(kind).empty()) {
            TypedGeometry geometry;geometry.kind=kind;geometry.address=blocks[kind]->address;
            geometry.bytes=data(kind).size();geometry.payload=typed_block(kind);
            typed.up_geometry.push_back(std::move(geometry));
        }
        unsigned texture_count=0;required.ForEachTexture([&](unsigned){++texture_count;});
        typed.textures.reserve(texture_count);
        if(event.materialize_textures) {
            std::array<GraphicsBoundaryBlock*,64> cpu_blocks{};size_t count=0;
            // Classify once using the original GPU-only predicate, AFTER the
            // queued CPU invalidations above. Do not read unused/owned textures.
            required.ForEachTexture([&](unsigned slot) {
                const auto fetch=Fetch(state,0x480+slot*24);
                if((fetch[0]&3)!=2)throw std::runtime_error("Invalid live texture fetch binding");
                TypedTexture binding;binding.slot=slot;binding.fetch=fetch;
                if(((fetch[5]>>9)&3)==1)binding.identity=TypedIdentity(fetch);
                if(binding.identity&&s.GPUOnlyTexture(fetch))binding.source=TextureSource::GpuPreviousOrAlias;
                else for(unsigned kind:{32+slot*2,33+slot*2})
                    if(blocks[kind]&&blocks[kind]->deferred_physical_bytes)cpu_blocks[count++]=blocks[kind];
                typed.textures.push_back(std::move(binding));
            });
            if(count) {
                event.materialize_textures(std::span<GraphicsBoundaryBlock* const>(cpu_blocks).first(count));
                for(size_t i=0;i<count;++i)if(cpu_blocks[i]->deferred_physical_bytes)
                    throw std::runtime_error("Deferred texture batch was not completed");
            }
            // The final canonical resources retain the exact original extents
            // and owners. No internal descriptor or cache entry pointer escapes.
            for(auto& binding:typed.textures)if(binding.source!=TextureSource::GpuPreviousOrAlias) {
                const auto slot=binding.slot;
                if(!data(32+slot*2).empty())binding.base_payload=typed_block(32+slot*2);
                if(!data(33+slot*2).empty())binding.mip_payload=typed_block(33+slot*2);
            }
        } else {
        required.ForEachTexture([&](unsigned slot) {
            const auto fetch=Fetch(state,0x480+slot*24);
            if ((fetch[0]&3)!=2) throw std::runtime_error("Invalid live texture fetch binding");
            TypedTexture binding;binding.slot=slot;binding.fetch=fetch;
            if (((fetch[5]>>9)&3)==1) binding.identity=TypedIdentity(fetch);
            if(binding.identity&&s.GPUOnlyTexture(fetch)) {
                binding.source=TextureSource::GpuPreviousOrAlias;
            } else {
                // CPU invalidations have already revoked stale GPU ownership.
                // Materialize only the base/mip snapshots this draw requires;
                // a fully GPU-owned image never reads its stale CPU mapping.
                materialize_texture(32+slot*2);
                materialize_texture(33+slot*2);
                if (!data(32+slot*2).empty()) binding.base_payload=typed_block(32+slot*2);
                if (!data(33+slot*2).empty()) binding.mip_payload=typed_block(33+slot*2);
            }
            typed.textures.push_back(std::move(binding));
        });
        }
        s.Emit(std::move(typed));
        // One relaxed publication per successful typed operation. This counts
        // producer digest/owner copies, never GPU accepted or retired draws.
        if(handoffs)State::Count(s.direct_resource_handoffs,handoffs);
    }
    // Resolve validation still uses the local surface map above. Transfer it
    // only after every branch has finished, preserving one immutable owner.
    if(json_control) {
        op["surfaces"]=std::move(surfaces);
        s.Emit(std::move(op),diagnostic_hash_field);
    }
    if(s.operation_ends.size()>=65536)throw std::runtime_error("Too many native API operations in one frame");
    s.RecordOperationEnd(event.sequence,s.frame->operations.size());
    if(event.command_span)s.PacketPrefix(std::move(*event.command_span));
    if(s.query_prefixes&&event.function==profile::d3d::kOcclusionQueryReport&&!a[2]) {
        if(s.ready_prefix)throw std::runtime_error("Live query prefix was not consumed");
        const size_t end=s.frame->operations.size();
        auto batch=s.Prefix(end);
        const auto& query=s.frame->operations.back();
        s.ready_prefix=LiveQueryPrefix{std::move(batch),s.prefix_operation,end,
            query.QueryReportAddress(),query.QueryGeneration()};
        s.prefix_operation=end;
    }
    if (event.function!=kPresent) return {};
    // Ownership is captured at the draw, never retroactively from the end of
    // this frame: a later CPU overwrite or resolve must not change its inputs.
    s.previous_frame=event.frame;
    s.prefix_operation=0;
    s.operation_ends.clear();
    s.operation_dependencies.clear();
    s.operation_dependency_ends.clear();
    if(s.frame->lazy_payloads)s.frame->lazy_payloads->Seal();
    s.previous_lazy_payloads=s.frame->lazy_payloads;
    s.lazy_states_previous=std::move(s.lazy_states_current);s.lazy_states_current.clear();
    s.same_size_previous=std::move(s.same_size_current);s.same_size_current.clear();
    s.previous_snapshots={s.frame->payloads,s.frame->shaders};
    s.previous_fingerprints=std::move(s.fingerprints);
    s.previous_device_states=std::move(s.device_states);
    s.device_states.clear();
    for(auto& index:s.fingerprints)index.clear();
    std::erase_if(s.snapshot_identities,[](const auto& entry){return entry.second.first.expired();});
    return std::exchange(s.frame,{});
}

struct GearsLiveOwnership::State {
    OwnershipRanges ownership;
    uint64_t query_generation=0;
};
GearsLiveOwnership::GearsLiveOwnership(std::function<void(uint32_t,uint32_t)> watch_memory):state_(std::make_unique<State>()) {
    state_->ownership.watch_memory=std::move(watch_memory);
}
GearsLiveOwnership::~GearsLiveOwnership()=default;
void GearsLiveOwnership::InvalidateMemory(uint32_t address,uint32_t bytes) {
    if(!bytes||uint64_t(address)+bytes>0x20000000ull)throw std::runtime_error("Invalid CPU memory invalidation");
    state_->ownership.Invalidate(address,bytes);
}
GearsLiveOwnership::Decision GearsLiveOwnership::Observe(GraphicsBoundaryEvent& event) {
    auto& s=*state_;Decision decision;
    if(!event.capture_error.empty()||event.span_only)return decision;
    // Same event selection as GearsLivePlan::Push.
    const bool draw=(event.phase==1 && (event.function==profile::d3d::kDrawVertices || event.function==profile::d3d::kDrawIndexedVertices)) ||
        (event.phase==2 && (event.function==profile::d3d::kBeginVertices || event.function==profile::d3d::kBeginIndexedVertices));
    if(!FunctionName(event.function)||(!draw&&event.phase!=0))return decision;
    std::array<GraphicsBoundaryBlock*,105> blocks{};
    for(auto& block:event.blocks)if(block.kind&&block.kind<blocks.size()&&!blocks[block.kind])blocks[block.kind]=&block;
    auto data=[&](unsigned kind)->std::span<const uint8_t> {return blocks[kind]?blocks[kind]->data():std::span<const uint8_t>();};
    const auto& a=event.args;
    if(event.function==profile::d3d::kOcclusionQueryReport) {
        if(a[2])++s.query_generation;
        decision.query_begin=a[2];decision.query_end=!a[2];
        decision.query_handle=a[1];decision.query_generation=s.query_generation;
        return decision;
    }
    if(event.function==profile::d3d::kResolve) {
        s.ownership.Mark(ResolveDestinationFootprint(s.ownership,data(100),data(98),data(101)));
        return decision;
    }
    if(!draw)return decision;
    const auto state=data(1);
    if(state.size()!=0x3600)throw std::runtime_error("Missing live device state");
    if(!data(2).empty()&&(Word(data(2),0)&0x10)) {
        Bytes shader(data(2).begin(),data(2).end());shader.insert(shader.end(),data(3).begin(),data(3).end());
        if(event.function!=profile::d3d::kDrawVertices)throw std::runtime_error("Unsupported native memory-export draw ABI");
        const auto copy=DecodeMemoryExportCopy(shader,state,a);
        const auto gpu_ranges=s.ownership.GPUByteRanges(copy.source,copy.bytes);
        s.ownership.Mark({{copy.destination,copy.destination+copy.bytes}});
        decision.memory_copy=GearsLivePlan::MemoryCopy{copy.source,copy.destination,copy.bytes,gpu_ranges.empty()};
        return decision;
    }
    for(unsigned kind=32;kind<=95;++kind) {
        auto* block=blocks[kind];
        if(!block||!block->deferred_physical_bytes)continue;
        const auto fetch=Fetch(state,0x480+(kind-32)/2*24);
        if(((fetch[5]>>9)&3)==1&&s.ownership.GPUOnlyTexture(fetch))continue;
        if(decision.texture_count==decision.textures.size())throw std::runtime_error("Too many live texture snapshots");
        decision.textures[decision.texture_count++]=block;
    }
    return decision;
}
} // namespace gow3::renderer
