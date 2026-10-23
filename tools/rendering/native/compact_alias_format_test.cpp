#include "renderer/native/compact_alias_format.h"
#include "renderer/native/retired_image_pool.h"
#include <array>
#include <iostream>
#include <stdexcept>
using namespace gow3::renderer;
namespace {
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Resource {unsigned id;};
}
int main() try {
    // Independent policy table: only a 2D single-layer source with one or two
    // meaningful channels can shrink, and each shader has separate admission.
    constexpr std::array<std::array<unsigned,4>,4> expected{{
        {{4,4,4,4}},{{1,4,4,4}},{{4,2,4,4}},{{1,2,4,4}}}};
    unsigned checks=0;
    for(unsigned mask=0;mask<4;++mask)for(unsigned dimension=0;dimension<4;++dimension)
    for(unsigned layers:{0u,1u,2u,6u})for(unsigned components=0;components<=5;++components) {
        const CompactAliasFormats available{bool(mask&1),bool(mask&2)};
        const unsigned want=dimension==1&&layers==1&&components>=1&&components<=4?
            expected[mask][components-1]:4;
        Require(DecodedAliasChannels(dimension,layers,components,available)==want,
            "Unsupported format/scope was compacted or a valid enabled format was missed");++checks;
    }
    // Format must distinguish equal extents in the same bounded idle bank.
    RetiredImagePool<Resource> pool(2,96);
    for(unsigned format:{1u,2u})pool.Share(std::make_unique<Resource>(Resource{format}),4,4,32,format).reset();
    Require(!pool.Take(4,4,4),"Format identity reused incompatible storage");
    auto one=pool.Take(4,4,1),two=pool.Take(4,4,2);
    Require(one&&one->id==1&&two&&two->id==2,"Format-aware checkout lost exact storage");
    pool.Share(std::move(one),4,4,32,1).reset();pool.Share(std::move(two),4,4,32,2).reset();
    pool.Share(std::make_unique<Resource>(Resource{4}),4,4,64,4).reset();
    Require(pool.statistics().idle_images==2&&pool.statistics().idle_bytes==96&&
        !pool.Take(4,4,1)&&pool.Take(4,4,2)&&pool.Take(4,4,4),
        "Mixed-format pool did not preserve combined byte/FIFO bounds");
    std::cout<<"PASS: "<<checks<<" format/scope/capability cases and exact format-key shared pool budget\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
