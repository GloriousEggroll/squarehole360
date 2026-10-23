#include "live_draw_json.h"
#include <cstdio>

namespace gow3::renderer {
namespace {
using Json=nlohmann::json;
std::string Hex(uint32_t value) {
    char text[9];std::snprintf(text,sizeof(text),"%08x",value);return text;
}
Json Hash(const std::optional<CanonicalResourceRef>& resource) {
    return resource?Json(std::string(resource->hash.View())):Json(nullptr);
}
Json Geometry(const TypedGeometry& source) {
    Json out=Json::object();out["kind"]=source.kind;out["address"]=Hex(source.address);
    out["bytes"]=source.bytes;out["sha256"]=source.payload.hash.View();
    if(source.gpu_memory)out["source"]="gpu_memory";
    if(source.fetch_slot)out["fetch_slot"]=*source.fetch_slot;
    if(source.endian)out["endian"]=*source.endian;
    return out;
}
}
Json ToJson(const TypedSurface& source) {
    Json out=Json::object();out["address"]=Hex(source.address);out["depth"]=source.depth;
    out["width"]=source.width;out["height"]=source.height;out["surface_info"]=source.surface_info;
    out["format_info"]=source.format_info;out["edram_base"]=source.edram_base;out["format"]=source.format;
    return out;
}
Json ToJson(const TypedTextureIdentity& source) {
    Json out=Json::object();out["base"]=Hex(source.base);out["format"]=source.format;
    out["width"]=source.width;out["height"]=source.height;out["pitch"]=source.pitch;
    out["tiled"]=source.tiled;out["endian"]=source.endian;out["dimension"]=source.dimension;
    out["packed"]=source.packed;out["layers"]=source.layers;return out;
}
Json ToJson(const TypedLiveQuery& source) {
    // Same keys and number kinds as GearsLivePlan's former JSON envelope.
    Json surfaces=Json::object();
    for(unsigned slot=0;slot<source.surfaces.size();++slot)
        if(source.surfaces[slot])surfaces[std::to_string(slot+6)]=ToJson(*source.surfaces[slot]);
    Json out=Json::object();out["event"]=source.event;out["frame"]=source.frame;
    out["type"]=source.begin?"query_begin":"query_end";out["query_handle"]=source.handle;
    out["record_index"]=uint32_t(source.begin);out["report_address"]=source.report_address;
    out["query_generation"]=source.generation;out["surfaces"]=std::move(surfaces);
    return out;
}
Json ToJson(const TypedLiveDraw& source) {
    Json out=Json::object();out["event"]=source.event;out["frame"]=source.frame;
    out["type"]="draw";out["function"]=DrawFunctionName(source.function);out["args"]=source.args;
    out["device_state"]=(source.lazy_device_state?source.lazy_device_state->Digest():source.device_state.hash.View());out["vertex_shader"]=Hash(source.vertex_shader);
    out["pixel_shader"]=Hash(source.pixel_shader);
    if(source.rectangle_list)out["vertex_input"]="affine_float4_rectangle";
    out["vertex_slots"]=source.vertex_fetch_slots;
    out["geometry"]=Json::array();for(const auto& geometry:source.geometry)out["geometry"].push_back(Geometry(geometry));
    out["up_geometry"]=Json::array();for(const auto& geometry:source.up_geometry)out["up_geometry"].push_back(Geometry(geometry));
    out["surfaces"]=Json::object();for(unsigned i=0;i<source.surfaces.size();++i)
        if(source.surfaces[i])out["surfaces"][std::to_string(i+6)]=ToJson(*source.surfaces[i]);
    out["textures"]=Json::array();
    for(const auto& texture:source.textures) {
        Json binding=Json::object();binding["slot"]=texture.slot;binding["fetch"]=texture.fetch;
        switch(texture.source) {
            case TextureSource::CpuSnapshot:binding["source"]="cpu_snapshot";break;
            case TextureSource::GpuPreviousOrAlias:binding["source"]="gpu_alias_or_previous_frame";break;
            case TextureSource::GpuResolve:binding["source"]="gpu_resolve";break;
        }
        binding["base_payload"]=Hash(texture.base_payload);binding["mip_payload"]=Hash(texture.mip_payload);
        if(texture.identity)binding["identity"]=ToJson(*texture.identity);
        out["textures"].push_back(std::move(binding));
    }
    return out;
}
} // namespace gow3::renderer
