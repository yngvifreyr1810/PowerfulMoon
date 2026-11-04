// SPDX-License-Identifier: MIT
#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <string.h>
#include "controller_motion.h"
#include "motion_math.h"
#include "rt.h"
#include "settings.h"
#include "hd_transport.h"

_Static_assert(sizeof(PMMotionState)==0x60,"nn::hid::SixAxisSensorState ABI");
_Static_assert(sizeof(PMVibration)==16,"nn::hid::VibrationValue ABI");
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static SDL_Gamepad *device; // borrowed; accessed only on the SDL owner thread
static PMMotionState history[64];
static unsigned head, used;
static uint64_t last_sample, last_log, last_send;
static int64_t sequence;
static double quaternion[4]={1,0,0,0}, angles[3];
static bool sensors, permitted=true, dirty, at_rest;
static PMVibration vibration[2];
static uint64_t vibration_time[2], motion_reads, vibration_requests;
static float last_accel[3];
static uint16_t previous_low,previous_high;
static bool bridge_device;
static uint64_t test_until;

static void direct_locked(uint64_t now) {
    PMVibration v[2]={{0,160,0,320},{0,160,0,320}};
    for (int i=0;i<2;i++) if (g_settings.rumble && permitted && now-vibration_time[i]<500000000ull) {
        v[i]=vibration[i];
        v[i].low_amplitude*=g_settings.rumble_strength;v[i].high_amplitude*=g_settings.rumble_strength;
    }
    if (now<test_until && g_settings.rumble && permitted) {
        v[0]=(PMVibration){.75f*g_settings.rumble_strength,160,.45f*g_settings.rumble_strength,320};
        v[1]=v[0];
    }
    pm_hd_send(v,now);
}

static void reset_motion_locked(void) {
    head=used=0;last_sample=0;sequence=0;at_rest=true;
    memset(history,0,sizeof history);memset(angles,0,sizeof angles);
    quaternion[0]=1;quaternion[1]=quaternion[2]=quaternion[3]=0;
}
void pm_motion_reset(void) { pthread_mutex_lock(&lock);reset_motion_locked();pthread_mutex_unlock(&lock); }

static void feed(const float accel[3],const float gyro[3],uint64_t now) {
    if (!g_settings.motion) return;
    double a[3],g[3];pm_sensor_axes(accel,gyro,a,g);
    for (int i=0;i<3;i++) if (!isfinite(a[i]) || !isfinite(g[i])) return;
    pthread_mutex_lock(&lock);
    if (last_sample && now<=last_sample) { pthread_mutex_unlock(&lock);return; }
    double dt=last_sample ? (now-last_sample)*1e-9 : .005;
    if (dt>.1) dt=.005;
    last_sample=now;
    double norm=pm_norm3(a),omega[3]={g[0],g[1],g[2]};
    at_rest=fabs(norm-1)<.08 && pm_norm3(g)<.035;
    // Gentle roll/pitch gravity correction only near 1 G. Shakes must remain
    // accelerations, rather than being mistaken for a new gravity direction.
    if (norm>.85 && norm<1.15) {
        float direction[9];pm_direction(quaternion,direction);
        double predicted[3]={-direction[2],-direction[5],-direction[8]};
        double measured[3]={a[0]/norm,a[1]/norm,a[2]/norm};
        omega[0]+=.6*(measured[1]*predicted[2]-measured[2]*predicted[1]);
        omega[1]+=.6*(measured[2]*predicted[0]-measured[0]*predicted[2]);
        omega[2]+=.6*(measured[0]*predicted[1]-measured[1]*predicted[0]);
    }
    pm_quaternion_step(quaternion,omega,dt);
    PMMotionState state={0};state.delta_ns=(int64_t)(dt*1e9);state.sampling=++sequence;
    for (int i=0;i<3;i++) {
        state.acceleration[i]=(float)a[i];
        state.angular_velocity[i]=(float)(g[i]/(2*M_PI));
        angles[i]+=g[i]*dt/(2*M_PI);state.angle[i]=(float)angles[i];
    }
    pm_direction(quaternion,state.direction);state.attributes=1;
    history[head]=state;head=(head+1)%64;if (used<64) used++;
    pthread_mutex_unlock(&lock);
}

