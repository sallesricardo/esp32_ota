#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_ota_ops.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "ota.h"

static const char *TAG = "blink_ota";

static EventGroupHandle_t s_wifi_event_group;
static EventGroupHandle_t s_ota_event_group;

#define WIFI_CONNECTED_BIT BIT0
#define OTA_REQUEST_BIT BIT1

static int s_retry_num = 0;
static httpd_handle_t s_http_server = NULL;


// --- EVENT HANDLER DO WI-FI ---
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WIFI_EVENT_STA_START");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "WIFI_EVENT_STA_DISCONNECTED");
        if (s_retry_num < 5) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Tentando reconectar ao Wi-Fi...");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "IP_EVENT_STA_GOT_IP");
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Obteve IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// --- INICIALIZAÇÃO DO WI-FI ---
void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
    s_ota_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    ESP_LOGI(TAG, "Conectando na rede '%s'...", CONFIG_WIFI_SSID);
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Wi-Fi inicializado.");
}

// ==================== SERVIDOR HTTP (NOVO!) ====================

// Handler: GET /ota - Dispara a OTA
static esp_err_t ota_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "📡 Recebido comando OTA via HTTP");

    // Sinaliza a ota_task
    xEventGroupSetBits(s_ota_event_group, OTA_REQUEST_BIT);

    const char *resp = "<html><body><h1>OTA Triggered!</h1>"
                       "<p>Check serial monitor for progress.</p>"
                       "<a href='/'>Back</a></body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

// Handler: GET /status - Retorna status em JSON
static esp_err_t status_handler(httpd_req_t *req)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "firmware_version", app_desc->version);
    cJSON_AddStringToObject(root, "compile_date", app_desc->date);
    cJSON_AddStringToObject(root, "compile_time", app_desc->time);
    cJSON_AddStringToObject(root, "idf_version", app_desc->idf_ver);
    cJSON_AddStringToObject(root, "running_partition", running ? running->label : "unknown");
    cJSON_AddStringToObject(root, "ota_status", s_last_ota_status);
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "uptime_seconds", esp_timer_get_time() / 1000000);

    char *json_str = cJSON_PrintUnformatted(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

// Handler: GET /restart - Reinicia o ESP32
static esp_err_t restart_handler(httpd_req_t *req)
{
    const char *resp = "<html><body><h1>Restarting...</h1></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp, strlen(resp));

    vTaskDelay(500 / portTICK_PERIOD_MS);
    esp_restart();
    return ESP_OK;
}

// Handler: GET / - Página principal com botões
static esp_err_t root_handler(httpd_req_t *req)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();

    char html[1024];
    snprintf(html, sizeof(html),
        "<!DOCTYPE html><html><head>"
        "<title>ESP32 OTA Control</title>"
        "<style>"
        "body{font-family:sans-serif;max-width:600px;margin:40px auto;padding:20px;}"
        "button{padding:15px 30px;margin:10px;font-size:16px;cursor:pointer;border:none;border-radius:5px;color:white;}"
        ".btn-ota{background:#4CAF50;}.btn-restart{background:#ff9800;}"
        ".info{background:#f0f0f0;padding:15px;border-radius:5px;margin:20px 0;}"
        "</style></head><body>"
        "<h1>ESP32 OTA Control Panel</h1>"
        "<div class='info'>"
        "<p><b>Version:</b> %s</p>"
        "<p><b>Build:</b> %s %s</p>"
        "<p><b>IDF:</b> %s</p>"
        "<p><b>Status:</b> %s</p>"
        "</div>"
        "<button class='btn-ota' onclick=\"location.href='/ota'\">Trigger OTA Update</button>"
        "<button class='btn-restart' onclick=\"location.href='/restart'\">Restart Device</button>"
        "<hr><p><a href='/status'>View JSON Status</a></p>"
        "</body></html>",
        app_desc->version, app_desc->date, app_desc->time,
        app_desc->idf_ver, s_last_ota_status);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, strlen(html));
    return ESP_OK;
}

