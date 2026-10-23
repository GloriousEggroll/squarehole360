#include "renderer/native/opaque_color_overwrite_compiler.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace gow3::renderer;
int main(int argc,char** argv) try {
    if(argc!=3)throw std::runtime_error("Usage: compiler-test audited49f2-source-container candidate-output.spv");
    std::ifstream source(argv[1],std::ios::binary|std::ios::ate);
    if(!source||source.tellg()<36||source.tellg()>16u*1024*1024)throw std::runtime_error("Invalid source container extent");
    std::vector<uint8_t> bytes(size_t(source.tellg()));source.seekg(0);
    if(!source.read(reinterpret_cast<char*>(bytes.data()),bytes.size()))throw std::runtime_error("Truncated source container");
    NativeShaderCompiler canonical;
    const auto program=TryPrepareOpaqueColorOverwrite(canonical,bytes);
    if(!program)throw std::runtime_error("Audited candidate preparation refused");
    std::ofstream output(argv[2],std::ios::binary);
    output.write(reinterpret_cast<const char*>(program->spirv.data()),program->spirv.size()*4);
    if(!output)throw std::runtime_error("Cannot write candidate module");
    std::cout<<"PASS: source="<<program->source_sha256<<" baseline="<<program->compiler_version
        <<" variant="<<program->variant_version<<" modified_common="<<program->modified_common_sha256
        <<" spirv="<<program->compiled_spirv_sha256<<" words="<<program->spirv.size()<<'\n';
} catch(const std::exception& exception) {std::cerr<<exception.what()<<'\n';return 1;}
