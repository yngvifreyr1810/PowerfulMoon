// Network connection manager (nn::nifm, Odyssey's online features). The nifm:u service here is a logging stub, so the
// SDK's client never got its service object and nn::nifm::SubmitNetworkRequest crashed on a null pointer. Replaced at
// the API level as a console without a network: a request is decided at once (not on hold) and the network is never
// available, so the game carries on offline.
#include "../hle.h"

#define NIFM_NO_NETWORK MAKE_RESULT(110, 3400)  // (any failure: the game only checks for success)

HLE_FUNC("_ZN2nn4nifm10InitializeEv", nifm_initialize) { c->x[0] = RES_OK; }
HLE_FUNC("_ZN2nn4nifm19SetLocalNetworkModeEb", nifm_set_local_network_mode) { (void)c; }
HLE_FUNC("_ZN2nn4nifm20SubmitNetworkRequestEv", nifm_submit_network_request) { (void)c; }
HLE_FUNC("_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", nifm_submit_network_request_and_wait) { (void)c; }
HLE_FUNC("_ZN2nn4nifm22IsNetworkRequestOnHoldEv", nifm_is_network_request_on_hold) { c->x[0] = 0; }
HLE_FUNC("_ZN2nn4nifm18IsNetworkAvailableEv", nifm_is_network_available) { c->x[0] = 0; }
HLE_FUNC("_ZN2nn4nifm26HandleNetworkRequestResultEv", nifm_handle_network_request_result) { c->x[0] = NIFM_NO_NETWORK; }
HLE_FUNC("_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", nifm_get_current_proxy_setting) {
    c->x[0] = NIFM_NO_NETWORK;
}
HLE_FUNC("_ZN2nn4nifm26GetCurrentPrimaryIpAddressEP7in_addr", nifm_get_current_primary_ip_address) {
    c->x[0] = NIFM_NO_NETWORK;
}
