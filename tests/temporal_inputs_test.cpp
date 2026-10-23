// CPU checks for the temporal upscaling inputs: jitter, constant patching,
// HUD detection, projection description and camera reprojection, against a
// synthetic camera with Gears' conventions (row vectors, translated world,
// reverse-Z infinite projection with near 10).
#undef NDEBUG
#include "renderer/native/temporal_inputs.h"
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace gow3::renderer;
namespace {
using Matrix=std::array<std::array<double,4>,4>;
Matrix Multiply(const Matrix& a,const Matrix& b) {
    Matrix r{};
    for(int i=0;i<4;++i)for(int j=0;j<4;++j)for(int k=0;k<4;++k)r[i][j]+=a[i][k]*b[k][j];
    return r;
}
// View (camera at translated origin, yaw about +z, forward along +x in world)
// times reverse-Z infinite projection: ndc.z = 0.999 - 9.99 / w.
NativeTemporalCamera Camera(double yaw,std::array<double,3> position,double fov_y) {
    const double c=std::cos(yaw),s=std::sin(yaw);
    // World axes into view axes (x right, y up, z forward); rows are inputs.
    Matrix view{};
    // forward=( c, s,0), right=( s,-c,0), up=(0,0,1)
    view[0]={s,0,c,0};view[1]={-c,0,s,0};view[2]={0,1,0,0};view[3]={0,0,0,1};
    const double f=1/std::tan(fov_y/2),aspect=16.0/9.0;
    Matrix projection{};
    projection[0]={f/aspect,0,0,0};
    projection[1]={0,f,0,0};
    projection[2]={0,0,0.999,1};
    projection[3]={0,0,-9.99,0};
    NativeTemporalCamera camera;
    camera.view_projection=Multiply(view,projection);
    for(int i=0;i<3;++i)camera.pre_view_translation[i]=-position[i];
    return camera;
}
std::array<double,4> Clip(const NativeTemporalCamera& camera,std::array<double,3> world) {
    std::array<double,4> p{world[0]+camera.pre_view_translation[0],world[1]+camera.pre_view_translation[1],
                           world[2]+camera.pre_view_translation[2],1};
    std::array<double,4> clip{};
    for(int j=0;j<4;++j)for(int i=0;i<4;++i)clip[j]+=p[i]*camera.view_projection[i][j];
    return clip;
}
// Pixel position and device depth (1 - ndc.z) of a world point.
std::array<double,3> Pixel(const NativeTemporalCamera& camera,std::array<double,3> world,int w,int h) {
    const auto c=Clip(camera,world);
    return {(c[0]/c[3]*0.5+0.5)*w,(0.5-c[1]/c[3]*0.5)*h,1-c[2]/c[3]};
}
void Near(double a,double b,double tolerance,const char* what) {
    if(std::abs(a-b)>tolerance) {std::fprintf(stderr,"%s: %.9g != %.9g\n",what,a,b);std::abort();}
}
std::vector<uint8_t> State(const NativeTemporalCamera& camera) {
    std::vector<uint8_t> state(0x3600);
    auto store=[&](unsigned reg,unsigned component,float value) {
        const uint32_t word=std::byteswap(std::bit_cast<uint32_t>(value));
        std::memcpy(state.data()+0x780+reg*16+component*4,&word,4);
    };
    for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)store(r,c,float(camera.view_projection[r][c]));
    for(unsigned c=0;c<3;++c)store(5,c,float(camera.pre_view_translation[c]));
    return state;
}
}

