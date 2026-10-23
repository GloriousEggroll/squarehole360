#include "renderer/native/query_readiness_census.h"
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace gow3::renderer;
static void Check(bool ok) {if(!ok)throw std::runtime_error("Query readiness receipt mismatch");}
int main() {
    try {
        QueryReadinessCensus c(2);
        Check(!c.Read(10));
        c.Begin(10,0x1000); c.End(10,{1,2,0x1000,false});
        Check(c.Read(10)&&!c.Read(10)->published);
        Check(!c.Publish(3,1,0x1000)&&!c.Publish(2,1,0x2000)&&!c.Publish(2,2,0x1000));
        Check(!c.Read(10)->published);
        Check(c.Publish(2,1,0x1000)&&c.Read(10)->published);
        // Reusing the handle or its report allocation invalidates the old
        // evidence before any new GPU end/publication can arrive.
        c.Begin(10,0x2000); Check(!c.Read(10)); Check(!c.Publish(2,1,0x1000));
        c.End(10,{2,2,0x2000,false});
        c.Begin(20,0x2000); Check(!c.Read(10));
        c.End(20,{3,2,0x2000,false});
        Check(!c.Publish(2,2,0x2000)); Check(!c.Read(20)->published);
        Check(c.Publish(2,3,0x2000));
        c.End(30,{4,2,0x3000,false});
        c.End(40,{5,2,0x4000,false}); // Bounded reset, never stale readiness.
        Check(!c.Read(20)&&!c.Read(30)&&c.Read(40));
        Check(!c.Publish(2,3,0x2000)&&!c.Publish(2,4,0x3000));
        c.End(50,{5,2,0x5000,false}); // Generation collision is not evidence.
        Check(!c.Read(40)&&!c.Read(50));
        QueryReadinessCensus disabled(0); disabled.End(10,{1,2,0x1000,false});
        Check(!disabled.Read(10)&&!disabled.Publish(2,1,0x1000));
        // Producer and publisher may overlap; reads are whole receipts. No
        // earlier address generation may make a newer query appear published.
        QueryReadinessCensus raced;
        std::atomic<bool> finished{false},bad{false};
        std::thread producer([&] {
            for(uint64_t n=1;n<=100000;++n) {
                raced.Begin(10,0x1000);
                raced.End(10,{n,7,0x1000,false});
                if(n>1)raced.Publish(7,n-1,0x1000);
                const auto current=raced.Read(10);
                if(!current||current->published||current->generation!=n)bad=true;
            }
            finished=true;
        });
        std::thread reader([&] {
            while(!finished)if(const auto v=raced.Read(10))
                if(v->frame!=7||v->address!=0x1000||v->published||!v->generation)bad=true;
        });
        producer.join();reader.join();Check(!bad);
        std::cout << "Query readiness census: generation/address/frame identity, handle/report reuse, bounded retention and100000 concurrent generations passed\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
