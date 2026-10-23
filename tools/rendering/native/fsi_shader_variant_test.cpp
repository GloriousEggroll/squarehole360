#include "renderer/native/fsi_shader_variant.h"
#include "renderer/native/cache_bookkeeping.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace gow3::renderer;
namespace fs=std::filesystem;
namespace {
using Words=std::vector<uint32_t>;
size_t checks=0;
void Check(bool condition,const char* text) {
    if(!condition)throw std::runtime_error(text);
    ++checks;
}
Words Read(const fs::path& path) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    if(!f||f.tellg()<20||f.tellg()%4)throw std::runtime_error("Bad fixture SPIR-V: "+path.string());
    Words words(size_t(f.tellg())/4);f.seekg(0);
    if(!f.read(reinterpret_cast<char*>(words.data()),words.size()*4))throw std::runtime_error("Fixture read failed");
    return words;
}
std::vector<size_t> Offsets(const Words& words,uint32_t opcode) {
    std::vector<size_t> result;
    for(size_t at=5;at<words.size();at+=words[at]>>16)
        if((words[at]&65535)==opcode)result.push_back(at);
    return result;
}
// Independent preservation oracle: physically remove only the two audited
// OpMemoryBarrier instructions. Every other original word must remain equal.
Words Oracle(const Words& original) {
    Words expected(original.begin(),original.begin()+5);unsigned removed=0;
    for(size_t at=5;at<original.size();) {
        const size_t count=original[at]>>16;
        if((original[at]&65535)==225) {Check(count==3,"Oracle barrier extent");++removed;}
        else expected.insert(expected.end(),original.begin()+at,original.begin()+at+count);
        at+=count;
    }
    Check(removed==2,"Oracle requires exactly two barriers");return expected;
}
void Reject(const Words& words) {
    bool rejected=false;
    try{(void)TransformNativeFsiImplicitBarriers(words);}
    catch(const std::invalid_argument&){rejected=true;}
    if(!rejected)throw std::runtime_error("Unsafe/malformed shader variant accepted after check "+std::to_string(checks));
    ++checks;
}
void Negative(const Words& valid) {
    const auto original=valid;
    auto test=[&](auto modify){auto input=valid;input.reserve(input.size()+64);modify(input);Reject(input);};
    test([](auto& w){w[0]=0;});
    test([](auto& w){w[1]=0x10700;});
    test([](auto& w){w[4]=1;});
    test([](auto& w){w[5]&=65535;});
    test([](auto& w){w.pop_back();});
    test([](auto& w){w[Offsets(w,14).front()+2]=3;});
    test([](auto& w){w[Offsets(w,15).front()+1]=0;});
    test([](auto& w){for(auto at:Offsets(w,16))if(w[at+2]==5366){w[at+2]=5367;return;}});
    test([](auto& w){for(auto at:Offsets(w,71))if(w[at+2]==23){w[at+2]=24;return;}});
    test([](auto& w){for(auto at:Offsets(w,71))if(w[at+2]==34&&w[at+3]==4){w[at+3]=3;return;}});
    test([](auto& w){const auto at=Offsets(w,225).front();w[at+1]=UINT32_MAX;});
    test([](auto& w){const auto id=w[Offsets(w,225).front()+2];for(auto at:Offsets(w,43))if(w[at+2]==id){w[at+3]^=0x800;return;}});
    test([](auto& w){const auto at=Offsets(w,225).front();w.erase(w.begin()+at,w.begin()+at+3);});
    test([](auto& w){const auto at=Offsets(w,225).front();w.insert(w.begin()+at+3,0x10000|5364);});
    // Closed whitelist covers extension write families, not just core atomics.
    for(auto op:{63u,64u,99u,227u,242u,318u,319u,259u,5614u,5615u,6035u,5359u,5360u,5294u,65535u})
        test([&](auto& w){w.insert(w.begin()+5,0x10000|op);});
    test([](auto& w){w[Offsets(w,17).front()+1]=6033;});
    test([](auto& w){const auto at=Offsets(w,11).front();w[at+2]^=1;});
    for(auto op:{35u,51u,9999u})
        test([&](auto& w){w[Offsets(w,12).front()+4]=op;});
    test([](auto& w){w[Offsets(w,12).front()+3]=UINT32_MAX;});
    // A second, genuinely writable descriptor is forbidden, regardless of
    // whether optimizations happened to leave an actual store to it.
    test([](auto& w){
        uint32_t ptr=0;for(auto at:Offsets(w,59))if(w[at+3]==12){ptr=w[at+1];break;}
        const uint32_t id=w[3]++;const std::array<uint32_t,4> variable{(4u<<16)|59u,ptr,id,12};
        w.insert(w.begin()+Offsets(w,54).front(),variable.begin(),variable.end());
    });
    // Root classification must reject a physical-pointer store, even though
    // physical-address constant reads are allowed by the native ABI.
    test([](auto& w){
        uint32_t physical_type=0;for(auto at:Offsets(w,32))if(w[at+2]==5349){physical_type=w[at+1];break;}
        uint32_t physical=0;
        for(auto op:{61u,124u})for(auto at:Offsets(w,op))if(w[at+1]==physical_type)physical=w[at+2];
        Check(physical!=0,"Fixture needs actual physical constant pointer");
        w[Offsets(w,62).front()+1]=physical;
    });
    Reject(TransformNativeFsiImplicitBarriers(valid));
    Check(valid==original,"Rejected transformations mutated source bytes");
}
}
int main(int argc,char** argv)try {
    if(argc==4&&std::string_view(argv[1])=="--transform") {
        const auto words=TransformNativeFsiImplicitBarriers(Read(argv[2]));
        std::ofstream output(argv[3],std::ios::binary);
        if(!output.write(reinterpret_cast<const char*>(words.data()),words.size()*4))
            throw std::runtime_error("Unable to write validated variant fixture");
        return 0;
    }
    if(argc<3)throw std::runtime_error("Usage: fsi-shader-variant-test original-dir python-oracle-dir [fresh-production.spv ...]");
    size_t modules=0;Words negative_fixture;
    for(const auto& item:fs::directory_iterator(argv[1])) {
        if(!item.is_regular_file()||!item.path().filename().string().ends_with(".fsi.spv"))continue;
        const auto input=Read(item.path());const auto transformed=TransformNativeFsiImplicitBarriers(input);
        Check(transformed==Read(fs::path(argv[2])/item.path().filename()),"C++/Python variant mismatch");
        Check(transformed==Oracle(input),"Transform changed a non-barrier word");++modules;
    }
    Check(modules!=0,"No actual native FSI modules checked");
    for(int i=3;i<argc;++i) {
        auto words=Read(argv[i]);Check(TransformNativeFsiImplicitBarriers(words)==Oracle(words),"Fresh native module oracle mismatch");
        if(i==3)negative_fixture=std::move(words);
    }
    if(!negative_fixture.empty()) {
        Negative(negative_fixture);
        // GLSL.std.450 Cos has one value input and no external effects. It
        // occurs in a current-version user shader beyond the firing capture.
        auto cosine=negative_fixture;bool replaced=false;
        for(auto at:Offsets(cosine,12))if((cosine[at]>>16)==6) {
            cosine[at+4]=14;replaced=true;break;
        }
        Check(replaced,"Fixture needs one-operand pure GLSL instruction");
        Check(TransformNativeFsiImplicitBarriers(cosine)==Oracle(cosine),
              "Audited cosine changed non-barrier words");
    }
    std::map<ShaderCacheIdentity,int,ShaderCacheLess> keys;
    const std::string digest(64,'a');
    keys.emplace(ShaderCacheIdentity{digest,true},1);
    keys.emplace(ShaderCacheIdentity{digest,true,NativeShaderVariant::ImplicitCoherentInterlockV1},2);
    keys.emplace(ShaderCacheIdentity{"depth-only",true,NativeShaderVariant::ImplicitCoherentInterlockV1},3);
    Check(keys.size()==3,"Variant module identities collided");
    Check(keys.find(ShaderCacheLookup{digest,true})->second==1,"Default module lookup changed");
    Check(keys.find(ShaderCacheLookup{digest,true,NativeShaderVariant::ImplicitCoherentInterlockV1})->second==2,"Implicit module lookup changed");
    Check(keys.find(ShaderCacheLookup{"depth-only",true})==keys.end(),"Builtin variant identity collided");
    std::cout<<"PASS: "<<modules<<" actual native FSI modules byte-identical to Python, "<<checks<<" checks including writable-resource refusals and module identities\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
