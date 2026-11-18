#include "ota.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

#if __has_include("ota_bootstrap.h")
#include "ota_bootstrap.h"
#endif

enum { PORT = 8266, KEY_SIZE = 32, NONCE_SIZE = 16, HASH_SIZE = 32 };
static const char *TAG = "ota";
static uint8_t key[KEY_SIZE], nonce[NONCE_SIZE];
static int64_t nonce_deadline;
static bool key_ready;
static httpd_handle_t server;
static esp_timer_handle_t validation_timer;
static atomic_bool reboot_pending;
static atomic_bool validating;
static atomic_uint probe_health;

enum {
    PROBE_MASK = (1U << PROBE_COUNT) - 1,
    EXIT_HEALTH_BITS = 2 * PROBE_COUNT,
    PROBE_POLICY_RESTORED = 1U << (2 * EXIT_HEALTH_BITS),
    PROBE_ROUND_REPORTED = PROBE_POLICY_RESTORED << 1,
};

static unsigned exit_health_bits(const exit_result_t *exit)
{
    unsigned bits = 0;
    if (!exit->measured) return bits;
    for (unsigned i = 0; i < PROBE_COUNT; i++) {
        if (exit->probes[i].http_ok) bits |= 1U << i;
        if (exit->probes[i].nat_ok) bits |= 1U << (PROBE_COUNT + i);
    }
    return bits;
}

void ota_report_probe_round(const round_result_t *round)
{
    unsigned bits = PROBE_ROUND_REPORTED | exit_health_bits(&round->primary) |
                    (exit_health_bits(&round->backup) << EXIT_HEALTH_BITS);
    if (round->restored) bits |= PROBE_POLICY_RESTORED;
    atomic_store(&probe_health, bits);
}

static void hex_encode(const uint8_t *bytes, size_t size, char *output)
{
    const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < size; ++i) {
        output[2 * i] = digits[bytes[i] >> 4];
        output[2 * i + 1] = digits[bytes[i] & 15];
    }
    output[size * 2] = '\0';
}

static bool read_hex_header(httpd_req_t *request, const char *name, uint8_t *out, size_t size)
{
    const char digits[] = "0123456789abcdef";
    char value[2 * HASH_SIZE + 1];
    if (httpd_req_get_hdr_value_len(request, name) != size * 2 ||
        httpd_req_get_hdr_value_str(request, name, value, sizeof(value)) != ESP_OK) return false;
    for (size_t i = 0; i < size; ++i) {
        const char *high = strchr(digits, value[2 * i]), *low = strchr(digits, value[2 * i + 1]);
        if (!high || !low) return false;
        out[i] = (high - digits) * 16 + (low - digits);
    }
    return true;
}

static bool equal_bytes(const uint8_t *left, const uint8_t *right, size_t size)
{
    unsigned difference = 0;
    for (size_t i = 0; i < size; ++i) difference |= left[i] ^ right[i];
    return difference == 0;
}

static esp_err_t status_handler(httpd_req_t *request)
{
    char response[320], image_id[2 * HASH_SIZE + 1];
    const esp_app_desc_t *app = esp_app_get_description();
    unsigned health = atomic_load(&probe_health);
    unsigned first = health;
    unsigned backup = health >> EXIT_HEALTH_BITS;
    hex_encode(app->app_elf_sha256, sizeof(app->app_elf_sha256), image_id);
    snprintf(response, sizeof(response),
             "version=%s\nslot=%s\nvalidating=%d\nimage=%s\n"
             "probe_reported=%d\nprobe_policy_restored=%d\n"
             "primary_http_mask=%u\nprimary_nat_mask=%u\nbackup_http_mask=%u\nbackup_nat_mask=%u\n",
             app->version, esp_ota_get_running_partition()->label,
             atomic_load(&validating), image_id,
             !!(health & PROBE_ROUND_REPORTED), !!(health & PROBE_POLICY_RESTORED),
             first & PROBE_MASK, (first >> PROBE_COUNT) & PROBE_MASK,
             backup & PROBE_MASK, (backup >> PROBE_COUNT) & PROBE_MASK);
    return httpd_resp_sendstr(request, response);
}

static esp_err_t challenge_handler(httpd_req_t *request)
{
    char response[2 * NONCE_SIZE + 1];
    esp_fill_random(nonce, sizeof(nonce));
    nonce_deadline = esp_timer_get_time() + 30 * INT64_C(1000000);
    hex_encode(nonce, sizeof(nonce), response);
    return httpd_resp_sendstr(request, response);
}

