#include "renderer/native/shader_byte_segments.h"
#include "renderer/native/content_hash.h"
#include <picosha2.h>
#include <iostream>
#include <set>
#include <unordered_map>

using namespace gow3::renderer;
namespace {
void Require(bool valid,const char* message) {if(!valid)throw std::runtime_error(message);}
}
int main() try {
    size_t cases=0;
    for(size_t size:{0u,1u,2u,3u,31u,32u,63u,64u,65u,127u,128u,255u,4096u}) {
        std::vector<uint8_t> original(size);
        for(size_t i=0;i<size;++i)original[i]=uint8_t((i*131)^(i>>3)^size);
        for(size_t split:std::set<size_t>{0,size/2,size,size?size-1:0,std::min(size,size_t(1))}) {
            const auto first=std::span<const uint8_t>(original).first(split);
            const auto second=std::span<const uint8_t>(original).subspan(split);
            ShaderByteSegments segments(first,second);
            // Independent legacy construction is the canonical byte oracle.
            std::vector<uint8_t> concatenated(first.begin(),first.end());
            concatenated.insert(concatenated.end(),second.begin(),second.end());
            const auto snapshot=segments.Snapshot();
            Require(segments.size()==concatenated.size()&&segments.Matches(concatenated)&&*snapshot==concatenated,
                    "Segmented shader changed concatenated byte identity");
            Require(ContentHash(*snapshot)==picosha2::hash256_hex_string(concatenated),
                    "Segmented shader changed canonical SHA-256");
            Require(segments.Fingerprint()==ShaderByteSegments(first,second).Fingerprint(),
                    "Segmented shader fingerprint is not stable");
            auto longer=concatenated;longer.push_back(0);
            Require(!segments.Matches(longer),"Segmented comparison accepted a different total extent");
            if(size) {
                auto shorter=concatenated;shorter.pop_back();
                Require(!segments.Matches(shorter),"Segmented comparison accepted truncated bytes");
                for(size_t changed:std::set<size_t>{0,size/2,size-1,split?split-1:0,std::min(split,size-1)}) {
                    auto different=concatenated;different[changed]^=0x80;
                    Require(!segments.Matches(different),"Segmented comparison ignored changed header/code bytes");
                }
            }
            ++cases;
        }
    }
    // Fingerprints never establish equality. Place unlike same-size blobs in
    // one bucket and verify both sides of the split participate in selection.
    std::unordered_multimap<size_t,std::vector<uint8_t>> collisions;
    collisions.emplace(0,std::vector<uint8_t>{1,2,3,4});
    collisions.emplace(0,std::vector<uint8_t>{1,2,9,4});
    collisions.emplace(0,std::vector<uint8_t>{8,2,3,4});
    for(const auto& wanted:std::vector<std::vector<uint8_t>>{{1,2,3,4},{1,2,9,4},{8,2,3,4},{8,2,9,4}}) {
        ShaderByteSegments segments{std::span<const uint8_t>(wanted).first(2),
                                    std::span<const uint8_t>(wanted).subspan(2)};
        size_t matches=0;const auto [first,last]=collisions.equal_range(0);
        for(auto it=first;it!=last;++it)if(segments.Matches(it->second)) {
            Require(it->second==wanted,"Fingerprint collision selected another shader");++matches;
        }
        Require(matches==(wanted==std::vector<uint8_t>{8,2,9,4}?0u:1u),"Collision lookup lost exact byte selection");
    }
    std::vector<uint8_t> mutable_header{1,2},mutable_code{3,4};
    ShaderByteSegments mutable_segments(mutable_header,mutable_code);
    const auto retained=mutable_segments.Snapshot();mutable_header[0]=9;mutable_code[1]=8;
    Require(*retained==std::vector<uint8_t>{1,2,3,4}&&!mutable_segments.Matches(*retained),
            "Later source mutation changed an owned shader snapshot");
    std::cout<<"Shader byte segments PASS: "<<cases<<" concatenation/SHA oracles, empty spans, split boundaries, "
        "changed extents/header/code, forced collisions and immutable snapshot lifetime\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
