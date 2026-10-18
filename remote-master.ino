/*
 * ESP32-C3 IR input -> BLE HID output.
 * Arduino ESP32 3.x, ArduinoJson 7.x, IRremote 4.x,
 * NimBLE-Arduino 2.5.x, WebSockets 2.x.
 * Upload data/ separately as a LittleFS image.
 */
#include <WiFi.h>
#include <esp_wifi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#define RAW_BUFFER_LENGTH 750
#define RECORD_GAP_MICROS 12000
#include <IRremote.hpp>
#include <LittleFS.h>
#include "bindings.h"
#include "ble_hid.h"
#include "globals.h"
#include "remote_store.h"
#include "file_api.h"
#include "action_router.h"
#include "ir_tx.h"

static const uint8_t kIrDataPin = 5, kSensorGroundPin = 6, kSensorVccPin = 7, kLedPin = 8;
static const char kApSsid[] = "Remote Master";
static const char kApPassword[] = "";
WebServer server(80);
WebSocketsServer sockets(81);

// 热点名称与密码来自 remoteConfig["settings"]，常量仅作缺省回退。
static void applyAccessPoint() {
  String ssid = remoteConfig["settings"]["apSsid"] | kApSsid;
  String password = remoteConfig["settings"]["apPassword"] | kApPassword;
  // WPA2 密码最短 8 位：1~7 位的密码会让 softAP 静默失败（返回 false），
  // 此时回退为开放热点并在串口给出明确提示，避免"搜得到却连不上"。
  bool ok;
  if (password.length() > 0 && password.length() < 8) {
    Serial.printf("[WiFi] apPassword length %u < 8 is invalid; starting OPEN ap\n",
                  (unsigned)password.length());
    ok = WiFi.softAP(ssid.c_str(), nullptr, 6, false, 4);
  } else {
    ok = WiFi.softAP(ssid.c_str(), password.c_str(), 6, false, 4);
  }
  Serial.printf("[WiFi] AP %s: ssid=\"%s\" secured=%u ip=%s freeHeap=%lu\n",
                ok ? "started" : "FAILED", ssid.c_str(),
                password.length() >= 8 ? 1 : 0,
                WiFi.softAPIP().toString().c_str(),
                (unsigned long)ESP.getFreeHeap());
}

// WiFi 事件日志：客户端关联/离开/获址可见，断开原因码（如 15=四次握手
// 超时）直接指示"连不上"的阶段。无输出说明客户端根本没摸到设备。
static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_AP_START:
      Serial.println("[WiFi] event: AP start"); break;
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      Serial.printf("[WiFi] event: station joined aid=%u\n", info.wifi_ap_staconnected.aid); break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      Serial.printf("[WiFi] event: station left aid=%u reason=%u\n",
                    info.wifi_ap_stadisconnected.aid, info.wifi_ap_stadisconnected.reason); break;
    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
      Serial.println("[WiFi] event: station got IP"); break;
    default: break;
  }
}
Binding bindings[kMaxBindings];
uint8_t bindingCount = 0;
bool learningMode = false, hidRunning = false, macroRunning = false, hidNeedsRelease = false;
uint32_t frameCount = 0, hidDeadline = 0, hidStarted = 0;
char lastProto[16] = "", lastCode[32] = "";
size_t macroStep = 0, macroTextOffset = 0;
JsonDocument runningSteps;
static String learningRemote;
static uint32_t learningUntil = 0, lastFrameAt = 0, ledUntil = 0;
static bool storageReady = false;
static bool stateDirty = false;
static bool pendingFactoryReset = false;  // system.reset 延迟到 loop() 执行
static JsonDocument pendingCapture;
static String pendingCaptureId;
static String pendingCaptureRemote;
static bool filesystemMounted = false;

void irTxPauseReceiver(bool paused) {
  // Preserve external-key repeat history; reception is stopped through TX and settling.
  if (paused) IrReceiver.stop();
  else IrReceiver.start();
}

void socketSend(uint8_t client, JsonDocument& doc) {
  String message;
  serializeJson(doc, message);
  sockets.sendTXT(client, message);
}

// 差分推送：追踪上次推送的状态，只发送变化的部分。
// config 变化用 configRevision 版本号判断（saveRemoteConfig 成功落盘 +1），
// 不再每次推送都对整个配置做深拷贝 + 全文序列化比较（旧实现一次推送峰值
// 堆占用超过 200KB，C3 上随时可能 OOM）。
static uint32_t lastConfigRevision = UINT32_MAX;
static uint32_t lastFrames = UINT32_MAX;
static uint32_t lastHeap = UINT32_MAX;
static uint32_t heapCheckedAt = 0;
static bool lastStorageReady = true;
static bool lastHidRunning = true;
static bool lastIrTxRunning = true;
static uint32_t lastFsTotal = UINT32_MAX;
static uint32_t lastFsUsed = UINT32_MAX;
static String lastBleJson;
static String lastLearningRemote;

