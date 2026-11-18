#include "router.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/dns.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"
#include "nvs.h"

static const char *TAG = "netcraze";

typedef struct {
    int fd;
    unsigned char input[1024];
    size_t cursor, length;
} console_t;

typedef void (*line_handler_t)(char *line, void *context);

static bool send_all(int fd, const void *data, size_t length)
{
    const char *bytes = data;
    while (length) {
        ssize_t count = send(fd, bytes, length, 0);
        if (count <= 0) return false;
        bytes += count;
        length -= count;
    }
    return true;
}

static int receive_byte(console_t *console, int64_t deadline)
{
    if (console->cursor == console->length) {
        int64_t remaining = deadline - esp_timer_get_time();
        if (remaining <= 0) return -1;
        struct timeval timeout = {.tv_sec = remaining / 1000000, .tv_usec = remaining % 1000000};
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(console->fd, &readable);
        if (select(console->fd + 1, &readable, NULL, NULL, &timeout) != 1) return -1;
        int count = recv(console->fd, console->input, sizeof(console->input), 0);
        if (count <= 0) return -1;
        console->length = count;
        console->cursor = 0;
    }
    return console->input[console->cursor++];
}

static bool receive_until(console_t *console, const char *prompt, int timeout_ms,
                          line_handler_t handler, void *context)
{
    char line[256];
    size_t used = 0;
    size_t matched = 0;
    bool truncated = false;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        int byte = receive_byte(console, deadline);
        if (byte < 0) return false;
        if (byte == 255) {
            int command = receive_byte(console, deadline);
            if (command < 0) return false;
            if (command >= 251 && command <= 254) {
                int option = receive_byte(console, deadline);
                if (option < 0) return false;
                if (command == 251 || command == 253) {
                    unsigned char reply[] = {255, command == 251 ? 254 : 252, option};
                    if (!send_all(console->fd, reply, sizeof(reply))) return false;
                }
            }
            continue;
        }
        if (byte == 27 && receive_byte(console, deadline) == '[') {
            do {
                byte = receive_byte(console, deadline);
                if (byte < 0 || esp_timer_get_time() >= deadline) return false;
            } while (byte < 0x40 || byte > 0x7e);
            continue;
        }
        if (byte == prompt[matched] && (matched || prompt[0] != '(' || used == 0 ||
                                      (used == 1 && line[0] == ' '))) {
            if (prompt[++matched] == '\0') return true;
        } else {
            matched = 0;
        }

        if (byte == '\n') {
            line[used] = '\0';
            if (handler && !truncated) handler(line, context);
            used = 0;
            truncated = false;
        } else if (byte != '\r') {
            if (used < sizeof(line) - 1) line[used++] = byte;
            else truncated = true;
        }
    }
    return false;
}

static bool send_line(console_t *console, const char *line)
{
    return send_all(console->fd, line, strlen(line)) && send_all(console->fd, "\r\n", 2);
}

static bool request(console_t *console, const char *command, line_handler_t handler, void *context)
{
    return send_line(console, command) && receive_until(console, "(config)>", 12000, handler, context);
}

