#include "renderer/native/raw_float_texture.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
using namespace gow3::renderer;
namespace {
unsigned checks=0;
void Require(bool value,const char* message) {++checks;if(!value)throw std::runtime_error(message);}
using Fetch=std::array<uint32_t,6>;
constexpr Fetch kLiveFetch{536871254,401105062,4095,3345,0,512};
constexpr Fetch kOtherLiveFetch{536871254,401039526,4095,3345,0,512};
uint32_t ReadLittle(const uint8_t* p) {
    return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;
}
void PutBig(uint8_t* p,uint32_t bits) {
    for(unsigned byte=0;byte<4;++byte)p[byte]=uint8_t(bits>>(24-byte*8));
}
template<class Mutate>void Refuse(const TextureLayout& original,const char* why,Mutate change) {
    auto t=original;change(t);Require(!RawFloatCpuTextureLayout(t),why);
}
void LayoutRefusals(const TextureLayout& t) {
    Refuse(t,"format",[](auto& x){x.format=37;});
    Refuse(t,"dimension",[](auto& x){x.dimension=0;});
    Refuse(t,"width",[](auto& x){x.width=4095;});
    Refuse(t,"height",[](auto& x){x.height=2;});
    Refuse(t,"depth",[](auto& x){x.depth=2;});
    Refuse(t,"layers",[](auto& x){x.layers=2;});
    Refuse(t,"tiled",[](auto& x){x.tiled=true;});
    for(uint32_t endian:{0u,1u,3u})Refuse(t,"endian",[&](auto& x){x.endian=endian;});
    Refuse(t,"minimum mip",[](auto& x){x.min_mip=1;});
    Refuse(t,"maximum mip",[](auto& x){x.max_mip=1;});
    Refuse(t,"missing base",[](auto& x){x.base_address=0;});
    Refuse(t,"unaligned base",[](auto& x){++x.base_address;});
    Refuse(t,"mip address",[](auto& x){x.mip_address=0x100000;});
    Refuse(t,"block width",[](auto& x){x.block.block_width=4;});
    Refuse(t,"block height",[](auto& x){x.block.block_height=4;});
    Refuse(t,"block bytes",[](auto& x){x.block.bytes=8;});
    Refuse(t,"block components",[](auto& x){x.block.components=2;});
    Refuse(t,"missing level",[](auto& x){x.levels.clear();});
    Refuse(t,"second level",[](auto& x){x.levels.push_back(x.levels.front());});
    Refuse(t,"level mip",[](auto& x){x.levels.front().mip=1;});
    Refuse(t,"level layer",[](auto& x){x.levels.front().layer=1;});
    Refuse(t,"level width",[](auto& x){x.levels.front().width=4095;});
    Refuse(t,"level height",[](auto& x){x.levels.front().height=2;});
    Refuse(t,"level depth",[](auto& x){x.levels.front().depth=2;});
    Refuse(t,"pitch",[](auto& x){x.levels.front().pitch_blocks=4128;});
    Refuse(t,"slice rows",[](auto& x){x.levels.front().slice_rows=1;});
    Refuse(t,"packed x",[](auto& x){x.levels.front().x=1;});
    Refuse(t,"packed y",[](auto& x){x.levels.front().y=1;});
    Refuse(t,"packed z",[](auto& x){x.levels.front().z=1;});
    Refuse(t,"offset",[](auto& x){x.levels.front().offset=uint64_t(1)<<32;});
    Refuse(t,"mip provenance",[](auto& x){x.levels.front().mips=true;});
}
}
int main()try {
    const auto t=DescribeTexture(kLiveFetch),other=DescribeTexture(kOtherLiveFetch);
    Require(RawFloatCpuTextureLayout(t)&&RawFloatCpuTextureLayout(other),"Observed layouts refused");
    Require(t.base_address==401104896&&other.base_address==401039360,"Observed bases decoded incorrectly");
    Require(((t.fetch[0]>>2)&255)==85&&(t.fetch[3]&1)==1,"Observed signs/number format changed");
    Require(t.fetch==kLiveFetch&&other.fetch==kOtherLiveFetch,"Policy changed original fetch");
    // Shape admission must not reinterpret/clear sampling properties. Their
    // original validation and BindTexture(decoded_float=false) are kept by Scene.
    for(uint32_t signs:{0u,85u,170u,255u})for(uint32_t number:{0u,1u}) {
        auto fetch=kLiveFetch;fetch[0]=(fetch[0]&~(255u<<2))|(signs<<2);fetch[3]=(fetch[3]&~1u)|number;
        const auto shaped=DescribeTexture(fetch);Require(RawFloatCpuTextureLayout(shaped),"Sampling properties became raw byte conversion conditions");
        Require(shaped.fetch==fetch,"Shape policy mutated sampling properties");
    }
    LayoutRefusals(t);
    for(size_t n:{size_t(0),size_t(65535)})Require(!RawFloatCpuTexturePayload(n),"Truncated payload admitted");
    for(size_t n:{size_t(65536),size_t(65537),size_t(2097152)})Require(RawFloatCpuTexturePayload(n),"Valid payload padding refused");
    const auto ranges=TextureMemoryRanges(t,false);
    Require(ranges==std::vector<std::pair<uint32_t,uint32_t>>{{t.base_address,65536}},"Sampled footprint is not exact single64KiB run");
    Require(TextureMemoryRanges(t,true).empty(),"Base-only layout has mip footprint");
    Require(TextureMemoryRanges(other,false)==std::vector<std::pair<uint32_t,uint32_t>>{{other.base_address,65536}},"Second base footprint wrong");
    const auto& level=t.levels.front();
    const uint64_t allocation_stride=uint64_t(level.pitch_blocks)*level.slice_rows*t.block.bytes;
    Require(allocation_stride==2097152&&ranges.front().second*32ull==allocation_stride,"Unused31 slice rows became sampled bytes");
    Require(TextureBlockOffset(t,level,0,0,0)==0&&TextureBlockOffset(t,level,4095,0,0)==65520,"Linear row bounds wrong");
    constexpr std::array<uint32_t,16> edge_bits{
        0x00000000,0x80000000,0x3f800000,0xbf800000,0x7f800000,0xff800000,0x7fc12345,0x7f812345,
        0xffc12345,0xff812345,0x00000001,0x80000001,0x007fffff,0x7f7fffff,0x01234567,0x89abcdef};
    std::vector<uint8_t> exact(kRawFloatCpuTextureBytes),padded(allocation_stride,0xa5);
    for(size_t word=0;word<exact.size()/4;++word)PutBig(exact.data()+word*4,edge_bits[word%edge_bits.size()]);
    std::copy(exact.begin(),exact.end(),padded.begin());
    const auto decoded=DecodeTexture(kLiveFetch,exact,{}),decoded_padded=DecodeTexture(kLiveFetch,padded,{});
    Require(decoded.levels.size()==1&&decoded.levels.front().bytes.size()==65536,"Decoded extent changed");
    Require(decoded.levels.front().bytes==decoded_padded.levels.front().bytes,"Unused allocation padding affected texels");
    const auto& out=decoded.levels.front().bytes;
    for(size_t word=0;word<exact.size()/4;++word) {
        const uint32_t raw=ReadLittle(exact.data()+word*4),candidate=std::byteswap(raw);
        Require(candidate==edge_bits[word%edge_bits.size()],"UINT endian permutation changed edge bits");
        Require(ReadLittle(out.data()+word*4)==candidate,"CPU byte-copy oracle differs from raw UINT permutation");
    }
    exact.pop_back();bool truncated=false;
    try{(void)DecodeTexture(kLiveFetch,exact,{});}catch(const std::runtime_error& e){truncated=std::string(e.what())=="Texture payload is missing or truncated at mip 0";}
    Require(truncated,"Original truncated payload failure changed");
    std::cout<<"PASS: checks="<<checks<<" exact_layout_refusals=true texel_words=16384 edge_bits_preserved=true sampled_bytes=65536 unused_padding_bytes=2031616\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
