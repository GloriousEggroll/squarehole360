#include "renderer/native/retired_image_pool.h"
#include <atomic>
#include <deque>
#include <iostream>
#include <random>
#include <thread>
#include <vector>
using gow3::renderer::RetiredImagePool;
namespace {
void Require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
struct Image {
    static inline std::atomic<int> alive=0,destroyed=0;
    unsigned id;int content;
    explicit Image(unsigned value):id(value),content(int(value)*17){++alive;}
    ~Image(){--alive;++destroyed;}
};
using Pool=RetiredImagePool<Image>;
auto Make(Pool& pool,unsigned id,uint32_t width,uint32_t height,uint64_t bytes) {
    return pool.Share(std::make_unique<Image>(id),width,height,bytes);
}
void Ownership() {
    Pool pool;
    auto producer=Make(pool,1,16,16,8192);
    std::weak_ptr<Image> old_generation=producer;
    auto transfer=producer,draw=producer;
    producer.reset();transfer.reset();
    Require(!pool.Take(16,16)&&!old_generation.expired(),"Image reused while draw owned it");
    draw.reset();Require(old_generation.expired(),"Retired lease control block survived");
    Require(!pool.Take(8,32),"Equal-area different extent aliased");
    auto storage=pool.Take(16,16);
    Require(storage&&storage->id==1&&storage->content==17,"Exact image identity/content lost");
    auto next=pool.Share(std::move(storage),16,16,8192);
    Require(old_generation.expired()&&next->id==1,"Old weak lease resurrected");
    next.reset();const auto stats=pool.statistics();
    Require(stats.hits==1&&stats.misses==2&&stats.idle_bytes==8192&&stats.idle_images==1,
        "Accounting used logical texel size or missed leases");
}
void LimitsAndFifo() {
    Pool pool(2,100);
    Make(pool,1,1,1,70).reset();Make(pool,2,2,1,30).reset();
    Make(pool,3,3,1,40).reset();
    Require(!pool.Take(1,1)&&pool.statistics().idle_bytes==70,"Byte-pressure FIFO evicted wrong image");
    Make(pool,4,4,1,1).reset();
    Require(!pool.Take(2,1)&&pool.statistics().idle_bytes==41,"Count-pressure FIFO evicted wrong image");
    Make(pool,5,5,1,101).reset();Make(pool,6,6,1,0).reset();
    const auto stats=pool.statistics();
    Require(stats.evictions==2&&stats.rejected==2&&stats.peak_idle_bytes==100&&stats.peak_idle_images==2,
        "Hard accounting/oversized/unknown-allocation rejection changed");
    Pool disabled(0,0);Make(disabled,7,1,1,1).reset();
    Require(disabled.statistics().rejected==1&&!disabled.Take(1,1),"Zero budget retained resource");
    Pool bounded;
    for(unsigned i=1;i<=17;++i)Make(bounded,i,i,1,Pool::kMaxBytes/16).reset();
    Require(bounded.statistics().idle_images==16&&bounded.statistics().idle_bytes==Pool::kMaxBytes&&
        bounded.statistics().evictions==1&&!bounded.Take(1,1),"Production hard limits exceeded");
    for(unsigned which=0;which<2;++which) {
        bool rejected=false;try{Pool bad(which?16:17,which?Pool::kMaxBytes+1:1);}
        catch(const std::invalid_argument&){rejected=true;}
        Require(rejected,"Oversized pool accepted");
    }
}
void IndependentOracle() {
    struct Entry {unsigned id,width,height;uint64_t bytes;};
    std::deque<Entry> oracle;uint64_t total=0;
    Pool pool(7,1000);std::mt19937 rng(0x78a912);unsigned serial=0;
    for(unsigned step=0;step<10000;++step) {
        const unsigned width=1+rng()%7,height=1+rng()%3;
        if(rng()%3==0) {
            auto expected=oracle.end();for(auto it=oracle.begin();it!=oracle.end();++it)
                if(it->width==width&&it->height==height){expected=it;break;}
            auto actual=pool.Take(width,height);
            Require(bool(actual)==(expected!=oracle.end()),"Oracle hit/miss mismatch");
            if(actual){Require(actual->id==expected->id,"Oracle FIFO identity mismatch");total-=expected->bytes;oracle.erase(expected);}
        } else {
            const uint64_t bytes=rng()%1200;const unsigned id=++serial;
            Make(pool,id,width,height,bytes).reset();
            if(bytes&&bytes<=1000) {
                while(oracle.size()==7||total+bytes>1000){total-=oracle.front().bytes;oracle.pop_front();}
                oracle.push_back({id,width,height,bytes});total+=bytes;
            }
        }
        const auto stats=pool.statistics();
        Require(stats.idle_images==oracle.size()&&stats.idle_bytes==total,"Oracle idle budget mismatch");
    }
}
void ShutdownAndThreads() {
    std::shared_ptr<Image> survivor;
    {
        auto pool=std::make_unique<Pool>();
        survivor=Make(*pool,100,1,1,32);Make(*pool,101,1,1,32).reset();
        const auto before=Image::destroyed.load();pool.reset();
        Require(Image::destroyed==before+1&&survivor->content==1700,"Pool destruction damaged active lease");
    }
    std::thread late([held=std::move(survivor)]() mutable {held.reset();});late.join();
    Pool pool;std::vector<std::shared_ptr<Image>> held;
    for(unsigned i=0;i<64;++i)held.push_back(Make(pool,i,1,1,4));
    std::array<std::thread,4> workers;
    for(unsigned t=0;t<4;++t)workers[t]=std::thread([&,t] {
        for(unsigned i=t;i<held.size();i+=4){auto copy=held[i];held[i].reset();copy.reset();}
    });
    for(auto& worker:workers)worker.join();
    Require(pool.statistics().idle_images==16&&pool.statistics().returns==64&&pool.statistics().evictions==48,
        "Concurrent final-owner admission lost limits/resources");
}
}
int main() try {
    Ownership();LimitsAndFifo();IndependentOracle();ShutdownAndThreads();
    Require(Image::alive==0,"Pool ownership cycle or resource leak");
    std::cout<<"PASS: exact keys, last-owner leases, actual-byte/FIFO limits, 10000 oracle operations, late/concurrent release\n";
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
