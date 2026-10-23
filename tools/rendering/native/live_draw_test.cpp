#include <algorithm>
#include "renderer/native/live_plan.h"
#include "renderer/native/content_hash.h"
#include <atomic>
#include <iostream>
#include <thread>
#include <type_traits>

using namespace gow3::renderer;
using Json=nlohmann::json;
namespace {
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
CanonicalResourceRef Resource(uint8_t value) {
    auto bytes=std::make_shared<const std::vector<uint8_t>>(37,value);
    return {CanonicalDigest(ContentHash(*bytes)),std::move(bytes)};
}
// Deliberately spell out the old JSON contract independently of the serializer.
Json Expected(const TypedLiveDraw& draw) {
    auto hash=[](const CanonicalResourceRef& resource){return ContentHash(*resource.owner);};
    Json result={{"event",draw.event},{"frame",draw.frame},{"type","draw"},
        {"function","DrawIndexedPrimitive"},{"args",{1,2,3,4,5,6,7,8}},
        {"device_state",hash(draw.device_state)},{"vertex_shader",hash(*draw.vertex_shader)},
        {"pixel_shader",nullptr},{"vertex_slots",{0,79,95,96}},
        {"vertex_input","affine_float4_rectangle"},
        {"surfaces",{{"6",{{"address","00001234"},{"width",1280},{"height",720},{"depth",false},
            {"surface_info",11},{"format_info",22},{"edram_base",33},{"format",4}}}}},
        {"geometry",Json::array({{{"kind",14},{"address","00abcdef"},{"bytes",37},
            {"sha256",hash(draw.geometry[0].payload)},{"source","gpu_memory"},{"fetch_slot",95},{"endian",0}}})},
        {"up_geometry",Json::array({{{"kind",96},{"address","00001000"},{"bytes",37},
            {"sha256",hash(draw.up_geometry[0].payload)}}})},
        {"textures",Json::array({{{"slot",31},{"fetch",{2,3,4,5,6,7}},{"source","cpu_snapshot"},
            {"base_payload",hash(*draw.textures[0].base_payload)},{"mip_payload",nullptr},
            {"identity",{{"base","001ab000"},{"format",18},{"width",128},{"height",64},
                {"pitch",256},{"endian",3},{"dimension",1},{"layers",6},{"tiled",true},{"packed",false}}}}})}};
    return result;
}
TypedLiveDraw Draw() {
    TypedLiveDraw draw;draw.event=UINT64_C(0x1FFFFFFFF);draw.frame=18;
    draw.function=DrawFunction::DrawIndexedPrimitive;draw.args={1,2,3,4,5,6,7,8};
    draw.rectangle_list=true;draw.device_state=Resource(1);draw.vertex_shader=Resource(2);
    draw.surfaces[0]=TypedSurface{0x1234,1280,720,11,22,33,4,false};
    draw.geometry.push_back({14,0xabcdef,37,Resource(3),true,95,0});
    draw.up_geometry.push_back({96,0x1000,37,Resource(4),false,{},{}});
    draw.vertex_fetch_slots={0,79,95,96};
    TypedTexture texture;texture.slot=31;texture.fetch={2,3,4,5,6,7};texture.base_payload=Resource(5);
    texture.identity=TypedTextureIdentity{0x1ab000,18,128,64,256,3,1,6,true,false};
    draw.textures.push_back(std::move(texture));return draw;
}
}
int main() {
    try {
        static_assert(std::is_same_v<decltype(*std::declval<LiveOperations>().begin()),const LiveOperation&>);
        {
            bool rejected=false;
            try {CanonicalDigest{}.View();}catch(const std::invalid_argument&){rejected=true;}
            Check(rejected,"Unassigned canonical digest was exposed as a valid resource key");
        }
        {
            // Compare against the actual pre-change map type, not its updated
            // alias. LiveResourceHash (2026-10-16) mixes the digest prefix, so
            // traversal order differs from std::hash; it only picks which files
            // a capped diagnostic dump includes first. Contents must match.
            std::unordered_map<std::string,LiveResourceBytes> legacy;
            LiveResourceMap transparent;
            for(unsigned i=0;i<1024;++i) {
                const auto key=ContentHash(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&i),sizeof(i)));
                auto owner=std::make_shared<const std::vector<uint8_t>>(1,uint8_t(i));
                legacy.emplace(key,owner);transparent.emplace(key,owner);
                Check(legacy.bucket_count()==transparent.bucket_count(),"Canonical bank bucket growth changed");
                Check(legacy.size()==transparent.size()&&std::all_of(legacy.begin(),legacy.end(),[&](const auto& entry) {
                    const auto found=transparent.find(entry.first);
                    return found!=transparent.end()&&found->second==entry.second;}),"Canonical bank contents changed");
                Check(transparent.find(std::string_view(key))->second==owner,"Borrowed digest lookup changed owner");
            }
        }
        for(const auto& malformed:{std::string(63,'a'),std::string(65,'a'),std::string(64,'g'),std::string(64,'A')}) {
            bool failed=false;try {CanonicalDigest digest(malformed);}catch(const std::invalid_argument&){failed=true;}
            Check(failed,"Noncanonical digest accepted");
        }
        {
            // Exercise every byte in every position and sixteen alignments.
            // The alphabet oracle also rejects embedded NUL and signed chars.
            constexpr std::string_view alphabet="0123456789abcdef";
            static_assert(sizeof(CanonicalDigest)==64);
            for(size_t alignment=0;alignment<16;++alignment) {
                std::string storage(alignment+64,'a');
                for(size_t position=0;position<64;++position) {
                    for(unsigned byte=0;byte<256;++byte) {
                        storage[alignment+position]=static_cast<char>(byte);
                        const auto input=std::string_view(storage).substr(alignment);
                        bool accepted=false;
                        try {
                            const CanonicalDigest digest(input);
                            Check(digest.View()==input,"Digest validation changed canonical text");
                            accepted=true;
                        } catch(const std::invalid_argument& error) {
                            Check(std::string_view(error.what())=="Invalid canonical resource digest",
                                  "Digest validation changed its rejection reason");
                        }
                        Check(accepted==(alphabet.find(static_cast<char>(byte))!=std::string_view::npos),
                              "Digest validation changed its accepted alphabet");
                    }
                    storage[alignment+position]='a';
                }
            }
        }
        auto draw=Draw();const auto expected=Expected(draw);
        Check(ToJson(draw)==expected,"Typed draw changed legacy fields");
        for(auto [kind,name]:{std::pair{DrawFunction::DrawPrimitive,"DrawPrimitive"},
            {DrawFunction::DrawIndexedPrimitive,"DrawIndexedPrimitive"},{DrawFunction::BeginVertices,"BeginVertices"},
            {DrawFunction::BeginIndexedVertices,"BeginIndexedVertices"}}) {
            draw.function=kind;Check(ToJson(draw).at("function")==name,"Typed draw changed ABI spelling");
        }
        for(auto [kind,name]:{std::pair{TextureSource::CpuSnapshot,"cpu_snapshot"},
            {TextureSource::GpuPreviousOrAlias,"gpu_alias_or_previous_frame"},{TextureSource::GpuResolve,"gpu_resolve"}}) {
            draw.textures[0].source=kind;Check(ToJson(draw).at("textures")[0]["source"]==name,"Typed texture changed source");
        }
        LiveRenderFrame detached;std::weak_ptr<const std::vector<uint8_t>> lifetime;
        const LiveOperation* operation=nullptr;const char* digest_address=nullptr;
        {
            LiveRenderFrame source;auto typed=Draw();lifetime=typed.device_state.owner;
            source.operations.push_back(Json{{"type","unused"}});source.operations.push_back(std::move(typed));
            operation=&source.operations[1];digest_address=operation->Draw()->device_state.hash.View().data();
            detached.operations=source.operations.Slice(1,2);detached.prefix_resources.emplace(2);
            const auto& ref=operation->Draw()->device_state;
            detached.prefix_resources->Append(ref.hash.View(),ref.owner,false);
            detached.prefix_resources->Append(ref.hash.View(),ref.owner,true);
            detached.retained_bytes=detached.prefix_resources->Seal();
            for(unsigned i=0;i<8192;++i)source.operations.push_back(Json{{"type","growth"},{"event",i}});
            Check(&detached.operations[0]==operation,"Producer growth moved typed operation");
            Check(detached.retained_bytes==74,"Cross namespace retained byte accounting changed");
        }
        Check(!lifetime.expired(),"Source destruction released typed immutable payload");
        LiveRenderFrame copied(detached),moved(std::move(copied));detached=LiveRenderFrame{};
        Check(&moved.operations[0]==operation&&operation->Draw()->device_state.hash.View().data()==digest_address,
            "Frame copy/move invalidated typed inline digest anchor");
        Check(moved.ResourceAt(operation->Draw()->device_state.hash.View())==lifetime.lock(),"Typed prefix payload changed owner");
        Check(moved.ResourceAt(operation->Draw()->device_state.hash.View(),true)==lifetime.lock(),"Typed prefix alias changed owner");
        Check(moved.operations[0].Kind()=="draw"&&moved.operations[0].Event()==UINT64_C(0x1FFFFFFFF)&&
            moved.operations[0].Frame()==18,"Typed envelope metadata narrowed or changed");
        std::atomic<bool> correct=true;
        auto export_many=[&] {for(unsigned i=0;i<1000;++i) {
            auto diagnostic=moved.operations[0].ToJson();if(diagnostic!=expected)correct=false;
            diagnostic["textures"][0]["fetch"][0]=0;
        }};
        std::thread thread(export_many);export_many();thread.join();
        Check(correct,"Concurrent cold exports mutated typed packet");
        moved=LiveRenderFrame{};Check(lifetime.expired(),"Last detached typed owner leaked");
        std::cout<<"Typed draw canonical serialization, malformed hashes, ownership/copy/move/slices, dual namespaces and concurrent cold exports passed\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
