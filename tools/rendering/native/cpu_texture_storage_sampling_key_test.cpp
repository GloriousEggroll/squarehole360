#include "renderer/native/cpu_texture_storage_sampling_key.h"
#include "renderer/native/texture_data.h"
#include "renderer/native/cpu_image_slot_lookup.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>
using namespace gow3::renderer;
struct Captured {std::array<uint32_t,6> fetch;std::string_view base,mips;uint64_t version;size_t base_bytes,mip_bytes;};
#include "cpu_texture_storage_sampling_cases.inc"
uint64_t checks=0,decoded_bytes=0,decoded_pairs=0;
void Check(bool value,const char* message) {++checks;if(!value)throw std::runtime_error(message);}
auto Level(const TextureLevel& l) {return std::tuple(l.mip,l.layer,l.width,l.height,l.depth,l.pitch_blocks,l.slice_rows,l.x,l.y,l.z,l.offset,l.mips);}
auto Layout(const TextureLayout& t) {return std::tuple(t.width,t.height,t.depth,t.layers,t.dimension,t.format,t.endian,t.min_mip,t.max_mip,t.base_address,t.mip_address,t.tiled,t.block.block_width,t.block.block_height,t.block.bytes,t.block.components);}
std::vector<uint8_t> Payload(size_t bytes,uint32_t seed) {
    std::vector<uint8_t> data(bytes);
    for(auto& byte:data){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;byte=uint8_t(seed);}
    return data;
}
void Compare(const std::array<uint32_t,6>& a,const std::array<uint32_t,6>& b,
             std::span<const uint8_t> base,std::span<const uint8_t> mips) {
    const auto x=DescribeTexture(a),y=DescribeTexture(b);
    Check(Layout(x)==Layout(y),"storage layout changed");Check(x.levels.size()==y.levels.size(),"level count changed");
    for(size_t i=0;i<x.levels.size();++i)Check(Level(x.levels[i])==Level(y.levels[i]),"level layout changed");
    for(bool region:{false,true})Check(TextureMemoryRanges(x,region)==TextureMemoryRanges(y,region),"sampled footprint changed");
    const auto dx=DecodeTexture(a,base,mips),dy=DecodeTexture(b,base,mips);
    Check(dx.layout.fetch==a&&dy.layout.fetch==b,"original fetch was replaced in decode");
    Check(dx.levels.size()==dy.levels.size(),"decoded level count changed");
    for(size_t i=0;i<dx.levels.size();++i) {
        Check(Level(dx.levels[i].layout)==Level(dy.levels[i].layout),"decoded mip/layer changed");
        Check(dx.levels[i].bytes==dy.levels[i].bytes,"decoded bytes changed");decoded_bytes+=dx.levels[i].bytes.size();
    }
    ++decoded_pairs;
}
std::optional<std::string> Digest(std::string_view x) {return x.empty()?std::nullopt:std::optional(std::string(x));}
CpuImageIdentity Key(const Captured& c,bool enabled) {return {CpuTextureStorageSamplingKey(c.fetch,enabled),Digest(c.base),Digest(c.mips),c.version};}
int main() try {
    std::set<CpuImageIdentity,CpuImageLess> raw_keys,keys,bias_keys;
    for(const auto& c:captured) {
        raw_keys.insert(Key(c,false));keys.insert(Key(c,true));
        auto bias=Key(c,false);bias.fetch[4]&=~0x003ff000u;bias_keys.insert(bias);
        Check(CpuTextureStorageSamplingKey(c.fetch,false)==c.fetch,"disabled identity changed");
        const auto base=Payload(c.base_bytes,0x84531),mips=Payload(c.mip_bytes,0x156721);
        Compare(c.fetch,CpuTextureStorageSamplingKey(c.fetch,true),base,mips);
    }
    Check(raw_keys.size()==64&&bias_keys.size()==11&&keys.size()==9,"saved corpus grouping changed");
    // Content keys drop only the guest addresses: every captured layout must
    // decode the same bytes into the same levels, and keep its sampling state.
    for(const auto& c:captured) {
        const auto content=CpuTextureContentKey(c.fetch);
        const auto base=Payload(c.base_bytes,0x2741),mips=Payload(c.mip_bytes,0x9913);
        const auto x=DescribeTexture(c.fetch),y=DescribeTexture(content);
        Check(x.min_mip==y.min_mip&&x.max_mip==y.max_mip&&bool(x.base_address)==bool(y.base_address)&&
              bool(x.mip_address)==bool(y.mip_address),"content key changed resident levels");
        const auto dx=DecodeTexture(c.fetch,base,mips),dy=DecodeTexture(content,base,mips);
        Check(dx.levels.size()==dy.levels.size(),"content key changed level count");
        for(size_t i=0;i<dx.levels.size();++i)
            Check(Level(dx.levels[i].layout)==Level(dy.levels[i].layout)&&dx.levels[i].bytes==dy.levels[i].bytes,
                  "content key changed decoded bytes");
        for(unsigned word=0;word<6;++word)for(unsigned bit=0;bit<32;++bit) {
            const uint32_t b=uint32_t{1}<<bit;
            if((word==1||word==5)&&(b&0x1FFFF000u))continue;
            auto changed=c.fetch;changed[word]^=b;
            Check(CpuTextureContentKey(changed)!=content,"content key lost a non-address bit");
        }
        auto moved=c.fetch;
        for(const unsigned word:{1u,5u})if(moved[word]&0x1FFFF000u)moved[word]=(moved[word]&~0x1FFFF000u)|0x0ABCD000u;
        Check(CpuTextureContentKey(moved)==content,"moved texture changed its content key");
    }
    for(const auto& c:captured) {
        const auto canonical=CpuTextureStorageSamplingKey(c.fetch,true);
        for(unsigned word=0;word<6;++word)for(unsigned bit=0;bit<32;++bit) {
            const uint32_t b=uint32_t{1}<<bit;
            if((word==3&&(b&0x01800000u))||(word==4&&(b&0x003ff000u)))continue;
            auto changed=c.fetch;changed[word]^=b;
            Check(CpuTextureStorageSamplingKey(changed,true)!=canonical,"non-sampler identity bit lost");
        }
        auto key=Key(c,true),different=key;++different.version;
        Check(CpuImageLess{}(key,different)||CpuImageLess{}(different,key),"version lost");
        different=key;different.base=std::string(64,'a');
        if(different.base==key.base)different.base=std::string(64,'b');
        Check(CpuImageLess{}(key,different)||CpuImageLess{}(different,key),"base digest lost");
        different=key;different.mips=std::string(64,'c');
        if(different.mips==key.mips)different.mips=std::string(64,'d');
        Check(CpuImageLess{}(key,different)||CpuImageLess{}(different,key),"mip digest lost");
    }
    // Exhaust all signed LOD encodings and mip-filter values on a small real
    // mip layout through the actual decoder. Filter=3 remains a binding error;
    // these CPU functions have never validated sampling state.
    auto small=captured[2].fetch;
    small[0]=(small[0]&~0x7fc00000u)|(1u<<22);
    small[1]=(small[1]&~63u)|18u;
    small[2]=3u|(3u<<13);
    small[4]=(small[4]&~0x3fcu)|(2u<<6);
    small[5]&=~2048u;
    auto base=Payload(65536,4123),mips=Payload(65536,6789);
    for(uint32_t bias=0;bias<1024;++bias)for(uint32_t filter=0;filter<4;++filter) {
        auto f=small;f[3]=(f[3]&~0x01800000u)|(filter<<23);f[4]=(f[4]&~0x003ff000u)|(bias<<12);
        Compare(f,CpuTextureStorageSamplingKey(f,true),base,mips);
    }
    // The actual byte decoder must preserve malformed payload refusal. The
    // same original validation is required on image creation, never bypassed
    // by using canonical fetch state as a rendering input.
    auto f=captured.front().fetch;
    for(bool canonical:{false,true}) {
        bool failed=false;
        try{(void)DecodeTexture(CpuTextureStorageSamplingKey(f,canonical),{},{});}catch(const std::runtime_error&){failed=true;}
        Check(failed,"missing payload was accepted");
    }
    // Existing16/global4096 bounds remain; observed zero-bias final states are
    // admitted after a long ramp without evicting any live binding. This models
    // admission only; actual GPU binding/fence equivalence is a separate test.
    using BindingKey=std::array<uint32_t,8>;std::map<BindingKey,unsigned> bindings;
    unsigned next=0,transients=0;
    const auto use=[&](uint32_t bias,uint32_t mip,uint32_t aniso) {
        auto fetch=captured[2].fetch;fetch[4]=(fetch[4]&~0x003ff000u)|(bias<<12);fetch[3]=(fetch[3]&~0x01800000u)|(mip<<23);
        BindingKey k{};std::copy(fetch.begin(),fetch.end(),k.begin());k[6]=aniso;k[7]=16;
        if(auto it=bindings.find(k);it!=bindings.end())return it->second;
        const auto id=++next;
        if(bindings.size()<CpuTextureSamplingBindingLimit(fetch,true))bindings.emplace(k,id);else ++transients;
        Check(bindings.size()<=16,"binding cap grew");return id;
    };
    for(uint32_t bias=63;bias;--bias)use(bias,1,UINT32_MAX);
    Check(bindings.size()==12,"ramp did not reserve four entries");
    for(uint32_t mip=0;mip<3;++mip) {
        const auto id=use(0,mip,UINT32_MAX);
        for(unsigned repeat=0;repeat<20;++repeat)Check(use(0,mip,UINT32_MAX)==id,"final binding was not cached");
    }
    Check(bindings.size()==15,"final states missing");
    const auto changed_override=use(0,1,0);Check(use(0,1,0)==changed_override,"override identity lost");
    Check(bindings.size()==16,"existing cap not filled");
    use(0,1,1);Check(bindings.size()==16,"override overflow exceeded cap");
    for(const auto& c:captured)Check(CpuTextureSamplingBindingLimit(c.fetch,false)==16,"disabled binding policy changed");
    std::cout<<"{\"passed\":true,\"checks\":"<<checks<<",\"captured_fetches\":64,\"raw_keys\":64,\"bias_keys\":11,\"sampling_keys\":9,\"decoded_pairs\":"<<decoded_pairs<<",\"decoded_bytes_compared\":"<<decoded_bytes<<",\"binding_model_transients\":"<<transients<<",\"gpu_claim\":false,\"fps_claim\":false}\n";
    return 0;
} catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
