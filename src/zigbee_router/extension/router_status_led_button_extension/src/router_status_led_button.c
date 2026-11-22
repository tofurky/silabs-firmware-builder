/*
 * router_status_led_button.c
 *
 * Status LED and reset/pairing button implementation for single-LED,
 * single-button Zigbee routers (e.g. Aeotec Range Extender Zi / WG001-Z01).
 */

#include "router_status_led_button.h"
#include "router_status_led_button_config.h"

#include "app/framework/include/af.h"
#include "network-steering.h"
#include "stack-info.h"
#include "sl_token_api.h"

#include "sl_led.h"
#include "sl_simple_led_instances.h"
#include "sl_button.h"
#include "sl_simple_button_instances.h"
#include "sl_sleeptimer.h"
#include "sl_gpio.h"
#include "em_core.h"

#define HOLD_POLL_INTERVAL_MS       50
#define FAST_BLINK_HALF_PERIOD_MS   100
#define SLOW_BLINK_HALF_PERIOD_MS   750
#define BREATHE_TICK_MS             1
#define BREATHE_PWM_PERIOD_TICKS    16
#define BREATHE_STEP_TICKS          125

typedef enum {
  LED_PATTERN_OFF = 0,
  LED_PATTERN_SOLID_ON,
  LED_PATTERN_BLINK_FAST,
  LED_PATTERN_BLINK_SLOW,
  LED_PATTERN_BREATHE,
} router_led_pattern_t;

typedef enum {
  BUTTON_ACTION_NONE = 0,
  BUTTON_ACTION_SINGLE_CLICK,
  BUTTON_ACTION_DOUBLE_CLICK,
  BUTTON_ACTION_REFRESH_LED,
  BUTTON_ACTION_FACTORY_RESET,
} router_button_action_t;

static sl_sleeptimer_timer_handle_t led_pattern_timer;
static sl_sleeptimer_timer_handle_t button_hold_timer;
static sl_sleeptimer_timer_handle_t button_click_timer;

static sl_zigbee_af_event_t button_action_event;
static sl_zigbee_af_event_t steering_retry_event;
static sl_zigbee_af_event_t factory_reset_reboot_event;

static router_led_pattern_t current_led_pattern = LED_PATTERN_OFF;
static bool blink_phase_on = false;
static uint32_t breathe_tick_counter = 0;

static volatile uint32_t button_press_start_ms = 0;
static volatile bool button_is_pressed = false;
static volatile bool reset_triggered = false;
static volatile uint8_t button_click_count = 0;
static volatile router_button_action_t pending_button_action = BUTTON_ACTION_NONE;

static bool steering_active = false;
static uint8_t steering_retries_remaining = 0;
static bool identify_active = false;

static bool device_has_stored_network_settings(void)
{
  tokTypeStackNodeData nodeData;
  halCommonGetToken(&nodeData, TOKEN_STACK_NODE_DATA);

  if (nodeData.panId == 0xFFFF) {
    return false;
  }

  if (nodeData.radioFreqChannel < 11 || nodeData.radioFreqChannel > 26) {
    return false;
  }

  return true;
}

static bool read_on_off_attribute(void)
{
  bool on_off = true;
  if (sl_zigbee_af_read_server_attribute(ROUTER_STATUS_LED_ENDPOINT,
                                         ZCL_ON_OFF_CLUSTER_ID,
                                         ZCL_ON_OFF_ATTRIBUTE_ID,
                                         (uint8_t *)&on_off,
                                         sizeof(on_off))
      != SL_ZIGBEE_ZCL_STATUS_SUCCESS) {
    return true;
  }
  return on_off;
}

static void led_pattern_timer_cb(sl_sleeptimer_timer_handle_t *handle, void *data)
{
  (void)handle;
  (void)data;

  if (current_led_pattern == LED_PATTERN_BLINK_FAST
      || current_led_pattern == LED_PATTERN_BLINK_SLOW) {
    blink_phase_on = !blink_phase_on;
    if (blink_phase_on) {
      sl_led_turn_on(&sl_led_status_led);
    } else {
      sl_led_turn_off(&sl_led_status_led);
    }
  } else if (current_led_pattern == LED_PATTERN_BREATHE) {
    // 4000 ms breath cycle using a 16-level software PWM carrier (62.5 Hz)
    breathe_tick_counter++;
    uint32_t step = (breathe_tick_counter / BREATHE_STEP_TICKS) % 32;
    uint32_t duty = (step < 16) ? (step + 1) : (32 - step);
    uint32_t pwm_slot = breathe_tick_counter % BREATHE_PWM_PERIOD_TICKS;
    if (pwm_slot < duty) {
      sl_led_turn_on(&sl_led_status_led);
    } else {
      sl_led_turn_off(&sl_led_status_led);
    }
  }
}

