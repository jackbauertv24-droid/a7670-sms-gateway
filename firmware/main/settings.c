#include "settings.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "devlog.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

typedef enum { T_STR, T_SECRET, T_INT, T_BOOL, T_ENUM } ctype_t;

typedef struct {
  const char *key;    // NVS key, max 15 chars
  const char *group;  // admin page section; "internal" is hidden
  const char *label;
  const char *help;
  ctype_t type;
  const char *def;
  int min, max;         // INT: value range; STR/SECRET: length range
  const char *options;  // ENUM: "a|b|c"; STR: format "ip", "mac", "digits", "url" or NULL
  bool reboot;          // takes effect after a restart
} def_t;


static const def_t DEFS[] = {
    // Admin page and the board's own Wi-Fi (for the admin page only; it shares no internet)
    {"adm_pass", "Admin", "Admin password", "User name is always admin. 8+ characters.", T_SECRET, "smsgateway", 8, 64, NULL, false},
    {"ap_ssid", "Admin", "Board Wi-Fi name", "The board's own Wi-Fi, used to reach this page.", T_STR, "", 1, 32, NULL, true},
    {"ap_pass", "Admin", "Board Wi-Fi password", "WPA2, 8 to 63 characters.", T_SECRET, "smsgateway", 8, 63, NULL, true},
    {"ap_hidden", "Admin", "Hide the board's Wi-Fi name", NULL, T_BOOL, "0", 0, 0, NULL, true},
    {"ap_chan", "Admin", "Channel", "Used only without an upstream Wi-Fi; otherwise it follows the upstream channel.", T_INT, "6", 1, 13, NULL, true},
    {"ap_ip", "Admin", "Admin address", "http://smsgateway.local/ also works. If an upstream Wi-Fi uses the same range, the board moves to a spare one and says so.", T_STR, "10.77.77.1", 7, 15, "ip", true},
    // Internet
    {"w1_ssid", "Internet", "Wi-Fi 1 name", "Tried first. 2.4 GHz only.", T_STR, "", 0, 32, NULL, false},
    {"w1_pass", "Internet", "Wi-Fi 1 password", "Empty for open networks.", T_SECRET, "", 0, 63, NULL, false},
    {"w2_ssid", "Internet", "Wi-Fi 2 name", NULL, T_STR, "", 0, 32, NULL, false},
    {"w2_pass", "Internet", "Wi-Fi 2 password", NULL, T_SECRET, "", 0, 63, NULL, false},
    {"w3_ssid", "Internet", "Wi-Fi 3 name", NULL, T_STR, "", 0, 32, NULL, false},
    {"w3_pass", "Internet", "Wi-Fi 3 password", NULL, T_SECRET, "", 0, 63, NULL, false},
    {"lte_en", "Internet", "4G data as fallback", "Used only when no Wi-Fi works. Needs the USB DIP switch set to modem <-> ESP32 and a SIM with data.", T_BOOL, "1", 0, 0, NULL, true},
    {"apn", "Internet", "APN", "Empty = the modem's default.", T_STR, "", 0, 63, NULL, true},
    {"apn_user", "Internet", "APN user", NULL, T_STR, "", 0, 63, NULL, true},
    {"apn_pass", "Internet", "APN password", NULL, T_SECRET, "", 0, 63, NULL, true},
    {"probe_url", "Internet", "Internet check URL", "Plain-HTTP URL that answers 204 when there is internet.", T_STR, "http://connectivitycheck.gstatic.com/generate_204", 10, 200, "url", false},
    {"probe_s", "Internet", "Check interval (s)", NULL, T_INT, "60", 15, 3600, NULL, false},
    // SIM and SMS
    {"sim_pin", "SIM and SMS", "SIM PIN", "Only if the SIM asks for one.", T_SECRET, "", 0, 8, "digits", true},
    {"ph_cc", "SIM and SMS", "Country code", "Digits only, e.g. 852. Used to group conversations.", T_STR, "852", 1, 4, "digits", false},
    {"sms_poll_s", "SIM and SMS", "Check SIM for new SMS every (s)", NULL, T_INT, "15", 5, 600, NULL, false},
    {"sms_del", "SIM and SMS", "Remove from SIM after saving on the board", "Messages are only removed after they are safely written to the board's flash.", T_BOOL, "1", 0, 0, NULL, false},
    {"otp_min", "SIM and SMS", "Login code length min", "Codes in this length range are highlighted.", T_INT, "4", 3, 10, NULL, false},
    {"otp_max", "SIM and SMS", "Login code length max", NULL, T_INT, "8", 3, 12, NULL, false},
    {"otp_sender", "SIM and SMS", "Login code SMS sender contains", "Empty = any sender.", T_STR, "", 0, 40, NULL, false},
    {"otp_kw", "SIM and SMS", "Login code SMS text contains", "Empty = any text.", T_STR, "", 0, 40, NULL, false},
    // Firmware updates
    {"ota_mode", "Updates", "Update mode", "dev = check every few seconds/minutes and install at once (default, for development); auto = check hourly, install at once; nightly = install in the night window; notify = only tell; off = never check. Only updates signed by the project's key are accepted; a version that fails to start is rolled back and skipped.", T_ENUM, "dev", 0, 0, "dev|auto|nightly|notify|off", false},
    {"ota_dev_s", "Updates", "dev mode: check every (s) on Wi-Fi", NULL, T_INT, "60", 30, 3600, NULL, false},
    {"ota_4g_min", "Updates", "Check at most every (min) on 4G", "Each check costs about 10 KB (secure connection set-up), so on 4G checks are slower.", T_INT, "60", 10, 1440, NULL, false},
    {"ota_night", "Updates", "nightly: window start (hour)", "Updates install between this hour and two hours later (local time).", T_INT, "3", 0, 23, NULL, false},
    {"ota_url", "Updates", "Update manifest URL", "Release asset of the public repository.", T_STR, "https://github.com/jackbauertv24-droid/a7670-sms-gateway/releases/latest/download/manifest.json", 10, 250, "url", false},
    // Device
    {"tz", "Device", "Time zone (POSIX)", "HKT-8 for Hong Kong.", T_STR, "HKT-8", 1, 40, NULL, true},
    {"ntp", "Device", "Time server", "If blocked, the clock is set from the internet check instead.", T_STR, "pool.ntp.org", 1, 64, NULL, true},
    {"led_bri", "Device", "LED brightness", "0 = off.", T_INT, "24", 0, 255, NULL, false},
    // Internal
    {"setup_done", "internal", "", NULL, T_BOOL, "0", 0, 0, NULL, false},
};
#define N_DEFS (sizeof(DEFS) / sizeof(DEFS[0]))

