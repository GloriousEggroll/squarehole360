#include "renderer/native/fsi_draw_pass_groups.h"
#include <cstdio>
#include <cstdlib>

using namespace gow3::renderer;
namespace {
int failures=0;
void Check(bool value,const char* what) {if(!value){std::fprintf(stderr,"FAIL: %s\n",what);++failures;}}
NativeFsiConstants Raster(uint32_t pitch,uint32_t depth_base,uint32_t color_base,uint32_t format=3,uint32_t mask=15) {
    NativeFsiConstants r;r.pitch=pitch;r.depth_base=depth_base;r.color[0]={color_base,format,mask,0x10001};
    return r;
}
}

int main() {
    NativeFsiPassPlanes a,b,merged;
    // Primary 1280x720: depth tiles [0,720), color [720,1440).
    Check(NativeFsiPassPlanesFor(Raster(1280,0,720),720,a),"primary layout admitted");
    Check(a.count==2&&a.planes[0].tiles==720&&a.planes[1].tiles==720,"primary plane extents");
    // Depth-only draw (color mask 0) joins the same pass.
    Check(NativeFsiPassPlanesFor(Raster(1280,0,720,3,0),720,b)&&b.count==1,"depth-only planes");
    Check(NativeFsiPassJoin(a,b,merged)&&merged.count==2,"depth-only joins primary");
    // Same mapping, different 32-bit format (7e3 vs 8888) still joins.
    Check(NativeFsiPassPlanesFor(Raster(1280,0,720,0),720,b)&&NativeFsiPassJoin(a,b,merged),"format change on same plane joins");
    // Overlapping color and depth inside one draw is never eligible.
    Check(!NativeFsiPassPlanesFor(Raster(1280,0,360),720,b),"self-overlap refused");
    // Moving the color base so it overlaps depth at other pixels is a boundary.
    Check(NativeFsiPassPlanesFor(Raster(1280,1440,400),360,b),"half-res layout admitted alone");
    Check(!NativeFsiPassJoin(a,b,merged),"overlapping new plane is a boundary");
    // A disjoint extra target joins (e.g. small target placed after primary).
    Check(NativeFsiPassPlanesFor(Raster(1280,0,720),720,b),"same layout");
    NativeFsiConstants extra=Raster(1280,0,720);extra.color[1]={1500,3,15,0x10001};
    Check(!NativeFsiPassPlanesFor(extra,720,b),"third full plane wraps into depth and is refused");
    NativeFsiPassPlanes half;
    Check(NativeFsiPassPlanesFor(Raster(1280,0,720),360,half)&&NativeFsiPassPlanesFor(extra,360,b)&&
          NativeFsiPassJoin(half,b,merged)&&merged.count==3,"disjoint MRT joins");
    // Pitch change is always a boundary.
    Check(NativeFsiPassPlanesFor(Raster(640,0,720),360,b)&&!NativeFsiPassJoin(a,b,merged),"pitch change is a boundary");
    // 64-bit color doubles tiles per row: 1280 wide -> 32 tiles/row.
    Check(!NativeFsiPassPlanesFor(Raster(1280,0,720,7),720,b),"wide full plane wraps into depth and is refused");
    Check(NativeFsiPassPlanesFor(Raster(1280,0,720,7),360,b)&&b.planes[1].tiles==23*32,"wide plane extent");
    Check(!NativeFsiPassJoin(half,b,merged),"wide plane over 32-bit plane is a boundary");
    // Wrap-around: plane [1900,2148) mod 2048 overlaps depth at [0,..).
    Check(!NativeFsiPassPlanesFor(Raster(1280,0,1900),256,b),"wrapping plane overlap refused");
    Check(NativeFsiPassPlanesFor(Raster(1280,100,1900),128,b),"wrapping plane disjoint admitted");
    // Taller extent with identical mapping extends the plane and stays disjoint.
    NativeFsiPassPlanes small;
    Check(NativeFsiPassPlanesFor(Raster(1280,0,720),360,small)&&NativeFsiPassJoin(small,a,merged)&&merged.planes[0].tiles==720,"extent growth merges");
    // Growth that would make planes collide is a boundary.
    Check(NativeFsiPassPlanesFor(Raster(1280,0,400),360,small),"short layout admitted");
    Check(!NativeFsiPassJoin(small,a,merged),"growth into another plane is a boundary");
    // Oversized planes are refused.
    Check(!NativeFsiPassPlanesFor(Raster(8192,0,0),8192,b),"oversized refused");
    if(failures)return 1;
    std::printf("fsi draw pass group plane checks passed\n");
    return 0;
}
