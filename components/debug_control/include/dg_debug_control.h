#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void dg_debug_quiet_set(bool enabled);
bool dg_debug_quiet_enabled(void);

#ifdef __cplusplus
}
#endif