int main() {
    constexpr int w=1280,h=720;
    const double fov=29.39*3.14159265358979/180;
    const auto previous=Camera(0.30,{8266.18,-3586.71,-2861.0},fov);
    const auto current=Camera(0.31,{8270.0,-3580.0,-2861.0},fov);

    // Projection description recovers near, field of view and infinity depth.
    NativeTemporalProjection projection;
    assert(NativeTemporalDescribeProjection(current,projection));
    Near(projection.near_plane,10,1e-6,"near");
    Near(projection.fov_y,fov,1e-6,"fov");
    Near(projection.infinity_depth,0.001,1e-9,"infinity depth");

    // Reading the camera back from big-endian constants.
    const auto state=State(current);
    const auto read=NativeTemporalReadCamera(state);
    assert(read);
    Near(read->view_projection[0][0],current.view_projection[0][0],1e-6,"read c0.x");
    Near(read->pre_view_translation[1],current.pre_view_translation[1],1e-3,"read c5.y");

    // Reprojection: a static world point moves exactly as the camera did.
    const auto reprojection=NativeTemporalReprojection(current,previous);
    for(const std::array<double,3> world:{std::array<double,3>{8600.0,-3480.0,-2840.0},
                                          std::array<double,3>{8350.0,-3560.0,-2870.0}}) {
        const auto now=Pixel(current,world,w,h),before=Pixel(previous,world,w,h);
        assert(now[0]>0&&now[0]<w&&now[1]>0&&now[1]<h);
        const double xn=now[0]/w*2-1,yn=1-now[1]/h*2,zn=1-now[2];
        std::array<double,4> clip{};
        for(int j=0;j<4;++j)clip[j]=xn*reprojection[0][j]+yn*reprojection[1][j]+zn*reprojection[2][j]+reprojection[3][j];
        Near((clip[0]/clip[3]*0.5+0.5)*w,before[0],0.02,"reprojected x");
        Near((0.5-clip[1]/clip[3]*0.5)*h,before[1],0.02,"reprojected y");
    }
    // Same camera: no motion, including at infinity.
    const auto still=NativeTemporalReprojection(current,current);
    for(double zn:{0.5,0.999}) {
        std::array<double,4> clip{};
        for(int j=0;j<4;++j)clip[j]=0.25*still[0][j]-0.5*still[1][j]+zn*still[2][j]+still[3][j];
        Near(clip[0]/clip[3],0.25,1e-5,"still x");Near(clip[1]/clip[3],-0.5,1e-5,"still y");
    }

    // Jitter: Halton values in range, and the patched constants move clip.xy
    // by the expected fraction of clip.w.
    for(uint32_t phase=0;phase<72;++phase) {
        const auto j=NativeTemporalJitterForPhase(phase,w,h);
        assert(j.pixels_x>=-0.5f&&j.pixels_x<0.5f&&j.pixels_y>=-0.5f&&j.pixels_y<0.5f);
        Near(j.clip_x,2.0*j.pixels_x/w,1e-9,"clip x");Near(j.clip_y,-2.0*j.pixels_y/h,1e-9,"clip y");
    }
    const auto jitter=NativeTemporalJitterForPhase(3,w,h);
    auto jittered=state;
    NativeTemporalApplyJitter(jittered,jitter);
    const auto patched=NativeTemporalReadCamera(jittered);
    NativeTemporalCamera patched_camera=*patched;
    const std::array<double,3> world{8600.0,-3480.0,-2840.0};
    const auto a=Clip(*read,world),b=Clip(patched_camera,world);
    Near(b[0]-a[0],jitter.clip_x*a[3],1e-3*std::abs(a[3]),"jitter x");
    Near(b[1]-a[1],jitter.clip_y*a[3],1e-3*std::abs(a[3]),"jitter y");
    Near(b[3],a[3],1e-6*std::abs(a[3]),"jitter w");

    // HUD (UE3 Canvas) orthographic matrix in c6..c9.
    std::vector<uint8_t> hud(0x3600);
    auto store=[&](unsigned reg,unsigned component,float value) {
        const uint32_t word=std::byteswap(std::bit_cast<uint32_t>(value));
        std::memcpy(hud.data()+0x780+reg*16+component*4,&word,4);
    };
    store(6,0,2.0f/w);store(7,1,-2.0f/h);store(8,2,1);
    store(9,0,-1.00078f);store(9,1,1.00139f);store(9,3,1);
    assert(NativeTemporalIsHudDraw(hud,w,h));
    assert(!NativeTemporalIsHudDraw(hud,640,360));
    assert(!NativeTemporalIsHudDraw(state,w,h));
    // Velocity pass: half-size scene-view draw writing red and green, with the
    // capture's PS c11 = (8.84598, -4.975863): 72.35 px per unit on both axes.
    std::vector<uint8_t> velocity(0x3600);
    auto store_word=[&](size_t offset,uint32_t value) {
        const uint32_t word=std::byteswap(value);std::memcpy(velocity.data()+offset,&word,4);
    };
    auto store_float=[&](size_t offset,float value){store_word(offset,std::bit_cast<uint32_t>(value));};
    store_float(0x2918,-1.f);store_float(0x291c,1.f);store_word(0x28dc,3);
    store_float(0x1780+11*16,8.84598f);store_float(0x1780+11*16+4,-4.975863f);
    assert(NativeTemporalIsVelocityDraw(velocity,w/2,h/2,w,h));
    assert(!NativeTemporalIsVelocityDraw(velocity,w,h,w,h));
    const auto scale=NativeTemporalVelocityScale(velocity,w,h);
    assert(scale);
    Near((*scale)[0],72.35,0.01,"velocity scale x");Near((*scale)[1],72.35,0.01,"velocity scale y");
    store_word(0x28dc,15);
    assert(!NativeTemporalIsVelocityDraw(velocity,w/2,h/2,w,h));
    store_float(0x1780+11*16,0);store_float(0x1780+11*16+4,0);
    assert(!NativeTemporalVelocityScale(velocity,w,h));  // the player's zero-velocity draws
    std::puts("Temporal inputs PASS");
}
