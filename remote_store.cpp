#include "remote_store.h"
#include "globals.h"
#include "ir_tx.h"
#include <LittleFS.h>

JsonDocument remoteConfig;
uint32_t configRevision = 0;
static const char* kConfigFile = "/remote-config.json";
static constexpr size_t kMaxConfigBytes = 32768;

// Keys and mappings share the configuration byte budget, not a global key quota.
// Keep startup validation and mutation validation on the same limits.
static size_t collectionLimit(const char* collection) {
  if (strcmp(collection, "remotes") == 0) return 12;
  if (strcmp(collection, "hosts") == 0) return 8;
  if (strcmp(collection, "macros") == 0) return 50;
  return 0;
}

static JsonObject findItem(const char* collection, const String& id) {
  for (JsonObject item : remoteConfig[collection].as<JsonArray>())
    if (item["id"].as<String>() == id) return item;
  return JsonObject();
}

static String newId() {
  uint32_t next = remoteConfig["nextId"] | 1U;
  remoteConfig["nextId"] = next + 1;
  return String(next);
}

// 从零构造一份默认配置：版本号、空集合、默认设置，以及可选的出厂绑定导入。
// remoteStoreBegin（文件不存在时）与 remoteStoreFactoryReset 共用此路径。
static void resetRemoteConfig() {
  remoteConfig.clear();
  remoteConfig["version"] = 2;
  remoteConfig["nextId"] = 1;
  for (const char* name : {"remotes", "keys", "macros", "mappings", "hosts"})
    remoteConfig[name].to<JsonArray>();
  ensureDefaultSettings();
  loadBindings();
  if (bindingCount) {
    JsonObject remote = remoteConfig["remotes"].as<JsonArray>().add<JsonObject>();
    remote["id"] = newId();
    remote["name"] = "Imported IR remote";
    remote["transport"] = "ir";
    remote["device"] = "";
    String remoteId = remote["id"].as<String>();
    for (uint8_t i = 0; i < bindingCount; ++i) {
      const Binding& b = bindings[i];
      JsonObject key = remoteConfig["keys"].as<JsonArray>().add<JsonObject>();
      String keyId = newId();
      key["id"] = keyId;
      key["remoteId"] = remoteId;
      key["name"] = b.action[0] ? String(b.action) : String(b.code);
      key["proto"] = b.proto;
      String code = b.code;
      while (code.length() < 16) code = "0" + code;
      key["code"] = code;
      JsonObject mapping = remoteConfig["mappings"].as<JsonArray>().add<JsonObject>();
      writeBinding(mapping, b);
      mapping["id"] = newId();
      mapping["keyId"] = keyId;
      mapping["target"] = "ble-hid";
      if (b.type == kTypeMacro) {
        JsonObject macro = remoteConfig["macros"].as<JsonArray>().add<JsonObject>();
        macro["id"] = newId();
        macro["name"] = key["name"];
        macro["steps"] = serialized(b.macro);
        mapping["macroId"] = macro["id"];
        mapping.remove("steps");
      }
    }
    for (uint8_t i = 0; i < bindingCount; ++i) bindings[i] = Binding{};
    bindingCount = 0;
  }
}

// ---------- 设备设置（热点 / 蓝牙名称 / 信号过滤） ----------

static const char* kDefaultApSsid = "Remote Master";
static const char* kDefaultApPassword = "";
static const char* kDefaultBleName = "Remote Master";

void ensureDefaultSettings() {
  JsonObject settings = remoteConfig["settings"].as<JsonObject>();
  if (settings.isNull()) settings = remoteConfig["settings"].to<JsonObject>();
  String ssid = settings["apSsid"] | "";
  String password = settings["apPassword"] | "";
  String name = settings["bleName"] | "";
  if (ssid.isEmpty() || ssid.length() > 31) settings["apSsid"] = kDefaultApSsid;
  // 密码允许为空（开放 AP），或 8-63 字节（WPA2）。1-7 位对 WPA2 无效，回退默认（开放）。
  if (!password.isEmpty() && (password.length() < 8 || password.length() > 63))
    settings["apPassword"] = kDefaultApPassword;
  if (name.isEmpty() || name.length() > 29) settings["bleName"] = kDefaultBleName;
  if (!settings["minPulses"].is<int>() || settings["minPulses"].as<int>() < 0 ||
      settings["minPulses"].as<int>() > 750)
    settings["minPulses"] = 8;
  if (!validIrTxMode(settings["irTxMode"] | ""))
    settings["irTxMode"] = "off";
}