// Inicia o servidor HTTP
void start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_uri_handlers = 5;

    ESP_ERROR_CHECK(httpd_start(&s_http_server, &config));

    httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = root_handler };
    httpd_uri_t uri_ota = { .uri = "/ota", .method = HTTP_GET, .handler = ota_handler };
    httpd_uri_t uri_status = { .uri = "/status", .method = HTTP_GET, .handler = status_handler };
    httpd_uri_t uri_restart = { .uri = "/restart", .method = HTTP_GET, .handler = restart_handler };

    httpd_register_uri_handler(s_http_server, &uri_root);
    httpd_register_uri_handler(s_http_server, &uri_ota);
    httpd_register_uri_handler(s_http_server, &uri_status);
    httpd_register_uri_handler(s_http_server, &uri_restart);

    ESP_LOGI(TAG, "✓ HTTP server iniciado na porta %d", config.server_port);
}

// --- TAREFA DO BLINK ---
void blink_task(void *pvParameter)
{
    gpio_reset_pin(CONFIG_BLINK_GPIO);
    gpio_set_direction(CONFIG_BLINK_GPIO, GPIO_MODE_OUTPUT);

    int level = 0;
    while (1) {
        gpio_set_level(CONFIG_BLINK_GPIO, level);
        level = !level;
        ESP_LOGI(TAG, "Piscando! LED: %d", level);
        vTaskDelay(500 / portTICK_PERIOD_MS); // Pisca a cada 500ms
    }
}

void print_ota_info(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;

    if (running) {
        ESP_LOGI(TAG, "Partição atual: %s (endereço: 0x%x)",
                 running->label, running->address);

        if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
            const char *state_str;
            switch(state) {
                case ESP_OTA_IMG_VALID: state_str = "VALID"; break;
                case ESP_OTA_IMG_PENDING_VERIFY: state_str = "PENDING_VERIFY"; break;
                case ESP_OTA_IMG_INVALID: state_str = "INVALID"; break;
                case ESP_OTA_IMG_ABORTED: state_str = "ABORTED"; break;
                default: state_str = "UNKNOWN"; break;
            }
            ESP_LOGI(TAG, "Estado: %s", state_str);
        }
    }

    // Mostra próxima partição para OTA
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (next) {
        ESP_LOGI(TAG, "Próxima partição OTA: %s", next->label);
    }
}

// Função para verificar e confirmar a nova versão
void check_and_confirm_ota(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;

    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        ESP_LOGI(TAG, "Estado da partição atual: %d", ota_state);
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "Nova versão detectada. Estado: PENDING_VERIFY");

            // AQUI você faz seus testes de sanity check
            // Ex: verificar se periféricos funcionam, se conecta no servidor, etc.

            bool all_tests_passed = true; // Substitua por testes reais

            if (all_tests_passed) {
                ESP_LOGI(TAG, "✓ Testes passaram. Confirmando nova versão...");
                esp_ota_mark_app_valid_cancel_rollback();
            } else {
                ESP_LOGE(TAG, "✗ Testes falharam. Iniciando rollback...");
                esp_ota_mark_app_invalid_rollback_and_reboot();
            }
        } else {
            ESP_LOGI(TAG, "Versão atual já está confirmada (estado: %d)", ota_state);
        }
    }
}

// --- APP MAIN ---
void app_main(void)
{
    // Inicializa NVS (Obrigatório para o Wi-Fi e OTA)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Imprime informações do app atual
    const esp_app_desc_t *app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "===============================================");
    ESP_LOGI(TAG, "Versão do projeto: %s", app_desc->version);
    ESP_LOGI(TAG, "Data de compilação: %s %s", app_desc->date, app_desc->time);
    ESP_LOGI(TAG, "===============================================");

    print_ota_info();

    wifi_init_sta();

    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    start_http_server();

    // Inicia tarefas
    xTaskCreate(&blink_task, "blink_task", configMINIMAL_STACK_SIZE * 2, NULL, 5, NULL);

    ota_task_params_t *ota_params = malloc(sizeof(ota_task_params_t));
    ota_params->network_event_group = s_wifi_event_group;
    ota_params->ota_event_group = s_ota_event_group;
    ota_params->network_bit = WIFI_CONNECTED_BIT;
    ota_params->ota_bit = OTA_REQUEST_BIT;
    xTaskCreate(&ota_task, "ota_task", configMINIMAL_STACK_SIZE * 8, (void *) ota_params, 5, NULL);

    vTaskDelay(1000 / portTICK_PERIOD_MS);
    check_and_confirm_ota();
}
