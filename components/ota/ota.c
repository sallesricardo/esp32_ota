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
#include "cJSON.h"

#include "server_cert.h"

static const char *TAG = "OTA";

// --- CONFIGURAÇÕES ---
// Substitua pelo IP da sua máquina na rede local (ex: 192.168.1.10)
#define MANIFEST_URL   "https://192.168.15.50:8070/manifest.json"

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

void ota_task(void *pvParameter)
{
    ESP_LOGI(TAG, "Iniciando tarefa de OTA...");
    ota_task_params_t *params = (ota_task_params_t *) pvParameter;
    EventGroupHandle_t network_event_group = params->network_event_group;
    EventGroupHandle_t s_ota_event_group = params->ota_event_group;
    EventBits_t network_bit = params->network_bit;
    EventBits_t ota_bit = params->ota_bit;

    // Aguarda o Wi-Fi conectar
    xEventGroupWaitBits(network_event_group, network_bit, pdFALSE, pdTRUE, portMAX_DELAY);
    vTaskDelay(2000 / portTICK_PERIOD_MS); // Pequena pausa para estabilizar

    char ota_url[256] = {0};
    ESP_LOGI(TAG, "OTA task pronta. Aguardando comandos...");
    snprintf(s_last_ota_status, sizeof(s_last_ota_status), "Idle - waiting for trigger");

    while (1) {
        EventBits_t bits = xEventGroupWaitBits(
            s_ota_event_group,
            ota_bit,
            pdTRUE,   // limpa o bit ao sair
            pdFALSE,
            portMAX_DELAY
        );

        if (bits & ota_bit) {
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
