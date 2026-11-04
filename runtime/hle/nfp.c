// amiibo (nn::nfp, Odyssey). The SDK's client talks to the nfp:user service through an interface object that its
// Initialize never got (no such service here): nn::nfp::Initialize crashed on a null pointer. Replaced at the API
// level as a console with no NFC reader: Initialize works, ListDevices finds no device, so the game never asks the
// per-device functions anything; if it does, they say the device is gone.
#include "../hle.h"

#define NFP_DEVICE_NOT_FOUND MAKE_RESULT(115, 64)

HLE_FUNC("_ZN2nn3nfp10InitializeEv", nfp_initialize) { c->x[0] = RES_OK; }
HLE_FUNC("_ZN2nn3nfp8FinalizeEv", nfp_finalize) { c->x[0] = RES_OK; }

// ListDevices(DeviceHandle *out, int *count, int max) -> Result
HLE_FUNC("_ZN2nn3nfp11ListDevicesEPNS0_12DeviceHandleEPii", nfp_list_devices) {
    if (c->x[1]) *(int32_t *)(uintptr_t)c->x[1] = 0;
    c->x[0] = RES_OK;
}

// GetDeviceState(const DeviceHandle &) -> DeviceState (0: Init, nothing detected)
HLE_FUNC("_ZN2nn3nfp14GetDeviceStateERKNS0_12DeviceHandleE", nfp_get_device_state) { c->x[0] = 0; }

HLE_FUNC("_ZN2nn3nfp14StartDetectionERKNS0_12DeviceHandleE", nfp_start_detection) { c->x[0] = NFP_DEVICE_NOT_FOUND; }
HLE_FUNC("_ZN2nn3nfp13StopDetectionERKNS0_12DeviceHandleE", nfp_stop_detection) { c->x[0] = NFP_DEVICE_NOT_FOUND; }
HLE_FUNC("_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeENS0_11MountTargetE", nfp_mount) {
    c->x[0] = NFP_DEVICE_NOT_FOUND;
}
HLE_FUNC("_ZN2nn3nfp7UnmountERKNS0_12DeviceHandleE", nfp_unmount) { c->x[0] = NFP_DEVICE_NOT_FOUND; }
HLE_FUNC("_ZN2nn3nfp10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE", nfp_get_tag_info) {
    c->x[0] = NFP_DEVICE_NOT_FOUND;
}
HLE_FUNC("_ZN2nn3nfp12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE", nfp_get_model_info) {
    c->x[0] = NFP_DEVICE_NOT_FOUND;
}
HLE_FUNC("_ZN2nn3nfp9GetNpadIdEPjRKNS0_12DeviceHandleE", nfp_get_npad_id) { c->x[0] = NFP_DEVICE_NOT_FOUND; }
HLE_FUNC("_ZN2nn3nfp19AttachActivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE", nfp_attach_activate) {
    c->x[0] = NFP_DEVICE_NOT_FOUND;
}
HLE_FUNC("_ZN2nn3nfp21AttachDeactivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE", nfp_attach_deactivate) {
    c->x[0] = NFP_DEVICE_NOT_FOUND;
}
