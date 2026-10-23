#include "renderer/native/fsi_precision.h"
#include "renderer/native/fsi_device.h"
#include "renderer/native/resolve_memory.h"
#include "fsi_vertex_spv.h"
#include "fsi_fragment_spv.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <fstream>
#include <mutex>
#include <stdexcept>
namespace plume { std::unique_ptr<RenderInterface> CreateVulkanInterface(); }
using namespace plume;
using namespace gow3::renderer;
int main(int argc,char** argv) try {
    FsiDevice host;auto& device=host.device();auto queue=device.createCommandQueue(RenderCommandListType::DIRECT);
    std::vector<uint32_t> alternate;
    if(argc>1) {
        std::ifstream file(argv[1],std::ios::binary|std::ios::ate);
        if(!file||file.tellg()<20||file.tellg()%4)throw std::runtime_error("Invalid alternate SPIR-V");
        alternate.resize(size_t(file.tellg())/4);file.seekg(0);
        if(!file.read(reinterpret_cast<char*>(alternate.data()),alternate.size()*4))throw std::runtime_error("Cannot read alternate SPIR-V");
    }
    const bool builtin=argc>2&&std::string_view(argv[2])=="--builtin-depth";
    const bool production=builtin||(argc>2&&std::string_view(argv[2])=="--production");
    FsiPrecision renderer(device,*queue,fsi_vertex_spv,alternate.empty()?std::span(fsi_fragment_spv):std::span<const uint32_t>(alternate),true,production);
    size_t checks=0,cases=0;
    auto run=[&](const char* name,FsiRasterState state,std::vector<FsiFragment> fragments,
                 std::array<uint32_t,2> before,uint32_t before_depth,
                 std::array<uint32_t,2> after,uint32_t after_depth,std::array<float,4> polygon_bias=std::array<float,4>{}) {
        constexpr uint32_t width=16,height=16;
        SurfaceLayout color{width,height,state.pitch,state.color_base,state.color_format,false};
        SurfaceLayout depth{width,height,state.pitch,state.depth_base,state.depth_format,true};
        const bool wide=state.color_format==5||state.color_format==7||state.color_format==15;
        std::vector<uint32_t> seed(FsiPrecision::kBytes/4);
        for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x) {
            const auto c=SurfaceByteOffset(color,x,y)/4,d=SurfaceByteOffset(depth,x,y)/4;
            seed[c]=before[0];if(wide)seed[c+1]=before[1];seed[d]=before_depth;
        }
        renderer.Initialize(seed);renderer.Draw(state,fragments,width,height,polygon_bias);auto actual=renderer.Read();
        for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x) {
            const auto c=SurfaceByteOffset(color,x,y)/4,d=SurfaceByteOffset(depth,x,y)/4;
            if(actual[c]!=after[0]||(wide&&actual[c+1]!=after[1])||actual[d]!=after_depth) {
                std::cerr<<name<<" pixel "<<x<<','<<y<<" got color="<<std::hex<<actual[c]<<','<<actual[c+1]
                    <<" depth="<<actual[d]<<" expected="<<after[0]<<','<<after[1]<<" depth="<<after_depth<<std::dec<<'\n';
                throw std::runtime_error("Packed fragment-interlock result mismatch");
            }
            checks+=wide?3:2;
        }
        ++cases;
    };
    FsiRasterState state;
    if(builtin) {
        state.depth_control=2|4|(7<<4);
        run("builtin depth-only preserves bound color",state,{{{1,1,1,1},.5f}},
            {0x12345678,0},0xffffffab,{0x12345678,0},0x800000ab);
        if(!renderer.passing_samples())throw std::runtime_error("Builtin passing depth had no occlusion samples");
        state.depth_control=2|4|(4<<4);
        run("builtin failing depth rejects occlusion",state,{{{1,1,1,1},.5f}},
            {0x12345678,0},0xffffffab,{0x12345678,0},0xffffffab);
        if(renderer.passing_samples())throw std::runtime_error("Builtin failed depth contributed occlusion samples");
        std::cout<<"PASS: builtin depth-only interlock; "<<cases<<" cases, "<<checks<<" exact word comparisons\n";
        return 0;
    }
    state.blend_enabled=1;state.blend_control=6|(7<<8)|(1<<16)|(7<<24);
    run("ordered source-alpha blend",state,{{{1,0,0,1},0},{{0,1,0,.5f},0},{{0,0,1,.5f},0}},
        {0,0},0,{0xff804040,0},0);
    for(uint32_t factor=12;factor<=15;++factor) {
        state={};state.blend_enabled=1;state.blend_constant={.25f,.5f,.75f,.25f};
        state.blend_control=factor|(factor<<16);
        constexpr uint32_t expected[]{0x40bf8040,0xbf4080bf,0x40404040,0xbfbfbfbf};
        run("constant source blend",state,{{{1,1,1,1},0}},
            {0x12345678,0},0,{expected[factor-12],0},0);
        state.blend_control=(factor<<8)|(factor<<24);
        run("constant destination blend",state,{{{0,0,0,0},0}},
            {0xffffffff,0},0,{expected[factor-12],0},0);
    }
    state={};state.color_format=5;state.blend_enabled=1;state.blend_control=0x01010101;
    run("fixed16 rounds every fragment",state,std::vector<FsiFragment>(64,{{.0004f,.0004f,.0004f,.0004f},0}),
        {0,0},0,{0,0},0);
    run("fixed16 ordered accumulation",state,std::vector<FsiFragment>(1024,{{32.f/32767,32.f/32767,32.f/32767,32.f/32767},0}),
        {0,0},0,{0x04000400,0x04000400},0);
    state.color_format=3;
    run("7e3 rounds every fragment",state,std::vector<FsiFragment>(64,{{.0008f,.0008f,.0008f,0},0}),
        {0,0},0,{0,0},0);
    run("7e3 ordered accumulation",state,std::vector<FsiFragment>(64,{{1.f/512,1.f/512,1.f/512,0},0}),
        {0,0},0,{64u|(64u<<10)|(64u<<20),0},0);
    state={};state.depth_control=2|4|(4<<4);
    run("UNORM24 quantized comparison",state,{{{1,0,0,1},.25f},{{0,0,1,1},.25f+std::ldexp(1.f,-25)}},
        {0,0},0x57,{0xff0000ff,0},0x40000057);
    state.depth_format=1;
    run("float24 quantized comparison",state,{{{1,0,0,1},.5f},{{0,0,1,1},.5f+std::ldexp(1.f,-24)}},
        {0,0},0x57,{0xff0000ff,0},0xe0000057);
    state={};state.depth_control=1|(7<<8)|(2<<14);state.stencil_front=0x000fffa9;
    run("stencil pass masked replace",state,{{{1,0,0,1},.5f}},{0,0},0x12345657,{0xff0000ff,0},0x12345659);
    state.depth_control=1|(2<<8)|(6<<11);state.stencil_front=0x00ffff00;
    run("stencil fail wrap",state,{{{1,0,0,1},.5f}},{0x12345678,0},0x123456ff,{0x12345678,0},0x12345600);
    state.depth_control=1|2|(1<<4)|(7<<8)|(5<<17);
    run("depth fail stencil invert",state,{{{1,0,0,1},.5f}},{0x12345678,0},0x55,{0x12345678,0},0xaa);
    if(production&&renderer.passing_samples()!=0)throw std::runtime_error("Failed programmable depth contributed occlusion samples");
    state={};state.color_format=5;state.color_mask=2;
    run("masked noncanonical fixed16",state,{{{0,16,0,0},0}},{0x12348000,0xabcd9876},0,{0x40008000,0xabcd9876},0);
    state.color_format=7;state.color_mask=1;
    run("masked half NaN and extended bits",state,{{{1,0,0,0},0}},{0x7e557fff,0xfc007fff},0,{0x7e553c00,0xfc007fff},0);
    state={};state.color_format=2;state.blend_enabled=1;state.blend_control=0x01010101;
    run("10-bit per-fragment quantization",state,std::vector<FsiFragment>(64,{{.0004f,.0004f,.0004f,0},0}),{0,0},0,{0,0},0);
    state={};state.color_base=2047;state.depth_base=1023;
    run("last EDRAM tile",state,{{{1,0,1,1},0}},{0,0},0,{0xffff00ff,0},0);
    if(production&&renderer.passing_samples()==0)throw std::runtime_error("Passing programmable depth had zero occlusion samples");
    if(production) {
        state={};state.depth_control=2|4|(7<<4);
        run("front polygon constant offset",state,{{{1,0,0,1},.25f}},
            {0,0},0,{0xff0000ff,0},0x80000000,{0,.25f,0,.125f});
        run("back polygon constant offset",state,{{{1,0,0,1},.25f,{0,1,0}}},
            {0,0},0,{0xff0000ff,0},0x60000000,{0,.25f,0,.125f});
        run("exported depth bypasses polygon offset",state,{{{1,0,0,1},.25f,{1,0,0}}},
            {0,0},0,{0xff0000ff,0},0x40000000,{0,.25f,0,.125f});
        std::vector<uint32_t> seed(FsiPrecision::kBytes/4);
        renderer.Initialize(seed);
        const FsiFragment varying{{1,0,0,1},.125f,{0,0,1.f/64}};
        renderer.Draw(state,std::span(&varying,1),16,16,{2,.25f,0,0});
        const auto actual=renderer.Read();
        const SurfaceLayout depth{16,16,state.pitch,state.depth_base,state.depth_format,true};
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
            const float z=.125f+(float(x)+.5f)/64+2.f/64+.25f;
            const uint32_t expected=uint32_t(std::nearbyint(z*16777215.f))<<8;
            if(actual[SurfaceByteOffset(depth,x,y)/4]!=expected)
                throw std::runtime_error("Coarse polygon slope offset mismatch");
            ++checks;
        }
        ++cases;
    }
    std::cout<<"PASS: ordered packed EDRAM interlock; "<<cases<<" cases, "<<checks<<" exact word comparisons\n";
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
