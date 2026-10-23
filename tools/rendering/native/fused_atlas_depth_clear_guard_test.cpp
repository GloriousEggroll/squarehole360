#include "renderer/native/fused_atlas_depth_clear.h"
#include <bit>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace gow3::renderer;
namespace {
uint64_t checks=0;
void Require(bool condition,const char* message) {
    ++checks;if(!condition)throw std::runtime_error(message);
}
NativeFusedAtlasDepthClearInput Input() {
    NativeFusedAtlasDepthClearInput x;
    x.native_fsi=x.source_depth=true;x.width=x.pitch=1120;x.height=688;
    x.base=1440;x.surface_info=293602400;x.flags=16;x.depth=1.f;
    x.rectangle={0,0,138,74};return x;
}
struct ImageState {
    bool present=false,dirty=false;
    uint64_t host_version=0,packed_version=0,receipt_version=0;
    std::shared_ptr<uint32_t> receipt;
    uint32_t host_bits=0,packed_decode_bits=0;
    // Independent semantic oracle: the original cached image persists while
    // its publication token matches. A receipt retains those PRE-STORE bits;
    // it never authorizes decoding the quantized packed output instead.
    uint32_t ReadOriginalHost() {
        if(receipt&&receipt_version==packed_version)host_bits=*receipt;
        else if(!present||host_version!=packed_version)host_bits=packed_decode_bits;
        receipt.reset();present=true;host_version=packed_version;return host_bits;
    }
};
}
int main()try {
    const auto input=Input();Require(NativeFusedAtlasDepthClearAllowed(input),"Captured depth-one clear refused");
    const auto refuse=[&](auto mutate) {auto bad=input;mutate(bad);
        Require(!NativeFusedAtlasDepthClearAllowed(bad),"Unsupported clear admitted");};
    refuse([](auto& x){x.native_fsi=false;});refuse([](auto& x){x.query_active=true;});
    refuse([](auto& x){x.source_depth=false;});refuse([](auto& x){x.width=1119;});
    refuse([](auto& x){x.height=689;});refuse([](auto& x){x.pitch=1152;});
    refuse([](auto& x){x.base=0;});refuse([](auto& x){x.format=1;});
    refuse([](auto& x){x.surface_info|=0x10000;});refuse([](auto& x){x.surface_info|=0x20000;});
    refuse([](auto& x){x.surface_info^=1;});
    refuse([](auto& x){x.flags=0x30;});refuse([](auto& x){x.flags=0x11;});
    refuse([](auto& x){x.depth=0.f;});refuse([](auto& x){x.depth=std::nextafter(1.f,0.f);});
    refuse([](auto& x){x.depth=std::numeric_limits<float>::quiet_NaN();});
    refuse([](auto& x){x.depth=std::numeric_limits<float>::infinity();});
    refuse([](auto& x){x.rectangle[0]=-1;});refuse([](auto& x){x.rectangle[1]=-1;});
    refuse([](auto& x){x.rectangle[2]=1121;});refuse([](auto& x){x.rectangle[3]=689;});
    refuse([](auto& x){x.rectangle[2]=x.rectangle[0];});
    refuse([](auto& x){x.rectangle[3]=x.rectangle[1];});
    for(const auto& rectangle:std::vector<std::array<int32_t,4>>{
            {0,0,1120,688},{1119,687,1120,688},{41,17,179,91},{0,0,522,266}}) {
        auto allowed=input;allowed.rectangle=rectangle;
        Require(NativeFusedAtlasDepthClearAllowed(allowed),"Valid closed atlas rectangle refused");
    }
    // Exhaust independent provenance combinations. Flags are not inferred from
    // a selected result or synthetic hash; they are exact preaccept facts.
    for(unsigned bits=0;bits<128;++bits) {
        const bool defined=bits&1,present=bits&2,dirty=bits&4,current=bits&8,
            receipt=bits&16,other_dirty=bits&32,zero_token=bits&64;
        const uint64_t packed=zero_token?0:19,host=current?packed:7;
        const auto result=NativeFusedAtlasDepthClearChooseSource(
            defined,present,dirty,host,packed,receipt,other_dirty);
        const auto expected=(!defined||dirty||receipt||other_dirty||zero_token)?
            NativeFusedAtlasDepthClearSource::Refuse:
            present&&current?NativeFusedAtlasDepthClearSource::Host:NativeFusedAtlasDepthClearSource::Packed;
        Require(result==expected,"Provenance truth table changed source authority");
    }
    // A previous alias commit may change the token: observe it AFTER that
    // commit, so a formerly current host now selects original packed decode.
    Require(NativeFusedAtlasDepthClearChooseSource(true,true,false,19,20,false,false)==
        NativeFusedAtlasDepthClearSource::Packed,"Post-alias stale host used as current input");
    Require(NativeFusedAtlasDepthClearChooseSource(true,true,false,20,20,false,false)==
        NativeFusedAtlasDepthClearSource::Host,"Current host lost its exact float input");
    Require(!NativeFusedAtlasDepthClearReceiptCurrent(true,0,0),"Zero receipt token admitted");
    Require(!NativeFusedAtlasDepthClearReceiptCurrent(false,20,20),"Missing receipt admitted");
    Require(!NativeFusedAtlasDepthClearReceiptCurrent(true,20,21),"Changed packed version retained receipt");
    Require(NativeFusedAtlasDepthClearReceiptCurrent(true,20,20),"Exact receipt refused");
    // This UNORM24 roundtrip witness has distinct pre-encode and decoded
    // post-store bits. Same-version materialization must keep original floats.
    ImageState image{true,false,19,20,20,std::make_shared<uint32_t>(0x3e800002u),
        0x3e800002u,0x3e800000u};
    Require(image.ReadOriginalHost()==0x3e800002u,"Same-token receipt normalized original host float");
    Require(!image.receipt&&image.host_version==20,"Consumed receipt left stale host metadata");
    image.receipt=std::make_shared<uint32_t>(0x3e800002u);image.receipt_version=20;
    image.packed_version=21;image.packed_decode_bits=0x3f200000u;
    Require(image.ReadOriginalHost()==0x3f200000u,"Packed mutation reused obsolete receipt floats");
    // Ownership model only: delayed restoration requires a NEW packet reserve,
    // even if the original compute packet has already retired. No GPU fence
    // or actual NativeTransferBatch completion is claimed by this CPU fixture.
    auto receipt=std::make_shared<uint32_t>(0x3e800002u);std::weak_ptr<uint32_t> witness=receipt;
    std::vector<std::shared_ptr<uint32_t>> old_packet{receipt};
    old_packet.clear();Require(!witness.expired(),"Scene receipt failed to own delayed image");
    std::vector<std::shared_ptr<uint32_t>> restore_packet{receipt};receipt.reset();
    Require(!witness.expired(),"Delayed restore lacked independent packet owner");
    restore_packet.clear();Require(witness.expired(),"Retired model packet leaked receipt owner");
    std::cout<<"Fused atlas clear guard CPU PASS "<<checks<<" checks; no GPU or Scene execution\n";
    return 0;
} catch(const std::exception& error) {
    std::cerr<<error.what()<<'\n';return 1;
}
