/* Experimental WPA2 station. No AP service is exposed in this build. */
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/dhcpv4.h>
#include <net/wifi_ready.h>
#ifdef CONFIG_NRF70_SR_COEX
#include <coex.h>
#endif
#include <string.h>
#include "wifi.h"
#include "https_poc.h"
#include "config.h"

static K_SEM_DEFINE(ready_sem, 0, 1);
static atomic_t got_ip;
static atomic_t failure;
static atomic_t radio_ready;
static struct net_mgmt_event_callback wifi_events, ipv4_events;

static void ready(bool value)
{
    atomic_set(&radio_ready, value);
    if (value) { k_sem_give(&ready_sem); }
}

static void network_event(struct net_mgmt_event_callback *cb, uint64_t event,
                          struct net_if *iface)
{
    if (iface != net_if_get_first_wifi()) { return; }
    if (event == NET_EVENT_IPV4_DHCP_BOUND) {
        atomic_set(&got_ip, 1);
    } else if (event == NET_EVENT_WIFI_CONNECT_RESULT) {
        const struct wifi_status *s = cb->info;
        if (s->status) { atomic_set(&failure, 1); }
    } else if (event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
        atomic_set(&got_ip, 0);
    }
}

int wifi_init(void)
{
    struct net_if *iface = net_if_get_first_wifi();
    if (!iface) { return -ENODEV; }
    /* Zephyr matches layer and layer code exactly: L2 Wi-Fi and L3 IPv4
     * events cannot share a callback mask. */
    net_mgmt_init_event_callback(&wifi_events, network_event,
        NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT);
    net_mgmt_init_event_callback(&ipv4_events, network_event, NET_EVENT_IPV4_DHCP_BOUND);
    net_mgmt_add_event_callback(&wifi_events);
    net_mgmt_add_event_callback(&ipv4_events);
    wifi_ready_callback_t cb = {.wifi_ready_cb = ready};
    return register_wifi_ready_callback(cb, iface);
}

int wifi_station_connect(const char *ssid, const char *password, atomic_t *cancel)
{
    struct net_if *iface = net_if_get_first_wifi();
    if (!iface) { return -ENODEV; }
    atomic_clear(&got_ip);
    atomic_clear(&failure);
    k_sem_reset(&ready_sem);
    int ret = net_if_up(iface);
    if (ret && ret != -EALREADY) { return ret; }
    int64_t end = k_uptime_get() + 30000;
    while (!atomic_get(&radio_ready)) {
        k_sem_take(&ready_sem, K_MSEC(100));
        if (atomic_get(cancel)) { return -ECANCELED; }
        if (k_uptime_get() >= end) { return -ETIMEDOUT; }
    }
    struct wifi_reg_domain domain = {.oper = WIFI_MGMT_SET};
    memcpy(domain.country_code, config_get_wifi_reg_domain(), 2);
    ret = net_mgmt(NET_REQUEST_WIFI_REG_DOMAIN, iface, &domain, sizeof(domain));
    if (ret) { return ret; }
    struct wifi_connect_req_params req = {
        .ssid = (const uint8_t *)ssid, .ssid_length = strlen(ssid),
        .psk = (const uint8_t *)password, .psk_length = strlen(password),
        .security = WIFI_SECURITY_TYPE_PSK, .mfp = WIFI_MFP_OPTIONAL,
        .channel = WIFI_CHANNEL_ANY, .band = WIFI_FREQ_BAND_2_4_GHZ,
    };
    net_dhcpv4_start(iface);
    ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &req, sizeof(req));
    if (ret) { return ret; }
    while (!atomic_get(&got_ip)) {
        if (atomic_get(cancel)) { return -ECANCELED; }
        if (atomic_get(&failure)) { return -ENETUNREACH; }
        if (k_uptime_get() >= end) { return -ETIMEDOUT; }
        k_sleep(K_MSEC(100));
    }
#ifdef CONFIG_NRF70_SR_COEX
    /* Match the existing AP path: configure only after the radio is running. */
    bool separate = IS_ENABLED(CONFIG_COEX_SEP_ANTENNAS);
    bool ble = IS_ENABLED(CONFIG_SR_PROTOCOL_BLE);
    ret = nrf_wifi_coex_config_non_pta(separate, ble);
    if (!ret) {
        ret = nrf_wifi_coex_config_pta(NRF_WIFI_PTA_WLAN_OP_BAND_2_4_GHZ,
                                     separate, ble);
    }
    if (ret) { return ret; }
#endif
    return 0;
}

int wifi_on(void) { return -ENOTSUP; }
int wifi_off(void)
{
    struct net_if *iface = net_if_get_first_wifi();
    if (!iface) { return -ENODEV; }
    net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
    net_dhcpv4_stop(iface);
    atomic_clear(&got_ip);
#ifdef CONFIG_NRF70_SR_COEX
    nrf_wifi_coex_hw_reset();
#endif
    int ret = net_if_down(iface);
    return ret == -EALREADY ? 0 : ret;
}
bool wifi_is_interface_up(void)
{
    struct net_if *iface = net_if_get_first_wifi();
    return iface && net_if_is_admin_up(iface);
}
bool wifi_ap_is_running(void) { return false; }
bool wifi_is_sta_connected(void) { return atomic_get(&got_ip); }
const char *wifi_get_ssid(void) { return "station"; }
const char *wifi_get_password(void) { return ""; }
const char *wifi_get_ip_address(void) { return ""; }
