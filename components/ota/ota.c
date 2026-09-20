#include "ota.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_app_format.h"
#include "esp_http_server.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"

#include "server_cert.h"

static const char *TAG = "OTA";

static EventGroupHandle_t s_ota_event_group;
static httpd_handle_t s_http_server = NULL;

// --- CONFIGURAÇÕES ---
// Substitua pelo IP da sua máquina na rede local (ex: 192.168.1.10)
#define MANIFEST_URL   "https://192.168.15.50:8070/manifest.json"

#define OTA_REQUEST_BIT BIT1

bool check_for_update(char *ota_url, size_t url_size)
{
    ESP_LOGI(TAG, "Verificando atualizações em: %s", MANIFEST_URL);
    snprintf(s_last_ota_status, sizeof(s_last_ota_status), "Checking for updates...");

    esp_http_client_config_t config = {
        .url = MANIFEST_URL,
        .cert_pem = (const char *)esp32_cert_pem,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_open(client, 0);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao conectar no servidor: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        ESP_LOGE(TAG, "Erro ao buscar headers");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    // Lê o JSON completo
    char *response = malloc(content_length + 1);
    if (!response) {
        ESP_LOGE(TAG, "Sem memória para alocar resposta");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    int read_len = esp_http_client_read_response(client, response, content_length);
    response[read_len] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    // Parse do JSON
    cJSON *root = cJSON_Parse(response);
    if (!root) {
        ESP_LOGE(TAG, "Erro ao parsear JSON");
        free(response);
        return false;
    }

    // Extrai informações
    cJSON *version = cJSON_GetObjectItem(root, "version");
    cJSON *url = cJSON_GetObjectItem(root, "url");
    cJSON *changelog = cJSON_GetObjectItem(root, "changelog");

    if (!cJSON_IsString(version) || !cJSON_IsString(url)) {
        ESP_LOGE(TAG, "JSON inválido: faltando version ou url");
        cJSON_Delete(root);
        free(response);
        return false;
    }

    // Compara versões
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_app_desc_t *app_desc = malloc(sizeof(esp_app_desc_t));
    const esp_err_t partition_desc_status = esp_ota_get_partition_description(running, app_desc);
    if (partition_desc_status != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao obter descrição da partição atual");
        free(app_desc);
        free(response);
        return false;
    }
    const char *current_version = app_desc->version;
    const char *server_version = version->valuestring;

    ESP_LOGI(TAG, "Versão atual: %s", current_version);
    ESP_LOGI(TAG, "Versão no servidor: %s", server_version);

    if (changelog) {
        ESP_LOGI(TAG, "Changelog: %s", changelog->valuestring);
    }

    bool update_available = strcmp(current_version, server_version) != 0;

    if (update_available) {
        ESP_LOGI(TAG, "✓ Nova versão disponível!");
        strncpy(ota_url, url->valuestring, url_size - 1);
        ota_url[url_size - 1] = '\0';
    } else {
        ESP_LOGI(TAG, "✓ Firmware já está atualizado");
    }

    cJSON_Delete(root);
    free(response);

    return update_available;
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

void ota_task(void *pvParameter)
{
    ESP_LOGI(TAG, "Iniciando tarefa de OTA...");

    s_ota_event_group = xEventGroupCreate();

    ota_task_params_t *params = (ota_task_params_t *) pvParameter;
    EventGroupHandle_t network_event_group = params->network_event_group;
    EventBits_t network_bit = params->network_bit;

    // Aguarda o Wi-Fi conectar
    xEventGroupWaitBits(network_event_group, network_bit, pdFALSE, pdTRUE, portMAX_DELAY);
    vTaskDelay(2000 / portTICK_PERIOD_MS); // Pequena pausa para estabilizar

    start_http_server();

    char ota_url[256] = {0};
    ESP_LOGI(TAG, "OTA task pronta. Aguardando comandos...");
    snprintf(s_last_ota_status, sizeof(s_last_ota_status), "Idle - waiting for trigger");

    while (1) {
        EventBits_t bits = xEventGroupWaitBits(
            s_ota_event_group,
            OTA_REQUEST_BIT,
            pdTRUE,   // limpa o bit ao sair
            pdFALSE,
            portMAX_DELAY
        );

        if (bits & OTA_REQUEST_BIT) {
            ESP_LOGI(TAG, "⚡ Sinal de OTA recebido!");
            snprintf(s_last_ota_status, sizeof(s_last_ota_status), "OTA in progress...");

            if (!check_for_update(ota_url, sizeof(ota_url))) {
                ESP_LOGW(TAG, "Nenhuma atualização disponível");
                snprintf(s_last_ota_status, sizeof(s_last_ota_status), "No update available");
                continue;
            }

            ESP_LOGI(TAG, "Baixando: %s", ota_url);

            esp_http_client_config_t http_config = {
                .url = ota_url,
                .cert_pem = (const char *)esp32_cert_pem,
            };

            esp_https_ota_config_t ota_config = {
                .http_config = &http_config
            };

            esp_err_t ret = esp_https_ota(&ota_config);

            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "✓ OTA Sucessivo! Reiniciando em 3s...");
                snprintf(s_last_ota_status, sizeof(s_last_ota_status), "OTA OK - Restarting...");
                vTaskDelay(3000 / portTICK_PERIOD_MS);
                esp_restart();
            } else {
                ESP_LOGE(TAG, "✗ OTA falhou: %s", esp_err_to_name(ret));
                snprintf(s_last_ota_status, sizeof(s_last_ota_status), "OTA failed: %s", esp_err_to_name(ret));
            }
        }
    }
}