static void set_led_pattern(router_led_pattern_t pattern)
{
  if (current_led_pattern == pattern) {
    return;
  }

  sl_sleeptimer_stop_timer(&led_pattern_timer);
  current_led_pattern = pattern;

  switch (pattern) {
    case LED_PATTERN_OFF:
      sl_led_turn_off(&sl_led_status_led);
      break;

    case LED_PATTERN_SOLID_ON:
      sl_led_turn_on(&sl_led_status_led);
      break;

    case LED_PATTERN_BLINK_FAST:
      blink_phase_on = true;
      sl_led_turn_on(&sl_led_status_led);
      sl_sleeptimer_start_periodic_timer_ms(&led_pattern_timer,
                                            FAST_BLINK_HALF_PERIOD_MS,
                                            led_pattern_timer_cb,
                                            NULL,
                                            0,
                                            0);
      break;

    case LED_PATTERN_BLINK_SLOW:
      blink_phase_on = true;
      sl_led_turn_on(&sl_led_status_led);
      sl_sleeptimer_start_periodic_timer_ms(&led_pattern_timer,
                                            SLOW_BLINK_HALF_PERIOD_MS,
                                            led_pattern_timer_cb,
                                            NULL,
                                            0,
                                            0);
      break;

    case LED_PATTERN_BREATHE:
      breathe_tick_counter = 0;
      sl_led_turn_on(&sl_led_status_led);
      sl_sleeptimer_start_periodic_timer_ms(&led_pattern_timer,
                                            BREATHE_TICK_MS,
                                            led_pattern_timer_cb,
                                            NULL,
                                            0,
                                            0);
      break;
  }
}

static void update_led_from_state(void)
{
  if (identify_active) {
    set_led_pattern(LED_PATTERN_BLINK_FAST);
    return;
  }

  if (sl_zigbee_af_network_state() == SL_ZIGBEE_JOINED_NETWORK) {
    if (read_on_off_attribute()) {
      set_led_pattern(LED_PATTERN_SOLID_ON);
    } else {
      set_led_pattern(LED_PATTERN_OFF);
    }
  } else if (steering_active) {
    set_led_pattern(LED_PATTERN_BLINK_FAST);
  } else {
    set_led_pattern(LED_PATTERN_BREATHE);
  }
}

static void start_network_steering_session(void)
{
  steering_active = true;
  steering_retries_remaining = ROUTER_STEERING_MAX_RETRIES;
  update_led_from_state();

  sl_status_t status = sl_zigbee_af_network_steering_start();
  sl_zigbee_app_debug_println("Join network start: 0x%X", status);
}

static void factory_reset_reboot_event_handler(sl_zigbee_af_event_t *event)
{
  (void)event;

  tokTypeStackNodeData nodeData;
  memset(&nodeData, 0xFF, sizeof(nodeData));
  halCommonSetToken(TOKEN_STACK_NODE_DATA, &nodeData);

  sl_zigbee_clear_binding_table();
  sl_zigbee_clear_key_table();
  sl_zigbee_token_factory_reset(true, true);
  NVIC_SystemReset();
}

static void perform_factory_reset(void)
{
  sl_zigbee_app_debug_println("Reset product");

  bool default_on = true;
  sl_zigbee_af_write_server_attribute(ROUTER_STATUS_LED_ENDPOINT,
                                      ZCL_ON_OFF_CLUSTER_ID,
                                      ZCL_ON_OFF_ATTRIBUTE_ID,
                                      (uint8_t *)&default_on,
                                      ZCL_BOOLEAN_ATTRIBUTE_TYPE);

  set_led_pattern(LED_PATTERN_BREATHE);

  sl_status_t status = sl_zigbee_leave_network(SL_ZIGBEE_LEAVE_NWK_WITH_NO_OPTION);
  if (status == SL_STATUS_OK) {
    // Allow the stack leave announcement (2050 ms on routers) to complete before rebooting
    sl_zigbee_af_event_set_delay_ms(&factory_reset_reboot_event, 2200);
  } else {
    sl_zigbee_af_event_set_active(&factory_reset_reboot_event);
  }
}

