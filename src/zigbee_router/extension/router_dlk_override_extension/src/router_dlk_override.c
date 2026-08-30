#include "app/framework/include/af.h"

// The real (weak) internal function from libzigbee-dynamic-commissioning.a
extern void __real_slxi_zigbee_stack_gu_zdo_dlk_override_supported_params(
    uint8_t *method_mask, uint8_t *secret_mask);

// Our wrapper: add well-known key (bit 7) to the supported secrets bitmask
void __wrap_slxi_zigbee_stack_gu_zdo_dlk_override_supported_params(
    uint8_t *method_mask, uint8_t *secret_mask)
{
  // Call the original (does nothing in the default weak stub)
  __real_slxi_zigbee_stack_gu_zdo_dlk_override_supported_params(
      method_mask, secret_mask);

  // Add well-known key (ZigBeeAlliance09) support
  *secret_mask |= 0x80;

  sl_zigbee_af_app_println("DLK OVERRIDE: methods=0x%X secrets=0x%X",
                           *method_mask, *secret_mask);
}
