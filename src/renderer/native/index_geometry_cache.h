#pragma once
#include "index_data.h"
#include <algorithm>
#include <compare>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gow3::renderer {
enum class IndexGeometrySource : uint8_t {Sequential,CpuPayload,GpuMemory};
struct IndexGeometryParameters {
    IndexGeometrySource source=IndexGeometrySource::Sequential;
    uint32_t count=0,primitive=4,first_index=0,width=0,endian=0,minimum_index=0;
    bool up_padding=false,rectangle=false,strip_restart=false;
    auto operator<=>(const IndexGeometryParameters&)const=default;
};
// Validate on every lookup too: cached GPU storage must not hide changed draw
// state or an invalid provider extent. Base-vertex offset is applied by Draw,
// never incorporated into these index words.
inline uint32_t IndexGeometryCount(std::span<const uint8_t> bytes,const IndexGeometryParameters& p) {
    if(!p.count||p.count>4u*1024*1024)throw std::runtime_error("Invalid draw count");
    if(p.primitive!=2&&p.primitive!=3&&p.primitive!=4&&p.primitive!=6&&p.primitive!=13&&!(p.primitive==8&&p.rectangle))
        throw std::runtime_error("Unsupported scene primitive "+std::to_string(p.primitive));
    if(p.rectangle&&(p.source!=IndexGeometrySource::Sequential||!p.up_padding||p.primitive!=8||p.count!=3))
        throw std::runtime_error("Unsupported rectangle vertex program/layout");
    if(p.source!=IndexGeometrySource::Sequential) {
        if(p.up_padding&&p.minimum_index)throw std::runtime_error("Unsupported UP minimum index");
        ValidateIndexPayload(bytes,p.count,p.width,p.endian,p.up_padding);
    }
    uint64_t count=p.count;
    if(p.primitive==13) {
        if(p.count%4)throw std::runtime_error("Incomplete quad");
        count=uint64_t(p.count)/4*6;
    } else if(p.primitive==6) {
        if(p.strip_restart)throw std::runtime_error("Strip restart unsupported");
        count=p.count>2?uint64_t(p.count-2)*3:0;
    } else if(p.primitive==3) {
        // Line strips draw as line lists, like triangle strips as lists.
        if(p.strip_restart)throw std::runtime_error("Strip restart unsupported");
        count=p.count>1?uint64_t(p.count-1)*2:0;
    } else if(p.primitive==2)count=p.count/2*2;
    if(p.rectangle)count=6;
    if(!count||count*4>16u*1024*1024)throw std::runtime_error("Invalid draw upload extent");
    return uint32_t(count);
}
inline std::vector<uint32_t> BuildIndexGeometry(std::span<const uint8_t> bytes,const IndexGeometryParameters& p) {
    const auto count=IndexGeometryCount(bytes,p);
    if(p.rectangle)return {0,1,2,2,1,3};
    std::vector<uint32_t> source;
    if(p.source==IndexGeometrySource::Sequential) {
        source.resize(p.count);std::iota(source.begin(),source.end(),p.first_index);
    } else source=DecodeIndexPayload(bytes,p.count,p.width,p.endian,p.up_padding);
    if(p.primitive==2)source.resize(count);
    if(p.primitive!=3&&p.primitive!=6&&p.primitive!=13)return source;
    std::vector<uint32_t> triangles;triangles.reserve(count);
    if(p.primitive==3) {
        for(uint32_t i=1;i<p.count;++i){triangles.push_back(source[i-1]);triangles.push_back(source[i]);}
    } else if(p.primitive==13) {
        for(uint32_t i=0;i<p.count;i+=4)for(auto j:{0u,1u,2u,0u,2u,3u})triangles.push_back(source[i+j]);
    } else {
        for(uint32_t i=2;i<p.count;++i) {
            triangles.push_back(source[i%2?i-1:i-2]);triangles.push_back(source[i%2?i-2:i-1]);
            triangles.push_back(source[i]);
        }
    }
    return triangles;
}

template<class String> struct IndexGeometryKey {IndexGeometryParameters parameters;String digest;};
using IndexGeometryIdentity=IndexGeometryKey<std::string>;
using IndexGeometryLookup=IndexGeometryKey<std::string_view>;
struct IndexGeometryLess {
    using is_transparent=void;
    template<class A,class B>bool operator()(const IndexGeometryKey<A>& a,const IndexGeometryKey<B>& b)const {
        if(a.parameters!=b.parameters)return a.parameters<b.parameters;
        return std::string_view(a.digest)<std::string_view(b.digest);
    }
};