static void button_action_event_handler(sl_zigbee_af_event_t *event)
{
  (void)event;

  CORE_DECLARE_IRQ_STATE;
  CORE_ENTER_CRITICAL();
  router_button_action_t action = pending_button_action;
  pending_button_action = BUTTON_ACTION_NONE;
  CORE_EXIT_CRITICAL();

  switch (action) {
    case BUTTON_ACTION_SINGLE_CLICK:
      if (sl_zigbee_af_network_state() != SL_ZIGBEE_JOINED_NETWORK) {
        start_network_steering_session();
      }
      break;

    case BUTTON_ACTION_DOUBLE_CLICK:
      if (sl_zigbee_af_network_state() == SL_ZIGBEE_JOINED_NETWORK) {
        bool new_on_off = !read_on_off_attribute();
        sl_zigbee_af_write_server_attribute(ROUTER_STATUS_LED_ENDPOINT,
                                            ZCL_ON_OFF_CLUSTER_ID,
                                            ZCL_ON_OFF_ATTRIBUTE_ID,
                                            (uint8_t *)&new_on_off,
                                            ZCL_BOOLEAN_ATTRIBUTE_TYPE);
        update_led_from_state();
      }
      break;

    case BUTTON_ACTION_REFRESH_LED:
      update_led_from_state();
      break;

    case BUTTON_ACTION_FACTORY_RESET:
      perform_factory_reset();
      break;

    case BUTTON_ACTION_NONE:
    default:
      break;
  }
}

static void steering_retry_event_handler(sl_zigbee_af_event_t *event)
{
  (void)event;

  if (sl_zigbee_af_network_state() == SL_ZIGBEE_JOINED_NETWORK) {
    steering_active = false;
    steering_retries_remaining = 0;
    update_led_from_state();
    return;
  }

  if (steering_retries_remaining > 0) {
    steering_retries_remaining--;
    sl_status_t status = sl_zigbee_af_network_steering_start();
    sl_zigbee_app_debug_println("Join network start: 0x%X (retries left: %u)",
                                status,
                                steering_retries_remaining);
  } else {
    steering_active = false;
    update_led_from_state();
  }
}

static void button_hold_timer_cb(sl_sleeptimer_timer_handle_t *handle, void *data)
{
  (void)handle;
  (void)data;

  if (!button_is_pressed || reset_triggered) {
    return;
  }

  uint32_t now_ms = sl_sleeptimer_tick_to_ms(sl_sleeptimer_get_tick_count());
  uint32_t held_ms = now_ms - button_press_start_ms;

  if (held_ms >= ROUTER_BUTTON_HOLD_FORCE_RESET_MS) {
    reset_triggered = true;
    sl_sleeptimer_stop_timer(&button_hold_timer);
    pending_button_action = BUTTON_ACTION_FACTORY_RESET;
    sl_zigbee_af_event_set_active(&button_action_event);
  } else if (held_ms >= ROUTER_BUTTON_HOLD_RESET_READY_MS) {
    set_led_pattern(LED_PATTERN_SOLID_ON);
  } else if (held_ms >= ROUTER_BUTTON_HOLD_FAST_BLINK_MS) {
    set_led_pattern(LED_PATTERN_BLINK_FAST);
  } else if (held_ms >= ROUTER_BUTTON_HOLD_SLOW_BLINK_MS) {
    set_led_pattern(LED_PATTERN_BLINK_SLOW);
  }
}

static void button_click_timer_cb(sl_sleeptimer_timer_handle_t *handle, void *data)
{
  (void)handle;
  (void)data;

  uint8_t clicks = button_click_count;
  button_click_count = 0;

  if (clicks == 1) {
    pending_button_action = BUTTON_ACTION_SINGLE_CLICK;
    sl_zigbee_af_event_set_active(&button_action_event);
  } else if (clicks == 2) {
    pending_button_action = BUTTON_ACTION_DOUBLE_CLICK;
    sl_zigbee_af_event_set_active(&button_action_event);
  }
}

