#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bio_filter.h"
#include "intervention_policy.h"
#include "ld6002_types.h"
#include "sleep_history.h"
#include "sleep_score.h"

#ifdef __cplusplus
extern "C" {
#endif

void sleep_log_init(void);
void sleep_log_session_start(int64_t now_us);
bool sleep_log_session_is_active(void);
void sleep_log_session_update(const SleepBioState *bio,
                              const sleep_assessment_t *assessment,
                              const sleep_assist_status_t *assist,
                              const intervention_decision_t *decision,
                              int64_t now_us);
bool sleep_log_session_finish(const char *reason,
                              int64_t now_us,
                              char *report,
                              size_t report_size);
void sleep_log_append_summary(const ld6002_snapshot_t *radar,
                              const sleep_features_t *features,
                              const sleep_assessment_t *assessment,
                              const intervention_decision_t *decision);

#ifdef __cplusplus
}
#endif
