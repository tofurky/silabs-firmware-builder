#ifndef ROUTER_STATUS_LED_BUTTON_H
#define ROUTER_STATUS_LED_BUTTON_H

#include <stdbool.h>
#include <stdint.h>
#include "sl_status.h"

void router_status_led_button_init(uint8_t init_level);
void router_status_led_button_stack_status_cb(sl_status_t status);

#endif // ROUTER_STATUS_LED_BUTTON_H
