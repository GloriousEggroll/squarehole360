#pragma once
#include "live_prefix_resources.h"
#include "lazy_device_state.h"
#include <array>
#include <optional>
#include <string_view>

namespace gow3::renderer {
// Canonical SHA-256 text, never a pointer, generation, or cheap fingerprint.
// Store inline to avoid a heap allocation for each immutable binding field.
struct CanonicalDigest {
private:
    std::array<char,64> text{};
public:
    // Unassigned construction slot; producers must set every required field
    // from the canonical bank before publishing the immutable operation.
    CanonicalDigest()=default;
    explicit CanonicalDigest(std::string_view digest) {
        if(digest.size()!=text.size())throw std::invalid_argument("Invalid canonical resource digest");
        unsigned invalid=0;
        for(unsigned char c:digest)
            invalid|=unsigned(!((c>='0'&&c<='9')||(c>='a'&&c<='f')));
        if(invalid)throw std::invalid_argument("Invalid canonical resource digest");
        std::copy(digest.begin(),digest.end(),text.begin());
    }
    std::string_view View()const {
        if(!text[0])throw std::invalid_argument("Unassigned canonical resource digest");
        return {text.data(),text.size()};
    }
    bool operator==(const CanonicalDigest&)const=default;
};
struct CanonicalResourceRef {
    CanonicalDigest hash;
    // Live records always own immutable bytes. Offline lazy adapters may omit
    // this owner and keep provider reads at their original cache-miss sites.
    LiveResourceBytes owner;
};
enum class DrawFunction {DrawPrimitive,DrawIndexedPrimitive,BeginVertices,BeginIndexedVertices};
inline std::string_view DrawFunctionName(DrawFunction function) {
    switch(function) {
        case DrawFunction::DrawPrimitive:return "DrawPrimitive";
        case DrawFunction::DrawIndexedPrimitive:return "DrawIndexedPrimitive";
        case DrawFunction::BeginVertices:return "BeginVertices";
        case DrawFunction::BeginIndexedVertices:return "BeginIndexedVertices";
    }
    throw std::invalid_argument("Invalid native draw function");
}
struct TypedSurface {
    uint32_t address=0,width=0,height=0,surface_info=0,format_info=0,edram_base=0,format=0;
    bool depth=false;
};
struct TypedTextureIdentity {
    uint32_t base=0,format=0,width=0,height=0,pitch=0,endian=0,dimension=0,layers=0;
    bool tiled=false,packed=false;
};
struct TypedGeometry {
    uint32_t kind=0,address=0,bytes=0;
    CanonicalResourceRef payload;
    bool gpu_memory=false;
    std::optional<uint32_t> fetch_slot,endian;
};
enum class TextureSource {CpuSnapshot,GpuPreviousOrAlias,GpuResolve};
struct TypedTexture {
    uint32_t slot=0;
    std::array<uint32_t,6> fetch{};
    TextureSource source=TextureSource::CpuSnapshot;
    std::optional<CanonicalResourceRef> base_payload,mip_payload;
    std::optional<TypedTextureIdentity> identity;
};
struct TypedLiveDraw {
    uint64_t event=0;
    uint32_t frame=0;
    DrawFunction function=DrawFunction::DrawPrimitive;
    std::array<uint32_t,8> args{};
    bool rectangle_list=false;
    CanonicalResourceRef device_state;
    LazyDeviceStateRef lazy_device_state;
    std::optional<CanonicalResourceRef> vertex_shader,pixel_shader;
    std::array<std::optional<TypedSurface>,5> surfaces;
    std::vector<TypedGeometry> geometry,up_geometry;
    std::vector<uint32_t> vertex_fetch_slots;
    std::vector<TypedTexture> textures;
};
// Occlusion query control (query_begin/query_end). Holds exactly the fields of
// the legacy JSON envelope; ToJson reproduces it byte-for-byte, including the
// operation-time surface snapshot that no consumer reads on the live path.
struct TypedLiveQuery {
    uint64_t event=0,generation=0;
    uint32_t frame=0,handle=0,report_address=0;
    bool begin=false; // record_index == begin
    std::array<std::optional<TypedSurface>,5> surfaces;
};
} // namespace gow3::renderer
