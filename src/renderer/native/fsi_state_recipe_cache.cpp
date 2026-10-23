#include "fsi_state_recipe_cache.h"
#include <chrono>
#include <fstream>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
namespace gow3::renderer {
struct NativeFsiStateRecipeCache::Lease {
#ifdef _WIN32
    HANDLE handle=INVALID_HANDLE_VALUE;
    explicit Lease(const std::filesystem::path& path) {
        handle=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(handle==INVALID_HANDLE_VALUE)throw std::runtime_error("FSI state cache writer busy");
    }
    ~Lease(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
#else
    int handle=-1;
    explicit Lease(const std::filesystem::path& path) {
        handle=open(path.c_str(),O_CREAT|O_RDWR|O_CLOEXEC,0600);
        if(handle<0)throw std::runtime_error("FSI state cache writer unavailable");
        if(flock(handle,LOCK_EX|LOCK_NB)!=0){close(handle);handle=-1;throw std::runtime_error("FSI state cache writer busy");}
    }
    ~Lease(){if(handle>=0)close(handle);}
#endif
};
NativeFsiStateRecipeCache::NativeFsiStateRecipeCache(std::filesystem::path directory,std::string variant)
    :directory_(std::move(directory)),variant_(std::move(variant)) {
    if(directory_.empty()||variant_.size()!=64||!std::ranges::all_of(variant_,[](char c){
        return (c>='0'&&c<='9')||(c>='a'&&c<='f');}))throw std::invalid_argument("Invalid FSI state cache namespace");
    // Startup only. A bad or absent sidecar is an optional cache miss.
    try {
        std::ifstream input(directory_/"states.json",std::ios::binary|std::ios::ate);
        const auto size=input.tellg();
        if(input&&size>=0&&size<=std::streamoff(NativeFsiStateSidecar::kMaxBytes)) {
            std::string text(static_cast<size_t>(size),'\0');input.seekg(0);
            if(input.read(text.data(),size)) {
                auto parsed=NativeFsiStateSidecar::Parse(text,variant_);
                for(auto& record:parsed.records) {
                    slots_[count_].recipe=std::move(record.recipe);
                    slots_[count_].count.store(record.observed_count,std::memory_order_relaxed);++count_;
                }
            }
        }
    }catch(...){count_=0;}
    stats_.resident=count_;
    // Another process can retain the writer lease. Reading and resident draw
    // counts still work; failure to persist never disables a generic draw.
    try {std::filesystem::create_directories(directory_);lease_=std::make_unique<Lease>(directory_/"writer.lock");}
    catch(...){++stats_.failures;}
    if(lease_)worker_=std::thread([this]{Worker();});
}
NativeFsiStateRecipeCache::~NativeFsiStateRecipeCache(){StopAccepting();if(worker_.joinable())worker_.join();}
std::atomic<uint64_t>* NativeFsiStateRecipeCache::Bind(const NativeFsiStateRecipe& recipe) noexcept {
    if(!ValidFsiStateRecipe(recipe))return nullptr;
    std::unique_lock lock(mutex_,std::try_to_lock);
    if(!lock.owns_lock()){++contended_drops_;return nullptr;}
    if(stopping_)return nullptr;
    for(size_t i=0;i<count_;++i)if(slots_[i].recipe==recipe)return &slots_[i].count;
    if(count_==kMaxRecipes){++stats_.dropped;return nullptr;}
    try {slots_[count_].recipe=recipe;slots_[count_].count.store(0,std::memory_order_relaxed);
         auto* counter=&slots_[count_++].count;++stats_.bound;stats_.resident=count_;changed_.notify_one();return counter;}
    catch(...){++stats_.dropped;return nullptr;}
}
NativeFsiStateSidecar NativeFsiStateRecipeCache::Snapshot() const {
    std::lock_guard lock(mutex_);NativeFsiStateSidecar result;result.variant_version=variant_;
    result.records.reserve(count_);
    for(size_t i=0;i<count_;++i)result.records.push_back({slots_[i].recipe,slots_[i].count.load(std::memory_order_relaxed)});
    return result;
}
std::vector<RankedNativeFsiStateRecipe> NativeFsiStateRecipeCache::Load(std::span<const ObservedNativePipeline> sources) const noexcept {
    try {return Snapshot().Authenticated(sources);}catch(...){return {};}
}
void NativeFsiStateRecipeCache::StopAccepting() noexcept {
    std::lock_guard lock(mutex_);if(!stopping_){stopping_=true;force_=true;++requested_;}changed_.notify_all();
}
void NativeFsiStateRecipeCache::Drain() {
    std::unique_lock lock(mutex_);
    if(!worker_.joinable()||exited_)return;
    const auto target=++requested_;force_=true;changed_.notify_all();
    changed_.wait(lock,[&]{return exited_||completed_>=target;});
}
NativeFsiStateRecipeCache::Statistics NativeFsiStateRecipeCache::statistics() const {
    std::lock_guard lock(mutex_);auto result=stats_;result.dropped+=contended_drops_.load();return result;
}
void NativeFsiStateRecipeCache::Worker() noexcept {
    std::string previous;
    for(;;) {
        bool last=false;uint64_t request=0;
        {std::unique_lock lock(mutex_);
         changed_.wait_for(lock,std::chrono::seconds(1),[&]{return stopping_||force_;});
         last=stopping_;request=requested_;working_=true;}
        try {
            const auto text=Snapshot().Json().dump();
            if(text.size()>NativeFsiStateSidecar::kMaxBytes)throw std::runtime_error("FSI sidecar byte budget");
            if(text!=previous) {
                static std::atomic<uint64_t> serial{0};
                const auto temporary=directory_/("states.json.tmp-"+std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(serial++));
                try {
                    {std::ofstream output(temporary,std::ios::binary|std::ios::trunc);
                     if(!output.write(text.data(),std::streamsize(text.size()))||!output.flush())
                         throw std::runtime_error("FSI state sidecar write failed");}
#ifdef _WIN32
                    if(!MoveFileExW(temporary.c_str(),(directory_/"states.json").c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
                        throw std::runtime_error("FSI state sidecar atomic publication failed");
#else
                    std::filesystem::rename(temporary,directory_/"states.json");
#endif
                }catch(...){std::error_code ignored;std::filesystem::remove(temporary,ignored);throw;}
                previous=text;std::lock_guard lock(mutex_);++stats_.written;
            }
        }catch(...){std::lock_guard lock(mutex_);++stats_.failures;}
        {std::lock_guard lock(mutex_);completed_=std::max(completed_,request);
         force_=requested_>request;working_=false;if(last)exited_=true;changed_.notify_all();}
        if(last)return;
    }
}
}
