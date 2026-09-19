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
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_app_format.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "server_cert.h"

// --- CONFIGURAÇÕES ---
// Substitua pelo IP da sua máquina na rede local (ex: 192.168.1.10)
#define MANIFEST_URL   "https://192.168.15.50:8070/manifest.json"

static const char *TAG = "blink_ota";

static EventGroupHandle_t s_wifi_event_group;
static EventGroupHandle_t s_ota_event_group;

#define WIFI_CONNECTED_BIT BIT0
#define OTA_REQUEST_BIT BIT1

static int s_retry_num = 0;
static httpd_handle_t s_http_server = NULL;
static char s_last_ota_status[128] = "Idle";

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

    // ⭐ LÓGICA DE COMPARAÇÃO DE VERSÕES
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

// --- TAREFA DE OTA ---
void ota_task(void *pvParameter)
{
    ESP_LOGI(TAG, "Iniciando tarefa de OTA...");

    // Aguarda o Wi-Fi conectar
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    vTaskDelay(2000 / portTICK_PERIOD_MS); // Pequena pausa para estabilizar

    char ota_url[256] = {0};
    ESP_LOGI(TAG, "OTA task pronta. Aguardando comandos...");
    snprintf(s_last_ota_status, sizeof(s_last_ota_status), "Idle - waiting for trigger");

    while (1) {
        // ⭐ AGUARDA sinal do servidor HTTP (bloqueia aqui até receber)
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
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_app_desc_t *partition_desc = malloc(sizeof(esp_app_desc_t));
    const esp_err_t partition_desc_status = esp_ota_get_partition_description(running, partition_desc);
    if (partition_desc_status != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao obter descrição da partição atual");
    }
    ESP_LOGI(TAG, "Partition Versão atual: %s", partition_desc->version);
    ESP_LOGI(TAG, "Parttion Data de compilação: %s %s", partition_desc->date, partition_desc->time);
    free(partition_desc);

    print_ota_info();

    wifi_init_sta();

    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    start_http_server();

    // Inicia tarefas
    xTaskCreate(&blink_task, "blink_task", configMINIMAL_STACK_SIZE * 2, NULL, 5, NULL);
    xTaskCreate(&ota_task, "ota_task", configMINIMAL_STACK_SIZE * 8, NULL, 5, NULL);

    vTaskDelay(1000 / portTICK_PERIOD_MS);
    check_and_confirm_ota();
}
