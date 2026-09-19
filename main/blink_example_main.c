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
#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_netif.h"

#define VERSION 3

// --- CONFIGURAÇÕES ---
// Substitua pelo IP da sua máquina na rede local (ex: 192.168.1.10)
#define OTA_URL        "http://192.168.15.50:8070/blink_ota.bin"
#define MANIFEST_URL   "http://192.168.15.50:8070/manifest.json"

static const char *TAG = "blink_ota";
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
static int s_retry_num = 0;

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

    esp_http_client_config_t config = {
        .url = MANIFEST_URL,
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

    // Verifica se há atualização
    if (!check_for_update(ota_url, sizeof(ota_url))) {
        ESP_LOGI(TAG, "Nenhuma atualização disponível. Continuando...");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Conectado ao Wi-Fi. Iniciando download de: %s", ota_url);

    esp_http_client_config_t config = {
        .url = ota_url,
        .cert_pem = NULL,
        .skip_cert_common_name_check = true,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    // A função esp_https_ota cuida de todo o processo de download, validação e gravação
    esp_err_t ret = esp_https_ota(&ota_config);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA Sucessivo! Reiniciando em 3 segundos...");
        vTaskDelay(3000 / portTICK_PERIOD_MS);
        esp_restart();
    } else {
        ESP_LOGE(TAG, "Falha no OTA (Erro: %s). Continuando com a versão atual.", esp_err_to_name(ret));
        vTaskDelete(NULL); // Encerra a tarefa de OTA se falhar
    }
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

    // Inicia tarefas
    xTaskCreate(&blink_task, "blink_task", configMINIMAL_STACK_SIZE * 2, NULL, 5, NULL);
    xTaskCreate(&ota_task, "ota_task", configMINIMAL_STACK_SIZE * 8, NULL, 5, NULL);

    vTaskDelay(1000 / portTICK_PERIOD_MS);
    check_and_confirm_ota();
}