static char *vals[N_DEFS];
static char *runtime_def[N_DEFS];
static SemaphoreHandle_t mtx;

static int find(const char *key) {
  for (size_t i = 0; i < N_DEFS; i++)
    if (!strcmp(DEFS[i].key, key)) return (int)i;
  return -1;
}

static const char *default_of(int i) { return runtime_def[i] ? runtime_def[i] : DEFS[i].def; }

static void put(int i, const char *v) {
  free(vals[i]);
  vals[i] = strdup(v);
}

void cfg_init(void) {
  mtx = xSemaphoreCreateMutex();
  nvs_handle_t h;
  bool open = nvs_open("cfg", NVS_READONLY, &h) == ESP_OK;
  for (size_t i = 0; i < N_DEFS; i++) {
    char buf[300];
    size_t len = sizeof(buf);
    if (open && nvs_get_str(h, DEFS[i].key, buf, &len) == ESP_OK) put((int)i, buf);
    else put((int)i, DEFS[i].def);
  }
  if (open) nvs_close(h);
}

void cfg_set_runtime_default(const char *key, const char *value) {
  int i = find(key);
  if (i < 0) return;
  free(runtime_def[i]);
  runtime_def[i] = strdup(value);
  if (!vals[i][0] || !strcmp(vals[i], DEFS[i].def)) put(i, value);
}

void cfg_get(const char *key, char *out, size_t cap) {
  int i = find(key);
  if (!cap) return;
  out[0] = 0;
  if (i < 0) {
    devlog("cfg: unknown key %s", key);
    return;
  }
  xSemaphoreTake(mtx, portMAX_DELAY);
  snprintf(out, cap, "%s", vals[i]);
  xSemaphoreGive(mtx);
}

int cfg_int(const char *key) {
  char b[32];
  cfg_get(key, b, sizeof(b));
  return atoi(b);
}

bool cfg_bool(const char *key) {
  char b[8];
  cfg_get(key, b, sizeof(b));
  return b[0] == '1';
}

