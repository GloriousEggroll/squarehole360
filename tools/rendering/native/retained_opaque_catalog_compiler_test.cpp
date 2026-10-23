#include "renderer/native/shader_recompiler_pch.h"
#include "shader_recompiler.h"
#include "renderer/native/content_hash.h"
#include "renderer/native/retained_opaque_catalog.h"
#include "renderer/native/retained_opaque_catalog_compiler.h"
#include "renderer/native/retained_opaque_writer.h"
#include "renderer/native/native_packed_opaque_shader.h"
#include "native_shader_embedded.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstring>

using namespace gow3::renderer;
namespace {
void Check(bool b, const char* message) { if (!b) throw std::runtime_error(message); }
std::vector<uint8_t> Read(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    const auto n=f.tellg();
    Check(bool(f)&&n>=0&&n<=16*1024*1024,"Invalid bounded input");
    std::vector<uint8_t> bytes(static_cast<size_t>(n)); f.seekg(0);
    Check(bool(f.read(reinterpret_cast<char*>(bytes.data()),n)),"Truncated input");
    return bytes;
}
void Write(const std::filesystem::path& path, std::span<const uint8_t> bytes) {
    std::ofstream f(path,std::ios::binary);
    Check(bool(f.write(reinterpret_cast<const char*>(bytes.data()),bytes.size())),"Cannot retain proof bytes");
}
void Write(const std::filesystem::path& path, std::string_view s) {
    Write(path,{reinterpret_cast<const uint8_t*>(s.data()),s.size()});
}
std::span<const uint8_t> Bytes(const std::vector<uint32_t>& words) {
    return {reinterpret_cast<const uint8_t*>(words.data()),words.size()*sizeof(uint32_t)};
}
}
int main(int argc,char** argv) try {
    Check(argc==3,"Usage: retained-catalog-compiler-test inspected-directory NEW-output-directory");
    const std::filesystem::path inspected=argv[1], output=argv[2];
    Check(!std::filesystem::exists(output),"Use a fresh output directory");
    NativeShaderCompiler compiler(output/"canonical");
    nlohmann::json result{{"scope","Actual three-source startup compiler and byte-exact canonical cache checks; no GPU execution or performance claim"},
        {"canonical_version",compiler.CacheVersion()},
        {"catalog_version",NativeRetainedOpaqueCatalogCompilerVersion()}, {"sources",nlohmann::json::array()}};
    Check(compiler.CacheVersion()=="1c697af73d13496ae2424cfd4e65de2856f16bd612bd715b92160403c8bd762b","Canonical compiler namespace changed");
    Check(NativeRetainedOpaqueCatalogCompilerVersion()!=compiler.CacheVersion(),"Derived namespace reused canonical identity");
    for (const auto& pair:kRetainedOpaqueCatalogPairs) {
        const std::string ps(pair.pixel), vs(pair.vertex);
        const auto pixel=Read(inspected/(ps+".pixel.bin")), vertex=Read(inspected/(vs+".vertex.bin"));
        Check(ContentHash(pixel)==ps&&ContentHash(vertex)==vs,"Pinned source digest mismatch");
        const auto canonical=compiler.Get(pixel,NativeShaderMode::FragmentInterlock);
        const auto canonical_vertex=compiler.Get(vertex,NativeShaderMode::Raster);
        const auto pixel_cache=output/"canonical"/compiler.CacheVersion()/(ps+".fsi.g3spv");
        const auto vertex_cache=output/"canonical"/compiler.CacheVersion()/(vs+".g3spv");
        const auto saved_pixel=Read(pixel_cache), saved_vertex=Read(vertex_cache);
        const auto program=TryPrepareNativeRetainedOpaqueCatalog(compiler,pixel);
        Check(program&&program->source_sha256==ps&&program->vertex_sha256==vs&&
            program->baseline_version==compiler.CacheVersion()&&program->variant_version==NativeRetainedOpaqueCatalogCompilerVersion()&&
            program->specialization_mask==canonical->specialization_mask&&program->specialization_mask==1794&&
            program->spirv!=canonical->spirv&&program->spirv_sha256==ContentHash(Bytes(program->spirv)),"Derived source/ABI/payload invalid");
        const auto again=TryPrepareNativeRetainedOpaqueCatalog(compiler,pixel);
        Check(again&&again->spirv==program->spirv&&again->variant_version==program->variant_version,"Startup compilation not repeatable");
        Check(!TryPrepareNativeRetainedOpaqueCatalog(compiler,vertex),"Vertex accepted as a catalog pixel");
        auto mutated=pixel; mutated.back()^=1;
        Check(!TryPrepareNativeRetainedOpaqueCatalog(compiler,mutated),"Mutated source accepted");
        Check(!TryPrepareNativeRetainedOpaqueCatalog(compiler,std::span<const uint8_t>(pixel).first(35)),"Short source accepted");
        Check(Read(pixel_cache)==saved_pixel&&Read(vertex_cache)==saved_vertex&&
            compiler.Get(pixel,NativeShaderMode::FragmentInterlock)->spirv==canonical->spirv&&
            compiler.Get(vertex,NativeShaderMode::Raster)->spirv==canonical_vertex->spirv,"Canonical cache bytes changed");
        // Independently retain the exact canonical generator output and closed
        // adapter output used by the authenticated producer for source review.
        std::vector<uint32_t> aligned((pixel.size()+3)/4);
        std::memcpy(aligned.data(),pixel.data(),pixel.size());
        ShaderRecompiler recompiler;
        recompiler.recompile(reinterpret_cast<const uint8_t*>(aligned.data()),kNativeShaderCommon);
        const auto adapted=TryNativePackedOpaqueSource(recompiler.out,kNativeShaderCommon);
        Check(bool(adapted)&&recompiler.specConstantsMask==program->specialization_mask,"Canonical source adapter refused");
        Write(output/(ps+".canonical.hlsl"),recompiler.out);
        Write(output/(ps+".candidate.hlsl"),*adapted);
        Write(output/(ps+".candidate.spv"),Bytes(program->spirv));
        Write(output/(ps+".canonical.spv"),Bytes(canonical->spirv));
        Write(output/(vs+".canonical.spv"),Bytes(canonical_vertex->spirv));
        result["sources"].push_back({{"pixel",ps},{"vertex",vs},{"mask",program->specialization_mask},
            {"module_bytes",Bytes(program->spirv).size()},{"candidate_sha256",program->spirv_sha256},
            {"canonical_cache_unchanged",true},{"repeat_exact",true},{"wrong_stage_mutated_truncated_refused",true}});
    }
    result["passed"]=true;
    Write(output/"startup-compiler-proof.json",result.dump(2)+"\n");
    std::cout<<result.dump(2)<<'\n';
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
