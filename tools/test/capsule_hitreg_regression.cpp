// Offline shot geometry tests. Includes the implementation so primitive edge
// cases can be checked without changing the game's public API.
#include "../../src/shared/capsule_trace.cpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>

static int checks = 0, failures = 0;
static void require(bool ok, const char* label) {
    ++checks;
    if (!ok) { ++failures; if (failures < 25) std::printf("FAIL %s\n", label); }
}

// Independent double-precision ray/solid intersection. The solid is a finite
// linearly tapered circular body plus a sphere at each end, matching the shapes
// supplied to the engine. Test both roots and both spheres, then take the first
// positive entry (or zero when starting inside).
static double reference(const float* o, const float* d, const float* a,
                        const float* b, double ra, double rb) {
    double axis[3], m[3];
    double aa=0, nd=0, mn=0, dm=0, mm=0, dd=0;
    for (int i=0;i<3;++i) {
        axis[i]=double(b[i])-a[i]; m[i]=double(o[i])-a[i];
        aa+=axis[i]*axis[i]; nd+=axis[i]*d[i]; mn+=axis[i]*m[i];
        dm+=m[i]*d[i]; mm+=m[i]*m[i]; dd+=double(d[i])*d[i];
    }
    double best=std::numeric_limits<double>::infinity();
    auto roots=[&](double A,double B,double C,auto accept) {
        if (std::fabs(A)<1e-20) {
            if (std::fabs(B)>1e-20) accept(-C/B);
            return;
        }
        double disc=B*B-4*A*C;
        if (disc<0) return;
        double q=-0.5*(B+std::copysign(std::sqrt(disc),B));
        if (q==0) { accept(-B/(2*A)); return; }
        accept(q/A); accept(C/q);
    };
    auto sphere=[&](const float* center,double r) {
        double c=0,v=0;
        for(int i=0;i<3;++i) { double s=double(o[i])-center[i]; c+=s*s;v+=s*d[i]; }
        if(c<=r*r) best=0;
        roots(dd,2*v,c-r*r,[&](double t){if(t>=0)best=std::min(best,t);});
    };
    sphere(a,ra); sphere(b,rb);
    if(aa>1e-16) {
        double dr=rb-ra;
        double r=ra+dr*mn/aa, slope=dr*nd/aa;
        double A=dd-nd*nd/aa-slope*slope;
        double B=2*(dm-mn*nd/aa-r*slope);
        double C=mm-mn*mn/aa-r*r;
        if(mn>=0&&mn<=aa&&C<=0)best=0;
        roots(A,B,C,[&](double t){double s=mn+nd*t;if(t>=0&&s>=0&&s<=aa)best=std::min(best,t);});
    }
    return best;
}

static float primitive(const float* o,const float* d,const float* a,const float* b,float ra,float rb) {
    // Same near-equal radius normalisation as cap_test.
    float t=cap_ray_tapered(o,d,a,b,ra,std::fabs(ra-rb)<.02f?ra:rb);
    if(t<0) {
        float c[3],off[3];float along=cap_closest_on_segment(o,a,b,c);
        cap_sub(o,c,off);
        if(cap_dot(off,off)<=std::pow(ra+(rb-ra)*along,2)) return 0;
        return std::numeric_limits<float>::infinity();
    }
    return t;
}

static void primitives() {
    const float a[3]={0,0,0},b[3]={0,0,20};
    struct Case { float o[3],d[3];float ra,rb;const char* name; };
    const Case cases[]={
        {{0,0,-10},{0,0,1},3,3,"axis entry"},
        {{0,0,30},{0,0,-1},3,3,"reverse axis entry"},
        {{2.9f,0,-10},{0,0,1},3,3,"parallel inside radius"},
        {{3.1f,0,-10},{0,0,1},3,3,"parallel outside radius"},
        {{3,0,-10},{0,0,1},3,3,"parallel tangent"},
        {{-10,3,10},{1,0,0},3,3,"side tangent"},
        {{-10,3.001f,10},{1,0,0},3,3,"side just outside"},
        {{-10,2.999f,10},{1,0,0},3,3,"side just inside"},
        {{0,0,10},{1,0,0},3,3,"inside constant body"},
        {{0,0,-2},{0,0,-1},3,3,"inside rounded end"},
        {{-10,0,10},{-1,0,0},3,3,"ray pointing away"},
        {{0,0,-10},{0,0,1},5,2,"taper axis entry"},
        {{3,0,-10},{0,0,1},2,5,"taper axial side entry"},
        {{0,0,10},{1,0,0},5,2,"inside tapered body"},
        {{-8192,2.999f,10},{1,0,0},3,3,"long grazing shot"},
        {{-8192,3.001f,10},{1,0,0},3,3,"long grazing miss"},
        {{4.87f,0,19},{1,0,0},2,5,"inside overlapping end sphere"},
    };
    for(const auto& c:cases) {
        float got=primitive(c.o,c.d,a,b,c.ra,c.rb);
        double want=reference(c.o,c.d,a,b,c.ra,c.rb);
        bool ok=std::isfinite(got)==std::isfinite(want);
        if(ok&&std::isfinite(want))ok=std::fabs(got-want)<.002;
        require(ok,c.name);
        if(!ok)std::printf("  got %.9g reference %.9g\n",got,want);
    }
}

