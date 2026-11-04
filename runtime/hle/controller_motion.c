// SPDX-License-Identifier: MIT
// nn::hid SDK entry points used by Odyssey. ABI layouts are documented by
// Switchbrew/libnx; no Nintendo SDK source is included.
#include <string.h>
#include "../hle.h"
#include "../controller_motion.h"

HLE_FUNC("_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", sensor_handles) {
    uint32_t *out=(uint32_t *)(uintptr_t)c->x[0];
    int count=(int)c->x[1];
    uint32_t id=c->x[2]?*(uint32_t *)(uintptr_t)c->x[2]:0;
    if (!out || count<1 || id!=0) { c->x[0]=0;return; }
    out[0]=0x00020000; // FullKey, player 1, whole-controller device
    c->x[0]=1;
}
HLE_FUNC("_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", sensor_start) { c->x[0]=0; }
HLE_FUNC("_ZN2nn3hid17StopSixAxisSensorERKNS0_19SixAxisSensorHandleE", sensor_stop) { c->x[0]=0; }
HLE_FUNC("_ZN2nn3hid21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE", sensor_state) {
    PMMotionState *out=(PMMotionState *)(uintptr_t)c->x[0];
    if (out && !pm_motion_read(out,1)) { memset(out,0,sizeof *out);out->acceleration[2]=-1;out->direction[0]=out->direction[4]=out->direction[8]=1; }
}
HLE_FUNC("_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE", sensor_states) {
    c->x[0]=(uint64_t)pm_motion_read((PMMotionState *)(uintptr_t)c->x[0],(int)c->x[1]);
}
HLE_FUNC("_ZN2nn3hid21IsSixAxisSensorAtRestERKNS0_19SixAxisSensorHandleE", sensor_rest) { c->x[0]=pm_motion_at_rest(); }
HLE_FUNC("_ZN2nn3hid34ResetSixAxisSensorFusionParametersERKNS0_19SixAxisSensorHandleE", sensor_reset) { pm_motion_reset();c->x[0]=0; }
HLE_FUNC("_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", vibration_value) {
    if (c->x[0] && c->x[1]) pm_vibration_send(*(uint32_t *)(uintptr_t)c->x[0],(const PMVibration *)(uintptr_t)c->x[1]);
}
HLE_FUNC("_ZN2nn3hid19SendVibrationValuesEPKNS0_21VibrationDeviceHandleEPKNS0_14VibrationValueEi", vibration_values) {
    const uint32_t *handles=(const uint32_t *)(uintptr_t)c->x[0];
    const PMVibration *values=(const PMVibration *)(uintptr_t)c->x[1];
    int count=(int)c->x[2];
    if (!handles || !values || count<0 || count>16) return;
    for (int i=0;i<count;i++) pm_vibration_send(handles[i],&values[i]);
}
HLE_FUNC("_ZN2nn3hid23GetActualVibrationValueEPNS0_14VibrationValueERKNS0_21VibrationDeviceHandleE", vibration_actual) {
    if (c->x[0] && c->x[1]) *(PMVibration *)(uintptr_t)c->x[0]=pm_vibration_actual(*(uint32_t *)(uintptr_t)c->x[1]);
}
