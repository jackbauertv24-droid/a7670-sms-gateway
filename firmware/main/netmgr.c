#include "netmgr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <net/if.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>

#include "devlog.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lte.h"
#include "mdns.h"
#include "nvs.h"
#include "psram_task.h"
#include "settings.h"
#include "textutil.h"

static esp_netif_t *ap_if, *sta_if;
static volatile bool sta_connected, sta_has_ip, reconnect_now, recheck_now;
static char sta_ssid[33], sta_ip[16], ap_ip[16];
static int sta_slot = -1, last_fail_reason;
static unsigned sta_fail_streak;
static volatile bool wifi_online, lte_online;
static const char *volatile uplink = "none";
static bool sntp_started;

// ---------- board Wi-Fi (admin page) ----------

static void apply_ap_ip(const char *ip) {
  esp_netif_ip_info_t info = {0};
  esp_netif_str_to_ip4(ip, &info.ip);
  info.gw = info.ip;
  esp_netif_str_to_ip4("255.255.255.0", &info.netmask);
  esp_netif_dhcps_stop(ap_if);
  esp_netif_set_ip_info(ap_if, &info);
  // Devices on the board's Wi-Fi only need to reach this page: no gateway or DNS is offered.
  uint8_t no_router = 0;
  esp_netif_dhcps_option(ap_if, ESP_NETIF_OP_SET, ESP_NETIF_ROUTER_SOLICITATION_ADDRESS, &no_router, sizeof(no_router));
  esp_netif_dhcps_start(ap_if);
  snprintf(ap_ip, sizeof(ap_ip), "%s", ip);
}

static bool overlaps(uint32_t a, uint32_t up, uint32_t mask) { return ((a ^ up) & (0x00FFFFFF & mask)) == 0; }

// ---------- upstream Wi-Fi ----------

static bool slot_ssid(int slot, char *ssid, size_t cap, char *pass, size_t pcap) {
  char k1[12], k2[12];
  snprintf(k1, sizeof(k1), "w%d_ssid", slot + 1);
  snprintf(k2, sizeof(k2), "w%d_pass", slot + 1);
  cfg_get(k1, ssid, cap);
  cfg_get(k2, pass, pcap);
  return ssid[0] != 0;
}

static void connect_next(void) {
  char ssid[33], pass[64];
  for (int i = 1; i <= 3; i++) {
    int slot = (sta_slot + i) % 3;
    if (!slot_ssid(slot, ssid, sizeof(ssid), pass, sizeof(pass))) continue;
    wifi_config_t wc = {0};
    snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", ssid);
    snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", pass);
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta_slot = slot;
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    devlog("wifi: connecting to upstream slot %d", slot + 1);
    esp_wifi_connect();
    return;
  }
  sta_slot = -1;
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    wifi_event_sta_disconnected_t *d = data;
    if (sta_connected) event_post("Upstream Wi-Fi disconnected (reason %d)", d->reason);
    else devlog("wifi: slot %d failed (reason %d)", sta_slot + 1, d->reason);
    last_fail_reason = d->reason;
    sta_connected = sta_has_ip = false;
    wifi_online = false;
    sta_ip[0] = 0;
    sta_fail_streak++;
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
    wifi_event_sta_connected_t *c = data;
    snprintf(sta_ssid, sizeof(sta_ssid), "%.*s", c->ssid_len, (const char *)c->ssid);
    sta_connected = true;
    sta_fail_streak = 0;
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *e = data;
    snprintf(sta_ip, sizeof(sta_ip), IPSTR, IP2STR(&e->ip_info.ip));
    sta_has_ip = true;
    event_post("Joined upstream Wi-Fi as %s", sta_ip);
    esp_ip4_addr_t a;
    esp_netif_str_to_ip4(ap_ip, &a);
    if (overlaps(a.addr, e->ip_info.ip.addr, e->ip_info.netmask.addr)) {
      static const char *const ALT[] = {"10.77.77.1", "172.31.77.1", "192.168.234.1", "10.43.0.1"};
      for (size_t i = 0; i < sizeof(ALT) / sizeof(ALT[0]); i++) {
        esp_ip4_addr_t c;
        esp_netif_str_to_ip4(ALT[i], &c);
        if (strcmp(ALT[i], ap_ip) && !overlaps(c.addr, e->ip_info.ip.addr, e->ip_info.netmask.addr)) {
          event_post("Upstream Wi-Fi uses %s too; admin page moved to http://%s/ (also http://smsgateway.local/)", ap_ip,
                     ALT[i]);
          apply_ap_ip(ALT[i]);
          esp_wifi_deauth_sta(0);  // devices rejoin and get an address in the new range
          break;
        }
      }
    }
    recheck_now = true;
  }
}