static void random_primitives() {
    std::mt19937 rng(0x125ca95u);
    std::uniform_real_distribution<float> u(-1,1),r(.5f,6),len(2,30);
    int bad=0;double worst=0;
    for(int n=0;n<20000;++n) {
        float a[3]={u(rng)*25,u(rng)*25,u(rng)*25};
        float axis[3]={u(rng),u(rng),u(rng)};
        float norm=std::sqrt(cap_dot(axis,axis)),l=len(rng);
        float b[3],o[3],d[3];
        for(int i=0;i<3;++i) { b[i]=a[i]+axis[i]*l/norm;o[i]=a[i]+u(rng)*50;d[i]=(a[i]+b[i])*.5f+u(rng)*6-o[i]; }
        norm=std::sqrt(cap_dot(d,d));for(int i=0;i<3;++i)d[i]/=norm;
        float ra=r(rng),rb=n%2?ra:r(rng);
        if(std::fabs(ra-rb)<.02f) rb=ra;
        float got=primitive(o,d,a,b,ra,rb);
        double want=reference(o,d,a,b,ra,rb);
        bool ok=std::isfinite(got)==std::isfinite(want);
        if(ok&&std::isfinite(want)) { double error=std::fabs(got-want);worst=std::max(worst,error);ok=error<.002; }
        if(!ok&&bad++<5) {
            std::printf("random %d got %.9g reference %.9g ra %.9g rb %.9g\n",n,got,want,ra,rb);
            std::printf(" a %.9g %.9g %.9g b %.9g %.9g %.9g o %.9g %.9g %.9g d %.9g %.9g %.9g\n",a[0],a[1],a[2],b[0],b[1],b[2],o[0],o[1],o[2],d[0],d[1],d[2]);
        }
    }
    std::printf("random primitives: 20000 rays, %d disagreements, worst distance error %.9g\n",bad,worst);
    require(bad==0,"random primitives agree with independent reference");
}

static capsuleBone_t bones[CAP_BONE_COUNT];
static void bone(int index,float x,float y,float z) {
    const float q[4]={0,0,0,1},p[3]={x,y,z};
    capsule_bone_set(bones+index,q,p,2,(unsigned short)(index+100));
}
static void standing() {
    std::memset(bones,0,sizeof(bones));
    bone(CAP_J_SPINE1,0,0,38);bone(CAP_J_SPINE2,0,0,43);bone(CAP_J_SPINE4,0,0,52);
    bone(CAP_J_NECK,0,0,55);bone(CAP_J_HEAD,0,0,58);
    const float headAxis[3][3]={{0,0,1},{1,0,0},{0,1,0}};
    std::memcpy(bones[CAP_J_HEAD].axis,headAxis,sizeof(headAxis));
    bone(CAP_J_HIP_LE,0,4,36);bone(CAP_J_HIP_RI,0,-4,36);
    bone(CAP_J_CLAVICLE_LE,0,2,55);bone(CAP_J_CLAVICLE_RI,0,-2,55);
    bone(CAP_J_SHOULDER_LE,0,8,54);bone(CAP_J_SHOULDER_RI,0,-8,54);
    bone(CAP_J_ELBOW_LE,0,16,44);bone(CAP_J_ELBOW_RI,0,-16,44);
    bone(CAP_J_WRIST_LE,0,22,34);bone(CAP_J_WRIST_RI,0,-22,34);
    bone(CAP_J_MID_LE_1,0,24,30);bone(CAP_J_MID_RI_1,0,-24,30);
    bone(CAP_J_KNEE_LE,0,7,20);bone(CAP_J_KNEE_RI,0,-7,20);
    bone(CAP_J_ANKLE_LE,0,7,5);bone(CAP_J_ANKLE_RI,0,-7,5);
    bone(CAP_J_BALL_LE,3,7,2);bone(CAP_J_BALL_RI,3,-7,2);
}

struct Expected {
    const float* o;const float* d;double length,limit,best;int hit,priority;
    const unsigned char* priorities;
    static void shape(void* context,const float* a,const float* b,float ra,float rb,int hit,unsigned short) {
        Expected& e=*static_cast<Expected*>(context);
        int priority=e.priorities?e.priorities[hit]:3;
        if(priority<=1)return;
        if(std::fabs(ra-rb)<.02f)rb=ra;
        double t=reference(e.o,e.d,a,b,ra,rb),f=t/e.length;
        if(f>=e.limit||priority<e.priority||(priority==e.priority&&f>=e.best))return;
        e.best=f;e.hit=hit;e.priority=priority;
    }
};

