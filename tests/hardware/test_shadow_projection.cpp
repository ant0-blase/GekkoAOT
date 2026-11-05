#include "../../runtime/gx/shadow_projection.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
using namespace GekkoAOT::GX::Shadow;
#define CHECK(x) do { if(!(x)){std::fprintf(stderr,"%d: %s\n",__LINE__,#x);std::abort();} } while(0)
std::array<double,4> Project(const Mat4& m, Vec3 p){
    std::array<double,4> q{};
    for(unsigned r=0;r<4;++r)q[r]=m[4*r]*p[0]+m[4*r+1]*p[1]+m[4*r+2]*p[2]+m[4*r+3];
    return q;
}
int main(){
    constexpr std::array<Vec3,6> axes={Vec3{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    const Vec3 origin={7,-11,19};
    for(bool reversed:{false,true}){
        const auto faces=PointFaces(origin,.25,40,reversed);CHECK(faces);
        for(unsigned i=0;i<6;++i){
            for(auto distance:{.25,3.,40.}){
                auto p=origin;for(unsigned k=0;k<3;++k)p[k]+=axes[i][k]*distance;
                auto q=Project((*faces)[i],p);CHECK(q[3]>0);
                CHECK(std::abs(q[0]/q[3])<1e-10 && std::abs(q[1]/q[3])<1e-10);
                CHECK(q[2]/q[3]>=-1e-10 && q[2]/q[3]<=1.+1e-10);
                if(distance==.25)CHECK(std::abs(q[2]/q[3]-(reversed?1.:0.))<1e-10);
                if(distance==40)CHECK(std::abs(q[2]/q[3]-(reversed?0.:1.))<1e-10);
            }
            // Every cube face has the same 90-degree edge coverage.
            auto p=origin;for(unsigned k=0;k<3;++k)p[k]+=axes[i][k]*3.+axes[(i+2)%6][k]*3.;
            const auto q=Project((*faces)[i],p);
            CHECK(std::abs(std::max(std::abs(q[0]),std::abs(q[1]))/q[3]-1.)<1e-10);
        }
    }
    std::array<Vec3,8> corners{};
    for(unsigned i=0;i<8;++i)corners[i]={i&1?3.:-3.,i&2?2.:-2.,i&4?20.:1.};
    for(auto direction:{Vec3{1,-2,3},Vec3{0,1,0},Vec3{0,0,-1}}){
        const auto fit=DirectionalCascade(corners,direction,1024,5);CHECK(fit);
        for(auto p:corners){const auto q=Project(fit->clip,p);
            CHECK(std::abs(q[0])<=1. && std::abs(q[1])<=1. && q[2]>=0 && q[2]<=1);}
    }
    const auto stable=DirectionalCascade(corners,{0,0,1},1024,5);CHECK(stable);
    for(auto& p:corners)p[0]+=stable->texelSize*.2;
    const auto moved=DirectionalCascade(corners,{0,0,1},1024,5);CHECK(moved);
    CHECK(stable->clip==moved->clip);
    for(unsigned count=1;count<=4;++count){
        const auto split=CascadeSplits(.1,1000,count);CHECK(split);
        CHECK((*split)[0]==.1 && (*split)[count]==1000);
        for(unsigned i=1;i<=count;++i)CHECK((*split)[i]>(*split)[i-1]);
    }
    CHECK(!CascadeSplits(0,100,4));CHECK(!CascadeSplits(1,100,5));
    CHECK(!PointFaces(origin,1,1));CHECK(!PointFaces({NAN,0,0},1,2));
    CHECK(!DirectionalCascade(corners,{0,0,0},1024,1));
    CHECK(!DirectionalCascade(corners,{0,1,0},0,1));
    CHECK(!DirectionalCascade(corners,{0,1,0},1024,-1));
    corners[0][0]=std::numeric_limits<double>::infinity();
    CHECK(!DirectionalCascade(corners,{0,1,0},1024,1));
    std::puts("shadow projection: six point faces, directional fitting/splits, snapping and invalid inputs passed");
}
