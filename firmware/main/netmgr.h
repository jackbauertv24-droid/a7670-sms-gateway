// Network: the board's own Wi-Fi (admin page only, no internet sharing), the
// upstream Wi-Fi list, 4G data as fallback, internet checks and the clock.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_netif.h"

void netmgr_start(void);
// Is there internet through the selected uplink?
bool netmgr_online(void);
// True while the board uses 4G data (Wi-Fi unavailable).
bool netmgr_uplink_is_lte(void);
// "wifi", "4g" or "none".
void netmgr_uplink(char *out, size_t cap);
void netmgr_reconnect(void);
void netmgr_ap_ip(char *out, size_t cap);
void netmgr_status_json(cJSON *root);
void netmgr_summary(char *out, size_t cap);