static bool open_console(console_t *console, const router_credentials_t *credentials)
{
    *console = (console_t){.fd = socket(AF_INET, SOCK_STREAM, 0)};
    if (console->fd < 0) return false;
    const struct timeval timeout = {.tv_sec = 2};
    const struct sockaddr_in address = {
        .sin_family = AF_INET, .sin_port = htons(23), .sin_addr.s_addr = inet_addr(CONFIG_NETCRAZE_ROUTER_IPV4),
    };
    if (setsockopt(console->fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
        setsockopt(console->fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0 &&
        connect(console->fd, (const struct sockaddr *)&address, sizeof(address)) == 0 &&
        receive_until(console, "Login:", 20000, NULL, NULL) && send_line(console, credentials->username) &&
        receive_until(console, "Password:", 12000, NULL, NULL) && send_line(console, credentials->password) &&
        receive_until(console, "(config)>", 20000, NULL, NULL)) return true;
    close(console->fd);
    console->fd = -1;
    return false;
}

bool router_valid_credentials(const router_credentials_t *credentials)
{
    console_t console;
    if (!open_console(&console, credentials)) return false;
    close(console.fd);
    return true;
}

static bool router_udp_dns_healthy(const uint8_t *query, size_t query_size)
{
    const struct sockaddr_in resolver = {
        .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = inet_addr(CONFIG_NETCRAZE_ROUTER_IPV4),
    };
    const struct timeval timeout = {.tv_sec = 2};
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return false;
    uint8_t reply[512];
    int received = -1;
    if (!setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) &&
        !connect(fd, (const struct sockaddr *)&resolver, sizeof(resolver)) &&
        send(fd, query, query_size, 0) == (ssize_t)query_size) {
        received = recv(fd, reply, sizeof(reply), 0);
    }
    close(fd);
    return received > (int)query_size && reply[0] == query[0] && reply[1] == query[1] &&
        (reply[2] & 0xfa) == 0x80 && !(reply[3] & 15) && (reply[6] || reply[7]) &&
        !memcmp(reply + 12, query + 12, query_size - 12);
}

bool router_dns_healthy(void)
{
    // Query the local resolver over the same transport used by LAN clients.
    static const uint8_t query[] = {
        0x91, 0x3b, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0,
        7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0, 0, 1, 0, 1,
    };
    return router_udp_dns_healthy(query, sizeof(query));
}

typedef struct {
    const char *label, *policy, *interface;
} exit_t;

static const exit_t exits[] = {
    {"primary", CONFIG_NETCRAZE_PRIMARY_POLICY, CONFIG_NETCRAZE_PRIMARY_INTERFACE},
    {"backup", CONFIG_NETCRAZE_BACKUP_POLICY, CONFIG_NETCRAZE_BACKUP_INTERFACE},
};

static bool valid_id(const char *value)
{
    size_t size = strlen(value);
    return size && size <= 20 &&
        strspn(value, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == size;
}

bool router_configured(void)
{
    struct in_addr address;
    return inet_pton(AF_INET, CONFIG_NETCRAZE_ROUTER_IPV4, &address) == 1 &&
        valid_id(exits[0].interface) && valid_id(exits[1].interface) &&
        valid_id(exits[0].policy) && valid_id(exits[1].policy) &&
        strcmp(exits[0].interface, exits[1].interface) &&
        strcmp(exits[0].policy, exits[1].policy);
}

typedef struct {
    const char *mac;
    int policy;
    int permits[2];
    bool expected_exit[2];
    bool conform;
    bool other_binding;
} binding_t;

static void parse_binding(char *line, void *context)
{
    binding_t *binding = context;
    if (*line && *line != ' ' && *line != '\t') {
        binding->policy = -1;
        for (int index = 0; index < 2; index++) {
            char expected[32];
            snprintf(expected, sizeof(expected), "ip policy %s", exits[index].policy);
            if (strcmp(line, expected) == 0) binding->policy = index;
        }
    }
    line += strspn(line, " \t");
    const char *permit = "permit global ";
    if (binding->policy >= 0 && strncmp(line, permit, strlen(permit)) == 0) {
        int index = binding->policy;
        binding->permits[index]++;
        binding->expected_exit[index] = strcmp(line + strlen(permit), exits[index].interface) == 0;
    }
    const char *host = "host ";
    if (strncmp(line, host, strlen(host)) || strncmp(line + strlen(host), binding->mac, 17) || line[22] != ' ') return;
    const char *rule = line + 23;
    if (strcmp(rule, "conform") == 0) binding->conform = true;
    if (strncmp(rule, "policy ", 7) == 0 && strcmp(rule + 7, exits[0].policy) &&
        strcmp(rule + 7, exits[1].policy)) binding->other_binding = true;
}

static bool read_binding(console_t *console, const char *mac, binding_t *binding)
{
    *binding = (binding_t){.mac = mac, .policy = -1};
    return request(console, "show running-config", parse_binding, binding);
}

static void find_applied(char *line, void *context)
{
    if (strstr(line, "applied to host")) *(bool *)context = true;
}

static bool apply_binding(console_t *console, const char *mac, const char *rule)
{
    char command[96];
    bool applied = false;
    snprintf(command, sizeof(command), "ip hotspot host %s %s", mac, rule);
    return request(console, command, find_applied, &applied) && applied;
}

static void parse_address(char *line, void *context)
{
    line += strspn(line, " \t");
    char candidate[16];
    struct in_addr address;
    if (sscanf(line, "address: %15s", candidate) == 1 && inet_pton(AF_INET, candidate, &address) == 1) {
        strcpy(context, candidate);
    }
}

typedef struct {
    char address[16];
    bool administratively_down;
} probe_interface_t;

static void parse_probe_interface(char *line, void *context)
{
    probe_interface_t *state = context;
    parse_address(line, state->address);
    const char *value = line + strspn(line, " \t");
    if (!strcmp(value, "state: down")) state->administratively_down = true;
}

typedef struct {
    char source[16], destination[16], translated[16];
    unsigned int port;
    bool header, next_is_reply, confirmed;
} nat_match_t;

static void parse_nat(char *line, void *context)
{
    nat_match_t *match = context;
    if (strstr(line, "Type") && strstr(line, "Source")) match->header = true;
    char source[16], destination[16];
    unsigned int source_port, destination_port, packets;
    if (match->next_is_reply &&
        sscanf(line, " %15s %u %15s %u %u", source, &source_port, destination, &destination_port, &packets) == 5 &&
        !strcmp(source, match->destination) && source_port == 443 &&
        !strcmp(destination, match->translated) && packets > 0) match->confirmed = true;
    match->next_is_reply =
        sscanf(line, " TCP %15s %u %15s %u", source, &source_port, destination, &destination_port) == 4 &&
        !strcmp(source, match->source) && source_port == match->port &&
        !strcmp(destination, match->destination) && destination_port == 443;
}

static bool send_http(esp_tls_t *tls, const char *request_text)
{
    size_t remaining = strlen(request_text);
    while (remaining) {
        ssize_t count = esp_tls_conn_write(tls, request_text, remaining);
        if (count <= 0) return false;
        request_text += count;
        remaining -= count;
    }
    return true;
}

static int read_status(esp_tls_t *tls)
{
    char line[128] = "";
    size_t used = 0;
    while (used < sizeof(line) - 1) {
        int count = esp_tls_conn_read(tls, line + used, sizeof(line) - 1 - used);
        if (count <= 0) return 0;
        used += count;
        line[used] = '\0';
        if (strchr(line, '\n')) {
            int status;
            if (sscanf(line, "HTTP/1.%*1[01] %d", &status) == 1) return status;
            return 0;
        }
    }
    return 0;
}

static bool public_ipv4(uint32_t ip)
{
    return (ip & 0xff000000U) != 0x00000000U &&
        (ip & 0xff000000U) != 0x0a000000U &&
        (ip & 0xffc00000U) != 0x64400000U &&
        (ip & 0xff000000U) != 0x7f000000U &&
        (ip & 0xffff0000U) != 0xa9fe0000U &&
        (ip & 0xfff00000U) != 0xac100000U &&
        (ip & 0xffffff00U) != 0xc0000000U &&
        (ip & 0xffff0000U) != 0xc0a80000U &&
        (ip & 0xfffe0000U) != 0xc6120000U && ip < 0xe0000000U;
}

static bool resolve_public_ipv4(const char *host, char address[16])
{
    const struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *answers;
    if (getaddrinfo(host, NULL, &hints, &answers) != 0) return false;

    bool found = false;
    for (const struct addrinfo *answer = answers; answer; answer = answer->ai_next) {
        const struct sockaddr_in *ipv4 = (const struct sockaddr_in *)answer->ai_addr;
        if (public_ipv4(ntohl(ipv4->sin_addr.s_addr)) &&
            inet_ntop(AF_INET, &ipv4->sin_addr, address, 16)) {
            found = true;
            break;
        }
    }
    freeaddrinfo(answers);
    return found;
}

static probe_result_t fetch_site(console_t *console, const char *translated,
                                 const char *host, const char *path, int expected)
{
    probe_result_t result = {0};
    char resolved[16];
    if (!resolve_public_ipv4(host, resolved)) return result;
    esp_tls_cfg_t config = {
        .timeout_ms = 3500, .crt_bundle_attach = esp_crt_bundle_attach, .common_name = host,
    };
    esp_tls_t *tls = esp_tls_init();
    nat_match_t match = {0};
    if (tls && esp_tls_conn_new_sync(resolved, strlen(resolved), 443, &config, tls) == 1) {
        int fd;
        struct sockaddr_in source, destination;
        socklen_t source_len = sizeof(source), destination_len = sizeof(destination);
        if (esp_tls_get_conn_sockfd(tls, &fd) == ESP_OK &&
            getsockname(fd, (struct sockaddr *)&source, &source_len) == 0 &&
            getpeername(fd, (struct sockaddr *)&destination, &destination_len) == 0 &&
            source.sin_family == AF_INET && destination.sin_family == AF_INET) {
            inet_ntop(AF_INET, &source.sin_addr, match.source, sizeof(match.source));
            inet_ntop(AF_INET, &destination.sin_addr, match.destination, sizeof(match.destination));
            strcpy(match.translated, translated);
            match.port = ntohs(source.sin_port);
        }
        char text[512];
        int length = snprintf(text, sizeof(text), "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: keep-alive\r\n\r\n", path, host);
        if (length > 0 && length < (int)sizeof(text) && send_http(tls, text)) {
            result.http_ok = read_status(tls) == expected;
        }
        if (match.port && !request(console, "show ip nat tcp", parse_nat, &match)) match.confirmed = false;
    }
    result.nat_ok = match.header && match.confirmed;
    if (tls) esp_tls_conn_destroy(tls);
    ESP_LOGI(TAG, "%s flow %s:%u -> %s:443", host, match.source, match.port, match.destination);
    return result;
}

static probe_result_t probe(console_t *console, const exit_t *exit, const char *translated,
                            const char *name, const char *host, const char *path, int expected)
{
    probe_result_t result = fetch_site(console, translated, host, path, expected);
    ESP_LOG_LEVEL(result.http_ok && result.nat_ok ? ESP_LOG_INFO : ESP_LOG_WARN, TAG,
                  "%s %s: expected HTTP %d %s, NAT %s via %s",
                  exit->label, name, expected, result.http_ok ? "matched" : "unconfirmed",
                  result.nat_ok ? "confirmed" : "unconfirmed", exit->interface);
    return result;
}

static bool read_pending(nvs_handle_t nvs, uint8_t *pending)
{
    esp_err_t result = nvs_get_u8(nvs, "restore_host", pending);
    return result == ESP_OK || result == ESP_ERR_NVS_NOT_FOUND;
}

static bool write_pending(nvs_handle_t nvs, uint8_t value)
{
    return nvs_set_u8(nvs, "restore_host", value) == ESP_OK && nvs_commit(nvs) == ESP_OK;
}

static bool restore(console_t *console, nvs_handle_t nvs, const char *mac)
{
    binding_t binding;
    return read_binding(console, mac, &binding) && !binding.other_binding &&
        (binding.conform || (apply_binding(console, mac, "conform") &&
                            read_binding(console, mac, &binding) && binding.conform)) &&
        write_pending(nvs, 0);
}

bool router_restore_host(const router_credentials_t *credentials, const char *mac)
{
    nvs_handle_t nvs;
    if (nvs_open("observer", NVS_READWRITE, &nvs) != ESP_OK) return false;
    uint8_t pending = 0;
    bool restored = read_pending(nvs, &pending);
    if (restored && pending) {
        restored = false;
        for (int retry = 0; retry < 2 && !restored; retry++) {
            console_t console;
            if (open_console(&console, credentials)) {
                restored = restore(&console, nvs, mac);
                close(console.fd);
            } else {
                ESP_LOGW(TAG, "ESP32 recovery: router console login timed out");
            }
            if (!restored && retry == 0) vTaskDelay(pdMS_TO_TICKS(1000));
        }
        ESP_LOG_LEVEL(restored ? ESP_LOG_INFO : ESP_LOG_ERROR, TAG, "%s",
                      restored ? "Recovered ESP32 policy before probes" : "ESP32 policy recovery pending");
    }
    nvs_close(nvs);
    return restored;
}

round_result_t router_probe_cycle(const router_credentials_t *credentials, const char *mac)
{
    round_result_t result = {0};
    exit_result_t *outcomes[] = {&result.primary, &result.backup};
    console_t console;
    if (!open_console(&console, credentials)) {
        ESP_LOGW(TAG, "Router console unavailable");
        return result;
    }
    nvs_handle_t nvs;
    if (nvs_open("observer", NVS_READWRITE, &nvs) != ESP_OK) {
        close(console.fd);
        return result;
    }
    uint8_t pending = 0;
    binding_t binding;
    bool safe = read_pending(nvs, &pending) && (!pending || restore(&console, nvs, mac)) &&
        read_binding(&console, mac, &binding) && binding.conform && !binding.other_binding;
    if (!safe || !write_pending(nvs, 1)) {
        ESP_LOGW(TAG, "Cannot verify original ESP32 policy or persist recovery state");
    } else {
        for (int index = 0; index < 2; index++) {
            const exit_t *exit = &exits[index];
            if (binding.permits[index] != 1 || !binding.expected_exit[index]) {
                ESP_LOGW(TAG, "%s skipped: %s does not exclusively use %s", exit->label, exit->policy, exit->interface);
                continue;
            }
            char command[64];
            probe_interface_t state = {0};
            snprintf(command, sizeof(command), "show interface %s", exit->interface);
            if (!request(&console, command, parse_probe_interface, &state)) continue;
            if (state.administratively_down) {
                outcomes[index]->measured = true;
                ESP_LOGW(TAG, "%s tunnel is administratively down", exit->label);
                continue;
            }
            const char *address = state.address;
            if (!address[0]) {
                ESP_LOGW(TAG, "%s tunnel address unavailable", exit->label);
                continue;
            }
            snprintf(command, sizeof(command), "policy %s", exit->policy);
            if (!apply_binding(&console, mac, command)) {
                ESP_LOGW(TAG, "%s ESP32 binding failed", exit->label);
                continue;
            }
            exit_result_t *outcome = outcomes[index];
            outcome->measured = true;
            vTaskDelay(pdMS_TO_TICKS(1000));
            outcome->probes[0] = probe(&console, exit, address, "YouTube", "www.youtube.com", "/generate_204", 204);
            if (!outcome->probes[0].http_ok || !outcome->probes[0].nat_ok) {
                ESP_LOGI(TAG, "%s: repeating YouTube check after policy change", exit->label);
                outcome->probes[0] = probe(&console, exit, address, "YouTube", "www.youtube.com", "/generate_204", 204);
            }
            outcome->probes[1] = probe(&console, exit, address, "ChatGPT", "chatgpt.com", "/", 200);
            outcome->probes[2] = probe(&console, exit, address, "Codex", "chatgpt.com", "/codex/", 200);
        }
        if (restore(&console, nvs, mac)) {
            result.restored = true;
            ESP_LOGI(TAG, "Original ESP32 policy restored");
        } else {
            close(console.fd);
            console.fd = -1;
            if (open_console(&console, credentials) && restore(&console, nvs, mac)) {
                result.restored = true;
                ESP_LOGI(TAG, "Original ESP32 policy restored after reconnect");
            } else {
                ESP_LOGE(TAG, "ESP32 policy recovery pending");
            }
        }
    }
    nvs_close(nvs);
    if (console.fd >= 0) close(console.fd);
    return result;
}

typedef struct {
    const char *network;
    const char *mask;
    const char *prefix;
} managed_prefix_t;

#if __has_include("site_config.h")
#include "site_config.h"
#endif
#ifndef NETCRAZE_MANAGED_PREFIXES
#define NETCRAZE_MANAGED_PREFIXES
#endif
#ifndef NETCRAZE_MANAGED_GROUPS
#define NETCRAZE_MANAGED_GROUPS
#endif
#ifndef NETCRAZE_MANAGED_RESOLVERS
#define NETCRAZE_MANAGED_RESOLVERS
#endif

typedef struct {
    const char *resolver;
    const char *host;
} managed_resolver_t;

static const managed_prefix_t managed_prefixes[] = { NETCRAZE_MANAGED_PREFIXES {0} };
static const char *const managed_groups[] = { NETCRAZE_MANAGED_GROUPS NULL };
static const managed_resolver_t managed_resolvers[] = { NETCRAZE_MANAGED_RESOLVERS {0} };
enum {
    PREFIX_COUNT = sizeof(managed_prefixes) / sizeof(managed_prefixes[0]) - 1,
    GROUP_COUNT = sizeof(managed_groups) / sizeof(managed_groups[0]) - 1,
    RESOLVER_COUNT = sizeof(managed_resolvers) / sizeof(managed_resolvers[0]) - 1,
    ROUTE_COUNT = PREFIX_COUNT + GROUP_COUNT,
    BINDING_COUNT = ROUTE_COUNT + RESOLVER_COUNT,
};
enum { ROUTE_PRIMARY = 1, ROUTE_BACKUP = 2, ROUTE_OTHER = 4 };

typedef struct {
    uint8_t bindings[BINDING_COUNT + 1];
} route_state_t;

static int route_flag(const char *interface)
{
    if (!strcmp(interface, exits[0].interface)) return ROUTE_PRIMARY;
    if (!strcmp(interface, exits[1].interface)) return ROUTE_BACKUP;
    return ROUTE_OTHER;
}

static void parse_routes(char *line, void *context)
{
    route_state_t *state = context;
    char entry[32], mask[16], interface[32], options[48] = "";
    if (sscanf(line, "ip route %31s %15s %31s %47[^\n]", entry, mask, interface, options) == 4) {
        for (int i = 0; i < PREFIX_COUNT; i++) {
            if (!strcmp(entry, managed_prefixes[i].network) && !strcmp(mask, managed_prefixes[i].mask)) {
                state->bindings[i] |= !strcmp(options, "auto") ? route_flag(interface) : ROUTE_OTHER;
            }
        }
    } else if (*line == ' ' &&
               sscanf(line, " route object-group %31s %31s %47[^\n]", entry, interface, options) == 3) {
        for (int i = 0; i < GROUP_COUNT; i++) {
            if (!strcmp(entry, managed_groups[i])) {
                state->bindings[PREFIX_COUNT + i] |= !strcmp(options, "auto reject") ? route_flag(interface) : ROUTE_OTHER;
            }
        }
    }
    char resolver[48], host[64], binding[32], extra;
    int fields = sscanf(line, "ip name-server %47s %63s on %31s %c",
                        resolver, host, binding, &extra);
    if (fields >= 2) {
        for (int i = 0; i < RESOLVER_COUNT; i++) {
            if (strcmp(host, managed_resolvers[i].host)) continue;
            int flag = fields == 3 && !strcmp(resolver, managed_resolvers[i].resolver)
                ? route_flag(binding) : ROUTE_OTHER;
            uint8_t *configured = &state->bindings[ROUTE_COUNT + i];
            *configured |= (*configured & flag) ? ROUTE_OTHER : flag;
        }
    }
}

static void parse_active_routes(char *line, void *context)
{
    route_state_t *state = context;
    char prefix[24], gateway[32], interface[32];
    if (sscanf(line, " %23s %31s %31s", prefix, gateway, interface) != 3) return;
    for (int i = 0; i < PREFIX_COUNT; i++) {
        if (!strcmp(prefix, managed_prefixes[i].prefix)) state->bindings[i] |= route_flag(interface);
    }
}

static bool all_routes(const route_state_t *state, int count, int target)
{
    for (int i = 0; i < count; i++) {
        if (state->bindings[i] != target) return false;
    }
    return true;
}

static bool verify_routes(console_t *console, int target)
{
    route_state_t config = {0}, active = {0};
    if (!request(console, "show running-config", parse_routes, &config) ||
        !request(console, "show ip route", parse_active_routes, &active)) {
        ESP_LOGW(TAG, "Could not read route tables");
        return false;
    }
    bool verified = all_routes(&config, BINDING_COUNT, target) &&
                    all_routes(&active, PREFIX_COUNT, target);
    if (!verified) {
        for (int i = 0; i < BINDING_COUNT; i++) {
            if (config.bindings[i] != target || (i < PREFIX_COUNT && active.bindings[i] != target)) {
                ESP_LOGW(TAG, "Route %d target %d: configured %u, active %u",
                         i + 1, target, config.bindings[i], active.bindings[i]);
            }
        }
    }
    return verified;
}

static void log_route_error(char *line, void *context)
{
    (void)context;
    if (strstr(line, "error[") || strstr(line, "failed")) ESP_LOGW(TAG, "%s", line);
}

static bool edit_route(console_t *console, int index, int target, bool remove)
{
    const char *interface = exits[target == ROUTE_PRIMARY ? 0 : 1].interface;
    char command[128];
    if (index < PREFIX_COUNT) {
        const managed_prefix_t *prefix = &managed_prefixes[index];
        snprintf(command, sizeof(command), "%sip route %s %s %s%s",
                 remove ? "no " : "", prefix->network, prefix->mask, interface, remove ? "" : " auto");
    } else if (index < ROUTE_COUNT) {
        snprintf(command, sizeof(command), "dns-proxy %sroute object-group %s %s%s",
                 remove ? "no " : "", managed_groups[index - PREFIX_COUNT], interface,
                 remove ? "" : " auto reject");
    } else {
        const managed_resolver_t *resolver = &managed_resolvers[index - ROUTE_COUNT];
        snprintf(command, sizeof(command), "%sip name-server %s %s on %s",
                 remove ? "no " : "", resolver->resolver, resolver->host, interface);
    }
    if (request(console, command, log_route_error, NULL)) return true;
    ESP_LOGW(TAG, "Route command timed out for entry %d while %s %s",
             index + 1, remove ? "removing" : "adding", interface);
    return false;
}

static bool put_routes(console_t *console, int target, bool require_active)
{
    route_state_t config = {0};
    if (!request(console, "show running-config", parse_routes, &config)) return false;
    for (int i = 0; i < BINDING_COUNT; i++) {
        if (config.bindings[i] & ROUTE_OTHER) return false;
    }
    int other = target == ROUTE_PRIMARY ? ROUTE_BACKUP : ROUTE_PRIMARY;
    for (int i = 0; i < BINDING_COUNT; i++) {
        if (i >= ROUTE_COUNT) {
            if (!(config.bindings[i] & target) && !edit_route(console, i, target, false)) return false;
            if ((config.bindings[i] & other) && !edit_route(console, i, other, true)) return false;
            continue;
        }
        if ((config.bindings[i] & other) && !edit_route(console, i, other, true)) return false;
        if (!(config.bindings[i] & target) && !edit_route(console, i, target, false)) return false;
    }
    for (int attempt = 0; attempt < 3; attempt++) {
        route_state_t after = {0};
        if (require_active ? verify_routes(console, target) :
            (request(console, "show running-config", parse_routes, &after) &&
             all_routes(&after, BINDING_COUNT, target))) return true;
        if (attempt < 2) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return false;
}

static bool save_router(console_t *console);
static void clear_probe_dns(void *context);
static const char *const route_recovery_key = "route_restore";

static bool route_journal(nvs_handle_t nvs, uint8_t pending)
{
    return nvs_set_u8(nvs, route_recovery_key, pending) == ESP_OK && nvs_commit(nvs) == ESP_OK;
}

bool router_restore_routes(const router_credentials_t *credentials)
{
    nvs_handle_t nvs;
    if (nvs_open("observer", NVS_READWRITE, &nvs) != ESP_OK) return false;
    uint8_t pending = 0;
    esp_err_t status = nvs_get_u8(nvs, route_recovery_key, &pending);
    bool restored = status == ESP_ERR_NVS_NOT_FOUND || (status == ESP_OK && !pending);
    if (ROUTE_COUNT && status == ESP_OK && (pending == ROUTE_PRIMARY || pending == ROUTE_BACKUP)) {
        for (int retry = 0; retry < 3 && !restored; retry++) {
            console_t console;
            if (open_console(&console, credentials)) {
                restored = put_routes(&console, pending, false) && save_router(&console) && route_journal(nvs, 0);
                close(console.fd);
            }
            if (!restored && retry < 2) vTaskDelay(pdMS_TO_TICKS(1000));
        }
        ESP_LOG_LEVEL(restored ? ESP_LOG_INFO : ESP_LOG_ERROR, TAG,
                      "Managed bindings rollback to %s: %s", exits[pending == ROUTE_PRIMARY ? 0 : 1].label,
                      restored ? "complete" : "pending");
    }
    nvs_close(nvs);
    return restored;
}

int router_active_exit(const router_credentials_t *credentials)
{
    if (!ROUTE_COUNT) return -1;
    console_t console;
    if (!open_console(&console, credentials)) return -1;
    route_state_t config = {0};
    bool read = request(&console, "show running-config", parse_routes, &config);
    close(console.fd);
    if (read && all_routes(&config, BINDING_COUNT, ROUTE_PRIMARY)) return 0;
    if (read && all_routes(&config, BINDING_COUNT, ROUTE_BACKUP)) return 1;
    return -1;
}

bool router_failover(const router_credentials_t *credentials, int active, bool exercise)
{
    if (!ROUTE_COUNT || !CONFIG_NETCRAZE_POSTCHECK_HOST[0] || active < 0 || active > 1) return false;
    int source = active == 0 ? ROUTE_PRIMARY : ROUTE_BACKUP;
    int target = active == 0 ? ROUTE_BACKUP : ROUTE_PRIMARY;
    const exit_t *exit = &exits[1 - active];
    console_t console;
    if (!open_console(&console, credentials)) return false;
    nvs_handle_t nvs;
    if (nvs_open("observer", NVS_READWRITE, &nvs) != ESP_OK) {
        close(console.fd);
        return false;
    }
    route_state_t config = {0};
    char command[48], address[16] = "";
    snprintf(command, sizeof(command), "show interface %s", exit->interface);
    bool armed = request(&console, "show running-config", parse_routes, &config) &&
        all_routes(&config, BINDING_COUNT, source) &&
        request(&console, command, parse_address, address) && address[0] &&
        route_journal(nvs, source);
    bool verified = armed && put_routes(&console, target, true);
    if (verified) {
        ESP_LOGI(TAG, "Failover: %d routes and %d DNS rules verified on %s",
                 ROUTE_COUNT, RESOLVER_COUNT, exit->label);
        // Use the board's normal policy to prove the domain group moved.
        for (int i = 0; i < 3; i++) {
            tcpip_callback_wait(clear_probe_dns, NULL);
            probe_result_t check = probe(&console, exit, address, "managed domain",
                                         CONFIG_NETCRAZE_POSTCHECK_HOST, CONFIG_NETCRAZE_POSTCHECK_PATH,
                                         CONFIG_NETCRAZE_POSTCHECK_STATUS);
            if (check.http_ok && check.nat_ok) {
                verified = verify_routes(&console, target);
                break;
            }
            if (i == 2) verified = false;
        }
    }
    bool committed = verified && !exercise && save_router(&console) && route_journal(nvs, 0);
    close(console.fd);
    nvs_close(nvs);
    bool restored = !armed || committed || router_restore_routes(credentials);
    ESP_LOG_LEVEL(verified && restored ? ESP_LOG_INFO : ESP_LOG_ERROR, TAG,
                  "Failover %s: HTTPS/NAT %s, %s", exit->label,
                  verified ? "passed" : "failed",
                  committed ? "committed" : restored ? "rolled back" : "rollback pending");
    return exercise ? verified && restored : committed;
}

static void clear_probe_dns(void *context)
{
    (void)context;
    dns_clear_cache();
}

static void find_saved(char *line, void *context)
{
    if (strstr(line, " saved") || strstr(line, " saving") || strstr(line, "Saved") || strstr(line, "Saving")) *(bool *)context = true;
}

static bool save_router(console_t *console)
{
    bool saved = false;
    return request(console, "system configuration save", find_saved, &saved) && saved;
}