void pushState(int client = -1) {
  // 优化：无 WebSocket 客户端时不序列化
  if (client < 0 && sockets.connectedClients() == 0) { stateDirty = false; return; }
  // 优化：节流，每 50ms 最多推送一次
  static uint32_t lastPush = 0;
  uint32_t now = millis();
  if (client < 0 && now - lastPush < 50) return;
  lastPush = now;

  // 首次连接或显式请求（client >= 0）强制全量 config
  bool configChanged = (configRevision != lastConfigRevision || client >= 0);
  if (configChanged) lastConfigRevision = configRevision;

  // 构建当前 status（只包含变化的字段）
  JsonDocument statusDoc;
  bool statusChanged = false;

  if (learningRemote != lastLearningRemote) { statusDoc["learningRemote"] = learningRemote; lastLearningRemote = learningRemote; statusChanged = true; }
  if (frameCount != lastFrames) { statusDoc["frames"] = frameCount; lastFrames = frameCount; statusChanged = true; }
  // heap 几乎每次读取都不同，降频到 1s 且变化超过 2KB 才上报，
  // 否则 statusChanged 恒真，每次推送都变成全量广播。
  if (now - heapCheckedAt >= 1000) {
    heapCheckedAt = now;
    uint32_t heap = ESP.getFreeHeap();
    if (lastHeap == UINT32_MAX || heap > lastHeap + 2048 || lastHeap > heap + 2048) {
      statusDoc["heap"] = heap; lastHeap = heap; statusChanged = true;
    }
  }
  if (storageReady != lastStorageReady) { statusDoc["storageReady"] = storageReady; lastStorageReady = storageReady; statusChanged = true; }
  if (hidRunning != lastHidRunning) { statusDoc["hidRunning"] = hidRunning; lastHidRunning = hidRunning; statusChanged = true; }
  bool irTx = irTxBusy();
  if (irTx != lastIrTxRunning) { statusDoc["irTxRunning"] = irTx; lastIrTxRunning = irTx; statusChanged = true; }
  // FS 用量走缓存：usedBytes() 是 O(整个分区) 的 flash 遍历，不能在高频路径上调
  uint32_t fsTotal, fsUsed;
  fsUsageCached(fsTotal, fsUsed);
  if (fsTotal != lastFsTotal) { statusDoc["fsTotal"] = fsTotal; lastFsTotal = fsTotal; statusChanged = true; }
  if (fsUsed != lastFsUsed) { statusDoc["fsUsed"] = fsUsed; lastFsUsed = fsUsed; statusChanged = true; }

  // BLE 状态比较（约 200 字节的小文档，序列化开销可接受）
  JsonDocument bleDoc;
  bleHostStatus(bleDoc.to<JsonObject>());
  String bleJson;
  serializeJson(bleDoc, bleJson);
  if (bleJson != lastBleJson) { statusDoc["ble"].set(bleDoc.as<JsonObjectConst>()); lastBleJson = bleJson; statusChanged = true; }

  // 没有任何变化则跳过（显式客户端请求除外）
  if (!configChanged && !statusChanged && client < 0) return;

  // 组装最终消息：config 只深拷贝这一次，直接进最终文档
  JsonDocument doc;
  doc["event"] = "state";
  if (configChanged) doc["config"].set(remoteConfig.as<JsonObjectConst>());
  if (statusChanged) doc["status"].set(statusDoc.as<JsonObjectConst>());

  String message;
  serializeJson(doc, message);
  if (client < 0) sockets.broadcastTXT(message);
  else sockets.sendTXT(client, message);
}

