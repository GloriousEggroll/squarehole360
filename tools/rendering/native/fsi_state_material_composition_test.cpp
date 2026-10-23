#include "renderer/native/fsi_state_material_composition.h"
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
using namespace gow3::renderer;
int main() try {
    size_t checks=0;
    const auto require=[&](bool value) {++checks;if(!value)throw std::runtime_error("Material admission changed");};
    const std::string ordinary_vertex(64,'1'),other_vertex(64,'2'),ordinary_pixel(64,'3');
    const std::array<std::string_view,6> vertices{ordinary_vertex,other_vertex,
        kNativeEarlyDepthVertexSource,kNativeEarlyDepthCatalogVertexSource,"","depth-only"};
    const std::array<std::string_view,6> pixels{ordinary_pixel,kNativeEarlyDepthPixelSource,
        kNativeEarlyDepthCatalogPixelSources[0],kNativeEarlyDepthCatalogPixelSources[1],"","depth-only"};
    // All 64 state combinations, including overlapping failures. Pixel-only
    // exclusions must hold with a different VS and dominate query/state refusal.
    for(size_t v=0;v<vertices.size();++v)for(size_t p=0;p<pixels.size();++p)
        for(unsigned bits=0;bits<64;++bits) {
            const FsiStateMaterialCompositionProof proof{bool(bits&1),bool(bits&2),bool(bits&4),
                bool(bits&8),bool(bits&16),bool(bits&32)};
            const auto expected=v>=4||p!=0?FsiStateMaterialCompositionAdmission::Source:
                bits&1?FsiStateMaterialCompositionAdmission::Query:
                bits==56?FsiStateMaterialCompositionAdmission::Admitted:FsiStateMaterialCompositionAdmission::State;
            require(ClassifyFsiStateMaterialComposition(vertices[v],pixels[p],proof)==expected);
        }
    for(size_t length: {size_t(0),size_t(1),size_t(63),size_t(65)}) {
        require(!FsiStateMaterialCompositionSourceAllowed(std::string(length,'4'),ordinary_pixel));
        require(!FsiStateMaterialCompositionSourceAllowed(ordinary_vertex,std::string(length,'4')));
    }
    // Opt-in affects only the inactive catalogue's exact two canonical pairs.
    // It never overrides active catalogue priority or any live state/query gate.
    for(bool admit:{false,true})for(bool catalogue:{false,true})
        for(size_t v=0;v<vertices.size();++v)for(size_t p=0;p<pixels.size();++p)
            for(unsigned bits=0;bits<64;++bits) {
                const bool exact_pair=v==3&&(p==2||p==3);
                const bool source=v<4&&(p==0||(admit&&!catalogue&&exact_pair));
                const FsiStateMaterialCompositionProof proof{bool(bits&1),bool(bits&2),bool(bits&4),
                    bool(bits&8),bool(bits&16),bool(bits&32)};
                const auto expected=!source?FsiStateMaterialCompositionAdmission::Source:
                    bits&1?FsiStateMaterialCompositionAdmission::Query:
                    bits==56?FsiStateMaterialCompositionAdmission::Admitted:FsiStateMaterialCompositionAdmission::State;
                require(FsiStateMaterialCompositionSourceAllowed(vertices[v],pixels[p],admit,catalogue)==source);
                require(ClassifyFsiStateMaterialComposition(vertices[v],pixels[p],proof,admit,catalogue)==expected);
            }
    for(auto pixel:kNativeEarlyDepthCatalogPixelSources) {
        for(size_t i=0;i<pixel.size();++i) {
            // A wrong PS stays under the ordinary existing source policy, while
            // an almost-correct VS must never release a pinned catalogue PS.
            auto vertex=std::string(kNativeEarlyDepthCatalogVertexSource);
            vertex[i]=vertex[i]=='0'?'1':'0';
            require(!FsiStateMaterialCompositionSourceAllowed(vertex,pixel,true,false));
        }
        for(size_t length:{size_t(0),size_t(1),size_t(63),size_t(65)})
            require(!FsiStateMaterialCompositionSourceAllowed(std::string(length,'4'),pixel,true,false));
    }
    std::cout<<"FSI material admission: "<<checks<<" checks passed\n";
    return 0;
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
