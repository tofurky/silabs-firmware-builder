#ifndef ROUTER_STATUS_LED_BUTTON_CONFIG_H
#define ROUTER_STATUS_LED_BUTTON_CONFIG_H

// <<< Use Configuration Wizard in Context Menu >>>

// <h>Router Status LED and Button Configuration

// <o ROUTER_STATUS_LED_ENDPOINT> Endpoint for On/Off LED state sync
// <i> Default: 1
#ifndef ROUTER_STATUS_LED_ENDPOINT
#define ROUTER_STATUS_LED_ENDPOINT              1
#endif

// <o ROUTER_BUTTON_HOLD_SLOW_BLINK_MS> Hold threshold for slow blink feedback (ms)
// <i> Default: 2000
#ifndef ROUTER_BUTTON_HOLD_SLOW_BLINK_MS
#define ROUTER_BUTTON_HOLD_SLOW_BLINK_MS        2000
#endif

// <o ROUTER_BUTTON_HOLD_FAST_BLINK_MS> Hold threshold for fast blink feedback (ms)
// <i> Default: 5000
#ifndef ROUTER_BUTTON_HOLD_FAST_BLINK_MS
#define ROUTER_BUTTON_HOLD_FAST_BLINK_MS        5000
#endif

// <o ROUTER_BUTTON_HOLD_RESET_READY_MS> Hold threshold to arm factory reset (ms)
// <i> Default: 10000
#ifndef ROUTER_BUTTON_HOLD_RESET_READY_MS
#define ROUTER_BUTTON_HOLD_RESET_READY_MS       10000
#endif

// <o ROUTER_BUTTON_HOLD_FORCE_RESET_MS> Hold threshold to auto-trigger factory reset (ms)
// <i> Default: 12000
#ifndef ROUTER_BUTTON_HOLD_FORCE_RESET_MS
#define ROUTER_BUTTON_HOLD_FORCE_RESET_MS       12000
#endif

// <o ROUTER_STEERING_MAX_RETRIES> Maximum network steering retries per pairing session
// <i> Default: 34
#ifndef ROUTER_STEERING_MAX_RETRIES
#define ROUTER_STEERING_MAX_RETRIES             34
#endif

// <o ROUTER_STEERING_RETRY_DELAY_MS> Delay between network steering attempts (ms)
// <i> Default: 1000
#ifndef ROUTER_STEERING_RETRY_DELAY_MS
#define ROUTER_STEERING_RETRY_DELAY_MS          1000
#endif

// <q ROUTER_AUX_GPIO_PC00_INIT_LOW> Drive PC00 push-pull low on startup
// <i> Default: 1
#ifndef ROUTER_AUX_GPIO_PC00_INIT_LOW
#define ROUTER_AUX_GPIO_PC00_INIT_LOW           1
#endif

// </h>

// <<< end of configuration section >>>

#endif // ROUTER_STATUS_LED_BUTTON_CONFIG_H