void socketEvent(uint8_t client, WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_CONNECTED) { pushState(client); return; }
  if (type != WStype_TEXT) return;
  JsonDocument request, response;
  String error;
  bool applyApAfter = false, applyBleNameAfter = false;
  if (length > 8192 || deserializeJson(request, payload, length)) error = "Invalid or oversized JSON";
  response["id"] = request["id"];
  String op = request["op"] | "";
  JsonObjectConst args = request["args"].as<JsonObjectConst>();
  if (error.isEmpty()) {
    if (op == "state.get") {
      pushState(client);
      response["ok"] = true;
      socketSend(client, response);
      return;
    } else if (op == "learn.start") {
      String id = args["remoteId"] | "";
      bool found = false;
      for (JsonObjectConst remote : remoteConfig["remotes"].as<JsonArrayConst>())
        if (remote["id"].as<String>() == id && strcmp(remote["transport"] | "", "ir") == 0) found = true;
      if (!found) error = "Select an IR remote first";
      else {
        irTxStop();
        stopHid("learning");
        learningRemote = id;
        learningMode = true;
        learningUntil = millis() + 300000;
        stateDirty = true;
      }
    } else if (op == "learn.stop" || op == "hid.stop") {
      irTxStop();
      stopHid("user");
      learningMode = false; learningRemote = ""; stateDirty = true;
    } else if (op == "keys.send") {
      if (irTxSendKey(args["id"] | "", error)) stateDirty = true;
    } else if (op == "hosts.pair") {
      blePairingOpen(); stateDirty = true;
    } else if (op == "hosts.reconnect") {
      stopHid("switch-host");
      String hostId = args["id"] | "";
      if (hostId.isEmpty()) error = "Host id required";
      else {
        bool found = false;
        for (JsonObjectConst host : remoteConfig["hosts"].as<JsonArrayConst>())
          if (host["id"].as<String>() == hostId) {
            bleReconnectToHost(host["address"].as<String>(), host["addressType"] | 0);
            found = true;
          }
        if (!found) error = "Host not found";
        stateDirty = true;
      }
    } else if (op == "mappings.test" || op == "macros.test") {
      if (learningMode) error = "Stop learning before testing";
      else {
        bool found = false;
        JsonDocument descriptor;
        if (op == "macros.test") {
          for (JsonObjectConst macro : remoteConfig["macros"].as<JsonArrayConst>()) {
            if (macro["id"].as<String>() != args["id"].as<String>()) continue;
            found = true;
            descriptor["type"] = kTypeMacro;
            descriptor["macroId"] = macro["id"];
            for (JsonObjectConst step : macro["steps"].as<JsonArrayConst>())
              if (strcmp(step["kind"] | "", "ir") == 0) descriptor["target"] = "ir-send";
          }
        } else {
          for (JsonObjectConst mapping : remoteConfig["mappings"].as<JsonArrayConst>()) {
            if (mapping["id"].as<String>() != args["id"].as<String>()) continue;
            found = true;
            descriptor.set(mapping);
          }
        }
        if (!found) error = "Item not found";
        else if (dispatchAction(descriptor.as<JsonObjectConst>(), error)) stateDirty = true;
      }
    } else if (op == "system.reset") {
      if (!filesystemMounted) error = "Filesystem unavailable";
      else if (pendingFactoryReset) error = "Reset already in progress";
      else {
        irTxApplyMode("off");
        stopHid("reset");
        learningMode = false; learningRemote = "";
        // 格式化整个 FS 需要数秒，不能在 WS 回调里同步执行（会杀死心跳、
        // 前端必然超时）。先回包，由 loop() 在下一轮执行真正的重置。
        pendingFactoryReset = true;
      }
    } else if (!storageReady) {
      error = "Storage unavailable; configuration is read-only";
    } else {
      // A failed mutation/save restores the complete previous configuration.
      JsonDocument previous;
      previous.set(remoteConfig);
      if (previous.overflowed()) {
        response["ok"] = false;
        response["error"] = "Insufficient memory to safely update configuration";
        socketSend(client, response);
        return;
      }

      // 批量保存按键：一次性验证并写入多个按键，只落盘一次
      if (op == "keys.batchSave") {
        JsonArrayConst batchKeys = args["keys"].as<JsonArrayConst>();
        Serial.printf("[CMD] keys.batchSave: %u keys\n", batchKeys.isNull() ? 0 : batchKeys.size());
        if (batchKeys.isNull() || batchKeys.size() == 0) {
          error = "No keys provided";
        } else {
          // 逐个验证 capture 文件存在性并执行内存中的保存
          for (JsonObjectConst keyArgs : batchKeys) {
            if (!error.isEmpty()) break;
            // 验证 capturePath（导入流程：有 capturePath 无 id）
            String capPath = keyArgs["capturePath"] | "";
            if (!capPath.isEmpty() && !keyArgs.containsKey("id")) {
              if (!LittleFS.exists(capPath)) {
                error = "Capture file not found: " + capPath;
                Serial.printf("[CMD] batchSave error: %s\n", error.c_str());
                break;
              }
            }
            // 调用 remoteStoreCommand 修改内存中的配置（不落盘）
            String keyError;
            if (!remoteStoreCommand("keys.save", keyArgs, keyError)) {
              error = keyError.isEmpty() ? "Key save failed" : keyError;
              Serial.printf("[CMD] batchSave key error: %s\n", error.c_str());
            }
          }
          // 所有按键验证通过后，一次性写入配置文件
          if (error.isEmpty()) {
            if (!saveRemoteConfig(&error)) {
              if (error.isEmpty()) error = "Configuration save failed";
              Serial.printf("[CMD] batchSave config save error: %s\n", error.c_str());
              remoteConfig.set(previous);
            } else {
              Serial.printf("[CMD] batchSave success: saved config\n");
              stateDirty = true;
            }
          } else {
            remoteConfig.set(previous);
          }
        }
      } else {
      String captureFile;
      if (op == "keys.save") {
        // 导入流程：有 capturePath 无 id（新按键，采样文件已预上传）
        if (!args["capturePath"].as<String>().isEmpty() && !args.containsKey("id")) {
          if (!LittleFS.exists(args["capturePath"].as<String>())) {
            error = "Capture file not found at specified path";
          }
        }
        // 学习流程：有 captureId（刚学到的信号）
        else if (args.containsKey("captureId") && !args["captureId"].as<String>().isEmpty()) {
          if (args["captureId"].as<String>() != pendingCaptureId ||
              args["remoteId"].as<String>() != pendingCaptureRemote ||
              args["proto"].as<String>() != pendingCapture["protocol"].as<String>() ||
              args["code"].as<String>() != pendingCapture["decodedRawHex"].as<String>()) {
            error = "Capture expired or mismatched; learn again";
          } else {
            LittleFS.mkdir("/captures");
            captureFile = "/captures/" + pendingCaptureId + ".json";
            File file = LittleFS.open("/capture.tmp", "w");
            size_t expected = measureJson(pendingCapture);
            bool ok = file && serializeJson(pendingCapture, file) == expected;
            if (file) { file.flush(); file.close(); }
            if (!ok || !LittleFS.rename("/capture.tmp", captureFile)) error = "Capture save failed";
            else fsUsageRefresh(true);
          }
        }
        // 全新按键但无 capturePath 也无 captureId
        else if (!args.containsKey("id")) {
          error = "A new key requires a full device capture";
        }
        // 修改现有按键（有 id）
        else {
          for (JsonObjectConst key : remoteConfig["keys"].as<JsonArrayConst>())
            if (key["id"].as<String>() == args["id"].as<String>() &&
                (key["proto"].as<String>() != args["proto"].as<String>() ||
                 key["code"].as<String>() != args["code"].as<String>() ||
                 key["remoteId"].as<String>() != args["remoteId"].as<String>()))
              error = "Changing input identity requires learning a new capture";
        }
      }
      bool forget = op == "hosts.delete";
      String address;
      uint8_t addressType = 0;
      if (forget)
        for (JsonObjectConst host : remoteConfig["hosts"].as<JsonArrayConst>())
          if (host["id"].as<String>() == args["id"].as<String>()) {
            address = host["address"].as<String>();
            addressType = host["addressType"] | 0;
          }
      bool changed = error.isEmpty() && remoteStoreCommand(op, args, error);
      if (changed && !captureFile.isEmpty()) {
        for (JsonObject key : remoteConfig["keys"].as<JsonArray>())
          if (strcmp(key["proto"] | "", args["proto"] | "") == 0 &&
              strcmp(key["code"] | "", args["code"] | "") == 0) {
            key["capturePath"] = captureFile; key["captureStatus"] = "envelope-complete";
          }
      }
      if (!changed || !saveRemoteConfig(&error)) {
        if (error.isEmpty()) error = "Configuration save failed";
        remoteConfig.set(previous);
        if (!captureFile.isEmpty()) { LittleFS.remove(captureFile); fsUsageRefresh(true); }
      } else {
        if (!captureFile.isEmpty()) { pendingCapture.clear(); pendingCaptureId = ""; }
        // Remove replaced/deleted capture files only after configuration commits.
        bool removedAny = false;
        for (JsonObjectConst oldKey : previous["keys"].as<JsonArrayConst>()) {
          String path = oldKey["capturePath"] | "";
          if (path.isEmpty()) continue;
          bool referenced = false;
          for (JsonObjectConst key : remoteConfig["keys"].as<JsonArrayConst>())
            if (key["capturePath"].as<String>() == path) referenced = true;
          if (!referenced) { LittleFS.remove(path); removedAny = true; }
        }
        if (removedAny) fsUsageRefresh(true);
        if (forget && !bleHostForget(address.c_str(), addressType)) {
          remoteConfig.set(previous);
          error = saveRemoteConfig() ? "BLE bond removal failed; host retained"
                                     : "BLE bond removal failed; configuration rollback failed";
        }
        // hosts.reconnect 已在上面处理；hosts.delete 触发 bleHostForget 内部已重置广播
        if (error.isEmpty() && op == "settings.save") {
          if (args.containsKey("irTxMode"))
            irTxApplyMode(remoteConfig["settings"]["irTxMode"] | "off");
          // 热点重启或蓝牙重广播都会扰动连接：先记下来，回包之后再应用。
          if (previous["settings"]["apSsid"].as<String>() !=
                  remoteConfig["settings"]["apSsid"].as<String>() ||
              previous["settings"]["apPassword"].as<String>() !=
                  remoteConfig["settings"]["apPassword"].as<String>())
            applyApAfter = true;
          if (previous["settings"]["bleName"].as<String>() !=
              remoteConfig["settings"]["bleName"].as<String>())
            applyBleNameAfter = true;
        }
        stateDirty = true;
      }
      } // end else (single-key commands)
    }
  }
  response["ok"] = error.isEmpty();
  if (!error.isEmpty()) response["error"] = error;
  socketSend(client, response);
  // 响应发出后再应用设置，避免热点重启吞掉成功回包。
  if (applyApAfter) applyAccessPoint();
  if (applyBleNameAfter) bleApplyName();
}

