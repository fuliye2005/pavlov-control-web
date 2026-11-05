#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>
/* Body X=front, Y=left, Z=up. Positive pitch means nose down.
 * All calibration vectors are measured specific force, in g (not raw counts). */
typedef struct {uint32_t version;float row[3][3],bias[3];} dog_pose_mount;
typedef struct {unsigned stage,count;bool collecting;uint32_t sequence;float sum[6],square[3];dog_pose_mount draft;const char *error;} dog_pose_wizard;
static inline float dog_pose_dot(const float *a,const float *b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
static inline bool dog_pose_unit(float *v)
{
    float n=sqrtf(dog_pose_dot(v,v));if(!isfinite(n)||n<.1f)return false;
    for(unsigned i=0;i<3;i++)v[i]/=n;
    return true;
}
static inline void dog_pose_cross(const float *a,const float *b,float *out)
{out[0]=a[1]*b[2]-a[2]*b[1];out[1]=a[2]*b[0]-a[0]*b[2];out[2]=a[0]*b[1]-a[1]*b[0];}
static inline bool dog_pose_mount_valid(const dog_pose_mount *m)
{
    if(m->version!=1)return false;
    for(unsigned i=0;i<3;i++){
        if(!isfinite(m->bias[i])||fabsf(m->bias[i])>10)return false;
        for(unsigned k=0;k<3;k++)if(!isfinite(m->row[i][k]))return false;
        if(fabsf(dog_pose_dot(m->row[i],m->row[i])-1)>.01f)return false;
        for(unsigned k=0;k<i;k++)if(fabsf(dog_pose_dot(m->row[i],m->row[k]))>.01f)return false;
    }
    float cross[3];dog_pose_cross(m->row[0],m->row[1],cross);
    return dog_pose_dot(cross,m->row[2])>.99f;
}
static inline bool dog_pose_begin(dog_pose_wizard *w,unsigned step)
{
    if(step>2||w->collecting||(step&&w->stage!=step))return false;
    if(!step){w->stage=0;w->draft.version=0;}
    w->count=0;w->collecting=true;w->error=NULL;
    for(unsigned i=0;i<6;i++)w->sum[i]=0;
    for(unsigned i=0;i<3;i++)w->square[i]=0;
    return true;
}
/* Returns true only when the complete, physically checked frame is ready to save.
 * Ten distinct MCU samples are required; no repeated HTTP sample can advance it. */
static inline bool dog_pose_collect(dog_pose_wizard *w,uint32_t sequence,const float *acc,const float *gyro)
{
    if(!w->collecting||sequence==w->sequence)return false;
    w->sequence=sequence;
    float norm=dog_pose_dot(acc,acc);
    if(!isfinite(norm)||norm<.75f*.75f||norm>1.25f*1.25f){w->error="unstable_gravity";w->collecting=false;return false;}
    for(unsigned i=0;i<3;i++){
        if(!isfinite(gyro[i])||fabsf(gyro[i])>10){w->error="keep_still";w->collecting=false;return false;}
        w->sum[i]+=acc[i];w->square[i]+=acc[i]*acc[i];w->sum[i+3]+=gyro[i];
    }
    if(++w->count<10)return false;
    w->collecting=false;float a[3];
    for(unsigned i=0;i<3;i++){
        a[i]=w->sum[i]/10;
        if(w->square[i]/10-a[i]*a[i]>.000625f){w->error="keep_still";return false;}
    }
    if(!dog_pose_unit(a)){w->error="invalid_gravity";return false;}
    if(w->stage==0){
        for(unsigned i=0;i<3;i++){w->draft.row[2][i]=a[i];w->draft.bias[i]=w->sum[i+3]/10;}
        w->stage=1;return false;
    }
    float vertical=dog_pose_dot(a,w->draft.row[2]);
    if(vertical<.42f||vertical>.94f){w->error="tilt_20_to_65_degrees";return false;}
    if(w->stage==1){
        for(unsigned i=0;i<3;i++)w->draft.row[0][i]=-(a[i]-vertical*w->draft.row[2][i]);
        if(!dog_pose_unit(w->draft.row[0])){w->error="invalid_forward_tilt";return false;}
        dog_pose_cross(w->draft.row[2],w->draft.row[0],w->draft.row[1]);
        w->stage=2;return false;
    }
    if(dog_pose_dot(a,w->draft.row[1])>-.34f||fabsf(dog_pose_dot(a,w->draft.row[0]))>.25f){w->error="left_tilt_not_detected";return false;}
    w->draft.version=1;
    if(!dog_pose_mount_valid(&w->draft)){w->error="invalid_frame";return false;}
    w->stage=3;return true;
}
static inline bool dog_pose_angles(const dog_pose_mount *m,const float *acc,float *roll,float *pitch,float *upright)
{
    if(!dog_pose_mount_valid(m))return false;
    float norm=dog_pose_dot(acc,acc);
    if(!isfinite(norm)||norm<.75f*.75f||norm>1.25f*1.25f)return false;
    float x=dog_pose_dot(m->row[0],acc),y=dog_pose_dot(m->row[1],acc),z=dog_pose_dot(m->row[2],acc);
    *roll=atan2f(y,z)*57.2957795f;*pitch=atan2f(-x,sqrtf(y*y+z*z))*57.2957795f;*upright=z/sqrtf(norm);return true;
}
