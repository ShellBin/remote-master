#include "action_router.h"
#include "remote_store.h"
#include "ble_hid.h"
#include "ir_tx.h"

bool dispatchAction(JsonObjectConst descriptor) {
  String error;
  return dispatchAction(descriptor, error);
}

bool dispatchAction(JsonObjectConst descriptor, String& error, bool repeat) {
  const char* target = descriptor["target"] | "ble-hid";
  if (strcmp(target, "ir-send") == 0) return irTxExecute(descriptor, error);
  // 注意：BLE 目标不再检查 irTxBusy()——IR 发射走 RMT DMA，与 BLE notify
  // 硬件独立，交叉阻塞会让 ~90ms 的发射窗口吞掉按键。
  Binding binding;
  if (!remoteAction(descriptor, binding) || !executeBinding(binding, repeat)) {
    error = "BLE must be encrypted, report subscribed, and HID idle"; return false;
  }
  return true;
}

void dispatchInput(const char* transport, const char* device,
                   const char* proto, const char* code, bool repeat) {
  JsonObjectConst mapping = resolveInputMapping(transport, device, proto, code);
  if (mapping.isNull() || (repeat && mapping["type"] == kTypeMacro)) return;
  // repeat 必须透传：长按延续帧与重新点击在 executeBinding 里的处理不同。
  String error;
  dispatchAction(mapping, error, repeat);
}