// ---------- internet check (also sets the clock) ----------

typedef struct {
  char date[40];
} probe_ctx_t;

static esp_err_t on_probe_event(esp_http_client_event_t *e) {
  probe_ctx_t *c = e->user_data;
  if (e->event_id == HTTP_EVENT_ON_HEADER && !strcasecmp(e->header_key, "Date"))
    snprintf(c->date, sizeof(c->date), "%s", e->header_value);
  return ESP_OK;
}

static bool probe(esp_netif_t *nif) {
  if (!nif) return false;
  char url[220];
  cfg_get("probe_url", url, sizeof(url));
  struct ifreq ifr = {0};
  probe_ctx_t ctx = {0};
  esp_http_client_config_t c = {.url = url, .timeout_ms = 8000, .event_handler = on_probe_event, .user_data = &ctx,
                                .disable_auto_redirect = true};
  if (esp_netif_get_netif_impl_name(nif, ifr.ifr_name) == ESP_OK) c.if_name = &ifr;
  esp_http_client_handle_t h = esp_http_client_init(&c);
  if (!h) return false;
  esp_err_t e = esp_http_client_perform(h);
  int status = e == ESP_OK ? esp_http_client_get_status_code(h) : -1;
  int64_t len = esp_http_client_get_content_length(h);
  esp_http_client_cleanup(h);
  // Networks that block NTP still pass web traffic: take the time from Date.
  if (time(NULL) < 1600000000 && ctx.date[0]) {
    long long t = http_date_parse(ctx.date);
    if (t > 1600000000) {
      struct timeval tv = {.tv_sec = (time_t)t};
      settimeofday(&tv, NULL);
      devlog("clock: set from the internet check's Date header");
    }
  }
  return status == 204 || (status == 200 && len == 0);
}

static void select_uplink(void) {
  esp_netif_t *ppp = lte_netif();
  const char *name = "none";
  esp_netif_t *sel = NULL;
  if (sta_has_ip && wifi_online) sel = sta_if, name = "wifi";
  else if (lte_up() && ppp && lte_online) sel = ppp, name = "4g";
  else if (sta_has_ip) sel = sta_if, name = "wifi";
  else if (lte_up() && ppp) sel = ppp, name = "4g";
  if (strcmp(name, uplink)) {
    event_post("Internet now via %s (was %s)", name, uplink);
    uplink = name;
    if (sel) esp_netif_set_default_netif(sel);
  }
  if (!sntp_started && sel && netmgr_online()) {
    char ntp[64];
    cfg_get("ntp", ntp, sizeof(ntp));
    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG(ntp);
    esp_netif_sntp_init(&sc);
    sntp_started = true;
  }
}

