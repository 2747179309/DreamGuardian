#pragma once

#include "esp_err.h"
#include "ld6002_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ld6002_task_start(const ld6002_task_config_t *config);
/* Re-apply the UART clock and GPIO matrix mapping from the radar task context. */
esp_err_t ld6002_task_request_recovery(void);
void ld6002_get_snapshot(ld6002_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
