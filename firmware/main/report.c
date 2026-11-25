#include "report.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "battery.h"
#include "build_info.h"
#include "devlog.h"
#include "esp_chip_info.h"
#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_ota_ops.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lte.h"
#include "modem_at.h"
#include "netmgr.h"
#include "ota.h"
#include "redact.h"
#include "settings.h"
#include "smsstore.h"

static const char *reset_reason(void) {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "crash (panic)";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout (power dip)";
    case ESP_RST_EXT: return "reset pin";
    default: return "other";
  }
}

cJSON *status_json(void) {
  cJSON *root = cJSON_CreateObject();
  cJSON *b = cJSON_AddObjectToObject(root, "board");
  cJSON_AddStringToObject(b, "fw", FW_VERSION);
  cJSON_AddNumberToObject(b, "build", FW_BUILD);
  cJSON_AddStringToObject(b, "commit", FW_COMMIT);
  cJSON_AddStringToObject(b, "built", FW_BUILT);
  cJSON_AddNumberToObject(b, "uptime_s", (double)(esp_timer_get_time() / 1000000));
  cJSON_AddStringToObject(b, "reset_reason", reset_reason());
  cJSON_AddNumberToObject(b, "internal_kb", (double)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
  cJSON_AddNumberToObject(b, "internal_min_kb", (double)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
  cJSON_AddNumberToObject(b, "psram_kb", (double)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
  size_t total = 0, used = 0;
  esp_spiffs_info("storage", &total, &used);
  cJSON_AddNumberToObject(b, "storage_used_kb", (double)(used / 1024));
  cJSON_AddNumberToObject(b, "storage_total_kb", (double)(total / 1024));
  cJSON_AddBoolToObject(b, "setup_done", cfg_bool("setup_done"));
  cJSON_AddNumberToObject(b, "sms_stored", smsstore_count());
  cJSON_AddNumberToObject(b, "sms_last_id", smsstore_last_id());
  uint32_t seen = smsstore_seen(), last = smsstore_last_id();
  cJSON_AddNumberToObject(b, "sms_unread", last > seen ? last - seen : 0);
  cJSON_AddNumberToObject(b, "time", (double)time(NULL));
  netmgr_status_json(root);
  lte_status_json(root);
  modem_status_json(root);
  ota_status_json(root);
  battery_status_json(root);
  cJSON_AddItemToObject(root, "events", events_json());
  return root;
}

typedef struct {
  char *b;
  size_t len, cap;
} sb_t;

static void put(sb_t *s, const char *t) {
  size_t n = strlen(t);
  if (s->len + n + 1 > s->cap) {
    s->cap = (s->len + n + 1) * 2;
    s->b = heap_caps_realloc(s->b, s->cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  memcpy(s->b + s->len, t, n + 1);
  s->len += n;
}

// Reports may leave the board: mask numbers, MAC addresses and Wi-Fi names.
static void redact_private(char *b) {
  redact_long_numbers(b);
  redact_macs(b);
  static const char *const keys[] = {"w1_ssid", "w2_ssid", "w3_ssid", "ap_ssid"};
  char name[40];
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
    cfg_get(keys[i], name, sizeof(name));
    redact_word(b, name);
  }
}

void report_memory(char *line, size_t cap) {
  snprintf(line, cap, "internal RAM: free %u KB, largest block %u KB, lowest since start %u KB; PSRAM free %u KB\n",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
           (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

char *report_build(void) {
  sb_t s = {0};
  char line[1024];
  time_t now = time(NULL);
  struct tm tm;
  localtime_r(&now, &tm);
  char when[32];
  strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
  snprintf(line, sizeof(line), "=== a7670-sms-gateway report ===\nfirmware: %s (build %d, commit %s, built %s)\ntime: %s\n",
           FW_VERSION, FW_BUILD, FW_COMMIT, FW_BUILT, when);
  put(&s, line);
  esp_chip_info_t ci;
  esp_chip_info(&ci);
  const esp_partition_t *run = esp_ota_get_running_partition();
  snprintf(line, sizeof(line), "chip: esp32s3 rev %d; running from %s; reset: %s; uptime %lld s\n", ci.revision,
           run ? run->label : "?", reset_reason(), esp_timer_get_time() / 1000000);
  put(&s, line);
  report_memory(line, sizeof(line));
  put(&s, line);
  extern void alloc_failures(char *out, size_t cap);
  alloc_failures(line, sizeof(line));
  put(&s, line);
  put(&s, "\n");
  UBaseType_t nt = uxTaskGetNumberOfTasks();
  TaskStatus_t *tl = heap_caps_malloc(sizeof(TaskStatus_t) * (nt + 4), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (tl) {
    nt = uxTaskGetSystemState(tl, nt + 4, NULL);
    put(&s, "tasks (unused stack bytes):");
    for (UBaseType_t i = 0; i < nt; i++) {
      snprintf(line, sizeof(line), " %s %u%s;", tl[i].pcTaskName, (unsigned)tl[i].usStackHighWaterMark,
               esp_ptr_external_ram(tl[i].pxStackBase) ? " psram" : "");
      put(&s, line);
    }
    put(&s, "\n");
    free(tl);
  }
  esp_core_dump_summary_t *cd = malloc(sizeof(esp_core_dump_summary_t));
  if (cd && esp_core_dump_get_summary(cd) == ESP_OK) {
    snprintf(line, sizeof(line), "LAST CRASH: task %s, pc 0x%08lx, backtrace:", cd->exc_task, (unsigned long)cd->exc_pc);
    put(&s, line);
    for (int i = 0; i < (int)cd->exc_bt_info.depth && i < 16; i++) {
      snprintf(line, sizeof(line), " 0x%08lx", (unsigned long)cd->exc_bt_info.bt[i]);
      put(&s, line);
    }
    put(&s, "\n");
  }
  free(cd);
  put(&s, "\n--- summaries ---\n");
  char *sum = malloc(2048);
  netmgr_summary(sum, 2048);
  put(&s, sum);
  modem_summary(sum, 2048);
  put(&s, sum);
  ota_summary(sum, 2048);
  put(&s, sum);
  free(sum);
  put(&s, "\n--- status ---\n");
  cJSON *st = status_json();
  char *j = cJSON_Print(st);
  cJSON_Delete(st);
  if (j) put(&s, j);
  free(j);
  put(&s, "\n\n--- settings (secrets hidden) ---\n");
  cJSON *v = cfg_values_json(false);
  j = cJSON_Print(v);
  cJSON_Delete(v);
  if (j) put(&s, j);
  free(j);
  put(&s, "\n\n--- device log ---\n");
  size_t ll;
  char *log = devlog_dump(&ll);
  if (log) put(&s, log);
  free(log);
  if (s.b) redact_private(s.b);
  return s.b;
}