void captureIrFrame() {
  const IRData data = IrReceiver.decodedIRData;
  // The ISR reuses rawbuf after resume(); snapshot every tick first.
  static uint16_t rawTicks[RAW_BUFFER_LENGTH];
  uint16_t rawCount = data.rawlen > 0 ? data.rawlen - 1 : 0;
  if (rawCount >= RAW_BUFFER_LENGTH) rawCount = RAW_BUFFER_LENGTH - 1;
  if (learningMode)
    for (uint16_t i = 0; i < rawCount; ++i) rawTicks[i] = IrReceiver.irparams.rawbuf[i + 1];
  // Resume reception before any networking or HID work.
  IrReceiver.resume();
  bool repeat = data.flags & IRDATA_FLAGS_IS_REPEAT;
  bool overflow = data.flags & IRDATA_FLAGS_WAS_OVERFLOW;
  // 可配置的脉冲数下限：短于 n 段的非重复帧视为干扰丢弃（学习与执行均生效）。
  // 重复帧本身只有几段脉冲，不参与该过滤。
  // 注意：不要按 decodedRawData 的数值大小过滤——Sony 12/15/20bit、RC5/RC6
  // 等短码协议的解码值永远小于 0x10000000，会被误杀（发射端明确支持它们）。
  int minPulses = remoteConfig["settings"]["minPulses"] | 8;
  if (minPulses < 0) minPulses = 0;
  if (minPulses > RAW_BUFFER_LENGTH) minPulses = RAW_BUFFER_LENGTH;
  if (!repeat && static_cast<int>(rawCount) < minPulses) {
    lastProto[0] = '\0';
    return;
  }
  if (overflow || (data.protocol == UNKNOWN && !learningMode)) {
    lastProto[0] = '\0';
    ++frameCount;
    if (learningMode && overflow) {
      pendingCapture.clear(); pendingCaptureId = "";
      String message = "{\"event\":\"capture.error\",\"error\":\"IR buffer overflow; no complete capture saved\"}";
      sockets.broadcastTXT(message);
    }
    return;
  }
  uint32_t now = millis();
  ledUntil = now + 300;
  String proto = getProtocolString(data.protocol);
  char code[17];
  snprintf(code, sizeof(code), "%016llX", static_cast<unsigned long long>(data.decodedRawData));
  if (repeat) {
    if (!lastProto[0] || now - lastFrameAt > 350) return;
    proto = lastProto;
    strlcpy(code, lastCode, sizeof(code));
  } else if (data.protocol != UNKNOWN && !overflow) {
    strlcpy(lastProto, proto.c_str(), sizeof(lastProto));
    strlcpy(lastCode, code, sizeof(lastCode));
  }
  lastFrameAt = now;
  ++frameCount;
  if (learningMode && !repeat) {
    if (rawCount < 3) return;
    pendingCapture.clear();
    pendingCapture["version"] = 1;
    pendingCapture["encoding"] = "alternating-mark-space-ticks";
    pendingCapture["startsWith"] = "mark";
    pendingCapture["tickMicros"] = MICROS_PER_TICK;
    pendingCapture["initialGapTicks"] = data.initialGapTicks;
    pendingCapture["recordGapMicros"] = RECORD_GAP_MICROS;
    pendingCapture["markExcessMicros"] = MARK_EXCESS_MICROS;
    pendingCapture["carrierHz"] = nullptr;
    pendingCapture["carrierMeasured"] = false;
    pendingCapture["protocol"] = proto;
    pendingCapture["address"] = data.address;
    pendingCapture["command"] = data.command;
    pendingCapture["extra"] = data.extra;
    pendingCapture["flags"] = data.flags;
    pendingCapture["bits"] = data.numberOfBits;
    pendingCapture["decodedRawHex"] = code;
    pendingCapture["overflow"] = false;
    pendingCapture["rawBufferCapacity"] = RAW_BUFFER_LENGTH;
    pendingCapture["captureScope"] = "single-frame-envelope";
    JsonArray ticks = pendingCapture["rawTicks"].to<JsonArray>();
    for (uint16_t i = 0; i < rawCount; ++i) ticks.add(rawTicks[i]);
    pendingCaptureId = String(esp_random(), HEX) + "-" + String(now, HEX);
    pendingCaptureRemote = learningRemote;
    JsonDocument doc;
    doc["event"] = "input";
    doc["remoteId"] = learningRemote;
    doc["transport"] = "ir";
    doc["proto"] = proto;
    doc["code"] = code;
    doc["bits"] = data.numberOfBits;
    doc["captureId"] = pendingCaptureId;
    doc["pulseCount"] = rawCount;
    String message;
    serializeJson(doc, message);
    sockets.broadcastTXT(message);
    learningUntil = now + 300000;
  } else if (!learningMode) {
    // 非学习模式也广播 input 事件，用于前端按键卡片高亮提示。
    // 节流到 100ms：狂按遥控器时高频广播 + 状态推送会挤占 loop，
    // 拖慢 WS/HTTP 响应。HID 分发（dispatchInput）不节流，按键响应不受影响。
    static uint32_t lastInputBroadcast = 0;
    if (now - lastInputBroadcast >= 100) {
      lastInputBroadcast = now;
      JsonDocument evt;
      evt["event"] = "input";
      evt["transport"] = "ir";
      evt["proto"] = proto;
      evt["code"] = code;
      evt["pulseCount"] = rawCount;
      String msg;
      serializeJson(evt, msg);
      sockets.broadcastTXT(msg);
    }
    dispatchInput("ir", "", proto.c_str(), code, repeat);
  }
}