// Resources remain immutable and resident until the caller's fenced frame
// boundary. Capacity misses use the existing uncached upload path, never force
// a submission/wait. The byte cap includes retained source + GPU index bytes;
// driver allocation overhead and bounded map/list metadata are not included.
// Different immutable owners must compare exact bytes, even for equal digests.
// This keeps file/legacy providers and malformed reused identities observable.
template<class Resource>class IndexGeometryCache {
public:
    using Bytes=std::vector<uint8_t>;
    using Snapshot=std::shared_ptr<const Bytes>;
    struct Statistics {uint64_t hits=0,misses=0,gpu_bypasses=0,changed_sources=0,admitted=0,capacity_misses=0,retired=0,adopted_owners=0;};
    explicit IndexGeometryCache(size_t max_bytes=32u*1024*1024,size_t max_entries=1024)
        :max_bytes_(max_bytes),max_entries_(max_entries){}
    Resource* Find(const IndexGeometryLookup& key,std::span<const uint8_t> source,const Snapshot& owner={}) {
        IndexGeometryCount(source,key.parameters);
        if(key.parameters.source==IndexGeometrySource::GpuMemory) {++statistics_.gpu_bypasses;return nullptr;}
        const auto found=entries_.find(key);
        if(found==entries_.end()) {++statistics_.misses;return nullptr;}
        auto& entry=found->second;
        if(key.parameters.source==IndexGeometrySource::CpuPayload) {
            const bool same_owner=owner&&owner==entry.source&&source.data()==owner->data()&&source.size()==owner->size();
            if(!same_owner&&(source.size()!=entry.source->size()||
               !std::equal(source.begin(),source.end(),entry.source->begin()))) {
                if(!entry.changed){entry.changed=true;++statistics_.changed_sources;}
            } else if(!same_owner&&!entry.changed&&owner&&source.data()==owner->data()&&source.size()==owner->size()) {
                // Verified identical bytes: retain the newest immutable owner so the
                // next draw from the same snapshot needs only the pointer check.
                // Equal sizes keep retained_bytes_ exact.
                entry.source=owner;++statistics_.adopted_owners;
            }
        }
        if(entry.changed) {++statistics_.misses;return nullptr;}
        ++statistics_.hits;entry.used=true;order_.splice(order_.end(),order_,entry.age);return &entry.resource;
    }
    bool CanAdmit(const IndexGeometryLookup& key,size_t source_bytes,size_t index_bytes) {
        if(key.parameters.source==IndexGeometrySource::GpuMemory||entries_.contains(key))return false;
        if(key.parameters.source==IndexGeometrySource::Sequential)source_bytes=0;
        if(!max_entries_||source_bytes>max_bytes_||index_bytes>max_bytes_-source_bytes)return false;
        const size_t bytes=source_bytes+index_bytes;
        if(entries_.size()>=max_entries_||bytes>max_bytes_-retained_bytes_) {
            pressure_=true;++statistics_.capacity_misses;return false;
        }
        return true;
    }
    template<class Create>Resource* Insert(const IndexGeometryLookup& key,Snapshot source,size_t index_bytes,Create&& create) {
        if(key.parameters.source==IndexGeometrySource::Sequential)source.reset();
        else if(!source)throw std::runtime_error("Missing cached index source owner");
        const auto bytes=source?std::span<const uint8_t>(*source):std::span<const uint8_t>();
        if(uint64_t(IndexGeometryCount(bytes,key.parameters))*4!=index_bytes)
            throw std::runtime_error("Cached index extent differs from validated geometry");
        if(!CanAdmit(key,bytes.size(),index_bytes))return nullptr;
        auto resource=create();
        order_.push_back({key.parameters,std::string(key.digest)});const auto age=std::prev(order_.end());
        try {
            const size_t retained=bytes.size()+index_bytes;
            auto [stored,inserted]=entries_.emplace(*age,Entry{std::move(resource),std::move(source),retained,index_bytes,true,false,age});
            if(!inserted)throw std::runtime_error("Duplicate cached index admission");
            retained_bytes_+=retained;index_bytes_+=index_bytes;++statistics_.admitted;
            return &stored->second.resource;
        } catch(...) {order_.erase(age);throw;}
    }
    // Caller must first fence every pending/preparing draw. Persistent entries
    // survive ordinary empty frames. Only pressure retires LRU entries unused
    // in this frame, toward 75% of both caps; changed sources retire here too.
    void RetireAfterFence() {
        const size_t target_bytes=max_bytes_/4*3+(max_bytes_%4)*3/4;
        const size_t target_entries=max_entries_/4*3+(max_entries_%4)*3/4;
        for(auto age=order_.begin();age!=order_.end();) {
            const auto found=entries_.find(*age);auto& entry=found->second;
            const bool pressure_victim=pressure_&&!entry.used&&
                (retained_bytes_>target_bytes||entries_.size()>target_entries);
            if(entry.changed||pressure_victim) {
                retained_bytes_-=entry.retained;index_bytes_-=entry.index_bytes;
                entries_.erase(found);age=order_.erase(age);++statistics_.retired;
            } else ++age;
        }
        for(auto& [key,entry]:entries_)entry.used=false;
        pressure_=false;
    }
    size_t retained_bytes()const{return retained_bytes_;}
    size_t index_bytes()const{return index_bytes_;}
    size_t size()const{return entries_.size();}
    const Statistics& statistics()const{return statistics_;}
private:
    using Order=std::list<IndexGeometryIdentity>;
    struct Entry {Resource resource;Snapshot source;size_t retained,index_bytes;bool used,changed;typename Order::iterator age;};
    size_t max_bytes_,max_entries_,retained_bytes_=0,index_bytes_=0;
    bool pressure_=false;
    Statistics statistics_;
    Order order_;
    std::map<IndexGeometryIdentity,Entry,IndexGeometryLess> entries_;
};
}