bool cfg_is(const char *key, const char *value) {
  char b[64];
  cfg_get(key, b, sizeof(b));
  return !strcmp(b, value);
}

static bool valid_ip(const char *s) {
  int a, b, c, d;
  char tail;
  return sscanf(s, "%d.%d.%d.%d%c", &a, &b, &c, &d, &tail) == 4 && a >= 0 && a < 256 && b >= 0 && b < 256 &&
         c >= 0 && c < 256 && d > 0 && d < 255;
}

static bool valid_mac(const char *s) {
  unsigned m[6];
  char tail;
  return sscanf(s, "%x:%x:%x:%x:%x:%x%c", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5], &tail) == 6;
}

// Normalises (booleans to "1"/"0") into `norm` and validates.
static esp_err_t validate(int i, const char *v, char *norm, size_t ncap, char *err, size_t ecap) {
  const def_t *d = &DEFS[i];
  snprintf(norm, ncap, "%s", v);
  switch (d->type) {
    case T_BOOL:
      if (!strcmp(v, "1") || !strcasecmp(v, "true") || !strcasecmp(v, "on")) snprintf(norm, ncap, "1");
      else if (!strcmp(v, "0") || !strcasecmp(v, "false") || !strcasecmp(v, "off") || !v[0]) snprintf(norm, ncap, "0");
      else goto bad;
      return ESP_OK;
    case T_INT: {
      char *end;
      long n = strtol(v, &end, 10);
      if (!v[0] || *end || n < d->min || n > d->max) {
        snprintf(err, ecap, "%s: whole number from %d to %d", d->label, d->min, d->max);
        return ESP_ERR_INVALID_ARG;
      }
      return ESP_OK;
    }
    case T_ENUM: {
      size_t vl = strlen(v);
      for (const char *o = d->options; *o;) {
        size_t ol = strcspn(o, "|");
        if (ol == vl && !strncmp(o, v, vl)) return ESP_OK;
        o += ol + (o[ol] ? 1 : 0);
      }
      snprintf(err, ecap, "%s: one of %s", d->label, d->options);
      return ESP_ERR_INVALID_ARG;
    }
    case T_STR:
    case T_SECRET: {
      int len = (int)strlen(v);
      if (len == 0 && d->min > 0) goto bad;
      if (len && (len < d->min || len > d->max)) {
        snprintf(err, ecap, "%s: %d to %d characters", d->label, d->min, d->max);
        return ESP_ERR_INVALID_ARG;
      }
      if (len && d->options) {
        if (!strcmp(d->options, "ip") && !valid_ip(v)) goto bad;
        if (!strcmp(d->options, "mac") && !valid_mac(v)) goto bad;
        if (!strcmp(d->options, "url") && strncmp(v, "http://", 7) && strncmp(v, "https://", 8)) goto bad;
        if (!strcmp(d->options, "decimal")) {
          char *end;
          double x = strtod(v, &end);
          if (*end || x < 0) goto bad;
        }
        if (!strcmp(d->options, "pin6")) {
          if (len != 6) goto bad;
          for (const char *p = v; *p; p++)
            if (!isdigit((unsigned char)*p)) goto bad;
        }
        if (!strcmp(d->options, "dtmf"))
          for (const char *p = v; *p; p++)
            if (!isdigit((unsigned char)*p) && *p != '*' && *p != '#') goto bad;
        if (!strcmp(d->options, "digits"))
          for (const char *p = v; *p; p++)
            if (!isdigit((unsigned char)*p)) goto bad;
      }
      return ESP_OK;
    }
  }
bad:
  snprintf(err, ecap, "%s: invalid value", d->label);
  return ESP_ERR_INVALID_ARG;
}

esp_err_t cfg_set(const char *key, const char *value, char *err, size_t errcap) {
  int i = find(key);
  if (i < 0) {
    snprintf(err, errcap, "unknown setting %s", key);
    return ESP_ERR_NOT_FOUND;
  }
  if (DEFS[i].type == T_SECRET && !strcmp(value, CFG_SECRET_MASK)) return ESP_OK;
  char norm[300];
  esp_err_t e = validate(i, value, norm, sizeof(norm), err, errcap);
  if (e != ESP_OK) return e;
  xSemaphoreTake(mtx, portMAX_DELAY);
  put(i, norm);
  xSemaphoreGive(mtx);
  return ESP_OK;
}

