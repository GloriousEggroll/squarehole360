#include "renderer/native/constant_data.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <string_view>
#include <vector>
using namespace gow3::renderer;
namespace {
void Require(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
std::vector<uint8_t> OldPacking(std::span<const uint8_t> raw,std::span<const uint8_t> shared) {
    std::vector<uint8_t> bytes(8192+shared.size());
    for(unsigned i=0;i<2048;++i) {
        const auto at=0x780+i*4;
        if(at+4>raw.size())throw std::runtime_error("Truncated captured word");
        const uint32_t value=uint32_t(raw[at])<<24|uint32_t(raw[at+1])<<16|
            uint32_t(raw[at+2])<<8|raw[at+3];
        std::memcpy(bytes.data()+i*4,&value,4);
    }
    std::memcpy(bytes.data()+8192,shared.data(),shared.size());
    return bytes;
}
template<class F>void RejectUnchanged(std::vector<uint8_t>& storage,F&& action) {
    const auto before=storage;bool rejected=false;
    try{action();}catch(const std::runtime_error&){rejected=true;}
    Require(rejected,"Invalid constant packing input was accepted");
    Require(storage==before,"Rejected packing input modified its destination");
}
}
int main(int argc,char** argv) try {
    const std::array<uint32_t,16> bit_patterns{0,0x80000000u,1,0x80000001u,
        0x007fffffu,0x00800000u,0x7f7fffffu,0xff7fffffu,0x7f800000u,0xff800000u,
        0x7f800001u,0x7fc12345u,0xff800001u,0xffc12345u,0x3f800000u,0xffffffffu};
    std::vector<uint8_t> guest(0x3600);
    uint32_t random=0x718c235du;
    for(auto& byte:guest){random^=random<<13;random^=random>>17;random^=random<<5;byte=uint8_t(random);}
    for(size_t i=0;i<bit_patterns.size();++i)for(unsigned byte=0;byte<4;++byte)
        guest[0x780+i*4+byte]=uint8_t(bit_patterns[i]>>(24-byte*8));
    size_t cases=0;
    for(const auto shared_size:{sizeof(SharedShaderConstants),sizeof(NativeFsiShaderConstants)}) {
        std::vector<uint8_t> shared_storage(shared_size+4);
        for(size_t i=0;i<shared_storage.size();++i)shared_storage[i]=uint8_t(i*193+(i>>3));
        for(size_t source_offset=0;source_offset<8;++source_offset) {
            std::vector<uint8_t> input(guest.size()+source_offset,0xa5);
            std::copy(guest.begin(),guest.end(),input.begin()+source_offset);
            const auto source=std::span(input).subspan(source_offset);
            for(size_t shared_offset=0;shared_offset<4;++shared_offset) {
                const auto shared=std::span(shared_storage).subspan(shared_offset,shared_size);
                const auto expected=OldPacking(source,shared);
                Require(PackedDrawConstantSize(source,shared)==expected.size(),"Packed constant ABI extent changed");
                for(size_t destination_offset=0;destination_offset<8;++destination_offset) {
                    std::vector<uint8_t> storage(expected.size()+16,0xa5);
                    auto destination=std::span(storage).subspan(destination_offset,expected.size());
                    PackDrawConstants(source,shared,destination);
                    Require(std::equal(expected.begin(),expected.end(),destination.begin()),
                        "Packed constants differ from the original bitwise ABI");
                    std::vector<uint8_t> scalar(kFloatConstantBytes);
                    detail::CopyFloatConstantsScalar(source.data()+kGuestFloatConstantOffset,scalar.data());
                    Require(std::equal(scalar.begin(),scalar.end(),expected.begin()),
                        "Scalar constant fallback differs from the original bitwise ABI");
                    Require(std::all_of(storage.begin(),storage.begin()+destination_offset,[](uint8_t b){return b==0xa5;})&&
                        std::all_of(storage.begin()+destination_offset+expected.size(),storage.end(),[](uint8_t b){return b==0xa5;}),
                        "Constant packing overwrote adjacent bytes");
                    ++cases;
                }
            }
        }
        std::vector<uint8_t> shared(shared_storage.begin(),shared_storage.begin()+shared_size);
        std::vector<uint8_t> destination(kFloatConstantBytes+shared_size,0xa5);
        for(size_t bytes:{size_t(0),kGuestFloatConstantOffset,kGuestFloatConstantOffset+kFloatConstantBytes-1})
            RejectUnchanged(destination,[&]{PackDrawConstants(std::span(guest).first(bytes),shared,destination);});
        for(size_t bytes:{size_t(0),shared_size-1,shared_size+1})
            RejectUnchanged(destination,[&]{PackDrawConstants(guest,std::span(shared_storage).first(bytes),destination);});
        RejectUnchanged(destination,[&]{PackDrawConstants(guest,shared,std::span(destination).first(destination.size()-1));});
        destination.push_back(0xa5);
        RejectUnchanged(destination,[&]{PackDrawConstants(guest,shared,destination);});
        auto aliased=guest;
        RejectUnchanged(aliased,[&]{PackDrawConstants(aliased,shared,std::span(aliased).first(kFloatConstantBytes+shared_size));});
        std::vector<uint8_t> shared_alias(kFloatConstantBytes+shared_size,0xa5);
        RejectUnchanged(shared_alias,[&]{PackDrawConstants(guest,std::span(shared_alias).subspan(kFloatConstantBytes),shared_alias);});
    }
    std::cout<<"PASS: "<<cases<<" exact old-ABI comparisons, unaligned inputs/output, NaN/signed-zero bits, both shared ABIs, guards and pre-write rejection\n";
    if(argc==2&&std::string_view(argv[1])=="--benchmark") {
        std::vector<uint8_t> shared(sizeof(NativeFsiShaderConstants),0x39),destination(kFloatConstantBytes+shared.size());
        for(unsigned method:{0u,1u,2u}) {
            const auto start=std::chrono::steady_clock::now();uint64_t checksum=0;
            for(unsigned i=0;i<100000;++i) {
                guest[0x780]=uint8_t(i);
                if(method==2)PackDrawConstants(guest,shared,destination);
                else if(method==1) {
                    detail::CopyFloatConstantsScalar(guest.data()+kGuestFloatConstantOffset,destination.data());
                    std::memcpy(destination.data()+kFloatConstantBytes,shared.data(),shared.size());
                } else {const auto bytes=OldPacking(guest,shared);std::memcpy(destination.data(),bytes.data(),bytes.size());}
                checksum+=destination[std::endian::native==std::endian::little?3:0];
            }
            std::cout<<(method==2?"dispatched":method==1?"scalar":"old")<<" 100000 packs ms="<<
                std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<<" checksum="<<checksum<<'\n';
        }
    } else if(argc!=1)throw std::runtime_error("Usage: native-constant-data-test [--benchmark]");
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
