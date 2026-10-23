#include "renderer/native/forward_pixel_catalog_compiler.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
using namespace gow3::renderer;
namespace fs=std::filesystem;
int main(int argc,char** argv)try {
    if(argc!=3)throw std::runtime_error("Usage: catalog-compiler-test source-cache-directory output-directory");
    fs::create_directories(argv[2]);NativeShaderCompiler canonical;
    size_t admission_checks=0;nlohmann::json report={{"programs",nlohmann::json::array()}};
    for(const auto digest:kForwardPixelCatalogSources) {
        std::ifstream source(fs::path(argv[1])/(std::string(digest)+".bin"),std::ios::binary|std::ios::ate);
        if(!source||source.tellg()<36||source.tellg()>16u*1024*1024)throw std::runtime_error("Catalog source extent");
        std::vector<uint8_t> bytes(size_t(source.tellg()));source.seekg(0);
        if(!source.read(reinterpret_cast<char*>(bytes.data()),bytes.size()))throw std::runtime_error("Catalog source read");
        const auto check=[&](bool ok){++admission_checks;if(!ok)throw std::runtime_error("Catalog admission fixture failed");};
        for(const auto key:{std::array<uint32_t,6>{3,1,1,4,0,0},std::array<uint32_t,6>{3,2,1,4,0,0},std::array<uint32_t,6>{1,2,1,4,0,0}}) {
            check(ForwardPixelCatalogAllowed(digest,key));check(!ForwardPixelCatalogAllowed("unknown-source",key));
            for(size_t i=0;i<key.size();++i){auto changed=key;changed[i]+=17;check(!ForwardPixelCatalogAllowed(digest,changed));}
        }
        const auto program=TryPrepareForwardPixelCatalog(canonical,bytes);
        if(!program)throw std::runtime_error("Actual color catalog source/compiler proof refused: "+std::string(digest));
        auto changed=bytes;changed.back()^=1;check(!TryPrepareForwardPixelCatalog(canonical,changed));
        std::ofstream output(fs::path(argv[2])/(std::string(digest)+".fsi.spv"),std::ios::binary);
        output.write(reinterpret_cast<const char*>(program->spirv.data()),program->spirv.size()*4);
        if(!output)throw std::runtime_error("Cannot write catalog module");
        report["programs"].push_back({{"source",program->source_sha256},{"baseline",program->compiler_version},
            {"variant",program->variant_version},{"baseline_hlsl",program->baseline_hlsl_sha256},
            {"modified_hlsl",program->modified_hlsl_sha256},{"spirv",program->compiled_spirv_sha256},
            {"words",program->spirv.size()},{"control_instructions",program->control_instructions},
            {"specialization_mask",program->specialization_mask},{"output_mask",program->output_mask}});
    }
    report["passed"]=true;report["source_count"]=kForwardPixelCatalogSources.size();report["admission_checks"]=admission_checks;
    std::ofstream json(fs::path(argv[2])/"compiler.json");json<<report.dump(2)<<'\n';if(!json)throw std::runtime_error("Cannot write catalog compiler report");
    std::cout<<"PASS: sources="<<kForwardPixelCatalogSources.size()<<" admission_checks="<<admission_checks<<" baseline="<<canonical.CacheVersion()<<" variant="<<ForwardPixelCatalogCompilerVersion()<<'\n';
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
