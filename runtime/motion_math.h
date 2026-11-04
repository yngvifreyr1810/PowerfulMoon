// SPDX-License-Identifier: MIT
// Small dependency-free math helpers, also used by tests/test_motion.c.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

static inline void pm_sensor_axes(const float a[3], const float g[3], double out_a[3], double out_g[3]) {
    // SDL: X right, Y up, Z toward player. nn::hid: X right, Y away,
    // Z out of the controller face. Acceleration is gravity, not proper force.
    out_a[0] = -a[0] / 9.80665; out_a[1] = a[2] / 9.80665; out_a[2] = -a[1] / 9.80665;
    out_g[0] = g[0]; out_g[1] = -g[2]; out_g[2] = g[1];
}
static inline double pm_norm3(const double v[3]) { return sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]); }
static inline void pm_quaternion_step(double q[4], const double w[3], double dt) {
    if (!isfinite(dt) || dt <= 0 || dt > .1) return;
    double speed=pm_norm3(w), half=speed*dt*.5;
    if (!isfinite(speed)) return;
    double k=speed>1e-12 ? sin(half)/speed : dt*.5;
    double d[4]={cos(half),w[0]*k,w[1]*k,w[2]*k};
    double r[4]={q[0]*d[0]-q[1]*d[1]-q[2]*d[2]-q[3]*d[3],
        q[0]*d[1]+q[1]*d[0]+q[2]*d[3]-q[3]*d[2],
        q[0]*d[2]-q[1]*d[3]+q[2]*d[0]+q[3]*d[1],
        q[0]*d[3]+q[1]*d[2]-q[2]*d[1]+q[3]*d[0]};
    double n=sqrt(r[0]*r[0]+r[1]*r[1]+r[2]*r[2]+r[3]*r[3]);
    if (n>1e-12) for (int i=0;i<4;i++) q[i]=r[i]/n;
}
static inline void pm_direction(const double q[4], float out[9]) {
    double w=q[0],x=q[1],y=q[2],z=q[3];
    // Each successive triple is one controller basis vector in world space.
    out[0]=1-2*(y*y+z*z); out[1]=2*(x*y+w*z); out[2]=2*(x*z-w*y);
    out[3]=2*(x*y-w*z); out[4]=1-2*(x*x+z*z); out[5]=2*(y*z+w*x);
    out[6]=2*(x*z+w*y); out[7]=2*(y*z-w*x); out[8]=1-2*(x*x+y*y);
}
static inline float pm_amplitude(float v) { return !isfinite(v) || v<0 ? 0 : v>1 ? 1 : v; }
static inline uint16_t pm_motor(float amplitude, float strength) {
    if (!isfinite(strength) || strength<=0) return 0;
    double v=pow(pm_amplitude(amplitude),.7)*strength;
    return (uint16_t)lround(fmin(1.,fmax(0.,v))*65535.);
}