// 红外序列宏：只允许“红外重放 + 等待”步骤，且每个重放步骤引用的
// 按键必须存在并带有已保存的原始脉冲。
static bool irMacroUsable(JsonArrayConst steps, String& error) {
  if (steps.isNull() || steps.size() == 0) {
    error = "IR sequence macro has no steps"; return false;
  }
  for (JsonObjectConst step : steps) {
    const char* kind = step["kind"] | "";
    if (strcmp(kind, "wait") == 0) continue;
    if (strcmp(kind, "ir") != 0) {
      error = "IR sequence macros may only contain IR replay and wait steps"; return false;
    }
    JsonObject key = findItem("keys", step["keyId"].as<String>());
    String capture = key.isNull() ? "" : (key["capturePath"] | "");
    if (key.isNull() || capture.isEmpty()) {
      error = "IR step references a key without captured pulses"; return false;
    }
  }
  return true;
}

static bool settingsCommand(const String& verb, JsonObjectConst args, String& error) {
  if (verb != "save") { error = "Unknown operation"; return false; }
  String apSsid, apPassword, bleName, irTxMode;
  int minPulses = -1;
  bool hasSsid = !args["apSsid"].isNull(), hasPassword = !args["apPassword"].isNull();
  bool hasName = !args["bleName"].isNull(), hasMinPulses = !args["minPulses"].isNull();
  bool hasIrTxMode = args.containsKey("irTxMode");
  if (hasSsid) {
    apSsid = args["apSsid"].as<String>();
    if (apSsid.isEmpty() || apSsid.length() > 31) {
      error = "AP SSID must be 1-31 bytes"; return false;
    }
  }
  if (hasPassword) {
    apPassword = args["apPassword"].as<String>();
    // 允许空字符串（开放 AP）或 8-63 字节（WPA2 要求）。
    if (!apPassword.isEmpty() && (apPassword.length() < 8 || apPassword.length() > 63)) {
      error = "AP password must be empty (open AP) or 8-63 bytes"; return false;
    }
  }
  if (hasName) {
    bleName = args["bleName"].as<String>();
    if (bleName.isEmpty() || bleName.length() > 29) {
      error = "BLE name must be 1-29 bytes"; return false;
    }
  }
  if (hasMinPulses) {
    if (!args["minPulses"].is<int>()) { error = "minPulses must be an integer"; return false; }
    minPulses = args["minPulses"].as<int>();
    if (minPulses < 0 || minPulses > 750) { error = "minPulses must be 0-750"; return false; }
  }
  if (hasIrTxMode) {
    if (!validIrTxMode(args["irTxMode"] | "")) {
      error = "irTxMode must be off, push-pull or sink"; return false;
    }
    irTxMode = args["irTxMode"].as<String>();
  }
  if (!hasSsid && !hasPassword && !hasName && !hasMinPulses && !hasIrTxMode) {
    error = "No settings provided"; return false;
  }
  JsonObject settings = remoteConfig["settings"].as<JsonObject>();
  if (settings.isNull()) settings = remoteConfig["settings"].to<JsonObject>();
  if (hasSsid) settings["apSsid"] = apSsid;
  if (hasPassword) settings["apPassword"] = apPassword;
  if (hasName) settings["bleName"] = bleName;
  if (hasMinPulses) settings["minPulses"] = minPulses;
  if (hasIrTxMode) settings["irTxMode"] = irTxMode;
  return !remoteConfig.overflowed();
}

bool saveRemoteConfig(String* error) {
  auto fail = [error](const char* message) {
    if (error) *error = message;
    return false;
  };
  if (remoteConfig.overflowed()) return fail("Insufficient memory for configuration");
  if (measureJson(remoteConfig) > kMaxConfigBytes)
    return fail("Configuration exceeds 32 KiB; remove unused keys, mappings or macros");
  File file = LittleFS.open("/remote-config.tmp", "w");
  if (!file) return fail("Could not open configuration file for writing");
  size_t written = serializeJson(remoteConfig, file);
  file.flush();
  file.close();
  if (written != measureJson(remoteConfig))
    return fail("Configuration write failed; filesystem may be full");
  if (!LittleFS.rename("/remote-config.tmp", kConfigFile))
    return fail("Configuration file replacement failed");
  ++configRevision;
  return true;
}

