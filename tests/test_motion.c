// SPDX-License-Identifier: MIT
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../runtime/motion_math.h"
#include "../runtime/controller_motion.h"

static int near(double a,double b) { return fabs(a-b)<1e-5; }
int main(void) {
    assert(sizeof(PMMotionState)==96);assert(sizeof(PMVibration)==16);
    float a[3]={0,9.80665f,0},g[3]={1,2,3};double gravity[3],omega[3];
    pm_sensor_axes(a,g,gravity,omega);
    assert(near(gravity[0],0)&&near(gravity[1],0)&&near(gravity[2],-1));
    assert(near(omega[0],1)&&near(omega[1],-3)&&near(omega[2],2));
    // A physical pitch must remain pitch; roll and yaw must not leak into it.
    for (int axis=0;axis<3;axis++) for (int sign=-1;sign<=1;sign+=2) {
        float v[3]={0,0,0};v[axis]=(float)sign;
        pm_sensor_axes(a,v,gravity,omega);
        int destination[3]={0,2,1};int sense[3]={1,1,-1};
        for (int i=0;i<3;i++) assert(near(omega[i],i==destination[axis]?sign*sense[axis]:0));
    }
    double q[4]={1,0,0,0},z[3]={0,0,M_PI/2};
    for (int i=0;i<200;i++) pm_quaternion_step(q,z,.005);
    float d[9];pm_direction(q,d);
    assert(near(d[0],0)&&near(d[1],1)&&near(d[3],-1)&&near(d[4],0)&&near(d[8],1));
    double saved[4];memcpy(saved,q,sizeof q);pm_quaternion_step(q,z,2);assert(!memcmp(saved,q,sizeof q));
    assert(pm_motor(0,1)==0);assert(pm_motor(1,1)==65535);assert(pm_motor(1,2)==65535);
    assert(pm_motor(NAN,1)==0);assert(pm_motor(-1,1)==0);assert(pm_motor(1,0)==0);
    assert(pm_motor(.5f,1)>pm_motor(.2f,1));
    puts("motion ABI, six signed axes, gravity, 90-degree integration, timing gaps and rumble bounds: PASS");
    return 0;
}