// These are synthetic joint arrangements, not captures of CoD2 animation
// assets. They check tracing under posture changes and rigid transformations;
// they do not establish visual fit to every live animated player model.
static void poses_and_models() {
    const char* models[]={nullptr,"playerbody_american_normandy01","playerbody_german_winterdark",
        "playerbody_russian_coat01","playerbody_british_africa01"};
    int shots=0,bad=0;double worst=0;
    std::mt19937 rng(0xc20f17u);std::uniform_real_distribution<float> u(-1,1);
    for(int pose=0;pose<3;++pose)for(int orientation=0;orientation<3;++orientation)for(const char* model:models) {
        standing();
        if(pose==1)for(auto& b:bones)if(b.valid) {
            if(b.origin[2]>22) { b.origin[0]+=5;b.origin[2]-=17; }
            else if(b.origin[2]>10) { b.origin[0]+=12;b.origin[2]-=7; }
        }
        if(pose==2)for(auto& b:bones)if(b.valid) {
            float z=b.origin[2];b.origin[2]=-b.origin[0]+9;b.origin[0]=z-20;
            for(auto& axis:b.axis) { float zAxis=axis[2];axis[2]=-axis[0];axis[0]=zAxis; }
        }
        float angle=orientation*1.1f,c=std::cos(angle),s=std::sin(angle);
        for(auto& b:bones)if(b.valid) {
            float x=b.origin[0];b.origin[0]=c*x-s*b.origin[1]+13;b.origin[1]=s*x+c*b.origin[1]-7;
            for(auto& axis:b.axis){float xAxis=axis[0];axis[0]=c*xAxis-s*axis[1];axis[1]=s*xAxis+c*axis[1];}
        }
        capsuleFit_t fit={};
        if(model) {
            capsule_fit_add_model(&fit,model);capsule_fit_add_model(&fit,"head_us_ranger_david");
            capsule_fit_add_model(&fit,"helmet_us_ranger_generic");
        }
        for(int n=0;n<400;++n) {
            float o[3]={13+u(rng)*120,-7+u(rng)*120,30+u(rng)*80};
            float end[3]={13+u(rng)*30,-7+u(rng)*25,30+u(rng)*35},d[3];
            cap_sub(end,o,d);float length=std::sqrt(cap_dot(d,d));for(auto& v:d)v/=length;
            unsigned char priority[19];std::memset(priority,2,sizeof(priority));
            if(n%3==0)priority[2]=4;
            if(n%5==0)priority[4]=1;
            float limit=n%7==0?.2f:1;
            float scale=n%2?.5f:1;
            Expected e{o,d,length,limit,limit,0,0,priority};
            capsule_for_each_shape(bones,scale,Expected::shape,&e,model?&fit:nullptr);
            capsuleTraceResult_t result={};result.fraction=limit;
            bool tested=capsule_trace(bones,o,end,priority,scale,&result,model?&fit:nullptr);
            bool ok=tested&&result.partGroup==e.hit;
            if(e.hit) {
                double error=std::fabs(result.fraction-e.best);worst=std::max(worst,error);
                float normalLen=std::sqrt(cap_dot(result.normal,result.normal));
                ok=ok&&error<2e-5&&std::fabs(normalLen-1)<.001&&result.partName!=0;
            } else ok=ok&&result.fraction==limit;
            if(!ok&&bad++<5)std::printf("pose=%d rotation=%d model=%s ray=%d got=%d want=%d fraction %.9g reference %.9g\n",pose,orientation,model?model:"shared",n,result.partGroup,e.hit,result.fraction,e.best);
            ++shots;
        }
    }
    std::printf("public API: %d synthetic pose/model rays, %d disagreements, worst fraction error %.9g\n",shots,bad,worst);
    require(bad==0,"public trace matches reference across poses/models/priorities/walls");
}

static void lifecycle_and_limits() {
    standing();
    const float inside[3]={0,0,48},outside[3]={100,0,48};
    capsuleTraceResult_t result={};result.fraction=0;
    capsule_trace(bones,inside,outside,nullptr,1,&result);
    require(result.partGroup==0&&result.fraction==0,"zero wall limit blocks start-inside hit");
    result={};result.fraction=1;
    capsule_trace(bones,inside,outside,nullptr,1,&result);
    require(result.partGroup==4&&result.fraction==0,"start-inside chest hits immediately");
    std::memset(bones,0,sizeof(bones));
    result={};result.fraction=.37f;result.partName=177;result.partGroup=9;result.surfaceType=123;
    capsuleTraceResult_t before=result;
    require(!capsule_trace(bones,outside,inside,nullptr,1,&result)&&!std::memcmp(&before,&result,sizeof(result)),"empty skeleton leaves native fallback result untouched");
    standing();
    result=before;
    require(!capsule_trace(bones,inside,inside,nullptr,1,&result)&&!std::memcmp(&before,&result,sizeof(result)),"zero-length ray leaves native fallback result untouched");
}

int main() {
    primitives();
    random_primitives();
    poses_and_models();
    lifecycle_and_limits();
    std::printf("capsule hit registration geometry: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
