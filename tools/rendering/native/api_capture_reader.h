#pragma once
#include "renderer/gears_bridge.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <stdexcept>
#include <utility>

namespace gow3::renderer::validation {
// Version-six API captures contain immutable payloads and backward references
// to earlier literal payloads. No address is dereferenced in the host process.
// This reader shares live_plan_test's format, with explicit per-record bounds.
template<class Visit>size_t ReadApiCapture(std::span<const uint8_t> bytes,Visit&& visit) {
    const auto le=[&](size_t offset) {
        if(offset>bytes.size()||bytes.size()-offset<4)throw std::runtime_error("Truncated API capture word");
        return uint32_t(bytes[offset])|uint32_t(bytes[offset+1])<<8|
            uint32_t(bytes[offset+2])<<16|uint32_t(bytes[offset+3])<<24;
    };
    constexpr std::array<uint8_t,8> magic{'G','3','D','3','D','0','0','1'};
    if(bytes.size()<32||bytes.size()>256u*1024*1024||!std::equal(magic.begin(),magic.end(),bytes.begin())||
       le(8)!=6||le(12)!=0x3600)throw std::runtime_error("Expected complete version-six API capture");
    std::map<uint32_t,std::pair<size_t,size_t>> references;
    size_t at=16,events=0;
    while(at<bytes.size()) {
        const auto size=le(at),function=le(at+4);
        if(size>bytes.size()-at)throw std::runtime_error("API event extends beyond capture");
        if(!function) {
            if(size!=16||bytes.size()-at!=16||le(at+8)!=0||le(at+12)!=events)
                throw std::runtime_error("Invalid API capture footer/count");
            return events;
        }
        if(size<60)throw std::runtime_error("Truncated API event header");
        const size_t end=at+size;
        GraphicsBoundaryEvent event;event.sequence=events++;event.function=function;
        event.phase=le(at+8);event.frame=le(at+12);event.lr=le(at+16);
        for(unsigned i=0;i<8;++i)event.args[i]=le(at+20+i*4);
        event.result=le(at+52);size_t cursor=at+60;
        const auto block_count=le(at+56);
        if(block_count>(end-cursor)/12)throw std::runtime_error("API block headers exceed event");
        event.blocks.reserve(block_count);
        for(uint32_t i=0;i<block_count;++i) {
            if(end-cursor<12)throw std::runtime_error("Truncated API block header");
            GraphicsBoundaryBlock block;block.kind=le(cursor);block.address=le(cursor+4);
            const uint32_t length=le(cursor+8);cursor+=12;
            size_t start=cursor,count=length;
            if(length&0x80000000u) {
                const auto found=references.find(length&0x7fffffffu);
                if(found==references.end())throw std::runtime_error("API payload reference is not an earlier literal block");
                start=found->second.first;count=found->second.second;
            } else {
                if(count>end-cursor)throw std::runtime_error("API payload exceeds its event record");
                if(count)references.emplace(uint32_t(cursor),std::pair{cursor,count});
                cursor+=count;
            }
            block.bytes.assign(bytes.begin()+start,bytes.begin()+start+count);
            event.blocks.push_back(std::move(block));
        }
        if(cursor!=end)throw std::runtime_error("API event block extent mismatch");
        visit(std::move(event));at=end;
    }
    throw std::runtime_error("API capture lacks completion footer");
}
} // namespace gow3::renderer::validation
