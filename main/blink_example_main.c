#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_ota_ops.h"
#include "driver/gpio.h"
#include "esp_netif.h"
#include "ota.h"

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


bool validate_after_update(void)
{
    return true;
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

    // Inicia tarefas
    xTaskCreate(&blink_task, "blink_task", configMINIMAL_STACK_SIZE * 2, NULL, 5, NULL);

    ota_task_params_t *ota_params = malloc(sizeof(ota_task_params_t));
    ota_params->network_event_group = s_wifi_event_group;
    ota_params->network_bit = WIFI_CONNECTED_BIT;
    xTaskCreate(&ota_task, "ota_task", configMINIMAL_STACK_SIZE * 8, (void *) ota_params, 5, NULL);

    vTaskDelay(1000 / portTICK_PERIOD_MS);
    check_and_confirm_ota(validate_after_update);
}
