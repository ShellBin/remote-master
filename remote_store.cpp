#include "remote_store.h"
#include "globals.h"
#include <LittleFS.h>

JsonDocument remoteConfig;
static const char* kConfigFile = "/remote-config.json";

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

// ---------- 设备设置（热点 / 蓝牙名称 / 信号过滤） ----------

static const char* kDefaultApSsid = "IR-C3-Remote";
static const char* kDefaultApPassword = "irremote123";
static const char* kDefaultBleName = "ESP32-C3 IR Remote";

void ensureDefaultSettings() {
  JsonObject settings = remoteConfig["settings"].to<JsonObject>();
  String ssid = settings["apSsid"] | "";
  String password = settings["apPassword"] | "";
  String name = settings["bleName"] | "";
  if (ssid.isEmpty() || ssid.length() > 31) settings["apSsid"] = kDefaultApSsid;
  if (password.length() < 8 || password.length() > 63) settings["apPassword"] = kDefaultApPassword;
  if (name.isEmpty() || name.length() > 29) settings["bleName"] = kDefaultBleName;
  if (!settings["minPulses"].is<int>() || settings["minPulses"].as<int>() < 0 ||
      settings["minPulses"].as<int>() > 750)
    settings["minPulses"] = 8;
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
  String apSsid, apPassword, bleName;
  int minPulses = -1;
  bool hasSsid = !args["apSsid"].isNull(), hasPassword = !args["apPassword"].isNull();
  bool hasName = !args["bleName"].isNull(), hasMinPulses = !args["minPulses"].isNull();
  if (hasSsid) {
    apSsid = args["apSsid"].as<String>();
    if (apSsid.isEmpty() || apSsid.length() > 31) {
      error = "AP SSID must be 1-31 bytes"; return false;
    }
  }
  if (hasPassword) {
    apPassword = args["apPassword"].as<String>();
    if (apPassword.length() < 8 || apPassword.length() > 63) {
      error = "AP password must be 8-63 bytes"; return false;
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
  if (!hasSsid && !hasPassword && !hasName && !hasMinPulses) {
    error = "No settings provided"; return false;
  }
  JsonObject settings = remoteConfig["settings"].to<JsonObject>();
  if (hasSsid) settings["apSsid"] = apSsid;
  if (hasPassword) settings["apPassword"] = apPassword;
  if (hasName) settings["bleName"] = bleName;
  if (hasMinPulses) settings["minPulses"] = minPulses;
  return !remoteConfig.overflowed();
}

bool saveRemoteConfig() {
  if (remoteConfig.overflowed() || measureJson(remoteConfig) > 32768) return false;
  File file = LittleFS.open("/remote-config.tmp", "w");
  if (!file) return false;
  size_t written = serializeJson(remoteConfig, file);
  file.flush();
  file.close();
  if (written != measureJson(remoteConfig)) return false;
  return LittleFS.rename("/remote-config.tmp", kConfigFile);
}

bool remoteStoreBegin() {
  File file = LittleFS.open(kConfigFile, "r");
  if (file) {
    auto err = deserializeJson(remoteConfig, file);
    file.close();
    if (err || remoteConfig["version"] != 2) return false;
    for (const char* name : {"remotes", "keys", "macros", "mappings", "hosts"}) {
      size_t limit = strcmp(name, "hosts") == 0 ? 8 : strcmp(name, "remotes") == 0 ? 12 : 50;
      if (!remoteConfig[name].is<JsonArray>() || remoteConfig[name].size() > limit) return false;
    }
    if (measureJson(remoteConfig) > 32768) return false;
    ensureDefaultSettings();
    return true;
  }
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
    if (collection == "remotes")
      for (JsonObjectConst k : remoteConfig["keys"].as<JsonArrayConst>())
        if (k["remoteId"].as<String>() == id) {
          error = "Remove remote keys first"; return false;
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
  size_t limit = collection == "remotes" ? 12 : collection == "hosts" ? 8 : 50;
  if (existing.isNull() && items.size() >= limit) { error = "Collection full"; return false; }
  JsonDocument candidate;
  candidate.set(args);
  JsonObject obj = candidate.as<JsonObject>();
  String name = obj["name"] | "";
  if (collection != "mappings" && (name.isEmpty() || name.length() > 63)) {
    error = "Name must be 1-63 bytes"; return false;
  }
  if (collection == "remotes") {
    // BLE central input is reserved, not advertised as implemented.
    if (obj["transport"].as<String>() != "ir") {
      error = "BLE remote input is reserved for a future transport adapter"; return false;
    }
    obj["device"] = "";
  } else if (collection == "keys") {
    // Capture data is written by firmware, not echoed back from browser input.
    if (!existing.isNull()) {
      obj["capturePath"] = existing["capturePath"];
      obj["captureStatus"] = existing["captureStatus"];
    } else {
      obj.remove("capturePath");
      obj.remove("captureStatus");
    }
    JsonObject remote = findItem("remotes", obj["remoteId"].as<String>());
    String proto = obj["proto"] | "", code = obj["code"] | "";
    if (remote.isNull() || proto.isEmpty() || proto.length() > 15 ||
        code.isEmpty() || code.length() > 16) {
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
          k["proto"].as<String>() == proto && k["code"].as<String>() == code) {
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
        for (JsonObjectConst step : macro["steps"].as<JsonArrayConst>())
          if (strcmp(step["kind"] | "", "ir") == 0) {
            error = "Bluetooth macros cannot contain IR replay steps"; return false;
          }
      }
      Binding action;
      if (type < 0 || type > 3 || !remoteAction(obj, action)) {
        error = "Invalid mapping"; return false;
      }
    } else {
      // 红外发射目标：发射管硬件就绪前只保存不执行（第三期实现重放）。
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
    if (!obj["priority"].is<int>() || obj["priority"].as<int>() < 0 ||
        obj["priority"].as<int>() > 99 || !obj["enabled"].is<bool>()) {
      error = "Invalid host priority or enabled flag"; return false;
    }
  }
  obj["id"] = id.isEmpty() ? newId() : id;
  obj.remove("captureId");
  if (existing.isNull()) items.add(obj);
  else existing.set(obj);
  return !remoteConfig.overflowed();
}
