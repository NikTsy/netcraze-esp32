#pragma once

#include <stdbool.h>

#include "observation.h"

void ota_initialize(void);
void ota_start_server(void);
void ota_report_probe_round(const round_result_t *round);
void ota_observer_checkpoint(bool healthy);
