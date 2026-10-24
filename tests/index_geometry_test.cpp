#include "renderer/native/index_geometry_cache.h"
#include <iostream>
#include <stdexcept>
using namespace gow3::renderer;
namespace {
std::vector<uint32_t> Sequential(uint32_t primitive,uint32_t count,uint32_t first=0) {
    IndexGeometryParameters p;p.primitive=primitive;p.count=count;p.first_index=first;
    return BuildIndexGeometry({},p);
}
void Expect(bool condition,const char* message) {if(!condition)throw std::logic_error(message);}
}
int main() try {
    Expect(Sequential(2,4)==std::vector<uint32_t>{0,1,2,3},"Line list indices changed");
    Expect(Sequential(2,5,10)==std::vector<uint32_t>{10,11,12,13},"Odd line list vertex was drawn");
    Expect(Sequential(3,4)==std::vector<uint32_t>{0,1,1,2,2,3},"Line strip was not expanded to a list");
    Expect(Sequential(6,4)==std::vector<uint32_t>{0,1,2,2,1,3},"Triangle strip winding changed");
    Expect(Sequential(13,4)==std::vector<uint32_t>{0,1,2,0,2,3},"Quad expansion changed");
    bool rejected=false;
    try {(void)Sequential(1,4);}catch(const std::runtime_error&){rejected=true;}
    Expect(rejected,"Point lists must stay unsupported");
    std::cout<<"Index geometry PASS\n";
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