void exportData() {
  String scope = server.arg("scope");
  String id = server.arg("id");
  if (scope == "remote" && id.isEmpty()) id = server.arg("remoteId");
  if (scope == "macro" && id.isEmpty()) id = server.arg("macroId");
  if (scope != "remotes" && scope != "macros" && scope != "all" && scope != "remote" && scope != "macro") {
    server.send(400, "application/json", "{\"error\":\"Invalid export scope\"}"); return;
  }
  if ((scope == "remote" || scope == "macro") && id.isEmpty()) {
    server.send(400, "application/json", "{\"error\":\"Missing id parameter\"}"); return;
  }
  if (!storageReady) {
    server.send(503, "application/json", "{\"error\":\"Configuration unavailable\"}"); return;
  }

  // scope=macro: 先收集该宏引用的所有 keyId（最多 12 个 IR 步骤）。
  // 必须在发出任何响应头之前完成——宏不存在要回 404，而旧实现先发了 200
  // 状态行再 send(404)，HTTP 流损坏，浏览器会挂起到 30s 超时。
  String macroKeyIds[12];
  int macroKeyIdCount = 0;
  JsonObjectConst exportedMacro;
  if (scope == "macro") {
    for (JsonObjectConst m : remoteConfig["macros"].as<JsonArrayConst>()) {
      if (m["id"].as<String>() == id) {
        exportedMacro = m;
        for (JsonObjectConst step : m["steps"].as<JsonArrayConst>()) {
          if (step["kind"].as<String>() == "ir" && macroKeyIdCount < 12) {
            macroKeyIds[macroKeyIdCount++] = step["keyId"].as<String>();
          }
        }
        break;
      }
    }
    if (exportedMacro.isNull()) {
      server.send(404, "application/json", "{\"error\":\"Macro not found\"}"); return;
    }
  }

  server.sendHeader("Content-Disposition", "attachment; filename=\"ir-remote-export.json\"");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json; charset=utf-8", "");
  server.sendContent("{\"format\":\"esp32-remote\"");
  String json;

  // 辅助：检查 keyId 是否在宏引用的集合中
  auto macroHasKeyId = [&](const String& keyId) -> bool {
    for (int i = 0; i < macroKeyIdCount; i++)
      if (macroKeyIds[i] == keyId) return true;
    return false;
  };

  // 1. remotes 数组
  JsonDocument outRemotes;
  JsonArray remotesArr = outRemotes.to<JsonArray>();
  if (scope == "remote") {
    for (JsonObjectConst r : remoteConfig["remotes"].as<JsonArrayConst>())
      if (r["id"].as<String>() == id) { remotesArr.add(r); break; }
  } else if (scope == "macro") {
    // 宏中引用的红外按键所属的遥控器（去重）
    for (JsonObjectConst r : remoteConfig["remotes"].as<JsonArrayConst>()) {
      bool added = false;
      for (JsonObjectConst k : remoteConfig["keys"].as<JsonArrayConst>()) {
        if (macroHasKeyId(k["id"].as<String>()) && k["remoteId"].as<String>() == r["id"].as<String>()) {
          remotesArr.add(r); added = true; break;
        }
      }
      (void)added; // 仅用于避免未使用警告
    }
  } else if (scope != "macros") {
    for (JsonObjectConst r : remoteConfig["remotes"].as<JsonArrayConst>()) remotesArr.add(r);
  }
  json = ""; serializeJson(outRemotes, json);
  server.sendContent(",\"remotes\":" + json);

  // 2. keys 数组（IR: proto + code + capturePath）
  JsonDocument outKeys;
  JsonArray keysArr = outKeys.to<JsonArray>();
  for (JsonObjectConst k : remoteConfig["keys"].as<JsonArrayConst>()) {
    String rid = k["remoteId"] | "";
    if (scope == "remote" && rid != id) continue;
    if (scope == "macros") continue;
    if (scope == "macro" && !macroHasKeyId(k["id"].as<String>())) continue;
    JsonObject ok = keysArr.add<JsonObject>();
    ok["id"] = k["id"];
    ok["name"] = k["name"];
    ok["remoteId"] = rid;
    String proto = k["proto"] | "";
    String code = k["code"] | "";
    if (!proto.isEmpty()) ok["proto"] = proto;
    if (!code.isEmpty()) ok["code"] = code;
    if (!k["capturePath"].isNull()) ok["capturePath"] = k["capturePath"];
  }
  json = ""; serializeJson(outKeys, json);
  server.sendContent(",\"keys\":" + json);

  // 3. captures 对象（按 capturePath 索引，便于导入时直接按路径还原）
  server.sendContent(",\"captures\":{");
  bool first = true;
  for (JsonObjectConst k : remoteConfig["keys"].as<JsonArrayConst>()) {
    if (scope == "remote" && k["remoteId"].as<String>() != id) continue;
    if (scope == "macro" && !macroHasKeyId(k["id"].as<String>())) continue;
    String path = k["capturePath"] | "";
    if (path.isEmpty()) continue;
    File capture = LittleFS.open(path, "r");
    if (!capture) continue;
    JsonDocument label; label.set(path);
    json = ""; serializeJson(label, json);
    server.sendContent((first ? "" : ",") + json + ":");
    first = false;
    char buffer[512];
    while (capture.available()) {
      size_t count = capture.readBytes(buffer, sizeof(buffer));
      server.sendContent(buffer, count);
    }
    capture.close();
  }
  server.sendContent("}");

  // 4. macros / mappings
  if (scope == "macro") {
    // 仅导出单个宏
    server.sendContent(",\"macros\":[");
    json = ""; serializeJson(exportedMacro, json);
    server.sendContent(json);
    server.sendContent("]");
  } else if (scope != "remotes" && scope != "remote") {
    json = ""; serializeJson(remoteConfig["macros"], json);
    server.sendContent(",\"macros\":" + json);
  }
  if (scope == "all") {
    json = ""; serializeJson(remoteConfig["mappings"], json);
    server.sendContent(",\"mappings\":" + json);
  }
  server.sendContent("}");
  server.sendContent("");
}

