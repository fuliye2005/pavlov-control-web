#pragma once
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "pose_geometry.h"
#define DOG_COMPASS_MAX_SAMPLES 256
typedef struct {int16_t mag[3],acc[3];} dog_compass_sample;
typedef struct {uint32_t version,range,samples;float offset[3],correction[3][3],alignment[3][3],rms,alignment_rms;} dog_compass_cal;
typedef struct {dog_compass_sample *data;unsigned count,rejected,range;uint32_t sequence;const char *error;} dog_compass_capture;
static inline double dog_compass_det(const double a[3][3])
{return a[0][0]*(a[1][1]*a[2][2]-a[1][2]*a[2][1])-a[0][1]*(a[1][0]*a[2][2]-a[1][2]*a[2][0])+a[0][2]*(a[1][0]*a[2][1]-a[1][1]*a[2][0]);}
static inline bool dog_compass_inverse(const double a[3][3],double b[3][3])
{
    double d=dog_compass_det(a);if(!isfinite(d)||fabs(d)<1e-12)return false;
    for(unsigned i=0;i<3;i++)for(unsigned j=0;j<3;j++)b[j][i]=(a[(i+1)%3][(j+1)%3]*a[(i+2)%3][(j+2)%3]-a[(i+1)%3][(j+2)%3]*a[(i+2)%3][(j+1)%3])/d;
    return true;
}
static inline bool dog_compass_sqrt(double a[3][3],float out[3][3])
{
    double v[3][3]={{1,0,0},{0,1,0},{0,0,1}};
    for(unsigned iter=0;iter<40;iter++){
        unsigned p=0,q=1;for(unsigned i=0;i<3;i++)for(unsigned j=i+1;j<3;j++)if(fabs(a[i][j])>fabs(a[p][q])){p=i;q=j;}
        if(fabs(a[p][q])<1e-15)break;
        double tau=(a[q][q]-a[p][p])/(2*a[p][q]);double t=(tau>=0?1:-1)/(fabs(tau)+sqrt(1+tau*tau));double c=1/sqrt(1+t*t),s=t*c;
        double off=a[p][q];a[p][p]-=t*off;a[q][q]+=t*off;a[p][q]=a[q][p]=0;
        for(unsigned k=0;k<3;k++){
            if(k!=p&&k!=q){double x=a[k][p],y=a[k][q];a[k][p]=a[p][k]=c*x-s*y;a[k][q]=a[q][k]=s*x+c*y;}
            double x=v[k][p],y=v[k][q];v[k][p]=c*x-s*y;v[k][q]=s*x+c*y;
        }
    }
    double lo=a[0][0],hi=lo;for(unsigned i=0;i<3;i++){if(a[i][i]<lo)lo=a[i][i];if(a[i][i]>hi)hi=a[i][i];}
    if(lo<=1e-12||hi/lo>25||!isfinite(hi))return false;
    for(unsigned i=0;i<3;i++)for(unsigned j=0;j<3;j++){
        double sum=0;for(unsigned k=0;k<3;k++)sum+=v[i][k]*sqrt(a[k][k])*v[j][k];out[i][j]=(float)sum;
    }
    return true;
}
static inline bool dog_compass_apply(const dog_compass_cal *cal,const int16_t *raw,float *out)
{
    for(unsigned i=0;i<3;i++){out[i]=0;for(unsigned j=0;j<3;j++)out[i]+=cal->correction[i][j]*(raw[j]-cal->offset[j]);}
    float n=sqrtf(dog_pose_dot(out,out));return isfinite(n)&&n>.75f&&n<1.25f;
}
static inline bool dog_compass_cal_valid(const dog_compass_cal *c)
{
    if(c->version!=1||(c->range!=8&&c->range!=16&&c->range!=32)||c->samples<96||c->samples>DOG_COMPASS_MAX_SAMPLES||!isfinite(c->rms)||c->rms<0||c->rms>.08f||!isfinite(c->alignment_rms)||c->alignment_rms<0||c->alignment_rms>.065f)return false;
    double a[3][3],r[3][3];
    for(unsigned i=0;i<3;i++){
        if(!isfinite(c->offset[i])||fabsf(c->offset[i])>32760)return false;
        for(unsigned j=0;j<3;j++){
            if(!isfinite(c->correction[i][j])||fabsf(c->correction[i][j])>.03f||!isfinite(c->alignment[i][j]))return false;
            a[i][j]=c->correction[i][j];r[i][j]=c->alignment[i][j];
        }
        if(fabsf(dog_pose_dot(c->alignment[i],c->alignment[i])-1)>.01f)return false;
        for(unsigned j=0;j<i;j++)if(fabsf(dog_pose_dot(c->alignment[i],c->alignment[j]))>.01f)return false;
    }
    for(unsigned i=0;i<3;i++)for(unsigned j=0;j<i;j++)if(fabs(a[i][j]-a[j][i])>1e-7)return false;
    return a[0][0]>0&&a[0][0]*a[1][1]-a[0][1]*a[1][0]>1e-12&&dog_compass_det(a)>1e-15&&dog_compass_det(r)>.99;
}
static inline bool dog_compass_add(dog_compass_capture *c,uint32_t sequence,const int16_t *mag,const int16_t *acc)
{
    if(sequence==c->sequence||c->count>=DOG_COMPASS_MAX_SAMPLES)return false;
    c->sequence=sequence;float norm=0;
    for(unsigned i=0;i<3;i++){float g=acc[i]/4096.f;norm+=g*g;if(mag[i]<-32700||mag[i]>32700){++c->rejected;return false;}}
    if(norm<.75f*.75f||norm>1.25f*1.25f){++c->rejected;return false;}
    for(unsigned n=0;n<c->count;n++){
        float dist=0;for(unsigned i=0;i<3;i++){float d=mag[i]-c->data[n].mag[i];dist+=d*d;}
        if(dist<1600){++c->rejected;return false;}
    }
    for(unsigned i=0;i<3;i++){c->data[c->count].mag[i]=mag[i];c->data[c->count].acc[i]=acc[i];}
    ++c->count;return true;
}
static inline bool dog_compass_fit(dog_compass_capture *c,dog_compass_cal *result)
{
    if(c->count<96){c->error="need_more_rotations";return false;}
    double mean[3]={0},scale[3],minimum[3]={32767,32767,32767},maximum[3]={-32768,-32768,-32768};
    for(unsigned n=0;n<c->count;n++)for(unsigned i=0;i<3;i++){
        double x=c->data[n].mag[i];mean[i]+=x/c->count;if(x<minimum[i])minimum[i]=x;if(x>maximum[i])maximum[i]=x;
    }
    for(unsigned i=0;i<3;i++){
        scale[i]=(maximum[i]-minimum[i])/2;
        double span_g=(maximum[i]-minimum[i])*c->range/32000.;
        if(span_g<.25||span_g>5){c->error="magnetic_span_invalid";return false;}
    }
    double system[9][10]={{0}};
    for(unsigned n=0;n<c->count;n++){
        double x=(c->data[n].mag[0]-mean[0])/scale[0],y=(c->data[n].mag[1]-mean[1])/scale[1],z=(c->data[n].mag[2]-mean[2])/scale[2];
        double row[9]={x*x,y*y,z*z,2*x*y,2*x*z,2*y*z,2*x,2*y,2*z};
        for(unsigned i=0;i<9;i++){system[i][9]+=row[i];for(unsigned j=0;j<9;j++)system[i][j]+=row[i]*row[j];}
    }
    for(unsigned k=0;k<9;k++){
        unsigned p=k;for(unsigned i=k+1;i<9;i++)if(fabs(system[i][k])>fabs(system[p][k]))p=i;
        if(fabs(system[p][k])<1e-8){c->error="rotate_all_axes";return false;}
        for(unsigned j=k;j<10;j++){double x=system[k][j];system[k][j]=system[p][j];system[p][j]=x;}
        double d=system[k][k];for(unsigned j=k;j<10;j++)system[k][j]/=d;
        for(unsigned i=0;i<9;i++)if(i!=k){double f=system[i][k];for(unsigned j=k;j<10;j++)system[i][j]-=f*system[k][j];}
    }
    double a[3][3]={{system[0][9],system[3][9],system[4][9]},{system[3][9],system[1][9],system[5][9]},{system[4][9],system[5][9],system[2][9]}},inverse[3][3],center[3]={0},b[3]={system[6][9],system[7][9],system[8][9]};
    if(!dog_compass_inverse(a,inverse)){c->error="fit_unstable";return false;}
    for(unsigned i=0;i<3;i++)for(unsigned j=0;j<3;j++)center[i]-=inverse[i][j]*b[j];
    double denominator=1;for(unsigned i=0;i<3;i++)for(unsigned j=0;j<3;j++)denominator+=center[i]*a[i][j]*center[j];
    if(denominator<=0||!isfinite(denominator)){c->error="fit_unstable";return false;}
    dog_compass_cal out={0};out.range=c->range;out.samples=c->count;
    for(unsigned i=0;i<3;i++){out.offset[i]=(float)(mean[i]+scale[i]*center[i]);for(unsigned j=0;j<3;j++)a[i][j]/=denominator*scale[i]*scale[j];}
    if(!dog_compass_sqrt(a,out.correction)){c->error="fit_unstable";return false;}
    unsigned octants[8]={0};double residual=0;
    for(unsigned n=0;n<c->count;n++){
        float v[3];if(!dog_compass_apply(&out,c->data[n].mag,v)){c->error="magnetic_interference";return false;}
        double e=sqrt(dog_pose_dot(v,v))-1;residual+=e*e;
        unsigned bin=(v[0]>0?1:0)|(v[1]>0?2:0)|(v[2]>0?4:0);++octants[bin];
    }
    for(unsigned i=0;i<8;i++)if(octants[i]<4){c->error="rotate_all_axes";return false;}
    out.rms=(float)sqrt(residual/c->count);if(out.rms>.08f){c->error="magnetic_interference";return false;}
    static const unsigned permutations[6][3]={{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
    double best=1e9,runner=1e9;
    for(unsigned p=0;p<6;p++)for(unsigned bits=0;bits<8;bits++){
        double rotation[3][3]={{0}};for(unsigned i=0;i<3;i++)rotation[i][permutations[p][i]]=(bits&(1u<<i))?-1:1;
        if(dog_compass_det(rotation)<.5)continue;
        double sum=0,square=0;
        for(unsigned n=0;n<c->count;n++){
            float v[3],g[3];dog_compass_apply(&out,c->data[n].mag,v);dog_pose_unit(v);
            for(unsigned i=0;i<3;i++)g[i]=c->data[n].acc[i]/4096.f;
            dog_pose_unit(g);
            double dot=0;for(unsigned i=0;i<3;i++)for(unsigned j=0;j<3;j++)dot+=g[i]*rotation[i][j]*v[j];sum+=dot;square+=dot*dot;
        }
        double average=sum/c->count,variance=square/c->count-average*average;double score=sqrt(variance>0?variance:0);
        if(fabs(average)>.98)continue;
        if(score<best){runner=best;best=score;for(unsigned i=0;i<3;i++)for(unsigned j=0;j<3;j++)out.alignment[i][j]=(float)rotation[i][j];}
        else if(score<runner)runner=score;
    }
    if(best>.065||runner-best<.01){c->error="sensor_alignment_unconfirmed";return false;}
    out.alignment_rms=(float)best;out.version=1;
    if(!dog_compass_cal_valid(&out)){c->error="fit_unstable";return false;}
    *result=out;c->error=NULL;return true;
}
static inline bool dog_compass_heading(const dog_compass_cal *c,const dog_pose_mount *mount,const int16_t *mag,const int16_t *acc,float *heading)
{
    if(!dog_compass_cal_valid(c)||!dog_pose_mount_valid(mount))return false;
    float v[3],g[3],aligned[3]={0},body[3]={0},gravity[3]={0};
    if(!dog_compass_apply(c,mag,v))return false;
    float norm=0;for(unsigned i=0;i<3;i++){g[i]=acc[i]/4096.f;norm+=g[i]*g[i];}
    if(norm<.75f*.75f||norm>1.25f*1.25f)return false;
    for(unsigned i=0;i<3;i++)for(unsigned j=0;j<3;j++)aligned[i]+=c->alignment[i][j]*v[j];
    for(unsigned i=0;i<3;i++)for(unsigned j=0;j<3;j++){body[i]+=mount->row[i][j]*aligned[j];gravity[i]+=mount->row[i][j]*g[j];}
    dog_pose_unit(gravity);float forward[3]={1-gravity[0]*gravity[0],-gravity[0]*gravity[1],-gravity[0]*gravity[2]},left[3];
    if(dog_pose_dot(forward,forward)<.1f||!dog_pose_unit(forward))return false;
    dog_pose_cross(gravity,forward,left);float x=dog_pose_dot(body,forward),y=dog_pose_dot(body,left);
    if(x*x+y*y<.04f)return false;
    float h=atan2f(y,x)*57.2957795f;*heading=fmodf(h+360,360);return true;
}