void sl_button_on_change(const sl_button_t *handle)
{
  if (handle != &sl_button_reset_button) {
    return;
  }

  bool pressed = (sl_button_get_state(handle) == SL_SIMPLE_BUTTON_PRESSED);
  uint32_t now_ms = sl_sleeptimer_tick_to_ms(sl_sleeptimer_get_tick_count());

  if (pressed) {
    sl_sleeptimer_stop_timer(&button_click_timer);
    button_is_pressed = true;
    reset_triggered = false;
    button_press_start_ms = now_ms;
    sl_sleeptimer_start_periodic_timer_ms(&button_hold_timer,
                                          HOLD_POLL_INTERVAL_MS,
                                          button_hold_timer_cb,
                                          NULL,
                                          0,
                                          0);
  } else {
    sl_sleeptimer_stop_timer(&button_hold_timer);
    if (!button_is_pressed || reset_triggered) {
      button_is_pressed = false;
      return;
    }
    button_is_pressed = false;

    uint32_t held_ms = now_ms - button_press_start_ms;
    if (held_ms >= ROUTER_BUTTON_HOLD_RESET_READY_MS) {
      button_click_count = 0;
      reset_triggered = true;
      pending_button_action = BUTTON_ACTION_FACTORY_RESET;
      sl_zigbee_af_event_set_active(&button_action_event);
    } else if (held_ms >= ROUTER_BUTTON_HOLD_SLOW_BLINK_MS) {
      button_click_count = 0;
      pending_button_action = BUTTON_ACTION_REFRESH_LED;
      sl_zigbee_af_event_set_active(&button_action_event);
    } else {
      button_click_count++;
      sl_sleeptimer_restart_timer_ms(&button_click_timer,
                                     ROUTER_BUTTON_MULTICLICK_WINDOW_MS,
                                     button_click_timer_cb,
                                     NULL,
                                     0,
                                     0);
    }
  }
}

void router_status_led_button_init(uint8_t init_level)
{
  (void)init_level;

#if ROUTER_AUX_GPIO_PC00_INIT_LOW
  sl_gpio_t pc00_gpio = { .port = SL_GPIO_PORT_C, .pin = 0 };
  sl_gpio_set_pin_mode(&pc00_gpio, SL_GPIO_MODE_PUSH_PULL, 0);
#endif

  // Activated from sleeptimer/GPIO ISRs, so it must be an ISR-safe event
  sl_zigbee_af_isr_event_init(&button_action_event, button_action_event_handler);
  sl_zigbee_af_event_init(&steering_retry_event, steering_retry_event_handler);
  sl_zigbee_af_event_init(&factory_reset_reboot_event, factory_reset_reboot_event_handler);

  update_led_from_state();

  if (!device_has_stored_network_settings()) {
    start_network_steering_session();
  }
}

void router_status_led_button_stack_status_cb(sl_status_t status)
{
  if (status == SL_STATUS_NETWORK_UP) {
    steering_active = false;
    steering_retries_remaining = 0;
    update_led_from_state();
  } else if (status == SL_STATUS_NETWORK_DOWN) {
    update_led_from_state();
  }
}

void sl_zigbee_af_network_steering_complete_cb(sl_status_t status,
                                               uint8_t totalBeacons,
                                               uint8_t joinAttempts,
                                               uint8_t finalState)
{
  (void)totalBeacons;
  (void)joinAttempts;
  (void)finalState;

  sl_zigbee_app_debug_println("Join network complete: 0x%X", status);

  if (status == SL_STATUS_OK) {
    steering_active = false;
    steering_retries_remaining = 0;
    update_led_from_state();
  } else if (steering_retries_remaining > 0) {
    sl_zigbee_af_event_set_delay_ms(&steering_retry_event, ROUTER_STEERING_RETRY_DELAY_MS);
  } else {
    steering_active = false;
    update_led_from_state();
  }
}

void sl_zigbee_af_post_attribute_change_cb(uint8_t endpoint,
                                           sl_zigbee_af_cluster_id_t clusterId,
                                           sl_zigbee_af_attribute_id_t attributeId,
                                           uint8_t mask,
                                           uint16_t manufacturerCode,
                                           uint8_t type,
                                           uint8_t size,
                                           uint8_t *value)
{
  (void)manufacturerCode;
  (void)type;
  (void)size;
  (void)value;

  if (endpoint == ROUTER_STATUS_LED_ENDPOINT
      && mask == CLUSTER_MASK_SERVER
      && clusterId == ZCL_ON_OFF_CLUSTER_ID
      && attributeId == ZCL_ON_OFF_ATTRIBUTE_ID) {
    update_led_from_state();
  }
}

void sl_zigbee_af_on_off_cluster_server_post_init_cb(uint8_t endpoint)
{
  if (endpoint == ROUTER_STATUS_LED_ENDPOINT) {
    update_led_from_state();
  }
}

void sl_zigbee_af_identify_start_feedback_cb(uint8_t endpoint, uint16_t identifyTime)
{
  (void)endpoint;
  sl_zigbee_app_debug_println("Identify Start (%u s)", identifyTime);
  identify_active = (identifyTime > 0);
  update_led_from_state();
}

void sl_zigbee_af_identify_stop_feedback_cb(uint8_t endpoint)
{
  (void)endpoint;
  sl_zigbee_app_debug_println("Identify Stop");
  identify_active = false;
  update_led_from_state();
}