void setup() {
  Serial.begin(115200);
  irTxApplyMode("off");
  pinMode(kSensorGroundPin, OUTPUT); digitalWrite(kSensorGroundPin, LOW);
  pinMode(kSensorVccPin, OUTPUT); digitalWrite(kSensorVccPin, HIGH);
  pinMode(kIrDataPin, INPUT);
  pinMode(kLedPin, OUTPUT); digitalWrite(kLedPin, HIGH);
  IrReceiver.begin(kIrDataPin, DISABLE_LED_FEEDBACK);
  // 固件自行完成 FS 初始化：挂载失败才格式化整盘；配置损坏只重建配置，
  // 前端与红外采样保留（remoteStoreRebuild 会把旧配置改名 .bad 留档）。
  filesystemMounted = LittleFS.begin(false);
  if (!filesystemMounted) {
    Serial.println("[FS] not mounted; formatting");
    filesystemMounted = LittleFS.format() && LittleFS.begin(false);
  }
  if (filesystemMounted) LittleFS.mkdir("/frontend");
  storageReady = filesystemMounted;
  if (storageReady) storageReady = remoteStoreBegin();
  if (!storageReady) {
    Serial.println("[STORE] config load failed; rebuilding defaults (frontend preserved)");
    storageReady = filesystemMounted && remoteStoreRebuild();
    if (!storageReady) {
      // 重建失败说明 FS 本身有问题，最后手段才是格式化整盘
      Serial.println("[STORE] rebuild failed; formatting filesystem");
      LittleFS.format();
      filesystemMounted = LittleFS.begin(false);
      if (filesystemMounted) LittleFS.mkdir("/frontend");
      storageReady = filesystemMounted && remoteStoreBegin();
      if (!storageReady) Serial.println("[STORE] rebuild failed; storage unavailable");
    }
  }
  irTxApplyMode(remoteConfig["settings"]["irTxMode"] | "off");
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  WiFi.onEvent(onWiFiEvent);
  applyAccessPoint();
  bleHidBegin();
  fileApiBegin(server);
  server.on("/api/export", HTTP_GET, exportData);
  server.on("/api/status", HTTP_GET, [] {
    JsonDocument doc;
    doc["storageReady"] = storageReady;
    doc["filesystemMounted"] = filesystemMounted;
    doc["websocketPort"] = 81;
    doc["heap"] = ESP.getFreeHeap();
    uint32_t fsTotal, fsUsed;
    fsUsageCached(fsTotal, fsUsed);
    doc["fsTotal"] = fsTotal;
    doc["fsUsed"] = fsUsed;
    // 便于在未部署前端时展示实际生效的热点与蓝牙名称。
    doc["apSsid"] = remoteConfig["settings"]["apSsid"] | kApSsid;
    doc["bleName"] = remoteConfig["settings"]["bleName"] | "Remote Master";
    doc["irTxMode"] = remoteConfig["settings"]["irTxMode"] | "off";
    doc["irTxRunning"] = irTxBusy();
    bleHostStatus(doc["ble"].to<JsonObject>());
    String message; serializeJson(doc, message);
    server.send(200, "application/json", message);
  });
  // 全量状态接口：页面加载时 HTTP 请求获取完整 config + status，后续用 WS 差分更新
  server.on("/api/state", HTTP_GET, [] {
    JsonDocument doc;
    doc["event"] = "state";
    writeConfigSummary(doc["config"].to<JsonObject>());
    JsonObject status = doc["status"].to<JsonObject>();
    status["learningRemote"] = learningRemote;
    status["frames"] = frameCount;
    status["heap"] = ESP.getFreeHeap();
    status["storageReady"] = storageReady;
    status["hidRunning"] = hidRunning;
    status["irTxRunning"] = irTxBusy();
    uint32_t fsTotal, fsUsed;
    fsUsageCached(fsTotal, fsUsed);
    status["fsTotal"] = fsTotal;
    status["fsUsed"] = fsUsed;
    bleHostStatus(status["ble"].to<JsonObject>());
    String message; serializeJson(doc, message);
    server.send(200, "application/json", message);
  });
  // 未命中任何显式路由时按 /frontend 静态资源处理（含 SPA 路由回退）。
  server.onNotFound([] { fileApiServeStatic(server.uri()); });
  server.begin();
  sockets.begin();
  sockets.onEvent(socketEvent);
  sockets.enableHeartbeat(30000, 5000, 2);
  Serial.printf("[WEB] http://%s; WebSocket port 81\n", WiFi.softAPIP().toString().c_str());
}

