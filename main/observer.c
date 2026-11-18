#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "router.h"
#include "ota.h"

static const char *TAG = "netcraze";
static EventGroupHandle_t network;
static const EventBits_t ONLINE = BIT0;

static void on_network_event(void *arg, esp_event_base_t base, int32_t event, void *data)
{
    if (base == IP_EVENT && event == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(network, ONLINE);
    } else if (base == WIFI_EVENT && event == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(network, ONLINE);
    }
}

static bool read_setting(const char *prompt, char *value, size_t capacity)
{
    printf("%s\n", prompt);
    fflush(stdout);
    size_t used = 0;
    bool too_long = false;
    for (;;) {
        uint8_t byte;
        if (uart_read_bytes(UART_NUM_0, &byte, 1, portMAX_DELAY) != 1) {
            return false;
        }
        if (byte == '\r') {
            continue;
        }
        if (byte == '\n') {
            value[used] = '\0';
            return !too_long;
        }
        if (used + 1 < capacity) {
            value[used++] = (char)byte;
        } else {
            too_long = true;
        }
    }
}

typedef struct {
    char ssid[33];
    char password[64];
} wifi_settings_t;

static bool load_wifi_settings(wifi_settings_t *settings)
{
    nvs_handle_t handle;
    if (nvs_open("observer", NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    wifi_settings_t saved;
    size_t size = sizeof(saved);
    esp_err_t result = nvs_get_blob(handle, "wifi", &saved, &size);
    nvs_close(handle);
    if (result != ESP_OK || size != sizeof(saved) || !saved.ssid[0] ||
        !memchr(saved.ssid, 0, sizeof(saved.ssid)) ||
        !memchr(saved.password, 0, sizeof(saved.password)) || strlen(saved.password) < 8) {
        return false;
    }
    *settings = saved;
    return true;
}

static esp_err_t save_wifi_settings(const wifi_settings_t *settings)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open("observer", NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_set_blob(handle, "wifi", settings, sizeof(*settings));
        if (result == ESP_OK) {
            result = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    return result;
}

static bool load_router_credentials(router_credentials_t *credentials)
{
    nvs_handle_t handle;
    if (nvs_open("observer", NVS_READONLY, &handle) != ESP_OK) return false;
    size_t size = sizeof(*credentials);
    esp_err_t result = nvs_get_blob(handle, "router_login", credentials, &size);
    nvs_close(handle);
    return result == ESP_OK && size == sizeof(*credentials) &&
        credentials->username[0] && credentials->password[0] &&
        memchr(credentials->username, 0, sizeof(credentials->username)) &&
        memchr(credentials->password, 0, sizeof(credentials->password));
}

static void provision_router_credentials(router_credentials_t *credentials)
{
    for (;;) {
        memset(credentials, 0, sizeof(*credentials));
        if (read_setting("Router username (send one line over USB)", credentials->username,
                         sizeof(credentials->username)) && credentials->username[0] &&
            read_setting("Router password (send one line over USB)", credentials->password,
                         sizeof(credentials->password)) && credentials->password[0] &&
            router_valid_credentials(credentials)) {
            break;
        }
        ESP_LOGW(TAG, "Router login failed");
    }
    nvs_handle_t handle;
    ESP_ERROR_CHECK(nvs_open("observer", NVS_READWRITE, &handle));
    ESP_ERROR_CHECK(nvs_set_blob(handle, "router_login", credentials, sizeof(*credentials)));
    ESP_ERROR_CHECK(nvs_commit(handle));
    nvs_close(handle);
    ESP_LOGI(TAG, "Router credentials validated and saved");
}

typedef enum { COMMAND_NONE, COMMAND_TEST, COMMAND_ARM, COMMAND_DISARM } command_t;

static command_t read_command(void)
{
    static char line[32];
    static size_t used;
    command_t command = COMMAND_NONE;
    uint8_t byte;
    while (uart_read_bytes(UART_NUM_0, &byte, 1, 0) == 1) {
        if (byte == '\r') continue;
        if (byte == '\n') {
            if (used < sizeof(line)) {
                line[used] = '\0';
                if (!strcmp(line, "test-failover")) command = COMMAND_TEST;
                if (!strcmp(line, "arm-failover")) command = COMMAND_ARM;
                if (!strcmp(line, "disarm-failover")) command = COMMAND_DISARM;
            }
            used = 0;
        } else if (used < sizeof(line)) {
            line[used++] = byte;
        }
    }
    return command;
}

static bool failover_armed(void)
{
    nvs_handle_t nvs;
    uint8_t armed = 0;
    if (nvs_open("observer", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, "auto_failover", &armed);
        nvs_close(nvs);
    }
    return armed == 1;
}

static bool set_failover_armed(bool armed)
{
    nvs_handle_t nvs;
    if (nvs_open("observer", NVS_READWRITE, &nvs) != ESP_OK) return false;
    bool saved = nvs_set_u8(nvs, "auto_failover", armed) == ESP_OK && nvs_commit(nvs) == ESP_OK;
    nvs_close(nvs);
    return saved;
}

void app_main(void)
{
    if (!router_configured()) {
        ESP_LOGE(TAG, "Set the router IPv4, two distinct interface IDs and two probe policies in menuconfig");
        return;
    }
    wifi_settings_t settings = {0};
    ESP_ERROR_CHECK(nvs_flash_init());
    ota_initialize();
    bool provision = !load_wifi_settings(&settings);
    router_credentials_t credentials;
    bool provision_router = !load_router_credentials(&credentials);
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0));
    if (provision) {
        do {
            memset(&settings, 0, sizeof(settings));
            if (read_setting("Wi-Fi SSID (send one line over USB)", settings.ssid, sizeof(settings.ssid)) &&
                settings.ssid[0] &&
                read_setting("Wi-Fi password (send one line over USB)", settings.password, sizeof(settings.password)) &&
                strlen(settings.password) >= 8) {
                break;
            }
            ESP_LOGW(TAG, "Invalid Wi-Fi settings");
        } while (true);
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    network = xEventGroupCreate();
    configASSERT(network);
    configASSERT(esp_netif_create_default_wifi_sta());
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_network_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_network_event, NULL));

    wifi_config_t wifi = {0};
    memcpy(wifi.sta.ssid, settings.ssid, strlen(settings.ssid));
    memcpy(wifi.sta.password, settings.password, strlen(settings.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
    ESP_ERROR_CHECK(esp_wifi_start());

    uint8_t station[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, station));
    char mac[18];
    snprintf(mac, sizeof(mac), MACSTR, MAC2STR(station));
    bool clock_ready = false;
    int active_failures = 0;
    int previous_active = -1;
    bool exercise_passed = false;
    for (;;) {
        if (!(xEventGroupGetBits(network) & ONLINE)) {
            esp_wifi_connect();
            if (!(xEventGroupWaitBits(network, ONLINE, pdFALSE, pdTRUE, pdMS_TO_TICKS(15000)) & ONLINE)) {
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
        }
        if (provision) {
            esp_err_t result = save_wifi_settings(&settings);
            if (result == ESP_OK) {
                ESP_LOGI(TAG, "Wi-Fi settings saved");
                provision = false;
            } else {
                ESP_LOGW(TAG, "Wi-Fi settings could not be saved: %s", esp_err_to_name(result));
            }
        }
        if (provision_router) {
            provision_router_credentials(&credentials);
            provision_router = false;
        }
        ota_start_server();
        if (!router_restore_routes(&credentials) || !router_restore_host(&credentials, mac)) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        ota_observer_checkpoint(false);
        if (!router_dns_healthy()) {
            ESP_LOGW(TAG, "Router DNS over UDP unavailable. Holding probes and new writes.");
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }
        if (!clock_ready) {
            esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp));
            clock_ready = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) == ESP_OK;
            if (!clock_ready) {
                esp_netif_sntp_deinit();
                ESP_LOGW(TAG, "Clock synchronization failed, delaying TLS probes");
                vTaskDelay(pdMS_TO_TICKS(15000));
                continue;
            }
        }
        TickType_t cycle_start = xTaskGetTickCount();
        command_t command = read_command();
        if (command == COMMAND_DISARM) {
            ESP_LOGI(TAG, "Disarm: %s", set_failover_armed(false) ? "saved" : "write failed");
        }
        round_result_t round = router_probe_cycle(&credentials, mac);
        ota_report_probe_round(&round);
        if (round.restored) {
            ota_observer_checkpoint(exit_healthy(&round.primary) || exit_healthy(&round.backup));
        }
        int active = router_active_exit(&credentials);
        if (previous_active != active) active_failures = 0;
        previous_active = active;
        if (command == COMMAND_ARM) {
            bool armed = exercise_passed && active >= 0 && set_failover_armed(true);
            ESP_LOGI(TAG, "Arm: %s", armed ? "saved" : "a successful exercise is required");
        }
        if (active >= 0) {
            round_result_t ordered = round;
            if (active == 1) {
                ordered.primary = round.backup;
                ordered.backup = round.primary;
            }
            bool qualify = observe_round(&ordered, &active_failures);
            ESP_LOGI(TAG, "%s active, failures %d/%d, reserve %s",
                     active == 0 ? "primary" : "backup", active_failures, FAILURE_THRESHOLD,
                     exit_healthy(&ordered.backup) ? "verified" : "unverified");
            if (command == COMMAND_TEST) {
                exercise_passed = round.restored && exit_healthy(&ordered.primary) &&
                    exit_healthy(&ordered.backup) && router_dns_healthy() &&
                    router_failover(&credentials, active, true);
                ESP_LOGI(TAG, "Exercise: %s", exercise_passed ? "passed and restored" : "failed or ineligible");
                active_failures = 0;
            } else if (qualify && failover_armed() && router_dns_healthy()) {
                router_failover(&credentials, active, false);
                active_failures = 0;
            }
        }
        xTaskDelayUntil(&cycle_start, pdMS_TO_TICKS(60000));
    }
}
