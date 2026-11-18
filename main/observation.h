#pragma once

#include <stdbool.h>

enum { PROBE_COUNT = 3, FAILURE_THRESHOLD = 3 };

typedef struct {
    bool http_ok;
    bool nat_ok;
} probe_result_t;

typedef struct {
    bool measured;
    probe_result_t probes[PROBE_COUNT];
} exit_result_t;

typedef struct {
    exit_result_t primary, backup;
    bool restored;
} round_result_t;

static inline bool exit_healthy(const exit_result_t *exit)
{
    if (!exit->measured) return false;
    for (int i = 0; i < PROBE_COUNT; i++) {
        if (!exit->probes[i].http_ok || !exit->probes[i].nat_ok) return false;
    }
    return true;
}

static inline bool observe_round(const round_result_t *round, int *active_failures)
{
    if (!round->restored || !round->primary.measured || exit_healthy(&round->primary)) {
        *active_failures = 0;
    } else if (*active_failures < FAILURE_THRESHOLD) {
        ++*active_failures;
    }
    return *active_failures == FAILURE_THRESHOLD && exit_healthy(&round->backup);
}
