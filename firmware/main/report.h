// Status for the admin page and the diagnostics report (masked, so it can be
// shared): numbers, MAC addresses and Wi-Fi names never leave the board.
#pragma once
#include <stddef.h>

#include "cJSON.h"

cJSON *status_json(void);
char *report_build(void);
void report_memory(char *line, size_t cap);