bool remoteStoreBegin() {
  File file = LittleFS.open(kConfigFile, "r");
  if (file) {
    auto err = deserializeJson(remoteConfig, file);
    file.close();
    // 不做版本向下兼容。仅当 JSON 解析失败、集合结构非法（非数组或超出
    // 条目上限）或体积超过 32KB 时视为致命问题，由调用方格式化 FS。
    if (err) return false;
    for (const char* name : {"remotes", "keys", "macros", "mappings", "hosts"}) {
      size_t limit = collectionLimit(name);
      if (!remoteConfig[name].is<JsonArray>() ||
          (limit && remoteConfig[name].size() > limit)) return false;
    }
    if (measureJson(remoteConfig) > kMaxConfigBytes) return false;
    ensureDefaultSettings();
    return true;
  }
  resetRemoteConfig();
  return saveRemoteConfig();
}

// 恢复初始状态：格式化整个文件系统（清空前端、配置、红外采样与出厂绑定），
// 重新挂载并重建 /frontend 目录与默认配置。BLE 配对由调用方（bleClearAllBonds）
// 负责清除。格式化后前端页面不可用（回到内置 fallback 页），需重新部署前端。
bool remoteStoreFactoryReset() {
  LittleFS.end();
  if (!LittleFS.format()) return false;
  if (!LittleFS.begin(false)) return false;
  LittleFS.mkdir("/frontend");
  resetRemoteConfig();
  return saveRemoteConfig();
}

// 配置损坏自愈：旧文件改名留档（.bad），重建默认配置并落盘。
// 与 factoryReset 不同，这里不动前端资源与红外采样。
bool remoteStoreRebuild() {
  LittleFS.remove("/remote-config.bad");
  if (LittleFS.exists(kConfigFile)) LittleFS.rename(kConfigFile, "/remote-config.bad");
  resetRemoteConfig();
  return saveRemoteConfig();
}

bool remoteAction(JsonObjectConst mapping, Binding& action) {
  JsonDocument doc;
  doc.set(mapping);
  doc["proto"] = "output";
  doc["code"] = "output";
  if (doc["type"] == kTypeMacro) {
    JsonObject macro = findItem("macros", doc["macroId"].as<String>());
    if (macro.isNull()) return false;
    doc["steps"] = macro["steps"];
  }
  return readBinding(doc.as<JsonObjectConst>(), action);
}

bool resolveRemoteInput(const char* transport, const char* device,
                        const char* proto, const char* code, Binding& action) {
  JsonObjectConst mapping = resolveInputMapping(transport, device, proto, code);
  return !mapping.isNull() && remoteAction(mapping, action);
}

JsonObjectConst resolveInputMapping(const char* transport, const char* device,
                                   const char* proto, const char* code) {
  for (JsonObjectConst key : remoteConfig["keys"].as<JsonArrayConst>()) {
    if (strcmp(key["proto"] | "", proto) || strcmp(key["code"] | "", code)) continue;
    JsonObject remote = findItem("remotes", key["remoteId"].as<String>());
    if (strcmp(remote["transport"] | "", transport) ||
        strcmp(remote["device"] | "", device)) continue;
    for (JsonObjectConst mapping : remoteConfig["mappings"].as<JsonArrayConst>())
      if (mapping["keyId"].as<String>() == key["id"].as<String>())
        return mapping;
  }
  return JsonObjectConst();
}

void writeConfigSummary(JsonObject obj) {
  obj.set(remoteConfig.as<JsonObjectConst>());
}

