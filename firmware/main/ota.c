#include "ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#include "build_info.h"
#include "devlog.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "netmgr.h"
#include "nvs.h"
#include "settings.h"

static char state[16] = "idle", result[160] = "", avail_ver[24] = "", url_ota[300] = "";
static volatile int progress = -1;  // percent while downloading
static int32_t avail_build, bad_build;
static int64_t last_check_us;
static time_t last_check_t;
static volatile bool want_check, want_install;
static int32_t notified_build;

static int32_t nvs_i32(const char *k, int32_t def) {
  nvs_handle_t h;
  int32_t v = def;
  if (nvs_open("ota", NVS_READONLY, &h) == ESP_OK) {
    nvs_get_i32(h, k, &v);
    nvs_close(h);
  }
  return v;
}

static void nvs_set(const char *k, int32_t v) {
  nvs_handle_t h;
  if (nvs_open("ota", NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_i32(h, k, v);
    nvs_commit(h);
    nvs_close(h);
  }
}

static void set_result(const char *st, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_result(const char *st, const char *fmt, ...) {
  snprintf(state, sizeof(state), "%s", st);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(result, sizeof(result), fmt, ap);
  va_end(ap);
  devlog("ota: %s - %s", st, result);
}

typedef struct {
  char *buf;
  int len, cap;
} body_t;

static esp_err_t on_body(esp_http_client_event_t *e) {
  body_t *b = e->user_data;
  if (e->event_id == HTTP_EVENT_ON_DATA && b->len + e->data_len < b->cap) {
    memcpy(b->buf + b->len, e->data, e->data_len);
    b->len += e->data_len;
    b->buf[b->len] = 0;
  }
  return ESP_OK;
}

// Reads the manifest of the latest release. Returns false with `result` set.
static bool fetch_manifest(void) {
  char url[260];
  cfg_get("ota_url", url, sizeof(url));
  char *buf = calloc(1, 2048);
  body_t b = {.buf = buf, .cap = 2048};
  esp_http_client_config_t c = {.url = url, .timeout_ms = 15000, .event_handler = on_body, .user_data = &b,
                                .crt_bundle_attach = esp_crt_bundle_attach, .max_redirection_count = 5,
                                .buffer_size = 2048, .buffer_size_tx = 1024};
  esp_http_client_handle_t h = buf ? esp_http_client_init(&c) : NULL;
  esp_err_t e = h ? esp_http_client_perform(h) : ESP_ERR_NO_MEM;
  int status = e == ESP_OK ? esp_http_client_get_status_code(h) : -1;
  if (h) esp_http_client_cleanup(h);
  bool ok = false;
  if (status != 200) {
    set_result("error", "manifest: %s", e != ESP_OK ? esp_err_to_name(e) : status == 404 ? "no release yet (404)" : "HTTP error");
  } else {
    cJSON *j = cJSON_Parse(buf);
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(j, "version"));
    const char *u = cJSON_GetStringValue(cJSON_GetObjectItem(j, "ota"));
    double bn = cJSON_GetNumberValue(cJSON_GetObjectItem(j, "build"));
    if (v && u && bn == bn && bn > 0) {
      snprintf(avail_ver, sizeof(avail_ver), "%s", v);
      snprintf(url_ota, sizeof(url_ota), "%s", u);
      avail_build = (int32_t)bn;
      ok = true;
    } else {
      set_result("error", "manifest is not valid");
    }
    cJSON_Delete(j);
  }
  free(buf);
  return ok;
}

static void install(void) {
  set_result("updating", "downloading %s", avail_ver);
  progress = 0;
  esp_http_client_config_t hc = {.url = url_ota, .timeout_ms = 20000, .crt_bundle_attach = esp_crt_bundle_attach,
                                 .keep_alive_enable = true, .buffer_size = 4096, .buffer_size_tx = 1024,
                                 .max_redirection_count = 5};
  esp_https_ota_config_t oc = {.http_config = &hc};
  esp_https_ota_handle_t h = NULL;
  esp_err_t e = esp_https_ota_begin(&oc, &h);
  if (e != ESP_OK) {
    set_result("error", "download did not start: %s", esp_err_to_name(e));
    progress = -1;
    return;
  }
  int total = esp_https_ota_get_image_size(h);
  while ((e = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
    int got = esp_https_ota_get_image_len_read(h);
    if (total > 0) progress = (int)((int64_t)got * 100 / total);
  }
  if (e != ESP_OK || !esp_https_ota_is_complete_data_received(h)) {
    esp_https_ota_abort(h);
    set_result("error", "download failed: %s", esp_err_to_name(e));
    progress = -1;
    return;
  }
  // Finishing checks the image, including its signature: an image not signed
  // with the project's key is refused here.
  e = esp_https_ota_finish(h);
  progress = -1;
  if (e != ESP_OK) {
    set_result("error", "%s refused: %s", avail_ver,
               e == ESP_ERR_OTA_VALIDATE_FAILED ? "image or signature check failed" : esp_err_to_name(e));
    bad_build = avail_build;  // do not download it again every minute
    nvs_set("bad", bad_build);
    event_post("Update %s refused (%s); it will be skipped", avail_ver, esp_err_to_name(e));
    return;
  }
  nvs_set("try", avail_build);
  set_result("restarting", "%s installed, restarting", avail_ver);
  event_post("Update %s installed (from %s); restarting", avail_ver, FW_VERSION);
  vTaskDelay(pdMS_TO_TICKS(2000));
  esp_restart();
}

static int interval_s(const char *mode) {
  bool lte = netmgr_uplink_is_lte();
  int lte_s = cfg_int("ota_4g_min") * 60;
  int s = !strcmp(mode, "dev") ? cfg_int("ota_dev_s") : !strcmp(mode, "auto") ? 3600 : 6 * 3600;
  return lte && s < lte_s ? lte_s : s;
}

static bool in_night_window(void) {
  time_t now = time(NULL);
  if (now < 1600000000) return false;
  struct tm tm;
  localtime_r(&now, &tm);
  int start = cfg_int("ota_night");
  return (tm.tm_hour - start + 24) % 24 < 2;
}

static void task(void *arg) {
  vTaskDelay(pdMS_TO_TICKS(20000));  // let the network settle after start
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    char mode[12];
    cfg_get("ota_mode", mode, sizeof(mode));
    bool due = last_check_us == 0 || esp_timer_get_time() - last_check_us > (int64_t)interval_s(mode) * 1000000;
    if (!want_check && !want_install && (!strcmp(mode, "off") || !due)) continue;
    if (!netmgr_online()) {
      if (want_check || want_install) set_result("waiting", "no internet yet");
      want_check = false;
      continue;
    }
    bool force_install = want_install;
    want_check = want_install = false;
    last_check_us = esp_timer_get_time();
    last_check_t = time(NULL);
    set_result("checking", "reading the latest release");
    if (!fetch_manifest()) continue;
    if (avail_build <= FW_BUILD) {
      set_result("idle", "up to date (%s, build %d)", FW_VERSION, FW_BUILD);
      continue;
    }
    if (avail_build == bad_build && !force_install) {
      set_result("idle", "%s (build %ld) was refused or failed before; waiting for a newer one", avail_ver, (long)avail_build);
      continue;
    }
    bool go = force_install || !strcmp(mode, "dev") || !strcmp(mode, "auto") || (!strcmp(mode, "nightly") && in_night_window());
    if (!go) {
      set_result("available", "%s is available (build %ld)", avail_ver, (long)avail_build);
      if (notified_build != avail_build) {
        notified_build = avail_build;
        event_post("Firmware %s is available; install it from the admin page (Updates)", avail_ver);
      }
      continue;
    }
    install();
  }
}

void ota_start(void) {
  bad_build = nvs_i32("bad", 0);
  int32_t tried = nvs_i32("try", 0);
  if (tried) {
    if (tried == FW_BUILD) {
      event_post("Now running %s (build %d)", FW_VERSION, FW_BUILD);
    } else {
      // The bootloader went back: the new version did not confirm it was healthy.
      bad_build = tried;
      nvs_set("bad", bad_build);
      event_post("Update to build %ld did not start properly; back on %s. That build will be skipped.", (long)tried, FW_VERSION);
    }
    nvs_set("try", 0);
  }
  // Flash is written from this task, so its stack stays in internal RAM.
  xTaskCreatePinnedToCore(task, "ota", 8192, NULL, 2, NULL, 1);
}

void ota_request(bool install) {
  if (install) want_install = true;
  else want_check = true;
}

void ota_status_json(cJSON *root) {
  cJSON *o = cJSON_AddObjectToObject(root, "ota");
  char mode[12];
  cfg_get("ota_mode", mode, sizeof(mode));
  cJSON_AddStringToObject(o, "mode", mode);
  cJSON_AddStringToObject(o, "version", FW_VERSION);
  cJSON_AddNumberToObject(o, "build", FW_BUILD);
  cJSON_AddStringToObject(o, "state", state);
  cJSON_AddStringToObject(o, "result", result);
  cJSON_AddStringToObject(o, "available", avail_build > FW_BUILD ? avail_ver : "");
  cJSON_AddNumberToObject(o, "available_build", avail_build);
  cJSON_AddNumberToObject(o, "bad_build", bad_build);
  cJSON_AddNumberToObject(o, "progress", progress);
  cJSON_AddNumberToObject(o, "last_check", (double)last_check_t);
  cJSON_AddNumberToObject(o, "next_check_s", last_check_us ? (double)(interval_s(mode) - (esp_timer_get_time() - last_check_us) / 1000000) : 0);
}

void ota_summary(char *out, size_t cap) {
  char mode[12];
  cfg_get("ota_mode", mode, sizeof(mode));
  snprintf(out, cap, "ota: mode %s, running %s build %d, state %s, %s; latest seen %s build %ld; skipped build %ld\n", mode,
           FW_VERSION, FW_BUILD, state, result, avail_ver, (long)avail_build, (long)bad_build);
}