static void net_task(void *arg) {
  int64_t next_connect = 0, next_wifi_probe = 0, next_lte_probe = 0;
  for (;;) {
    int64_t now = esp_timer_get_time();
    int period = cfg_int("probe_s");
    if (reconnect_now) {
      reconnect_now = false;
      sta_slot = -1;
      esp_wifi_disconnect();
      next_connect = 0;
    }
    if (!sta_connected && now >= next_connect) {
      connect_next();
      next_connect = now + (sta_fail_streak >= 3 ? 60LL : 15LL) * 1000000;
    }
    if (sta_has_ip && (recheck_now || now >= next_wifi_probe)) {
      bool was = wifi_online;
      wifi_online = probe(sta_if);
      if (was != wifi_online) event_post("Upstream Wi-Fi: %s", wifi_online ? "internet OK" : "no internet");
      next_wifi_probe = now + (int64_t)period * 1000000;
    }
    // 4G is checked less often: it is the fallback and every check costs data.
    if (lte_up() && (recheck_now || now >= next_lte_probe)) {
      lte_online = probe(lte_netif());
      next_lte_probe = now + (int64_t)period * 4 * 1000000;
    }
    if (!lte_up()) lte_online = false;
    recheck_now = false;
    select_uplink();
    for (int i = 0; i < 20 && !recheck_now && !reconnect_now; i++) vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void netmgr_start(void) {
  char tz[48];
  cfg_get("tz", tz, sizeof(tz));
  setenv("TZ", tz, 1);
  tzset();
  ap_if = esp_netif_create_default_wifi_ap();
  sta_if = esp_netif_create_default_wifi_sta();
  wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&ic));
  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_APSTA);
  wifi_config_t ac = {0};
  char ssid[33], pass[64];
  cfg_get("ap_ssid", ssid, sizeof(ssid));
  cfg_get("ap_pass", pass, sizeof(pass));
  snprintf((char *)ac.ap.ssid, sizeof(ac.ap.ssid), "%s", ssid);
  ac.ap.ssid_len = (uint8_t)strlen(ssid);
  snprintf((char *)ac.ap.password, sizeof(ac.ap.password), "%s", pass);
  ac.ap.channel = (uint8_t)cfg_int("ap_chan");
  ac.ap.max_connection = 4;
  ac.ap.ssid_hidden = cfg_bool("ap_hidden") ? 1 : 0;
  ac.ap.authmode = WIFI_AUTH_WPA2_PSK;
  esp_wifi_set_config(WIFI_IF_AP, &ac);
  ESP_ERROR_CHECK(esp_wifi_start());
  esp_wifi_set_ps(WIFI_PS_NONE);  // a gateway must not doze on its uplink
  char ip[16];
  cfg_get("ap_ip", ip, sizeof(ip));
  apply_ap_ip(ip);
  if (mdns_init() == ESP_OK) {
    mdns_hostname_set("smsgateway");
    mdns_instance_name_set("SMS gateway admin");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
  }
  devlog("board wifi: channel %d, admin at http://%s/", ac.ap.channel, ap_ip);
  // No flash access in this task (Wi-Fi config is kept in RAM), so its stack can be in PSRAM.
  task_psram(net_task, "netmgr", 8192, NULL, 4, 1);
}

bool netmgr_online(void) {
  if (!strcmp(uplink, "wifi")) return wifi_online;
  if (!strcmp(uplink, "4g")) return lte_online;
  return false;
}
bool netmgr_uplink_is_lte(void) { return !strcmp(uplink, "4g"); }
void netmgr_uplink(char *out, size_t cap) { snprintf(out, cap, "%s", uplink); }
void netmgr_reconnect(void) { reconnect_now = true; }
void netmgr_ap_ip(char *out, size_t cap) { snprintf(out, cap, "%s", ap_ip); }

void netmgr_status_json(cJSON *root) {
  cJSON *o = cJSON_AddObjectToObject(root, "net");
  cJSON_AddStringToObject(o, "uplink", uplink);
  cJSON_AddBoolToObject(o, "online", netmgr_online());
  cJSON_AddBoolToObject(o, "wifi_connected", sta_has_ip);
  cJSON_AddStringToObject(o, "wifi_ssid", sta_connected ? sta_ssid : "");
  cJSON_AddStringToObject(o, "wifi_ip", sta_ip);
  cJSON_AddBoolToObject(o, "wifi_online", wifi_online);
  wifi_ap_record_t ai;
  cJSON_AddNumberToObject(o, "wifi_rssi", sta_connected && esp_wifi_sta_get_ap_info(&ai) == ESP_OK ? ai.rssi : 0);
  cJSON_AddNumberToObject(o, "wifi_fail_reason", last_fail_reason);
  cJSON_AddBoolToObject(o, "lte_online", lte_online);
  cJSON_AddStringToObject(o, "ap_ip", ap_ip);
  char ssid[33];
  cfg_get("ap_ssid", ssid, sizeof(ssid));
  cJSON_AddStringToObject(o, "ap_ssid", ssid);
  cJSON_AddBoolToObject(o, "clock_ok", time(NULL) > 1600000000);
}

void netmgr_summary(char *out, size_t cap) {
  snprintf(out, cap, "uplink: %s, online %d; wifi: ip %d online %d (last failure %d); 4g: up %d online %d; admin %s\n", uplink,
           netmgr_online(), sta_has_ip, wifi_online, last_fail_reason, lte_up(), lte_online, ap_ip);
}