void pm_controller_attach(SDL_Gamepad *pad) {
    device=pad;pm_motion_reset();
    const char *serial=pad?SDL_GetGamepadSerial(pad):NULL;
    bridge_device=pad && SDL_GetGamepadVendor(pad)==0x057e && SDL_GetGamepadProduct(pad)==0x2009
        && serial && (!strcmp(serial,"PC2BRIDGE01") || !strcmp(serial,"01-00-32-43-50-02"));
    sensors=pad && SDL_GamepadHasSensor(pad,SDL_SENSOR_GYRO) && SDL_GamepadHasSensor(pad,SDL_SENSOR_ACCEL)
        && SDL_SetGamepadSensorEnabled(pad,SDL_SENSOR_GYRO,true)
        && SDL_SetGamepadSensorEnabled(pad,SDL_SENSOR_ACCEL,true);
    if (sensors) SDL_GetGamepadSensorData(pad,SDL_SENSOR_ACCEL,last_accel,3);
    LOG("[controller] %s: motion %s; SDL vibration bridge enabled\n",pad?SDL_GetGamepadName(pad):"none",sensors?"available":"unavailable");
}
void pm_controller_detach(void) {
    if (device) SDL_RumbleGamepad(device,0,0,0);
    device=NULL;sensors=false;pm_motion_reset();
    pthread_mutex_lock(&lock);memset(vibration,0,sizeof vibration);memset(vibration_time,0,sizeof vibration_time);direct_locked(plat_now_ns());pm_hd_close();dirty=true;pthread_mutex_unlock(&lock);
    previous_low=previous_high=0;
}
void pm_controller_event(const SDL_Event *event) {
    if (!device || !sensors || event->type!=SDL_EVENT_GAMEPAD_SENSOR_UPDATE || event->gsensor.which!=SDL_GetGamepadID(device)) return;
    if (event->gsensor.sensor==SDL_SENSOR_ACCEL) memcpy(last_accel,event->gsensor.data,sizeof last_accel);
    if (event->gsensor.sensor==SDL_SENSOR_GYRO) feed(last_accel,event->gsensor.data,plat_now_ns());
}
int pm_motion_read(PMMotionState *out,int count) {
    if (!out || count<=0 || !g_settings.motion) return 0;
    if (count>17) count=17;
    uint64_t now=plat_now_ns();
    pthread_mutex_lock(&lock);motion_reads++;
    if (!last_sample || now-last_sample>200000000ull) count=0;
    if ((unsigned)count>used) count=(int)used;
    for (int i=0;i<count;i++) out[i]=history[(head+63-i)%64];
    pthread_mutex_unlock(&lock);return count;
}
bool pm_motion_at_rest(void) { pthread_mutex_lock(&lock);bool result=at_rest;pthread_mutex_unlock(&lock);return result; }
bool pm_motion_available(void) { return sensors; }
void pm_vibration_send(uint32_t handle,const PMVibration *value) {
    if (!value) return;
    unsigned side=handle&1;PMVibration v=*value;
    v.low_amplitude=pm_amplitude(v.low_amplitude);v.high_amplitude=pm_amplitude(v.high_amplitude);
    if (!isfinite(v.low_frequency)) v.low_frequency=160;
    if (!isfinite(v.high_frequency)) v.high_frequency=320;
    v.low_frequency=fmaxf(1,fminf(1250,v.low_frequency));v.high_frequency=fmaxf(1,fminf(1250,v.high_frequency));
    pthread_mutex_lock(&lock);uint64_t now=plat_now_ns();vibration[side]=v;vibration_time[side]=now;dirty=true;vibration_requests++;direct_locked(now);pthread_mutex_unlock(&lock);
}
PMVibration pm_vibration_actual(uint32_t handle) {
    pthread_mutex_lock(&lock);PMVibration result=vibration[handle&1];pthread_mutex_unlock(&lock);return result;
}
void pm_vibration_permit(bool permit) { pthread_mutex_lock(&lock);permitted=permit;dirty=true;pthread_mutex_unlock(&lock); }
bool pm_vibration_permitted(void) { pthread_mutex_lock(&lock);bool result=permitted;pthread_mutex_unlock(&lock);return result; }
void pm_vibration_test(void) {
    pthread_mutex_lock(&lock);uint64_t now=plat_now_ns();
    test_until=now+2000000000ull;dirty=true;direct_locked(now);
    pthread_mutex_unlock(&lock);
}
void pm_controller_tick(void) {
    if (!device) return;
    uint64_t now=plat_now_ns();
    // Fallback for SDL backends that do not emit sensor events.
    pthread_mutex_lock(&lock);uint64_t sampled=last_sample;pthread_mutex_unlock(&lock);
    if (sensors && (!sampled || now-sampled>20000000ull)) {
        float a[3],g[3];
        if (SDL_GetGamepadSensorData(device,SDL_SENSOR_ACCEL,a,3) && SDL_GetGamepadSensorData(device,SDL_SENSOR_GYRO,g,3)) feed(a,g,now);
    }
    pthread_mutex_lock(&lock);
    pm_hd_poll(now,bridge_device && g_settings.hd_rumble);
    bool direct=pm_hd_active(now);
    if (direct) direct_locked(now);
    float low=0,high=0;
    for (int i=0;i<2;i++) if (now-vibration_time[i]<500000000ull) {
        low=fmaxf(low,vibration[i].low_amplitude);high=fmaxf(high,vibration[i].high_amplitude);
    }
    if (now<test_until) { low=.75f;high=.45f; }
    uint16_t l=!direct && g_settings.rumble && permitted ? pm_motor(low,g_settings.rumble_strength):0;
    uint16_t h=!direct && g_settings.rumble && permitted ? pm_motor(high,g_settings.rumble_strength):0;
    bool send=dirty || l!=previous_low || h!=previous_high || ((l||h) && now-last_send>100000000ull);
    dirty=false;
    if (now-last_log>10000000000ull && getenv("ODYSSEY_CONTROLLER_DIAGNOSTICS")) {
        LOG("[controller] motion samples=%lld reads=%llu vibration requests=%llu direct-haptics=%s\n",(long long)sequence,(unsigned long long)motion_reads,(unsigned long long)vibration_requests,direct?"active":"fallback");last_log=now;
    }
    pthread_mutex_unlock(&lock);
    if (send) { SDL_RumbleGamepad(device,l,h,200);previous_low=l;previous_high=h;last_send=now; }
}
