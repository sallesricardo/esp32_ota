#pragma once

#include "esp_event.h"


#ifdef __cplusplus
extern "C" {
#endif

static char s_last_ota_status[128] = "Idle";

typedef struct  {
    EventGroupHandle_t network_event_group;
    EventGroupHandle_t ota_event_group;
    EventBits_t network_bit;
    EventBits_t ota_bit;
} ota_task_params_t;

void ota_task(void *pvParameter);


#ifdef __cplusplus
}
#endif