void loop() {
  // loop 分段计时遥测：单轮超过 150ms 时输出各环节耗时（ms），用于定位
  // "按键后设备几秒无响应"这类阻塞问题。正常迭代 <5ms，遥测零噪音。
  // 排查结束后此块可整体移除（标记 [LOOP]）。
  uint32_t t0 = micros();
  // system.reset 的实际执行点：响应已在上一轮的 sockets.loop() 里发出，
  // 这里稍作延迟让 TCP 把回包推出去，再执行数秒级的格式化。
  if (pendingFactoryReset) {
    pendingFactoryReset = false;
    delay(100);
    Serial.println("[STORE] factory reset: formatting filesystem");
    bleClearAllBonds();
    bool ok = remoteStoreFactoryReset();
    storageReady = ok;
    fsUsageRefresh(true);
    Serial.printf("[STORE] factory reset %s\n", ok ? "done" : "FAILED");
    stateDirty = ok;  // 广播新状态（WS 多半已被格式化期间的停顿杀死，前端会重连）
  }
  irTxTick();
  uint32_t tIrTx = micros();
  if (!irTxBusy() && IrReceiver.decode()) captureIrFrame();
  uint32_t tIr = micros();
  tickBleHosts();
  uint32_t tBle = micros();
  tickHid();
  uint32_t tHid = micros();
  uint32_t now = millis();
  if (learningMode && static_cast<int32_t>(now - learningUntil) >= 0) {
    learningMode = false; learningRemote = ""; stateDirty = true;
  }
  digitalWrite(kLedPin, static_cast<int32_t>(ledUntil - now) > 0 && ((now / 60) & 1) ? LOW : HIGH);
  server.handleClient();
  uint32_t tHttp = micros();
  sockets.loop();
  uint32_t tWs = micros();
  tickKeepAlive();
  uint32_t tKa = micros();
  static bool wasConnected = false, wasRunning = false, wasIrTxRunning = false;
  static String previousIrTxError;
  if (wasIrTxRunning != irTxBusy()) stateDirty = true;
  wasIrTxRunning = irTxBusy();
  if (previousIrTxError != irTxLastError()) {
    previousIrTxError = irTxLastError();
    if (!previousIrTxError.isEmpty()) {
      JsonDocument event;
      event["event"] = "ir.error";
      event["error"] = previousIrTxError;
      String message;
      serializeJson(event, message);
      sockets.broadcastTXT(message);
    }
  }
  // BLE 状态轮询节流到 150ms：旧实现每轮 loop（约每秒上千次）都完整构建并
  // 序列化一次 BLE 状态 JSON 做字符串比较，纯属浪费 CPU 与堆。
  static size_t hosts = 0;
  static String previousBleStatus;
  static JsonDocument statusDoc;
  static uint32_t bleStatusAt = 0;
  if (now - bleStatusAt >= 150) {
    bleStatusAt = now;
    statusDoc.clear();
    bleHostStatus(statusDoc.to<JsonObject>());
    String bleStatus;
    serializeJson(statusDoc, bleStatus);
    if (previousBleStatus != bleStatus) {
      previousBleStatus = bleStatus;
      stateDirty = true;
    }
  }
  if (wasConnected != bleHidConnected() || wasRunning != hidRunning ||
      hosts != remoteConfig["hosts"].size()) stateDirty = true;
  wasConnected = bleHidConnected(); wasRunning = hidRunning;
  hosts = remoteConfig["hosts"].size();
  if (stateDirty) { stateDirty = false; pushState(); }
  uint32_t tEnd = micros();
  uint32_t totalMs = (tEnd - t0) / 1000;
  if (totalMs > 150) {
    Serial.printf("[LOOP] slow %lums: irTx=%lu ir=%lu bleHosts=%lu hid=%lu http=%lu ws=%lu keepAlive=%lu state=%lu\n",
                  (unsigned long)totalMs,
                  (unsigned long)((tIrTx - t0) / 1000), (unsigned long)((tIr - tIrTx) / 1000),
                  (unsigned long)((tBle - tIr) / 1000), (unsigned long)((tHid - tBle) / 1000),
                  (unsigned long)((tHttp - tHid) / 1000), (unsigned long)((tWs - tHttp) / 1000),
                  (unsigned long)((tKa - tWs) / 1000), (unsigned long)((tEnd - tKa) / 1000));
  }
  delay(1);
}
