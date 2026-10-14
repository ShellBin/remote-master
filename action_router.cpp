#include "action_router.h"
#include "remote_store.h"
#include "ble_hid.h"

bool dispatchAction(JsonObjectConst descriptor) {
  const char* target = descriptor["target"] | "ble-hid";
  if (strcmp(target, "ir-send") == 0) {
    // 红外发射需要发射管硬件（第三期）；映射可保存、可展示，执行留待硬件就绪。
    Serial.println(F("[IR] transmit requested but the IR LED is not available yet"));
    return false;
  }
  Binding binding;
  return remoteAction(descriptor, binding) && executeBinding(binding);
}

void dispatchInput(const char* transport, const char* device,
                   const char* proto, const char* code, bool repeat) {
  JsonObjectConst mapping = resolveInputMapping(transport, device, proto, code);
  if (mapping.isNull() || (repeat && mapping["type"] == kTypeMacro)) return;
  dispatchAction(mapping);
}
