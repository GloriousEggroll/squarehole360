#include "renderer/native/constant_delta_data.h"
#include <algorithm>
#include <iostream>
#include <random>
#include <vector>

using namespace gow3::renderer;
namespace {
size_t checks=0,flushes=0,failures=0,deterministic_updates=0,edge_words=0,scalar_blocks=0,shuffle_blocks=0;
void Require(bool value) {++checks;if(!value)throw std::runtime_error("Constant delta byte oracle mismatch");}
template<class F>void Reject(F&& f) {
    bool threw=false;try {f();}catch(const std::runtime_error&){threw=true;}Require(threw);
}
std::vector<uint8_t> Oracle(std::span<const uint8_t> guest,std::span<const uint8_t> shared) {
    std::vector<uint8_t> result(kFloatConstantBytes+shared.size());
    // Independent byte-position oracle, no production byte-swap helper.
    for(size_t i=0;i<kFloatConstantBytes;i+=4)
        for(size_t j=0;j<4;++j)result[i+j]=guest[kGuestFloatConstantOffset+i+(std::endian::native==std::endian::little?3-j:j)];
    std::copy(shared.begin(),shared.end(),result.begin()+kFloatConstantBytes);return result;
}
void Exercise(size_t shared_bytes,size_t alignment) {
    std::mt19937_64 random(0x7a6171u+shared_bytes+alignment);
    std::vector<uint8_t> guest(kGuestFloatConstantOffset+kFloatConstantBytes+alignment+1);
    std::vector<uint8_t> shared(shared_bytes+alignment+1);
    std::vector<uint8_t> backing(kFloatConstantBytes+shared_bytes+alignment+2,0xa5);
    auto g=std::span<uint8_t>(guest).subspan(alignment,kGuestFloatConstantOffset+kFloatConstantBytes);
    auto s=std::span<uint8_t>(shared).subspan(alignment,shared_bytes);
    auto destination=std::span<uint8_t>(backing).subspan(alignment+1,kFloatConstantBytes+shared_bytes);
    for(auto& value:g)value=uint8_t(random());
    for(auto& value:s)value=uint8_t(random());
    ConstantDeltaPacker packer;
    const auto flush=[&]{++flushes;};
    auto first=packer.Pack(g,s,destination,flush);
    Require(first.float_bytes==8192&&first.shared_bytes==shared_bytes&&first.unchanged_blocks==0&&packer.valid());
    Require(std::equal(destination.begin(),destination.end(),Oracle(g,s).begin()));
    for(size_t trial=0;trial<512;++trial) {
        const auto old=std::vector<uint8_t>(g.begin()+kGuestFloatConstantOffset,g.end());
        // Random byte mutations exercise sequential warm updates. Deterministic
        // lane/block/IEEE cases below provide the explicit coverage claims.
        size_t edits=trial%7;
        for(size_t i=0;i<edits;++i)g[kGuestFloatConstantOffset+(random()%8192)]^=uint8_t(1u<<(random()%8));
        for(size_t i=0;i<shared_bytes;++i)s[i]=uint8_t(random());
        uint64_t changed=0;
        for(size_t offset=0;offset<8192;offset+=64)
            if(!std::equal(old.begin()+offset,old.begin()+offset+64,g.begin()+kGuestFloatConstantOffset+offset))changed+=64;
        auto writes=packer.Pack(g,s,destination,flush);
        Require(writes.float_bytes==changed&&writes.shared_bytes==shared_bytes&&writes.unchanged_blocks==(8192-changed)/64);
        Require(std::equal(destination.begin(),destination.end(),Oracle(g,s).begin()));
        Require(backing.front()==0xa5&&backing.back()==0xa5);
    }
    // Every block is changed, and all64 byte positions are covered twice,
    // including all16 word lanes and each byte within a lane. This is not an
    // assertion that random edits happened to reach those positions.
    for(size_t block=0;block<128;++block) {
        g[kGuestFloatConstantOffset+block*64+block%64]^=uint8_t(1u<<(block%8));
        auto writes=packer.Pack(g,s,destination,flush);++deterministic_updates;
        Require(writes.float_bytes==64&&writes.unchanged_blocks==127);
        Require(std::equal(destination.begin(),destination.end(),Oracle(g,s).begin()));
    }
    // Explicit IEEE encodings are bytes, never host float operands. Check both
    // cold full packs and warm deltas, including negativezero and signalingNaN.
    constexpr uint32_t patterns[]={0x00000000u,0x80000000u,0x7f800000u,0xff800000u,
        0x7fc00001u,0x7fa00001u,0xffc01234u,0x00000001u,0x007fffffu,0x00800000u,0x3f800000u};
    for(size_t i=0;i<std::size(patterns);++i) {
        const size_t off=kGuestFloatConstantOffset+(i%2?8192-4:i*64);
        for(size_t j=0;j<4;++j)g[off+j]=uint8_t(patterns[i]>>(24-j*8));
        if(!(i%3))packer.Invalidate();
        packer.Pack(g,s,destination,flush);++edge_words;
        Require(std::equal(destination.begin(),destination.end(),Oracle(g,s).begin()));
    }
    // A shared-only change still writes the complete shared block and flushes.
    s.back()^=0xff;auto unchanged=packer.Pack(g,s,destination,flush);
    Require(unchanged.float_bytes==0&&unchanged.shared_bytes==shared_bytes&&unchanged.unchanged_blocks==128);
    Require(std::equal(destination.begin(),destination.end(),Oracle(g,s).begin()));
    const auto memory=backing;
    Reject([&]{packer.Pack(g.first(g.size()-1),s,destination,flush);});
    Require(backing==memory&&packer.valid());
    Reject([&]{packer.Pack(g,s,destination.first(destination.size()-1),flush);});
    Require(backing==memory&&packer.valid());
    Reject([&]{packer.Pack(g,s.first(s.size()-1),destination,flush);});
    Require(backing==memory&&packer.valid());
    // Aliased source/destination must refuse before mutation like legacy pack.
    std::vector<uint8_t> alias(kGuestFloatConstantOffset+8192+shared_bytes);
    const auto alias_before=alias;
    Reject([&]{packer.Pack(alias,s,std::span(alias).first(8192+shared_bytes),flush);});
    Require(alias==alias_before&&packer.valid());
    Reject([&]{packer.Pack(g,destination.last(shared_bytes),destination,flush);});
    Require(backing==memory&&packer.valid());
    // An actual failed callback after writes invalidates the mirror. Even a
    // corrupt destination then recovers through a full overwrite next attempt.
    g.back()^=1;
    Reject([&]{packer.Pack(g,s,destination,[&]{++failures;throw std::runtime_error("Injected full-range flush failure");});});
    Require(!packer.valid());std::fill(destination.begin(),destination.end(),0x6d);
    auto recovered=packer.Pack(g,s,destination,flush);
    Require(recovered.float_bytes==8192&&recovered.unchanged_blocks==0&&packer.valid());
    Require(std::equal(destination.begin(),destination.end(),Oracle(g,s).begin()));
    // Recreation has no handle/address-based ABA admission: explicit invalidation
    // forces all bytes into a different fresh allocation with identical inputs.
    packer.Invalidate();std::fill(destination.begin(),destination.end(),0xb3);
    auto recreated=packer.Pack(g,s,destination,flush);
    Require(recreated.float_bytes==8192&&packer.valid());
    Require(std::equal(destination.begin(),destination.end(),Oracle(g,s).begin()));
}
void ExplicitBlockOracles() {
    std::array<uint8_t,66> source{},scalar{},shuffle{},expected{};
    source.front()=source.back()=scalar.front()=scalar.back()=shuffle.front()=shuffle.back()=0x81;
    for(size_t i=0;i<64;++i)source[i+1]=uint8_t(i*37+19);
    for(size_t i=0;i<64;i+=4)
        for(size_t j=0;j<4;++j)expected[i+j+1]=source[i+1+(std::endian::native==std::endian::little?3-j:j)];
    detail::CopyConstantBlockScalar(source.data()+1,scalar.data()+1);++scalar_blocks;
    Require(std::equal(scalar.begin()+1,scalar.end()-1,expected.begin()+1));
    Require(scalar.front()==0x81&&scalar.back()==0x81);
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    if(detail::HasConstantShuffle()) {
        detail::CopyConstantBlockShuffle(source.data()+1,shuffle.data()+1);++shuffle_blocks;
        Require(std::equal(shuffle.begin()+1,shuffle.end()-1,expected.begin()+1));
        Require(shuffle.front()==0x81&&shuffle.back()==0x81);
    }
#endif
}
}
int main() {
    try {
        ExplicitBlockOracles();
        for(size_t extent:{sizeof(SharedShaderConstants),sizeof(NativeFsiShaderConstants)})
            for(size_t alignment:{size_t(0),size_t(1),size_t(3),size_t(15)})Exercise(extent,alignment);
        Require(flushes==8*655&&failures==8);
        Require(deterministic_updates==8*128&&edge_words==8*11&&scalar_blocks==1);
        std::cout<<"PASS constant delta checks="<<checks<<" actual_flush_callbacks="<<flushes
                 <<" reached_flush_failures="<<failures<<" deterministic_updates="<<deterministic_updates
                 <<" explicit_edge_words="<<edge_words<<" scalar_blocks="<<scalar_blocks
                 <<" supported_shuffle_blocks="<<shuffle_blocks<<" mirror_bytes="<<sizeof(ConstantDeltaPacker)<<"\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
