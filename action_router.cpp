#include "action_router.h"
#include "remote_store.h"
#include "ble_hid.h"
#include "ir_tx.h"

bool dispatchAction(JsonObjectConst descriptor) {
  String error;
  return dispatchAction(descriptor, error);
}

bool dispatchAction(JsonObjectConst descriptor, String& error) {
  const char* target = descriptor["target"] | "ble-hid";
  if (strcmp(target, "ir-send") == 0) return irTxExecute(descriptor, error);
  if (irTxBusy()) { error = "An IR action is already running"; return false; }
  Binding binding;
  if (!remoteAction(descriptor, binding) || !executeBinding(binding)) {
    error = "BLE must be encrypted, report subscribed, and HID idle"; return false;
  }
  return true;
}

void dispatchInput(const char* transport, const char* device,
                   const char* proto, const char* code, bool repeat) {
  JsonObjectConst mapping = resolveInputMapping(transport, device, proto, code);
  if (mapping.isNull() || (repeat && mapping["type"] == kTypeMacro)) return;
  dispatchAction(mapping);
}
