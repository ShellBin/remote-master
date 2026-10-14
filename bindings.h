#ifndef BINDINGS_H
#define BINDINGS_H

#include <Arduino.h>
#include <ArduinoJson.h>

// ==================== 常量 ====================
constexpr const char* kBindingsFile = "/ir_bindings.json";
const uint8_t kMaxBindings = 50;
const uint8_t kMaxMacroSteps = 12;
const size_t kMaxMacroBytes = 3072;
const size_t kMaxMacroStorage = 16384;
const size_t kMaxTextBytes = 512;
const uint32_t kMaxMacroDurationMs = 60000;

// 绑定类型
constexpr uint8_t kTypeKeyboard = 0;
constexpr uint8_t kTypeMouse    = 1;
constexpr uint8_t kTypeMedia    = 2;
constexpr uint8_t kTypeMacro    = 3;

// ==================== 绑定表结构 ====================
struct Binding {
  char proto[16];
  char code[32];
  uint8_t type;
  char action[64];
  uint8_t modifiers;
  uint8_t key;
  uint8_t mouseButton;
  int8_t  mouseX;
  int8_t  mouseY;
  int8_t  mouseWheel;
  uint16_t mediaCode;
  String macro;  // 压缩后的 JSON steps 字符串
};

// ==================== 绑定表操作 ====================
String bindingKey(const char* proto, const char* code);
int findBinding(const char* proto, const char* code);

// ==================== 序列化 / 反序列化 ====================
bool readBinding(JsonObjectConst obj, Binding& b);
void writeBinding(JsonObject obj, const Binding& b);

// ==================== 持久化 ====================
size_t macroStorageBytes();
void loadBindings();
bool saveBindings();

// ==================== 宏校验 ====================
bool validMacro(JsonArrayConst steps);

#endif // BINDINGS_H
