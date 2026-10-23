#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/string/buffer.h>

#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace {
struct Instruction {
    spv::Op opcode;
    std::vector<uint32_t> operands;
};
void Require(bool value,const char* message) {
    if(!value)throw std::runtime_error(message);
}
}

int main() try {
    // c7 -> r1; sample a stacked texture -> r2; r2 -> color0. The shader
    // contains both unsigned and signed sampling paths, selected at runtime.
    constexpr std::array<uint32_t,12> ucode{
        0x0,0x3001c400,0x20000004,0xc80f0001,0x0,0x22070700,
        0x90082021,0x0f1ff688,0x8000,0xc80f8000,0x0,0xe2020200};
    rex::graphics::Shader shader(rex::graphics::xenos::ShaderType::kPixel,0,
        ucode.data(),ucode.size(),std::endian::native);
    rex::string::StringBuffer disassembly;
    shader.AnalyzeUcode(disassembly);
    rex::graphics::SpirvShaderTranslator translator(
        rex::graphics::SpirvShaderTranslator::Features(false),false,false,false);
    auto* translation=shader.GetOrCreateTranslation(
        translator.GetDefaultPixelShaderModification(64));
    Require(translator.TranslateAnalyzedShader(*translation),"Stacked fixture translation failed");
    const auto& bytes=translation->translated_binary();
    Require(bytes.size()%4==0&&bytes.size()>=20,"Invalid translated SPIR-V");
    std::vector<uint32_t> words(bytes.size()/4);
    std::memcpy(words.data(),bytes.data(),bytes.size());
    std::unordered_map<uint32_t,Instruction> values;
    std::vector<Instruction> additions;
    for(size_t i=5;i<words.size();) {
        const auto count=words[i]>>16;
        const auto op=spv::Op(words[i]&65535);
        Require(count&&i+count<=words.size(),"Malformed translated instruction");
        // These arithmetic instructions all have type, result, then operands.
        if(op==spv::OpFSub||op==spv::OpVectorTimesScalar||op==spv::OpFAdd) {
            Require(count==5,"Unexpected arithmetic instruction size");
            Instruction instruction{op,{words[i+3],words[i+4]}};
            values.emplace(words[i+2],instruction);
            if(op==spv::OpFAdd)additions.push_back(std::move(instruction));
        }
        i+=count;
    }
    unsigned interpolations=0,checks=0;
    for(const auto& add:additions) {
        auto scaled=values.find(add.operands[1]);
        if(scaled==values.end()||scaled->second.opcode!=spv::OpVectorTimesScalar)continue;
        auto difference=values.find(scaled->second.operands[0]);
        if(difference==values.end()||difference->second.opcode!=spv::OpFSub)continue;
        const auto upper=difference->second.operands[0];
        const auto lower=difference->second.operands[1];
        // Interpret the actual emitted arithmetic graph with independent
        // sample values. Equal samples would hide the wrong endpoint bug.
        for(const auto endpoints:std::array<std::array<float,2>,3>{{{0,1},{0.2f,0.4f},{-2,3}}}) {
            for(float t:std::array<float,3>{0,0.5f,1}) {
                const float lower_value=endpoints[0],upper_value=endpoints[1];
                Require(add.operands[0]==lower||add.operands[0]==upper,
                    "Unrecognized interpolation origin");
                const float origin=add.operands[0]==lower?lower_value:upper_value;
                const float actual=origin+(upper_value-lower_value)*t;
                const float expected=lower_value*(1-t)+upper_value*t;
                Require(std::abs(actual-expected)<1e-6f,
                    "Translated stacked sampling does not interpolate between the two layers");
                ++checks;
            }
        }
        ++interpolations;
    }
    Require(interpolations==2,"Expected signed and unsigned stacked interpolation paths");
    std::cout<<"PASS: "<<checks<<" translated stacked-sampling endpoint/midpoint checks\n";
} catch(const std::exception& error) {
    std::cerr<<error.what()<<'\n';
    return 1;
}
