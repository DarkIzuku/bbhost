#include "host/scene_motion.h"
#include <cmath>
#include <cstdio>
#include <limits>
using namespace gpu;
namespace {
int fails=0;
void check(bool ok,const char* why){if(!ok){++fails; std::printf("FAIL %s\n",why);}}
bool near(float a,float b,float eps=0.002f){return std::fabs(a-b)<eps;}
SceneCamera camera(float angle=0){
    const float s=std::sin(angle),c=std::cos(angle);
    // Independent pinhole projection, near 0.1 / far 100. No game bytes.
    return {{1.5f*c,0,-1.5f*s,0,0,2,0,0,1.001001f*s,0,1.001001f*c,-0.1001001f,s,0,c,0},{0,0,0}};
}
std::array<float,3> project(const SceneCamera& c,std::array<float,3> point){
    const auto& m=c.clip_from_relative_world; float a[4]{};
    for(int r=0;r<4;++r){a[r]=m[r*4+3];for(int k=0;k<3;++k)a[r]+=m[r*4+k]*(point[k]-c.world_origin[k]);}
    return {0.5f+0.5f*a[0]/a[3],0.5f-0.5f*a[1]/a[3],a[2]/a[3]};
}
}
int main(){
    auto old=camera(),now=camera(0.12f); now.world_origin={1,2,0.5f};
    SceneMatrix transform; check(scene_reprojection(now,old,transform),"rotation and translation reprojection exists");
    for(auto point:{std::array<float,3>{0,0,10},std::array<float,3>{-2,4,8},std::array<float,3>{3,-1,20}}){
        auto a=project(now,point),b=project(old,point); TemporalSample mv;
        check(scene_camera_motion(transform,a[0],a[1],a[2],{1920,1080},mv),"finite visible world point motion");
        const float expected_x=(b[0]-a[0])*1920,expected_y=(b[1]-a[1])*1080;
        // Perspective division and the float depth input amplify matrix
        // roundoff. Keep error below 0.02 render pixels, not a UV tolerance.
        check(near(mv.x,expected_x,0.02f)&&near(mv.y,expected_y,0.02f),"motion matches two independent world projections, including Y sign");
    }
    check(scene_reprojection(old,old,transform),"stationary camera reprojection"); TemporalSample mv;
    check(scene_camera_motion(transform,0.25f,0.75f,0.9f,{3840,2160},mv)&&near(mv.x,0)&&near(mv.y,0),"stationary camera zero motion at 4K");
    auto up=old;up.world_origin[1]=1;check(scene_reprojection(up,old,transform),"vertical translation");
    auto p=project(up,{0,0,10});check(scene_camera_motion(transform,p[0],p[1],p[2],{2560,1440},mv)&&mv.y<0,"upward camera gives upward previous-pixel motion");
    SceneCamera jittered;check(scene_projection_jitter(now,{0.25f,-0.375f},{1280,720},jittered),"scene jitter accepted");
    auto a=project(now,{0,0,10}),b=project(jittered,{0,0,10});
    check(near((b[0]-a[0])*1280,0.25f)&&near((b[1]-a[1])*720,-0.375f),"jitter matches raster pixel displacement");
    check(now.clip_from_relative_world!=jittered.clip_from_relative_world && now.world_origin==jittered.world_origin,"jitter uses a separate scene camera copy");
    check(!scene_projection_jitter(now,{1,0},{1280,720},jittered),"invalid jitter refused");
    float fields[216]{};for(int k=0;k<3;++k){fields[180+k*5]=1;fields[183+k*4]=now.world_origin[k];}fields[195]=1;
    for(int k=0;k<16;++k) fields[200+k]=now.clip_from_relative_world[k];SceneCamera decoded;
    check(scene_camera_decode(fields,sizeof(fields),decoded)&&decoded.clip_from_relative_world==now.clip_from_relative_world&&decoded.world_origin==now.world_origin,"GX scene layout decodes origin and actual projection");
    check(!scene_camera_decode(fields,sizeof(fields)-1,decoded),"truncated constants refused");fields[180]=0;
    check(!scene_camera_decode(fields,sizeof(fields),decoded),"non-camera block refused");
    auto singular=old;singular.clip_from_relative_world[10]=0;singular.clip_from_relative_world[11]=0;
    check(!scene_camera_valid(singular),"singular camera refused");auto nan=old;nan.world_origin[1]=std::numeric_limits<float>::quiet_NaN();
    check(!scene_camera_valid(nan),"non-finite origin refused");
    auto ui=old;ui.clip_from_relative_world={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};check(!scene_camera_valid(ui),"orthographic HUD cannot be a scene camera");
    SceneCameraHistory h; check(h.prepare(old,100,1,transform)==HistoryReset::FirstFrame,"first valid scene resets");h.commit(old,100,1);
    check(h.prepare(old,101,1,transform)==HistoryReset::None,"consecutive stable scene retains history");
    check(h.prepare(old,103,1,transform)==HistoryReset::Load,"missing scene frames reset after loading");
    check(h.prepare(old,100,1,transform)==HistoryReset::Load,"duplicate frame does not advance history");
    check(h.prepare(old,101,2,transform)==HistoryReset::ResourceEpoch,"rebuilt targets reset history");
    auto warp=old;warp.world_origin[0]=70;check(h.prepare(warp,101,1,transform)==HistoryReset::Teleport,"teleport resets");
    check(h.prepare(camera(1.2f),101,1,transform)==HistoryReset::CameraCut,"large camera rotation resets");
    h.commit(nan,102,1);check(h.prepare(old,103,1,transform)==HistoryReset::FirstFrame,"invalid camera clears history");
    check(!scene_camera_motion(transform,0.5f,0.5f,2,{1920,1080},mv),"invalid depth refused");
    std::printf("scene motion: %s\n",fails?"FAILED":"passed");return fails?1:0;
}