static esp_err_t upload_handler(httpd_req_t *request)
{
    if (atomic_load(&reboot_pending) || atomic_load(&validating)) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        return httpd_resp_sendstr(request, "Update already running");
    }
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    uint32_t size = request->content_len;
    if (!target || !size || size > target->size)
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid firmware size");

    uint8_t message[NONCE_SIZE + HASH_SIZE + 4], signature[HASH_SIZE], expected[HASH_SIZE];
    memcpy(message, nonce, NONCE_SIZE);
    message[NONCE_SIZE + HASH_SIZE] = size >> 24;
    message[NONCE_SIZE + HASH_SIZE + 1] = size >> 16;
    message[NONCE_SIZE + HASH_SIZE + 2] = size >> 8;
    message[NONCE_SIZE + HASH_SIZE + 3] = size;
    if (esp_timer_get_time() >= nonce_deadline ||
        !read_hex_header(request, "X-Firmware-SHA256", message + NONCE_SIZE, HASH_SIZE) ||
        !read_hex_header(request, "X-Firmware-HMAC", signature, HASH_SIZE) ||
        mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, sizeof(key),
                        message, sizeof(message), expected) ||
        !equal_bytes(signature, expected, sizeof(signature)))
        return httpd_resp_send_err(request, HTTPD_401_UNAUTHORIZED, "Invalid update authorization");
    nonce_deadline = 0;

    uint8_t *buffer = malloc(4096);
    if (!buffer) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    esp_ota_handle_t handle;
    esp_err_t result = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    bool opened = result == ESP_OK;
    mbedtls_sha256_context digest;
    mbedtls_sha256_init(&digest);
    if (opened && mbedtls_sha256_starts(&digest, 0)) result = ESP_FAIL;
    int64_t deadline = esp_timer_get_time() + 90 * INT64_C(1000000);
    for (uint32_t remaining = size; result == ESP_OK && remaining;) {
        if (esp_timer_get_time() >= deadline) {
            result = ESP_ERR_TIMEOUT;
            break;
        }
        int got = httpd_req_recv(request, (char *)buffer, remaining < 4096 ? remaining : 4096);
        if (got <= 0 || mbedtls_sha256_update(&digest, buffer, got)) {
            result = ESP_FAIL;
            break;
        }
        result = esp_ota_write(handle, buffer, got);
        remaining -= got;
    }
    free(buffer);
    if (result == ESP_OK &&
        (mbedtls_sha256_finish(&digest, expected) ||
         !equal_bytes(expected, message + NONCE_SIZE, HASH_SIZE))) result = ESP_ERR_INVALID_CRC;
    mbedtls_sha256_free(&digest);
    if (opened) {
        if (result == ESP_OK) result = esp_ota_end(handle);
        else esp_ota_abort(handle);
    }
    if (result == ESP_OK) {
        esp_app_desc_t description;
        result = esp_ota_get_partition_description(target, &description);
        if (result == ESP_OK &&
            strncmp(description.project_name, esp_app_get_description()->project_name,
                    sizeof(description.project_name))) result = ESP_ERR_INVALID_ARG;
    }
    if (result == ESP_OK) result = esp_ota_set_boot_partition(target);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Update rejected: %s", esp_err_to_name(result));
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Firmware failed verification");
    }
    esp_err_t response = httpd_resp_sendstr(request, "Update accepted. Restart scheduled.\n");
    atomic_store(&reboot_pending, true);
    return response;
}

static void validation_expired(void *unused)
{
    (void)unused;
    if (atomic_load(&validating)) {
        ESP_LOGE(TAG, "New image did not pass health checks. Rolling back.");
        esp_ota_mark_app_invalid_rollback_and_reboot();
        esp_restart();
    }
}

void ota_initialize(void)
{
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        atomic_store(&validating, true);
        const esp_timer_create_args_t timer = {.callback = validation_expired, .name = "ota-check"};
        ESP_ERROR_CHECK(esp_timer_create(&timer, &validation_timer));
        ESP_ERROR_CHECK(esp_timer_start_once(validation_timer, 180 * UINT64_C(1000000)));
    }
    nvs_handle_t nvs;
    if (nvs_open("observer", NVS_READWRITE, &nvs) != ESP_OK) return;
    size_t length = sizeof(key);
    esp_err_t result = nvs_get_blob(nvs, "ota_key", key, &length);
    key_ready = result == ESP_OK && length == sizeof(key);
#ifdef NETCRAZE_OTA_BOOTSTRAP
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        static const uint8_t initial_key[KEY_SIZE] = NETCRAZE_OTA_BOOTSTRAP;
        memcpy(key, initial_key, sizeof(key));
        key_ready = nvs_set_blob(nvs, "ota_key", key, sizeof(key)) == ESP_OK && nvs_commit(nvs) == ESP_OK;
    }
#endif
    nvs_close(nvs);
}

void ota_start_server(void)
{
    if (server || !key_ready) return;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = PORT;
    config.stack_size = 8192;
    if (httpd_start(&server, &config) != ESP_OK) return;
    const httpd_uri_t endpoints[] = {
        {.uri = "/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/challenge", .method = HTTP_GET, .handler = challenge_handler},
        {.uri = "/firmware", .method = HTTP_POST, .handler = upload_handler},
    };
    for (size_t i = 0; i < sizeof(endpoints) / sizeof(*endpoints); ++i)
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &endpoints[i]));
    ESP_LOGI(TAG, "Update API ready on port %d", PORT);
}

void ota_observer_checkpoint(bool healthy)
{
    if (healthy && server && atomic_load(&validating) &&
        esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        atomic_store(&validating, false);
        esp_timer_stop(validation_timer);
        ESP_LOGI(TAG, "New image confirmed by observer");
    }
    if (atomic_load(&reboot_pending)) esp_restart();
}