bool remoteStoreCommand(const String& op, JsonObjectConst args, String& error) {
  int dot = op.indexOf('.');
  String collection = op.substring(0, dot);
  String verb = op.substring(dot + 1);
  if (collection != "remotes" && collection != "keys" && collection != "macros" &&
      collection != "mappings" && collection != "hosts" && collection != "settings") {
    error = "Unknown collection"; return false;
  }
  String id = args["id"] | "";
  if (collection == "settings") return settingsCommand(verb, args, error);
  JsonArray items = remoteConfig[collection].as<JsonArray>();
  JsonObject existing = findItem(collection.c_str(), id);
  if (verb == "delete") {
    if (existing.isNull()) { error = "Item not found"; return false; }
    for (JsonObjectConst m : remoteConfig["mappings"].as<JsonArrayConst>()) {
      if ((collection == "keys" &&
           (m["keyId"].as<String>() == id || m["outKeyId"].as<String>() == id)) ||
          (collection == "macros" && m["macroId"].as<String>() == id)) {
        error = "Remove referencing mappings first"; return false;
      }
    }
    if (collection == "keys")
      // 宏中的红外重放步骤也引用按键，删除前需先移除引用。
      for (JsonObjectConst macro : remoteConfig["macros"].as<JsonArrayConst>())
        for (JsonObjectConst step : macro["steps"].as<JsonArrayConst>())
          if (strcmp(step["kind"] | "", "ir") == 0 &&
              step["keyId"].as<String>() == id) {
            error = "Remove IR replay steps referencing this key first"; return false;
          }
    if (collection == "remotes") {
      // 级联删除前检查：该遥控器下的按键若被映射或宏引用，需要先清理
      // 收集该遥控器下的所有按键 ID
      std::vector<String> keyIds;
      for (JsonObjectConst key : remoteConfig["keys"].as<JsonArrayConst>())
        if (key["remoteId"].as<String>() == id)
          keyIds.push_back(key["id"].as<String>());
      // 检查这些按键是否被任何映射引用
      for (const String& keyId : keyIds) {
        for (JsonObjectConst m : remoteConfig["mappings"].as<JsonArrayConst>()) {
          if (m["keyId"].as<String>() == keyId || m["outKeyId"].as<String>() == keyId) {
            error = "Remove referencing mappings first";
            return false;
          }
        }
      }
      // 检查这些按键是否被任何宏的红外重放步骤引用
      for (const String& keyId : keyIds) {
        for (JsonObjectConst macro : remoteConfig["macros"].as<JsonArrayConst>()) {
          for (JsonObjectConst step : macro["steps"].as<JsonArrayConst>()) {
            if (strcmp(step["kind"] | "", "ir") == 0 && step["keyId"].as<String>() == keyId) {
              error = "Remove IR replay steps referencing this remote's keys first";
              return false;
            }
          }
        }
      }
      // 检查通过，级联删除：收集要删除的按键下标（逆序删除，保持下标有效）
      JsonArray keys = remoteConfig["keys"].as<JsonArray>();
      std::vector<size_t> removeIdx;
      for (size_t i = 0; i < keys.size(); i++)
        if (keys[i]["remoteId"].as<String>() == id)
          removeIdx.push_back(i);
      for (auto it = removeIdx.rbegin(); it != removeIdx.rend(); ++it)
        keys.remove(*it);
      // 删除遥控器本身
      for (size_t i = 0; i < items.size(); i++) {
        if (items[i]["id"].as<String>() == id) {
          items.remove(i);
          break;
        }
      }
      // 采样文件由 ino 按 previous vs current 配置差异自动清理
      return true;
    }
    // Resolve the requested ID explicitly before removing its array element.
    for (auto it = items.begin(); it != items.end(); ++it) {
      if ((*it)["id"].as<String>() == id) {
        items.remove(it);
        return true;
      }
    }
    error = "Item not found";
    return false;
  }
  if (verb != "save") { error = "Unknown operation"; return false; }
  if (!id.isEmpty() && existing.isNull()) { error = "Item not found"; return false; }
  size_t limit = collectionLimit(collection.c_str());
  if (limit && existing.isNull() && items.size() >= limit) {
    error = collection + " limit reached (" + String(limit) + ")";
    return false;
  }
  JsonDocument candidate;
  candidate.set(args);
  JsonObject obj = candidate.as<JsonObject>();
  String name = obj["name"] | "";
  if (collection != "mappings" && (name.isEmpty() || name.length() > 63)) {
    error = "Name must be 1-63 bytes"; return false;
  }
  if (collection == "remotes") {
    // BLE central input is reserved, not advertised as implemented.
    if (strcmp(obj["transport"] | "", "ir") != 0) {
      error = "BLE remote input is reserved for a future transport adapter"; return false;
    }
    obj["device"] = "";
  } else if (collection == "keys") {
    // Capture data is written by firmware (learning flow) or pre-uploaded (import flow).
    if (!existing.isNull()) {
      obj["capturePath"] = existing["capturePath"];
      obj["captureStatus"] = existing["captureStatus"];
    } else if (obj["capturePath"].isNull()) {
      obj.remove("capturePath");
      obj.remove("captureStatus");
    }
    JsonObject remote = findItem("remotes", obj["remoteId"].as<String>());
    String proto = obj["proto"] | "", code = obj["code"] | "";
    if (remote.isNull()) { error = "Remote not found"; return false; }
    if (proto.isEmpty() || proto.length() > 15 || code.isEmpty() || code.length() > 16) {
      error = "Invalid learned input"; return false;
    }
    for (char c : code) if (!isxdigit(static_cast<unsigned char>(c))) {
      error = "Code must be hexadecimal"; return false;
    }
    code.toUpperCase();
    while (code.length() < 16) code = "0" + code;
    obj["code"] = code;
    // 查重只限定在同一遥控器内：不同遥控器发出相同的红外码非常常见
    // （同一协议的通用遥控器、同型号设备等），跨遥控器允许共码。
    for (JsonObjectConst k : items) {
      if (k["id"].as<String>() != id &&
          k["remoteId"].as<String>() == obj["remoteId"].as<String>() &&
          strcmp(k["proto"] | "", proto.c_str()) == 0 &&
          strcmp(k["code"] | "", code.c_str()) == 0) {
        error = "This IR signal already belongs to a key on this remote"; return false;
      }
    }
  } else if (collection == "macros") {
    if (!validMacro(obj["steps"].as<JsonArrayConst>())) {
      error = "Invalid macro: 12 steps, 512 ASCII bytes, 60 seconds maximum"; return false;
    }
    size_t bytes = measureJson(obj["steps"]);
    for (JsonObjectConst m : items)
      if (m["id"].as<String>() != id) bytes += measureJson(m["steps"]);
    if (bytes > kMaxMacroStorage) { error = "Macro storage full"; return false; }
  } else if (collection == "mappings") {
    const char* target = obj["target"] | "ble-hid";
    bool irTarget = strcmp(target, "ir-send") == 0;
    if (!irTarget && strcmp(target, "ble-hid") != 0) {
      error = "Unknown output target"; return false;
    }
    obj["target"] = irTarget ? "ir-send" : "ble-hid";
    if (findItem("keys", obj["keyId"].as<String>()).isNull()) {
      error = "Mapping input key not found"; return false;
    }
    int type = obj["type"] | -1;
    if (!irTarget) {
      // 蓝牙目标不允许混入红外重放步骤。
      if (type == 3) {
        JsonObject macro = findItem("macros", obj["macroId"].as<String>());
        if (macro.isNull()) { error = "Invalid mapping"; return false; }
        JsonArrayConst steps = macro["steps"].as<JsonArrayConst>();
        for (JsonObjectConst step : steps)
          if (strcmp(step["kind"] | "", "ir") == 0) {
            error = "Bluetooth macros cannot contain IR replay steps"; return false;
          }
      }
      Binding action;
      if (type < 0 || type > 3 || !remoteAction(obj, action)) {
        error = "Invalid mapping"; return false;
      }
    } else {
      // 红外发射目标必须引用完整采样或纯红外序列。
      if (type == 4) {
        JsonObject outKey = findItem("keys", obj["outKeyId"].as<String>());
        String capture = outKey.isNull() ? "" : (outKey["capturePath"] | "");
        if (outKey.isNull() || capture.isEmpty()) {
          error = "IR replay needs a learned key with captured pulses"; return false;
        }
      } else if (type == 3) {
        JsonObject macro = findItem("macros", obj["macroId"].as<String>());
        if (macro.isNull()) { error = "Macro not found"; return false; }
        if (!irMacroUsable(macro["steps"].as<JsonArrayConst>(), error)) return false;
      } else {
        error = "IR mappings replay one key (type 4) or a macro (type 3)"; return false;
      }
    }
    for (JsonObjectConst m : items)
      if (m["id"].as<String>() != id && m["keyId"].as<String>() == obj["keyId"].as<String>()) {
        error = "A key can have only one mapping"; return false;
      }
  } else if (collection == "hosts") {
    if (existing.isNull()) { error = "Hosts are created by BLE pairing"; return false; }
    // Identity and bond information cannot be supplied by the browser.
    obj["address"] = existing["address"];
    obj["addressType"] = existing["addressType"];
    // priority / enabled 不再影响重连逻辑，保留字段仅作显示兼容
    if (!obj["priority"].is<int>()) obj["priority"] = 0;
    if (!obj["enabled"].is<bool>()) obj["enabled"] = true;
  }
  obj["id"] = id.isEmpty() ? newId() : id;
  obj.remove("captureId");
  if (existing.isNull()) items.add(obj);
  else existing.set(obj);
  return !remoteConfig.overflowed();
}
