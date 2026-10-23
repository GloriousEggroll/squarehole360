#include "renderer/native/memory_export_copy.h"
#include "renderer/native/resolve_memory.h"
#include "renderer/native/live_plan.h"
#include "memory_store_spv.h"
#include "memory_load_spv.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace plume {std::unique_ptr<RenderInterface> CreateVulkanInterface();}
using namespace gow3::renderer;
namespace fs=std::filesystem;
std::vector<uint8_t> Read(const fs::path& path) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);const auto size=f.tellg();
    if(!f||size<0||size>16*1024*1024)throw std::runtime_error("Invalid fixture input");
    std::vector<uint8_t> bytes(size);f.seekg(0);f.read(reinterpret_cast<char*>(bytes.data()),size);
    if(!f)throw std::runtime_error("Truncated fixture input");return bytes;
}
int main(int argc,char** argv) {
    try {
        if(argc!=2)throw std::runtime_error("usage: memory-export-copy-test failed-event-directory");
        const fs::path folder=argv[1];
        auto shader=Read(folder/"block-2.bin"),physical=Read(folder/"block-3.bin"),state=Read(folder/"block-1.bin");
        shader.insert(shader.end(),physical.begin(),physical.end());
        std::ifstream event_file(folder/"event.json");nlohmann::json event;event_file>>event;
        const auto args=event.at("args").get<std::array<uint32_t,8>>();
        const auto copy=DecodeMemoryExportCopy(shader,state,args);
        const auto payload=Read(folder/"block-14.bin");
        if(copy.source!=0x16b22000||copy.destination!=0x164ab000||copy.bytes!=8192||copy.source_offset)
            throw std::runtime_error("Captured memcpy decode mismatch");
        // Independent scalar interpretation of the captured VFETCH format,
        // yxwz swizzle, UINT conversion and export endian. Every 16-bit integer
        // is represented exactly by float, so no numeric precision is lost.
        std::vector<uint8_t> expected(copy.bytes);
        for(size_t at=0;at<expected.size();at+=8) {
            uint16_t fetched[4];
            for(unsigned c=0;c<4;++c)fetched[c]=uint16_t(payload[at+((2*c)^3)])|
                uint16_t(payload[at+((2*c+1)^3)])<<8;
            for(unsigned c=0;c<4;++c) {
                const uint16_t output=uint16_t(float(fetched[c^1]));
                expected[at+((2*c)^1)]=uint8_t(output);
                expected[at+((2*c+1)^1)]=uint8_t(output>>8);
            }
        }
        if(expected!=payload)throw std::runtime_error("Memcpy endian interpretation changed bytes");
        auto put=[](auto& bytes,size_t offset,uint32_t value){for(unsigned i=0;i<4;++i)bytes[offset+i]=value>>(24-i*8);};
        GraphicsBoundaryEvent boundary;boundary.function=event.at("function");boundary.phase=event.at("phase");
        boundary.frame=event.at("frame");boundary.sequence=event.at("event");boundary.args=args;
        for(const auto& block:event.at("blocks"))boundary.blocks.push_back({block.at("kind"),block.at("address"),
            Read(folder/block.at("path").get<std::string>())});
        GearsLivePlan planner;planner.Push(GraphicsBoundaryEvent(boundary));
        // Native output becomes the next copy's input without borrowing the
        // reference CPU snapshot. A later CPU overwrite must revoke ownership.
        auto reused=boundary;put(reused.blocks.at(0).bytes,0x480+95*8,copy.destination|3);
        put(reused.blocks.at(0).bytes,0x780+6*16,0x40000000|(copy.source>>2));
        reused.blocks.at(13).address=copy.destination;planner.Push(GraphicsBoundaryEvent(reused));
        auto cpu_update=payload;for(auto& b:cpu_update)b^=0xa7;
        planner.InvalidateMemory(copy.destination,copy.bytes,cpu_update);
        reused.blocks.at(13).bytes=cpu_update;planner.Push(std::move(reused));
        GraphicsBoundaryEvent present;present.function=0x82AF6B10;present.frame=boundary.frame;
        present.blocks={{1,0x83000000,std::vector<uint8_t>(0x3600)},
            {103,0x83020000,std::vector<uint8_t>(64)}};
        auto& header=present.blocks[1].bytes;
        put(header,28,(40u<<22)|2);put(header,32,0x10000006);put(header,36,1279|(719<<13));put(header,48,1<<9);
        const auto frame=planner.Push(std::move(present));
        if(!frame||frame->operations.size()!=5||frame->operations.at(0).Control().at("source_kind")!="cpu_snapshot"||
            frame->operations.at(1).Control().at("source_kind")!="gpu_memory"||frame->operations.at(2).Control().at("type")!="cpu_memory_write"||
            frame->operations.at(3).Control().at("source_kind")!="cpu_snapshot"||
            *frame->payloads.at(frame->operations.at(3).Control().at("payload").get<std::string>())!=cpu_update)
            throw std::runtime_error("Native copy ownership did not follow GPU producer and CPU invalidation");
        // Reallocation can leave a GPU-owned prefix/suffix around CPU bytes.
        // The old any-overlap test discarded the only source for those holes.
        GearsLivePlan mixed_planner;mixed_planner.Push(GraphicsBoundaryEvent(boundary));
        mixed_planner.InvalidateMemory(copy.destination+3,5);
        auto mixed_boundary=boundary;
        put(mixed_boundary.blocks.at(0).bytes,0x480+95*8,copy.destination|3);
        put(mixed_boundary.blocks.at(0).bytes,0x780+6*16,0x40000000|(copy.source>>2));
        mixed_boundary.blocks.at(13).address=copy.destination;
        mixed_boundary.blocks.at(13).bytes=cpu_update;
        mixed_planner.Push(std::move(mixed_boundary));
        GraphicsBoundaryEvent mixed_present;mixed_present.function=0x82AF6B10;mixed_present.frame=boundary.frame;
        mixed_present.blocks={{1,0x83000000,std::vector<uint8_t>(0x3600)},
            {103,0x83020000,std::vector<uint8_t>(64)}};
        auto& mixed_header=mixed_present.blocks[1].bytes;
        put(mixed_header,28,(40u<<22)|2);put(mixed_header,32,0x10000006);
        put(mixed_header,36,1279|(719<<13));put(mixed_header,48,1<<9);
        const auto mixed_frame=mixed_planner.Push(std::move(mixed_present));
        const std::vector<std::pair<uint32_t,uint32_t>> planned_ranges{
            {copy.destination,copy.destination+3},{copy.destination+8,copy.destination+copy.bytes}};
        const auto& mixed_op=mixed_frame->operations.at(2).Control();
        if(mixed_op.at("source_kind")!="mixed_memory"||mixed_op.at("gpu_ranges")!=nlohmann::json(planned_ranges)||
           *mixed_frame->payloads.at(mixed_op.at("payload").get<std::string>())!=cpu_update)
            throw std::runtime_error("Partial GPU copy discarded immutable CPU holes or changed exact byte ranges");
        auto reject=[&](auto code,auto raw,auto draw){
            bool rejected=false;try{DecodeMemoryExportCopy(code,raw,draw);}catch(const std::runtime_error&){rejected=true;}
            if(!rejected)throw std::runtime_error("Unsafe memcpy state accepted");
        };
        auto changed_shader=shader;changed_shader.back()^=1;reject(changed_shader,state,args);
        auto overlap=state;put(overlap,0x780+6*16,0x40000000|(copy.source>>2));reject(shader,overlap,args);
        auto endian=state;endian[0x480+95*8+7]^=1;reject(shader,endian,args);
        auto too_large=args;++too_large[3];reject(shader,state,too_large);
        auto subrange=args;subrange[2]=1;subrange[3]=1;
        const auto chunk=DecodeMemoryExportCopy(shader,state,subrange);
        if(chunk.source!=copy.source+64||chunk.destination!=copy.destination+64||chunk.bytes!=64)
            throw std::runtime_error("Memcpy draw index offset was lost");
        auto interface=plume::CreateVulkanInterface();auto device=interface->createDevice();
        if(!device)throw std::runtime_error("Vulkan unavailable");
        auto queue=device->createCommandQueue(plume::RenderCommandListType::DIRECT);
        ResolveMemory memory(*device,*queue,memory_store_spv,memory_load_spv,512*1024*1024);
        bool undefined=false;try{memory.Copy(copy.source,copy.destination,copy.bytes);}catch(const std::runtime_error&){undefined=true;}
        if(!undefined)throw std::runtime_error("GPU copied unknown source history");
        std::vector<uint8_t> guards(copy.bytes+8,0xa5);memory.Initialize(copy.destination-4,guards);
        const auto check_output=[&](const std::vector<uint8_t>& wanted) {
            const auto output=memory.ReadBytes(copy.destination-4,copy.bytes+8);
            if(!std::equal(wanted.begin(),wanted.end(),output.begin()+4)||
                !std::all_of(output.begin(),output.begin()+4,[](uint8_t b){return b==0xa5;})||
                !std::all_of(output.end()-4,output.end(),[](uint8_t b){return b==0xa5;}))
                throw std::runtime_error("Native CPU memcpy differed or overwrote adjacent bytes");
        };
        memory.CopyFromCpu(copy.source,copy.destination,payload);
        check_output(expected);
        if(memory.BytesDefined(copy.source,copy.bytes))
            throw std::runtime_error("CPU memcpy promoted its unwatched source to native history");
        memory.CopyFromCpu(copy.source,copy.destination,cpu_update);
        check_output(cpu_update);
        const auto exported=memory.ExportState();
        for(const auto& [first,count]:exported.ranges)
            if(uint64_t(first)+count>copy.source&&uint64_t(copy.source)+copy.bytes>first)
                throw std::runtime_error("CPU memcpy exported source ownership");
        const auto reject_cpu_copy=[&](uint32_t source,uint32_t destination,std::span<const uint8_t> bytes) {
            const auto serial=memory.mutation_serial();bool rejected=false;
            try{memory.CopyFromCpu(source,destination,bytes);}catch(const std::runtime_error&){rejected=true;}
            if(!rejected||serial!=memory.mutation_serial())
                throw std::runtime_error("Invalid CPU memcpy was accepted or mutated native history");
        };
        reject_cpu_copy(copy.source,copy.destination,{});
        reject_cpu_copy(copy.source+1,copy.destination,payload);
        reject_cpu_copy(copy.source,copy.destination+1,payload);
        reject_cpu_copy(copy.source,copy.destination,std::span(payload).first(payload.size()-1));
        reject_cpu_copy(512*1024*1024-4,copy.destination,payload);
        reject_cpu_copy(copy.source,512*1024*1024-4,payload);
        reject_cpu_copy(copy.source,copy.source+4,payload);
        reject_cpu_copy(copy.source,copy.source-4,payload);
        reject_cpu_copy(copy.source,copy.source,payload);
        check_output(cpu_update);
        // Supplying CPU bytes must not overwrite any preexisting source state.
        memory.Initialize(copy.source,payload);
        memory.CopyFromCpu(copy.source,copy.destination,cpu_update);
        if(memory.ReadBytes(copy.source,copy.bytes)!=payload)
            throw std::runtime_error("CPU memcpy overwrote preexisting source history");
        memory.Copy(copy.source,copy.destination,copy.bytes);
        const auto output=memory.ReadBytes(copy.destination-4,copy.bytes+8);
        if(!std::equal(expected.begin(),expected.end(),output.begin()+4)||
            !std::all_of(output.begin(),output.begin()+4,[](uint8_t b){return b==0xa5;})||
            !std::all_of(output.end()-4,output.end(),[](uint8_t b){return b==0xa5;}))
            throw std::runtime_error("GPU memcpy differed or overwrote adjacent bytes");
        bool gpu_overlap=false;try{memory.Copy(copy.source,copy.source+4,64);}catch(const std::runtime_error&){gpu_overlap=true;}
        if(!gpu_overlap)throw std::runtime_error("GPU overlapping copy accepted");
        // Compare the mixed operation to an independent byte-by-byte oracle.
        // Unaligned holes exercise readback padding without promoting it.
        memory.Invalidate(copy.source+3,5);memory.Invalidate(copy.source+4093,17);
        const std::vector<std::pair<uint32_t,uint32_t>> fragments{
            {copy.source,copy.source+3},{copy.source+8,copy.source+4093},
            {copy.source+4110,copy.source+copy.bytes}};
        auto mixed_expected=cpu_update;
        for(size_t i=0;i<payload.size();++i)
            if(i<3||(i>=8&&i<4093)||i>=4110)mixed_expected[i]=payload[i];
        const auto source_state=memory.ExportState();
        memory.CopyFromMixed(copy.source,copy.destination,cpu_update,fragments);
        check_output(mixed_expected);
        if(memory.ExportState().ranges!=source_state.ranges||memory.BytesDefined(copy.source+3,1)||
           memory.BytesDefined(copy.source+4093,1))
            throw std::runtime_error("Mixed copy promoted undefined source/padding bytes");
        for(const auto& [first,last]:fragments) {
            // Exact byte ownership is checked separately; only aligned interior
            // ranges may be used by the public ReadBytes contract.
            const uint32_t a=(first+3)&~3u,b=last&~3u;
            if(a<b&&memory.ReadBytes(a,b-a)!=std::vector<uint8_t>(payload.begin()+a-copy.source,payload.begin()+b-copy.source))
                throw std::runtime_error("Mixed copy changed authoritative source bytes");
        }
        const auto reject_mixed=[&](uint32_t source,uint32_t destination,
                std::span<const uint8_t> bytes,const std::vector<std::pair<uint32_t,uint32_t>>& ranges) {
            const auto serial=memory.mutation_serial();bool rejected=false;
            try{memory.CopyFromMixed(source,destination,bytes,ranges);}catch(const std::runtime_error&){rejected=true;}
            if(!rejected||memory.mutation_serial()!=serial)
                throw std::runtime_error("Invalid mixed copy was accepted or changed destination history");
            check_output(mixed_expected);
        };
        reject_mixed(copy.source,copy.destination,cpu_update,{});
        reject_mixed(copy.source,copy.destination,cpu_update,{{copy.source,copy.source+8}});
        reject_mixed(copy.source,copy.destination,cpu_update,{{copy.source-1,copy.source+3}});
        reject_mixed(copy.source,copy.destination,cpu_update,{{copy.source,copy.source+copy.bytes+1}});
        reject_mixed(copy.source,copy.destination,cpu_update,{{copy.source+8,copy.source+16},{copy.source+9,copy.source+12}});
        reject_mixed(copy.source,copy.source+4,cpu_update,fragments);
        reject_mixed(copy.source+1,copy.destination,cpu_update,fragments);
        reject_mixed(copy.source,copy.destination,std::span(cpu_update).first(cpu_update.size()-1),fragments);
        std::cout<<"Native memcpy: real8192-byte fixture, endian equivalence, CPU/GPU/mixed provenance, immutable CPU holes, unaligned GPU fragments, missing-history rejection, source ownership and destination guards passed\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
