#include "renderer/native/index_geometry_cache.h"
#include "renderer/native/owned_payload.h"
#include <nlohmann/json.hpp>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

using namespace gow3::renderer;
using Bytes=std::vector<uint8_t>;
using Indices=std::vector<uint32_t>;
using Owner=std::shared_ptr<const Bytes>;
using Buffer=std::shared_ptr<const Indices>;
using Json=nlohmann::json;
void Check(bool okay,const char* message){if(!okay)throw std::runtime_error(message);}
// Independent frozen pre-cache Draw implementation. The payload oracle uses
// explicit byte swaps and runtime-width reconstruction, not DecodeIndexPayload.
Indices Previous(std::span<const uint8_t> input,const IndexGeometryParameters& p) {
    if(!p.count||p.count>4u*1024*1024)throw std::runtime_error("Invalid draw count");
    if(p.primitive!=4&&p.primitive!=6&&p.primitive!=13&&!(p.primitive==8&&p.rectangle))
        throw std::runtime_error("Unsupported scene primitive "+std::to_string(p.primitive));
    if(p.rectangle&&(p.source!=IndexGeometrySource::Sequential||!p.up_padding||p.primitive!=8||p.count!=3))
        throw std::runtime_error("Unsupported rectangle vertex program/layout");
    Indices indices;
    if(p.source==IndexGeometrySource::Sequential) {
        indices.resize(p.count);std::iota(indices.begin(),indices.end(),p.first_index);
    } else {
        if(p.up_padding&&p.minimum_index)throw std::runtime_error("Unsupported UP minimum index");
        const uint64_t extent=uint64_t(p.count)*p.width;
        if((p.width!=2&&p.width!=4)||p.endian>3||(p.width==2&&p.endian>1)||
           input.size()!=(p.up_padding?(extent+3)/4*4:extent))throw std::runtime_error("Invalid index payload");
        Bytes bytes(input.begin(),input.end());
        if(p.endian==1||p.endian==2)for(size_t i=0;i<bytes.size();i+=2)std::swap(bytes[i],bytes[i+1]);
        if(p.endian==2||p.endian==3)for(size_t i=0;i<bytes.size();i+=4){std::swap(bytes[i],bytes[i+2]);std::swap(bytes[i+1],bytes[i+3]);}
        for(uint32_t i=0;i<p.count;++i) {
            uint32_t value=0;for(uint32_t j=0;j<p.width;++j)value|=uint32_t(bytes[size_t(i)*p.width+j])<<(8*j);
            indices.push_back(value);
        }
    }
    if(p.primitive==6||p.primitive==13) {
        Indices triangles;
        if(p.primitive==13) {
            if(p.count%4)throw std::runtime_error("Incomplete quad");
            for(uint32_t i=0;i<p.count;i+=4)for(auto j:{0,1,2,0,2,3})triangles.push_back(indices[i+j]);
        } else {
            if(p.strip_restart)throw std::runtime_error("Strip restart unsupported");
            for(uint32_t i=2;i<p.count;++i) {
                triangles.push_back(indices[i%2?i-1:i-2]);triangles.push_back(indices[i%2?i-2:i-1]);triangles.push_back(indices[i]);
            }
        }
        indices=std::move(triangles);
    }
    if(p.rectangle)indices={0,1,2,2,1,3};
    if(indices.empty()||indices.size()*4>16u*1024*1024)throw std::runtime_error("Invalid draw upload extent");
    return indices;
}
struct Outcome {Indices values;std::string error;bool operator==(const Outcome&)const=default;};
template<class F>Outcome Run(F&& f) {
    try{return {f(),{}};}catch(const std::runtime_error& e){return {{},e.what()};}
}
void Compare(std::span<const uint8_t> bytes,const IndexGeometryParameters& p) {
    Check(Run([&]{return Previous(bytes,p);})==Run([&]{return BuildIndexGeometry(bytes,p);}),
          "Index geometry differs from frozen Draw bytes or rejection");
}
size_t Synthetic() {
    size_t cases=0;
    for(auto source:{IndexGeometrySource::CpuPayload,IndexGeometrySource::GpuMemory})
    for(uint32_t width:{2u,4u})for(uint32_t endian=0;endian<4;++endian)
    for(bool up:{false,true})for(uint32_t count:{1u,2u,3u,4u,7u,8u,15u,32u,255u})
    for(uint32_t primitive:{4u,6u,13u}) {
        IndexGeometryParameters p;p.source=source;p.width=width;p.endian=endian;p.up_padding=up;p.count=count;p.primitive=primitive;
        const size_t extent=up?(size_t(count)*width+3)/4*4:size_t(count)*width;
        Bytes guarded(extent+5,0xda);for(size_t i=0;i<extent;++i)guarded[i+1]=uint8_t(i*37^(i>>3)^0x96);
        const auto unchanged=guarded;const std::span<const uint8_t> bytes(guarded.data()+1,extent);
        Compare(bytes,p);++cases;
        p.strip_restart=true;Compare(bytes,p);++cases;
        p.minimum_index=1;Compare(bytes,p);++cases;
        Check(guarded==unchanged,"Index cache changed immutable source/padding/guards");
    }
    for(uint32_t first:{0u,1u,0xfffffffdu})for(uint32_t count:{1u,2u,3u,4u,7u,8u,31u})
    for(uint32_t primitive:{4u,6u,13u})for(bool restart:{false,true}) {
        IndexGeometryParameters p;p.first_index=first;p.count=count;p.primitive=primitive;p.strip_restart=restart;
        Compare({},p);++cases;
    }
    IndexGeometryParameters rectangle;rectangle.count=3;rectangle.primitive=8;rectangle.up_padding=true;rectangle.rectangle=true;
    Compare({},rectangle);++cases;
    for(auto invalid:{0u,4u*1024*1024+1}){auto p=rectangle;p.count=invalid;Compare({},p);++cases;}
    auto p=rectangle;p.up_padding=false;Compare({},p);++cases;
    p=rectangle;p.primitive=4;Compare({},p);++cases;
    p={};p.source=IndexGeometrySource::CpuPayload;p.count=3;p.width=2;p.endian=1;
    for(size_t size:{0u,5u,7u,8u}){Compare(Bytes(size),p);++cases;}
    p.width=3;Compare(Bytes(9),p);++cases;
    p.width=4;p.endian=4;Compare(Bytes(12),p);++cases;
    return cases;
}
void OwnershipAndBounds() {
    IndexGeometryParameters p;p.source=IndexGeometrySource::CpuPayload;p.count=4;p.width=2;p.endian=1;
    const std::string digest(64,'a');IndexGeometryLookup key{p,digest};
    IndexGeometryCache<Buffer> cache(128,4);
    auto source=std::make_shared<const Bytes>(Bytes{0,1,0,2,0,3,0,4});
    std::weak_ptr<const Bytes> retained_source=source;
    const auto words=BuildIndexGeometry(*source,p);
    auto* stored=cache.Insert(key,source,words.size()*4,[&]{return std::make_shared<const Indices>(words);});
    Check(stored&&cache.retained_bytes()==24&&cache.index_bytes()==16,"Combined index/source budget mismatch");
    const auto* stable=stored;std::weak_ptr<const Indices> resource=*stored;
    Check(cache.Find(key,*source,source)==stored,"Same immutable owner missed index cache");
    const Bytes legacy=*source;
    Check(cache.Find(key,legacy)==stored,"Equal legacy/file bytes missed index cache");
    auto equal=std::make_shared<const Bytes>(legacy);
    Check(cache.Find(key,*equal,equal)==stored,"Different exact immutable owner missed cache");
    // A verified identical owner is adopted (adopted_owners); the cache now
    // retains that newest owner instead of the first one.
    Check(cache.statistics().adopted_owners==1,"Identical immutable owner was not adopted");
    retained_source=equal;
    auto changed=legacy;changed[3]^=0x40;
    Check(!cache.Find(key,changed),"Equal digest hid changed index bytes");
    Check(!cache.CanAdmit(key,changed.size(),16),"In-flight old index resource was replaceable");
    Check(!resource.expired()&&**stable==words,"Changed source destroyed or mutated pending index resource");
    source.reset();equal.reset();Check(!retained_source.expired(),"Cached source owner died before fence");
    cache.RetireAfterFence();Check(resource.expired()&&retained_source.expired()&&cache.size()==0,"Changed source survived fenced retirement");
    p.source=IndexGeometrySource::GpuMemory;key.parameters=p;
    unsigned creates=0;
    for(const auto& bytes:{legacy,changed}) {
        Check(!cache.Find(key,bytes),"GPU indices reused a stale CPU cache entry");
        auto owner=std::make_shared<const Bytes>(bytes);
        Check(!cache.Insert(key,owner,16,[&]{++creates;return Buffer{};}),"GPU index data was admitted");
        Compare(bytes,p);
    }
    Check(creates==0&&cache.statistics().gpu_bypasses==2,"GPU bypass allocated a cached resource");
    // Source ownership transfer preserves data addresses for owned and shared providers.
    Bytes owned{1,2,3,4};const auto* address=owned.data();OwnedPayload payload(std::move(owned));
    Check(!payload.snapshot(),"Owned provider invented a preexisting shared owner");
    auto shared=std::move(payload).TakeSnapshot();Check(shared->data()==address,"Retaining provider source copied bytes");
    OwnedPayload borrowed(shared);Check(borrowed.snapshot()==shared&&std::move(borrowed).TakeSnapshot()==shared,"Shared provider changed immutable owner");
    // Persistent LRU: ordinary empty frames retain resources; pressure evicts
    // only unused entries at the explicit fenced boundary, down to 75%.
    IndexGeometryCache<Buffer> bounded(64,4);p={};p.count=3;
    std::array<std::weak_ptr<const Indices>,4> old;
    std::array<Buffer*,4> pointers{};
    for(unsigned i=0;i<4;++i) {
        p.first_index=i;IndexGeometryLookup lookup{p,{}};
        pointers[i]=bounded.Insert(lookup,{},12,[&]{return std::make_shared<const Indices>(BuildIndexGeometry({},p));});
        old[i]=*pointers[i];Check(bounded.retained_bytes()<=64&&bounded.size()<=4,"Index cache exceeded hard cap");
    }
    bounded.RetireAfterFence();bounded.RetireAfterFence();Check(bounded.size()==4,"Empty frame evicted persistent indices");
    p.first_index=0;Check(bounded.Find({p,{}},{})==pointers[0],"Map growth invalidated cached buffer pointer");
    p.first_index=4;Check(!bounded.CanAdmit({p,{}},0,12),"Entry cap admitted excess resource");
    for(const auto& weak:old)Check(!weak.expired(),"Capacity pressure evicted a pending resource");
    bounded.RetireAfterFence();Check(bounded.size()==3&&old[1].expired()&&!old[0].expired(),"Fenced pressure eviction violated LRU/hot-entry retention");
    Check(bounded.CanAdmit({p,{}},0,12),"Pressure retirement did not create admission space");
    // A single over-budget entry is an uncached fallback, without evicting the working set.
    Check(!bounded.CanAdmit({p,{}},0,65),"Oversized index entry exceeded byte cap");
    bounded.RetireAfterFence();Check(bounded.size()==3,"Oversized fallback evicted unrelated cached resources");
    IndexGeometryCache<Buffer> bytes_only(23,99);p.source=IndexGeometrySource::CpuPayload;p.count=4;p.width=2;p.endian=1;
    Check(!bytes_only.CanAdmit({p,digest},8,16),"Source bytes were omitted from admission limit");
}
void KeyFields() {
    IndexGeometryParameters base;base.count=4;
    std::vector<IndexGeometryIdentity> keys{{base,"a"}};
    auto add=[&](auto change){auto p=base;change(p);keys.push_back({p,"a"});};
    add([](auto& p){p.source=IndexGeometrySource::CpuPayload;});add([](auto& p){++p.count;});
    add([](auto& p){++p.primitive;});add([](auto& p){++p.first_index;});add([](auto& p){++p.width;});
    add([](auto& p){++p.endian;});add([](auto& p){++p.minimum_index;});add([](auto& p){p.up_padding=true;});
    add([](auto& p){p.rectangle=true;});add([](auto& p){p.strip_restart=true;});keys.push_back({base,"b"});
    std::set<IndexGeometryIdentity,IndexGeometryLess> distinct(keys.begin(),keys.end());
    Check(distinct.size()==keys.size(),"Index cache omitted a transformation field from identity");
}
Bytes Read(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary|std::ios::ate);const auto size=input.tellg();
    if(!input||size<0||size>16*1024*1024)throw std::runtime_error("Invalid fixture payload "+path.string());
    Bytes bytes(static_cast<size_t>(size));input.seekg(0);input.read(reinterpret_cast<char*>(bytes.data()),size);
    if(!input)throw std::runtime_error("Truncated fixture payload");
    return bytes;
}
uint32_t BE(std::span<const uint8_t> b,size_t at) {
    if(at+4>b.size())throw std::runtime_error("Truncated capture state/header");
    return uint32_t(b[at])<<24|uint32_t(b[at+1])<<16|uint32_t(b[at+2])<<8|b[at+3];
}
void Captured(const std::filesystem::path& path) {
    std::ifstream input(path);Json plan;input>>plan;
    const auto folder=path.parent_path();IndexGeometryCache<Buffer> cache;
    std::map<std::string,Owner> owners;
    auto payload=[&](const Json& digest) {
        const auto key=digest.get<std::string>();auto& owner=owners[key];
        if(!owner)owner=std::make_shared<const Bytes>(Read(folder/(key+".payload.bin")));
        return owner;
    };
    size_t draws=0,words=0,gpu=0,fallback=0;
    for(const auto& operation:plan.at("operations")) {
        if(operation.at("type")=="present"){cache.RetireAfterFence();continue;}
        if(operation.at("type")!="draw")continue;
        const auto args=operation.at("args").get<std::array<uint32_t,8>>();
        const auto function=operation.at("function").get<std::string>();const bool up=function.starts_with("Begin");
        IndexGeometryParameters p;p.count=function=="BeginIndexedVertices"?args[4]:up?args[2]:args[3];
        p.primitive=args[1];p.up_padding=up;p.rectangle=operation.value("vertex_input","")=="affine_float4_rectangle";
        p.strip_restart=bool(BE(*payload(operation.at("device_state")),0x2948)&(1u<<21));
        Owner bytes;std::string digest;
        if(function=="DrawPrimitive"||function=="BeginVertices")p.first_index=up?0:args[2];
        else {
            std::map<unsigned,const Json*> geometry;
            for(const auto& block:operation.at(up?"up_geometry":"geometry"))geometry[block.at("kind")]=&block;
            if(up){p.minimum_index=args[2];p.width=args[5]&4?4:2;p.endian=p.width==4?2:1;}
            else {const auto flags=BE(*payload(geometry.at(30)->at("sha256")),0);p.width=flags>>31?4:2;p.endian=(flags>>29)&3;}
            const auto& block=*geometry.at(up?97:31);digest=block.at("sha256");bytes=payload(digest);
            p.source=block.value("source","")=="gpu_memory"?IndexGeometrySource::GpuMemory:IndexGeometrySource::CpuPayload;
            gpu+=p.source==IndexGeometrySource::GpuMemory;
        }
        const auto source=bytes?std::span<const uint8_t>(*bytes):std::span<const uint8_t>();
        const auto expected=Previous(source,p);const IndexGeometryLookup key{p,digest};
        auto* resource=cache.Find(key,source,bytes);
        if(!resource) {
            const auto actual=BuildIndexGeometry(source,p);Check(actual==expected,"Captured index miss differs from frozen Draw");
            resource=cache.Insert(key,bytes,actual.size()*4,[&]{return std::make_shared<const Indices>(actual);});
            if(!resource)++fallback;
        }
        if(resource)Check(**resource==expected,"Captured cached index data differs from frozen Draw");
        ++draws;words+=expected.size();
    }
    const auto& stats=cache.statistics();
    std::cout<<path.string()<<": draws="<<draws<<", words="<<words<<", hits="<<stats.hits
             <<", misses="<<stats.misses<<", GPU-bypasses="<<gpu<<", fallbacks="<<fallback
             <<", retained="<<cache.retained_bytes()<<", entries="<<cache.size()<<'\n';
}
int main(int argc,char** argv)try {
    const auto cases=Synthetic();OwnershipAndBounds();KeyFields();
    std::cout<<"Index geometry PASS: "<<cases<<" independent old/new byte/error cases; immutable/mutable-owner checks, all key fields, GPU bypass, bounded persistent LRU and fenced lifetime\n";
    for(int i=1;i<argc;++i)Captured(argv[i]);
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
