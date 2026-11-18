#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <netinet/in.h>

#include "observation.h"

typedef struct {
    char username[64];
    char password[128];
} router_credentials_t;

bool router_configured(void);
bool router_valid_credentials(const router_credentials_t *credentials);
round_result_t router_probe_cycle(const router_credentials_t *credentials, const char *mac);

bool router_restore_host(const router_credentials_t *credentials, const char *mac);
bool router_restore_routes(const router_credentials_t *credentials);
bool router_dns_healthy(void);

int router_active_exit(const router_credentials_t *credentials);
bool router_failover(const router_credentials_t *credentials, int active, bool exercise);
