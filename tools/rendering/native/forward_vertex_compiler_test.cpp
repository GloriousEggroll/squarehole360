#include "renderer/native/forward_vertex_compiler.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace gow3::renderer;
int main(int argc,char** argv) try {
    if(argc!=3)throw std::runtime_error("Usage: compiler-test auditedf7db-source-container candidate-output.spv");
    std::ifstream source(argv[1],std::ios::binary|std::ios::ate);
    if(!source||source.tellg()<36||source.tellg()>16u*1024*1024)throw std::runtime_error("Invalid source container extent");
    std::vector<uint8_t> bytes(size_t(source.tellg()));source.seekg(0);
    if(!source.read(reinterpret_cast<char*>(bytes.data()),bytes.size()))throw std::runtime_error("Truncated source container");
    size_t admission_checks=0;
    const auto check=[&](bool passed) {
        ++admission_checks;if(!passed)throw std::runtime_error("Forward-VS exact admission fixture failed");
    };
    for(const auto key:{std::array<uint32_t,6>{3,1,1,4,0,0},std::array<uint32_t,6>{3,2,1,4,0,0},std::array<uint32_t,6>{1,2,1,4,0,0}}) {
        check(ForwardVertexAllowed(kForwardVertexSource,key));
        check(!ForwardVertexAllowed("unknown-vertex",key));
        for(size_t field=0;field<key.size();++field) {
            auto unknown=key;unknown[field]+=17;
            check(!ForwardVertexAllowed(kForwardVertexSource,unknown));
        }
    }
    NativeShaderCompiler canonical;
    const auto program=TryPrepareForwardVertex(canonical,bytes);
    if(!program)throw std::runtime_error("Audited candidate preparation refused");
    std::ofstream output(argv[2],std::ios::binary);
    output.write(reinterpret_cast<const char*>(program->spirv.data()),program->spirv.size()*4);
    if(!output)throw std::runtime_error("Cannot write candidate module");
    std::cout<<"PASS: source="<<program->source_sha256<<" baseline="<<program->compiler_version
        <<" variant="<<program->variant_version<<" baseline_hlsl="<<program->baseline_hlsl_sha256
        <<" terminal="<<program->terminal<<" padding_clauses="<<program->padding_clauses
        <<" control_instructions="<<program->control_instructions<<" admission_checks="<<admission_checks
        <<" modified_hlsl="<<program->modified_hlsl_sha256
        <<" spirv="<<program->compiled_spirv_sha256<<" words="<<program->spirv.size()<<'\n';
} catch(const std::exception& exception) {std::cerr<<exception.what()<<'\n';return 1;}
