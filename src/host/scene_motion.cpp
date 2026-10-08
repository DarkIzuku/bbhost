#include "host/scene_motion.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace gpu {
namespace {
bool inverse(const SceneMatrix& m, SceneMatrix& out) {
    double a[4][8]{};
    for (int r=0;r<4;++r) for (int c=0;c<4;++c) {a[r][c]=m[r*4+c]; a[r][c+4]=r==c?1:0;}
    for (int c=0;c<4;++c) {
        int pivot=c;
        for (int r=c+1;r<4;++r) if (std::fabs(a[r][c])>std::fabs(a[pivot][c])) pivot=r;
        if (!std::isfinite(a[pivot][c]) || std::fabs(a[pivot][c])<1e-10) return false;
        if(pivot!=c) for(int k=0;k<8;++k) std::swap(a[pivot][k],a[c][k]);
        const double divisor=a[c][c]; for(double& x:a[c]) x/=divisor;
        for(int r=0;r<4;++r) if(r!=c) {const double factor=a[r][c]; for(int k=0;k<8;++k) a[r][k]-=factor*a[c][k];}
    }
    SceneMatrix result{};
    for(int r=0;r<4;++r) for(int c=0;c<4;++c) {
        const double x=a[r][c+4];
        if(!std::isfinite(x) || std::fabs(x)>std::numeric_limits<float>::max()) return false;
        result[r*4+c]=static_cast<float>(x);
    }
    out=result; return true;
}
SceneMatrix identity() {return {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};}
}
bool scene_camera_valid(const SceneCamera& c) {
    for(float x:c.world_origin) if(!std::isfinite(x)) return false;
    for(float x:c.clip_from_relative_world) if(!std::isfinite(x)) return false;
    const auto& m=c.clip_from_relative_world;
    // Perspective with an origin-relative camera: homogeneous W is its
    // direction dot position. An orthographic UI/shadow matrix is rejected.
    const double forward=m[12]*m[12]+m[13]*m[13]+m[14]*m[14];
    if (std::fabs(forward-1)>0.01 || std::fabs(m[15])>1e-5 || std::fabs(m[3])>1e-5 || std::fabs(m[7])>1e-5) return false;
    SceneMatrix unused; return inverse(m,unused);
}
bool scene_camera_decode(const void* constants, std::size_t bytes, SceneCamera& out) {
    if(!constants || bytes<216*sizeof(float)) return false;
    float fields[216]; std::memcpy(fields,constants,sizeof(fields));
    for(int r=0;r<3;++r) for(int s=0;s<3;++s) {
        double dot=0;
        for(int k=0;k<3;++k) dot+=fields[180+r*4+k]*fields[180+s*4+k];
        if(!std::isfinite(dot) || std::fabs(dot-(r==s?1:0))>0.01) return false;
    }
    if(fields[192]!=0 || fields[193]!=0 || fields[194]!=0 || fields[195]!=1) return false;
    SceneCamera c;
    for(int k=0;k<3;++k) c.world_origin[k]=fields[183+4*k];
    std::copy_n(fields+200,16,c.clip_from_relative_world.begin());
    if(!scene_camera_valid(c)) return false;
    out=c; return true;
}
bool scene_reprojection(const SceneCamera& current, const SceneCamera& previous, SceneMatrix& out) {
    if(!scene_camera_valid(current) || !scene_camera_valid(previous)) return false;
    SceneMatrix inv; if(!inverse(current.clip_from_relative_world,inv)) return false;
    // Engine positions are camera relative. Add current-origin minus
    // previous-origin before projecting, so translation is not lost.
    SceneMatrix relative=inv;
    for(int r=0;r<3;++r) for(int k=0;k<4;++k) relative[r*4+k]+= (current.world_origin[r]-previous.world_origin[r])*inv[12+k];
    SceneMatrix result{};
    for(int r=0;r<4;++r) for(int c=0;c<4;++c) {
        double x=0; for(int k=0;k<4;++k) x+=previous.clip_from_relative_world[r*4+k]*relative[k*4+c];
        if(!std::isfinite(x) || std::fabs(x)>std::numeric_limits<float>::max()) return false;
        result[r*4+c]=static_cast<float>(x);
    }
    out=result; return true;
}
bool scene_camera_motion(const SceneMatrix& m, float u, float v, float depth, UpscaleExtent extent, TemporalSample& out) {
    if(!extent.width || !extent.height || !std::isfinite(u) || !std::isfinite(v) || !std::isfinite(depth) || u<0 || u>1 || v<0 || v>1 || depth<0 || depth>1) return false;
    const double current[4]={2.0*u-1,1-2.0*v,depth,1}; double clip[4]{};
    for(int r=0;r<4;++r) for(int k=0;k<4;++k) clip[r]+=m[r*4+k]*current[k];
    if(!std::isfinite(clip[3]) || clip[3]<=1e-8) return false;
    const double x=(clip[0]/clip[3]-current[0])*0.5*extent.width;
    const double y=(clip[1]/clip[3]-current[1])*-0.5*extent.height;
    if(!std::isfinite(x) || !std::isfinite(y) || std::fabs(x)>std::numeric_limits<float>::max() || std::fabs(y)>std::numeric_limits<float>::max()) return false;
    out={static_cast<float>(x),static_cast<float>(y)}; return true;
}
bool scene_projection_jitter(const SceneCamera& c, TemporalSample jitter, UpscaleExtent extent, SceneCamera& out) {
    if(!scene_camera_valid(c) || !extent.width || !extent.height || !std::isfinite(jitter.x) || !std::isfinite(jitter.y) || std::fabs(jitter.x)>0.5f || std::fabs(jitter.y)>0.5f) return false;
    SceneCamera result=c;
    for(int k=0;k<4;++k) {
        result.clip_from_relative_world[k]+=2*jitter.x/extent.width*c.clip_from_relative_world[12+k];
        result.clip_from_relative_world[4+k]-=2*jitter.y/extent.height*c.clip_from_relative_world[12+k];
    }
    out=result; return true;
}
HistoryReset SceneCameraHistory::prepare(const SceneCamera& c, std::uint64_t frame, std::uint64_t epoch, SceneMatrix& out) const {
    out=identity();
    if(!scene_camera_valid(c)) return HistoryReset::BackendFailure;
    if(!have_previous_) return HistoryReset::FirstFrame;
    if(frame<=previous_frame_ || frame-previous_frame_!=1) return HistoryReset::Load;
    if(epoch!=resource_epoch_) return HistoryReset::ResourceEpoch;
    double distance=0; for(int k=0;k<3;++k) {const double d=c.world_origin[k]-previous_.world_origin[k]; distance+=d*d;}
    if(distance>64*64) return HistoryReset::Teleport;
    double forward_dot=0; for(int k=0;k<3;++k) forward_dot+=c.clip_from_relative_world[12+k]*previous_.clip_from_relative_world[12+k];
    if(forward_dot<0.70710678) return HistoryReset::CameraCut;
    if(!scene_reprojection(c,previous_,out)) return HistoryReset::BackendFailure;
    return HistoryReset::None;
}
void SceneCameraHistory::commit(const SceneCamera& c, std::uint64_t frame, std::uint64_t epoch) {
    if(!scene_camera_valid(c)) {clear(); return;}
    previous_=c; previous_frame_=frame; resource_epoch_=epoch; have_previous_=true;
}
}
