#pragma once

#include "esp_event.h"


#ifdef __cplusplus
extern "C" {
#endif

typedef struct  {
    EventGroupHandle_t network_event_group;
    EventBits_t network_bit;
} ota_task_params_t;

void ota_task(void *pvParameter);
void check_and_confirm_ota(bool (*callback) (void));
void print_ota_info(void);

#ifdef __cplusplus
}
#endif
