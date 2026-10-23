#include <rex/graphics/shared_memory.h>
#include <rex/memory.h>
#include <iostream>
#include <stdexcept>

namespace {
class MemoryModel final:public rex::graphics::SharedMemory {
public:
    explicit MemoryModel(rex::memory::Memory& memory):SharedMemory(memory) {InitializeCommon();}
    using SharedMemory::MakeRangeValid;
    using SharedMemory::PrepareForTraceDownload;
    using SharedMemory::trace_download_ranges;
    bool UploadRanges(const std::vector<std::pair<uint32_t,uint32_t>>& ranges) override {
        for(auto [first,count]:ranges)MakeRangeValid(first<<page_size_log2(),count<<page_size_log2(),false);
        return true;
    }
};
void Require(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
void Watched(const std::unique_lock<std::recursive_mutex>&,void* context,void*,uint64_t,bool) {
    ++*static_cast<unsigned*>(context);
}
}
int main() try {
    rex::memory::Memory memory;
    MemoryModel model(memory);
    constexpr uint32_t cpu=0x10000,gpu=0x20000,bytes=4096;
    unsigned cpu_reload=0,gpu_reload=0;
    model.MakeRangeValid(cpu,bytes,false);
    model.WatchMemoryRange(cpu,bytes,Watched,&cpu_reload,nullptr,0);
    model.MakeRangeValid(gpu,bytes,true);
    model.WatchMemoryRange(gpu,bytes,Watched,&gpu_reload,nullptr,0);
    model.SetSystemPageBlocksValidWithGpuDataWritten();
    Require(!model.IsRangeValid(cpu,bytes)&&model.IsRangeValid(gpu,bytes),"Fixture did not rotate CPU-valid flags");
    Require(!cpu_reload&&!gpu_reload,"Frame rotation should leave decoded textures resident");
    model.PrepareForTraceDownload();
    Require(cpu_reload==1,"Trace omitted resident CPU texture whose validity flags had rotated");
    Require(gpu_reload==1,"Trace did not refresh cached GPU texture from retained storage");
    Require(model.IsRangeValid(gpu,bytes),"Trace discarded GPU-written bytes");
    const auto& downloads=model.trace_download_ranges();
    Require(downloads.size()==1&&downloads[0]==std::pair(gpu,bytes),"Trace GPU download range changed");
    Require(model.RequestRange(cpu,bytes)&&model.IsRangeValid(cpu,bytes),"Invalidated CPU asset could not reload");
    std::cout<<"PASS: trace refreshes resident CPU/GPU views after validity rotation and retains GPU bytes\n";
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
