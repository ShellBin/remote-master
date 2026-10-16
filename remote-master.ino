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
  WiFi.softAP(ssid.c_str(), password.c_str(), 6, false, 4);
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

void pushState(int client = -1) {
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
  status["fsTotal"] = filesystemMounted ? LittleFS.totalBytes() : 0;
  status["fsUsed"] = filesystemMounted ? LittleFS.usedBytes() : 0;
  bleHostStatus(status["ble"].to<JsonObject>());
  // WiFi 已连接设备
  wifi_sta_list_t wifi_sta;
  if (esp_wifi_ap_get_sta_list(&wifi_sta) == ESP_OK) {
    JsonArray stations = status["wifiStations"].to<JsonArray>();
    for (int i = 0; i < wifi_sta.num; i++) {
      JsonObject sta = stations.add<JsonObject>();
      char macStr[18];
      snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
               wifi_sta.sta[i].mac[0], wifi_sta.sta[i].mac[1],
               wifi_sta.sta[i].mac[2], wifi_sta.sta[i].mac[3],
               wifi_sta.sta[i].mac[4], wifi_sta.sta[i].mac[5]);
      sta["mac"] = macStr;
      sta["rssi"] = wifi_sta.sta[i].rssi;
    }
  }
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
  if (length > 4096 || deserializeJson(request, payload, length)) error = "Invalid or oversized JSON";
  response["id"] = request["id"];
  String op = request["op"] | "";
  JsonObjectConst args = request["args"].as<JsonObjectConst>();
  if (error.isEmpty()) {
    if (op == "state.get") {
      pushState(client);
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
      else {
        irTxApplyMode("off");
        stopHid("reset");
        learningMode = false; learningRemote = "";
        bleClearAllBonds();
        if (!remoteStoreFactoryReset()) error = "Factory reset failed";
        else { stateDirty = true; storageReady = true; }
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
        if (!captureFile.isEmpty()) LittleFS.remove(captureFile);
      } else {
        if (!captureFile.isEmpty()) { pendingCapture.clear(); pendingCaptureId = ""; }
        // Remove replaced/deleted capture files only after configuration commits.
        for (JsonObjectConst oldKey : previous["keys"].as<JsonArrayConst>()) {
          String path = oldKey["capturePath"] | "";
          if (path.isEmpty()) continue;
          bool referenced = false;
          for (JsonObjectConst key : remoteConfig["keys"].as<JsonArrayConst>())
            if (key["capturePath"].as<String>() == path) referenced = true;
          if (!referenced) LittleFS.remove(path);
        }
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
  // Filter significant hex length, not the zero-padded storage representation.
  // Repeat packets may contain no code; only reuse an already accepted frame.
  if (!repeat && data.decodedRawData < 0x10000000ULL) {
    lastProto[0] = '\0';
    return;
  }
  // 可配置的脉冲数下限：短于 n 段的非重复帧视为干扰丢弃（学习与执行均生效）。
  // 重复帧本身只有几段脉冲，不参与该过滤。
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
    // 非学习模式也广播 input 事件，用于前端按键卡片高亮提示
    {
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
  String remoteId = server.arg("remoteId");
  if (scope != "remotes" && scope != "macros" && scope != "all" && scope != "remote") {
    server.send(400, "application/json", "{\"error\":\"Invalid export scope\"}"); return;
  }
  if (scope == "remote" && remoteId.isEmpty()) {
    server.send(400, "application/json", "{\"error\":\"Missing remoteId parameter\"}"); return;
  }
  if (!storageReady) {
    server.send(503, "application/json", "{\"error\":\"Configuration unavailable\"}"); return;
  }
  server.sendHeader("Content-Disposition", "attachment; filename=\"ir-remote-export.json\"");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json; charset=utf-8", "");
  server.sendContent("{\"format\":\"esp32-remote\"");
  String json;

  // 1. remotes 数组
  JsonDocument outRemotes;
  JsonArray remotesArr = outRemotes.to<JsonArray>();
  if (scope == "remote") {
    for (JsonObjectConst r : remoteConfig["remotes"].as<JsonArrayConst>())
      if (r["id"].as<String>() == remoteId) { remotesArr.add(r); break; }
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
    if (scope == "remote" && rid != remoteId) continue;
    if (scope == "macros") continue;
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
    if (scope == "remote" && k["remoteId"].as<String>() != remoteId) continue;
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

  // 4. macros / mappings（仅 scope=all 或 macros）
  if (scope != "remotes" && scope != "remote") {
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
  // 固件自行完成 FS 初始化：未挂载或配置存在致命兼容性问题时直接格式化，
  // 不做版本向下兼容。格式化后重建默认配置，deploy.mjs 不再负责初始化。
  filesystemMounted = LittleFS.begin(false);
  if (!filesystemMounted) {
    Serial.println("[FS] not mounted; formatting");
    filesystemMounted = LittleFS.format() && LittleFS.begin(false);
  }
  if (filesystemMounted) LittleFS.mkdir("/frontend");
  storageReady = filesystemMounted;
  if (storageReady) storageReady = remoteStoreBegin();
  if (!storageReady) {
    Serial.println("[STORE] config load failed; formatting filesystem and rebuilding");
    LittleFS.format();
    filesystemMounted = LittleFS.begin(false);
    if (filesystemMounted) LittleFS.mkdir("/frontend");
    storageReady = filesystemMounted && remoteStoreBegin();
    if (!storageReady) Serial.println("[STORE] rebuild failed; storage unavailable");
  }
  irTxApplyMode(remoteConfig["settings"]["irTxMode"] | "off");
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
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
    doc["fsTotal"] = filesystemMounted ? LittleFS.totalBytes() : 0;
    doc["fsUsed"] = filesystemMounted ? LittleFS.usedBytes() : 0;
    // 便于在未部署前端时展示实际生效的热点与蓝牙名称。
    doc["apSsid"] = remoteConfig["settings"]["apSsid"] | kApSsid;
    doc["bleName"] = remoteConfig["settings"]["bleName"] | "Remote Master";
    doc["irTxMode"] = remoteConfig["settings"]["irTxMode"] | "off";
    doc["irTxRunning"] = irTxBusy();
    bleHostStatus(doc["ble"].to<JsonObject>());
    String message; serializeJson(doc, message);
    server.send(200, "application/json", message);
  });
  // 未命中任何显式路由时按 /frontend 静态资源处理（含 SPA 路由回退）。
  server.onNotFound([] { fileApiServeStatic(server.uri()); });
  server.begin();
  sockets.begin();
  sockets.onEvent(socketEvent);
  sockets.enableHeartbeat(15000, 3000, 2);
  Serial.printf("[WEB] http://%s; WebSocket port 81\n", WiFi.softAPIP().toString().c_str());
}

void loop() {
  irTxTick();
  if (!irTxBusy() && IrReceiver.decode()) captureIrFrame();
  tickBleHosts();
  tickHid();
  uint32_t now = millis();
  if (learningMode && static_cast<int32_t>(now - learningUntil) >= 0) {
    learningMode = false; learningRemote = ""; stateDirty = true;
  }
  digitalWrite(kLedPin, static_cast<int32_t>(ledUntil - now) > 0 && ((now / 60) & 1) ? LOW : HIGH);
  server.handleClient();
  sockets.loop();
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
  static size_t hosts = 0;
  static String previousBleStatus;
  JsonDocument status;
  bleHostStatus(status.to<JsonObject>());
  String bleStatus;
  serializeJson(status, bleStatus);
  if (previousBleStatus != bleStatus) {
    previousBleStatus = bleStatus;
    stateDirty = true;
  }
  if (wasConnected != bleHidConnected() || wasRunning != hidRunning ||
      hosts != remoteConfig["hosts"].size()) stateDirty = true;
  wasConnected = bleHidConnected(); wasRunning = hidRunning;
  hosts = remoteConfig["hosts"].size();
  if (stateDirty) { stateDirty = false; pushState(); }
  delay(1);
}
