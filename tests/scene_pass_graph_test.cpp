#include "host/scene_pass_graph.h"
#include <array>
#include <cstdio>
using namespace gpu;
int main() {
    int failures=0;const auto check=[&](bool ok,const char* why){if(!ok){++failures;std::printf("FAIL %s\n",why);}};
    ScenePassGraph graph;graph.begin(100);
    const std::array<std::uint64_t,1> ui{10},scene{20},lighting{30},post{40},display{50};
    const std::array<std::uint64_t,2> composite{40,10};
    const std::span<const std::uint64_t> none;
    check(!graph.observe(ScenePassKind::Scaleform,false,none,ui),"offscreen UI before scene is not a resolve boundary");
    graph.seed(scene);
    check(!graph.observe(ScenePassKind::Engine,true,scene,lighting) && graph.scene_owned(30),"engine deferred lighting retains scene provenance");
    check(!graph.observe(ScenePassKind::Yebis,true,lighting,post) && graph.scene_owned(40),"YEBIS output retains scene provenance");
    check(graph.observe(ScenePassKind::Engine,true,composite,display)==40,"resolve the actual scene before a native offscreen-UI composite");
    check(!graph.scene_owned(50),"HUD composite cannot be a temporal input");
    check(!graph.observe(ScenePassKind::Scaleform,true,none,display),"second UI draw cannot resolve twice");
    graph.begin(101);graph.seed(scene);
    check(graph.observe(ScenePassKind::Scaleform,true,none,scene)==20,"resolve before Scaleform draws directly over scene");
    check(!graph.scene_owned(20),"scene with Scaleform cannot be jittered or resolved again");
    graph.begin(102);graph.seed(scene);graph.seed(post);
    const std::array<std::uint64_t,3> mixed{20,40,10};
    graph.observe(ScenePassKind::Scaleform,false,none,ui);
    check(!graph.observe(ScenePassKind::Engine,true,mixed,display),"ambiguous multi-scene composite declines dispatch");
    graph.begin(103);check(!graph.scene_owned(20),"loading/new frame cannot inherit stale targets");
    graph.begin(104);graph.observe(ScenePassKind::Scaleform,false,none,post);graph.seed(scene);
    graph.observe(ScenePassKind::Yebis,true,scene,post,true);
    check(graph.scene_owned(40),"opaque post overwrite drops earlier UI content in a reused target");
    check(graph.observe(ScenePassKind::Scaleform,true,none,post)==40,"reused target resolves only its new scene content");
    graph.begin(105);graph.seed(scene);graph.clear(20);
    check(!graph.scene_owned(20),"explicit clear invalidates prior target provenance");
    std::printf("scene pass graph: %s\n",failures?"FAILED":"passed");return failures?1:0;
}