esp_err_t cfg_commit(void) {
  nvs_handle_t h;
  esp_err_t e = nvs_open("cfg", NVS_READWRITE, &h);
  if (e != ESP_OK) return e;
  xSemaphoreTake(mtx, portMAX_DELAY);
  for (size_t i = 0; i < N_DEFS && e == ESP_OK; i++) e = nvs_set_str(h, DEFS[i].key, vals[i]);
  xSemaphoreGive(mtx);
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  return e;
}

void cfg_reset_all(void) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  for (size_t i = 0; i < N_DEFS; i++) put((int)i, default_of((int)i));
  xSemaphoreGive(mtx);
}

static const char *type_name(ctype_t t) {
  switch (t) {
    case T_SECRET: return "secret";
    case T_INT: return "int";
    case T_BOOL: return "bool";
    case T_ENUM: return "enum";
    default: return "str";
  }
}

cJSON *cfg_schema_json(void) {
  cJSON *arr = cJSON_CreateArray();
  for (size_t i = 0; i < N_DEFS; i++) {
    const def_t *d = &DEFS[i];
    if (!strcmp(d->group, "internal")) continue;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "key", d->key);
    cJSON_AddStringToObject(o, "group", d->group);
    cJSON_AddStringToObject(o, "label", d->label);
    if (d->help) cJSON_AddStringToObject(o, "help", d->help);
    cJSON_AddStringToObject(o, "type", type_name(d->type));
    cJSON_AddStringToObject(o, "def", d->type == T_SECRET ? "" : default_of((int)i));
    cJSON_AddNumberToObject(o, "min", d->min);
    cJSON_AddNumberToObject(o, "max", d->max);
    if (d->options) cJSON_AddStringToObject(o, "options", d->options);
    cJSON_AddBoolToObject(o, "reboot", d->reboot);
    cJSON_AddItemToArray(arr, o);
  }
  return arr;
}

cJSON *cfg_values_json(bool include_secrets) {
  cJSON *o = cJSON_CreateObject();
  xSemaphoreTake(mtx, portMAX_DELAY);
  for (size_t i = 0; i < N_DEFS; i++) {
    const char *v = vals[i];
    if (DEFS[i].type == T_SECRET && !include_secrets) v = v[0] ? CFG_SECRET_MASK : "";
    cJSON_AddStringToObject(o, DEFS[i].key, v);
  }
  xSemaphoreGive(mtx);
  return o;
}

esp_err_t cfg_apply_json(const cJSON *obj, bool *reboot, char *err, size_t errcap) {
  if (reboot) *reboot = false;
  // Validate everything first so a bad value leaves nothing half-applied.
  const cJSON *it;
  cJSON_ArrayForEach(it, obj) {
    int i = find(it->string);
    if (i < 0) continue;  // ignore unknown keys (older/newer backups)
    char norm[300];
    const char *v = cJSON_IsString(it) ? it->valuestring : cJSON_IsTrue(it) ? "1" : cJSON_IsFalse(it) ? "0" : NULL;
    char num[24];
    if (!v && cJSON_IsNumber(it)) {
      snprintf(num, sizeof(num), "%d", it->valueint);
      v = num;
    }
    if (!v) continue;
    if (DEFS[i].type == T_SECRET && !strcmp(v, CFG_SECRET_MASK)) continue;
    esp_err_t e = validate(i, v, norm, sizeof(norm), err, errcap);
    if (e != ESP_OK) return e;
  }
  cJSON_ArrayForEach(it, obj) {
    int i = find(it->string);
    if (i < 0) continue;
    const char *v = cJSON_IsString(it) ? it->valuestring : cJSON_IsTrue(it) ? "1" : cJSON_IsFalse(it) ? "0" : NULL;
    char num[24];
    if (!v && cJSON_IsNumber(it)) {
      snprintf(num, sizeof(num), "%d", it->valueint);
      v = num;
    }
    if (!v || (DEFS[i].type == T_SECRET && !strcmp(v, CFG_SECRET_MASK))) continue;
    char norm[300], e2[8];
    validate(i, v, norm, sizeof(norm), e2, sizeof(e2));
    xSemaphoreTake(mtx, portMAX_DELAY);
    bool changed = strcmp(vals[i], norm) != 0;
    if (changed) put(i, norm);
    xSemaphoreGive(mtx);
    if (changed && DEFS[i].reboot && reboot) *reboot = true;
  }
  return cfg_commit();
}
