#include "bindings.h"
#include "globals.h"
#include <LittleFS.h>

// ==================== 绑定表管理 ====================

String bindingKey(const char* proto, const char* code) {
  String k = proto;
  k += ":";
  k += code;
  return k;
}

int findBinding(const char* proto, const char* code) {
  String key = bindingKey(proto, code);
  for (uint8_t i = 0; i < bindingCount; i++) {
    if (bindingKey(bindings[i].proto, bindings[i].code) == key) return i;
  }
  return -1;
}

bool readBinding(JsonObjectConst obj, Binding& b) {
  b = Binding{};
  strlcpy(b.proto, obj["proto"] | "", sizeof(b.proto));
  strlcpy(b.code, obj["code"] | "", sizeof(b.code));
  int type = obj["type"] | -1;
  if (!b.proto[0] || !b.code[0] || type < 0 || type > 3) return false;
  b.type = type;
  strlcpy(b.action, obj["action"] | "", sizeof(b.action));
  b.modifiers = obj["modifiers"] | 0;
  b.key = obj["key"] | 0;
  b.mouseButton = obj["mouseButton"] | 0;
  b.mouseX = obj["mouseX"] | 0;
  b.mouseY = obj["mouseY"] | 0;
  b.mouseWheel = obj["mouseWheel"] | 0;
  b.mediaCode = obj["mediaCode"] | 0;
  if (b.type == kTypeKeyboard) {
    int key = obj["key"] | 0, mods = obj["modifiers"] | 0;
    if (key < 0 || key > 115 || (key > 0 && key < 4) ||
        mods < 0 || mods > 255 || (!key && !mods)) return false;
  } else if (b.type == kTypeMouse) {
    int button = obj["mouseButton"] | 0;
    int x = obj["mouseX"] | 0, y = obj["mouseY"] | 0, wheel = obj["mouseWheel"] | 0;
    if (button < 0 || button > 7 || x < -127 || x > 127 ||
        y < -127 || y > 127 || wheel < -127 || wheel > 127 ||
        (!button && !x && !y && !wheel)) return false;
  } else if (b.type == kTypeMedia) {
    int code = obj["mediaCode"] | 0;
    if (code < 1 || code > 1023) return false;
  }
  if (b.type == kTypeMacro) {
    JsonArrayConst steps = obj["steps"];
    if (!validMacro(steps)) return false;
    if (serializeJson(steps, b.macro) != measureJson(steps)) return false;
  }
  return true;
}

void writeBinding(JsonObject obj, const Binding& b) {
  obj["proto"] = b.proto;
  obj["code"] = b.code;
  obj["type"] = b.type;
  obj["action"] = b.action;
  obj["modifiers"] = b.modifiers;
  obj["key"] = b.key;
  obj["mouseButton"] = b.mouseButton;
  obj["mouseX"] = b.mouseX;
  obj["mouseY"] = b.mouseY;
  obj["mouseWheel"] = b.mouseWheel;
  obj["mediaCode"] = b.mediaCode;
  if (b.type == kTypeMacro) obj["steps"] = serialized(b.macro);
}

size_t macroStorageBytes() {
  size_t bytes = 0;
  for (uint8_t i = 0; i < bindingCount; i++) bytes += bindings[i].macro.length();
  return bytes;
}

void loadBindings() {
  bindingCount = 0;
  if (!LittleFS.exists(kBindingsFile)) return;

  File f = LittleFS.open(kBindingsFile, "r");
  if (!f) return;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return;

  JsonArray arr = doc.as<JsonArray>();
  if (!arr) return;

  for (JsonObject obj : arr) {
    if (bindingCount >= kMaxBindings) break;
    Binding& b = bindings[bindingCount];
    if (!readBinding(obj, b) || macroStorageBytes() + b.macro.length() > kMaxMacroStorage) {
      b = Binding{};
      Serial.println(F("[WARN] 跳过无效或超限的绑定"));
      continue;
    }
    bindingCount++;
  }
}

bool saveBindings() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();

  for (uint8_t i = 0; i < bindingCount; i++) {
    writeBinding(arr.add<JsonObject>(), bindings[i]);
  }

  if (doc.overflowed()) return false;
  const char* tempFile = "/ir_bindings.tmp";
  File f = LittleFS.open(tempFile, "w");
  if (!f) return false;
  size_t expected = measureJson(doc);
  size_t written = serializeJson(doc, f);
  f.close();
  if (written != expected) { LittleFS.remove(tempFile); return false; }
  return LittleFS.rename(tempFile, kBindingsFile);
}

// ==================== 宏校验 ====================

bool validMacro(JsonArrayConst steps) {
  if (steps.isNull() || steps.size() == 0 || steps.size() > kMaxMacroSteps ||
      measureJson(steps) > kMaxMacroBytes) return false;
  size_t textBytes = 0;
  uint32_t duration = 0;
  for (JsonObjectConst step : steps) {
    const char* kind = step["kind"] | "";
    if (strcmp(kind, "key") == 0) {
      if (!step["key"].is<int>() || !step["modifiers"].is<int>()) return false;
      int key = step["key"], mods = step["modifiers"];
      if ((key != 0 && (key < 4 || key > 115)) || mods < 0 || mods > 255 ||
          (key == 0 && mods == 0)) return false;
      duration += 70;
    } else if (strcmp(kind, "text") == 0) {
      if (!step["text"].is<const char*>()) return false;
      JsonString text = step["text"].as<JsonString>();
      if (!text.size()) return false;
      for (size_t i = 0; i < text.size(); i++) {
        uint8_t c = text.c_str()[i];
        if (c < 32 || c > 126) return false;
      }
      textBytes += text.size();
      duration += text.size() * 30;
    } else if (strcmp(kind, "wait") == 0) {
      if (!step["ms"].is<int>()) return false;
      int ms = step["ms"];
      if (ms < 0 || ms > 30000) return false;
      duration += ms;
    } else if (strcmp(kind, "ir") == 0) {
      // 此处校验结构，IR 执行器还会检查采样及实际帧时长。
      if (!step["keyId"].is<const char*>()) return false;
      JsonString keyId = step["keyId"].as<JsonString>();
      if (!keyId.size() || keyId.size() > 16) return false;
      duration += 150;  // 估算：重放一帧 + 按键间隔
    } else return false;
  }
  return textBytes <= kMaxTextBytes && duration <= kMaxMacroDurationMs;
}
