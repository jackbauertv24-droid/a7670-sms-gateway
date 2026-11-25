// Firmware updates from the public repository's GitHub Releases. Only images
// signed with the project's key are accepted (checked by ESP-IDF when the image
// is finished); a version that fails to start is rolled back by the bootloader
// and then skipped.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

void ota_start(void);
// Asks for a check now; with install=true also installs a newer version.
void ota_request(bool install);
void ota_status_json(cJSON *root);
void ota_summary(char *out, size_t cap);
