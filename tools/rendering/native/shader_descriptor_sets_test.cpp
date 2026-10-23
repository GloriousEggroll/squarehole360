#include "renderer/native/shader_descriptor_sets.h"
#include "readonly_depth_attachment_frag_spv.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>

using Words=std::vector<uint32_t>;
using gow3::renderer::NativeDeclaredDescriptorSets;
unsigned checks=0;
void Check(bool ok,const char* reason) {
    ++checks;
    if(!ok)throw std::runtime_error(reason);
}
uint32_t Mask(const Words& w) {
    return NativeDeclaredDescriptorSets({reinterpret_cast<const uint8_t*>(w.data()),w.size()*4});
}
// Declaration fragments exercise reflection, not full SPIR-V validation.
Words Base() {return {0x07230203,0x00010500,0,4096,0,(3u<<16)|14,5348,1,(5u<<16)|15,4,1,0x6e69616d,0};}
void Descriptor(Words& w,uint32_t id,uint32_t set,uint32_t storage=0) {
    w.insert(w.end(),{(4u<<16)|71,id,34,set,(4u<<16)|71,id,33,0,(4u<<16)|59,2,id,storage});
}
int main(int argc,char** argv)try {
    auto w=Base();Check(Mask(w)==0,"Descriptor-free module must bind no sets");
    Descriptor(w,20,0);Descriptor(w,21,3,2);Descriptor(w,22,4,12);
    Check(Mask(w)==25,"Every declared descriptor storage class must be included");
    Descriptor(w,23,1);Check(Mask(w)==27,"Unused globals must remain included");
    auto bad=w;bad.insert(bad.end(),{(4u<<16)|59,2,30,0});
    Check(Mask(bad)==UINT32_MAX,"Undecorated descriptor must retain all sets");
    bad=w;bad.insert(bad.end(),{(4u<<16)|71,20,34,2});
    Check(Mask(bad)==UINT32_MAX,"Conflicting sets must retain all");
    bad=Base();Descriptor(bad,20,32);Check(Mask(bad)==UINT32_MAX,"Out-of-range set must retain all");
    bad=Base();Descriptor(bad,20,31);Check(Mask(bad)==0x80000000u,"Highest representable set");
    bad=Base();bad.insert(bad.end(),{(4u<<16)|59,2,20,999});
    Check(Mask(bad)==UINT32_MAX,"Unknown variable storage must retain all");
    for(auto op:{73u,74u,75u,332u,5632u,5633u}) {
        bad=w;bad.push_back((1u<<16)|op);Check(Mask(bad)==UINT32_MAX,"Indirect decorations must retain all");
    }
    bad=Base();bad.insert(bad.end(),{(3u<<16)|10,0x444142,0});
    Check(Mask(bad)==UINT32_MAX,"Unknown extensions must retain all");
    bad=Base();bad.insert(bad.end(),{(3u<<16)|11,9,0});
    Check(Mask(bad)==UINT32_MAX,"Unknown instruction sets must retain all");
    bad=Base();bad[9]=5;Check(Mask(bad)==UINT32_MAX,"Non-raster execution model");
    for(size_t size=0;size<20;++size)
        Check(NativeDeclaredDescriptorSets({reinterpret_cast<const uint8_t*>(w.data()),size})==UINT32_MAX,"Short header");
    for(auto index:{0u,1u,3u}) {bad=w;bad[index]=0;Check(Mask(bad)==UINT32_MAX,"Invalid header");}
    bad=w;bad[5]=14;Check(Mask(bad)==UINT32_MAX,"Zero-length instruction");
    bad=w;bad.back()=999;Check(Mask(bad)==UINT32_MAX,"Invalid storage");
    bad=w;bad.pop_back();Check(Mask(bad)==UINT32_MAX,"Truncated instruction");
    bad=Base();bad.resize(8);Check(Mask(bad)==UINT32_MAX,"Missing entry point");
    bad=Base();bad.erase(bad.begin()+5,bad.begin()+8);Check(Mask(bad)==UINT32_MAX,"Missing memory model");
    bad=Base();for(unsigned i=0;i<128;++i)Descriptor(bad,20+i,i%5);
    Check(Mask(bad)==31,"Bounded maximum descriptor count");
    Descriptor(bad,200,0);Check(Mask(bad)==UINT32_MAX,"Reflection capacity must retain all");
    std::vector<uint8_t> unaligned(w.size()*4+1);
    std::memcpy(unaligned.data()+1,w.data(),w.size()*4);
    Check(NativeDeclaredDescriptorSets({unaligned.data()+1,w.size()*4})==27,"Unaligned input");
    Check(NativeDeclaredDescriptorSets({reinterpret_cast<const uint8_t*>(readonly_depth_attachment_frag_spv),
          sizeof(readonly_depth_attachment_frag_spv)})==0,"Actual D32 fragment uses only push constants and device addresses");
    size_t modules=0;std::map<uint32_t,size_t> masks;
    for(int i=1;i<argc;++i)for(const auto& file:std::filesystem::directory_iterator(argv[i])) {
        if(file.path().extension()!=".spv")continue;
        std::ifstream f(file.path(),std::ios::binary|std::ios::ate);
        Check(bool(f)&&f.tellg()>0&&f.tellg()<4*1024*1024,"Bounded archive module");
        std::vector<uint8_t> bytes(size_t(f.tellg()));f.seekg(0);
        Check(bool(f.read(reinterpret_cast<char*>(bytes.data()),bytes.size())),"Complete archive module");
        const auto mask=NativeDeclaredDescriptorSets(bytes);++modules;++masks[mask];
        const auto name=file.path().filename().string();
        if(name.starts_with("13b59")||name.starts_with("3f771")||name.starts_with("a14c"))
            Check(mask==0,"Actual pinned vertex has no descriptor declarations");
        if(name.ends_with(".fsi.spv")&&(name.starts_with("a3c9")||name.starts_with("e413")||name.starts_with("49f2")))
            Check(mask==25,"Actual material requires texture, sampler and EDRAM sets");
    }
    for(auto [mask,count]:masks)std::cout<<"mask="<<mask<<" modules="<<count<<'\n';
    std::cout<<"PASS checks="<<checks<<" actual_archive_modules="<<modules<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
