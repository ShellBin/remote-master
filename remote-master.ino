/*
 * ESP32-C3 IR input -> BLE HID output.
 * Arduino ESP32 3.x, ArduinoJson 7.x, IRremote 4.x,
 * NimBLE-Arduino 2.5.x, ESPAsyncWebServer + AsyncWebSocket.
 * Upload data/ separately as a LittleFS image.
 *
 * 迁移说明：原 WebSocketsServer::loop() 在客户端异常断连时 write() 阻塞
 * 约 20 秒（lwIP 重传超时），期间 server.handleClient() 被饿死。
 * 换用 AsyncWebSocket 后 I/O 在 TCP task 中异步执行，write 不会阻塞 loop。
 */
#include <WiFi.h>
#include <esp_wifi.h>
#include <ESPAsyncWebServer.h>
#define RAW_BUFFER_LENGTH 750
#define RECORD_GAP_MICROS 12000
#include <IRremote.hpp>
#include <LittleFS.h>
#include <memory>
#include <vector>
#include <freertos/semphr.h>
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
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

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

// AsyncWebSocket 发送：client==-1 时广播给所有已连接客户端。
void wsSend(const String& message, int clientId = -1) {
  if (clientId < 0) ws.textAll(message);
  else ws.text(clientId, message);
}

// 序列化 JSON 文档并发送到指定客户端（或广播 clientId==-1）。
void wsSendJson(JsonDocument& doc, int clientId = -1) {
  String message;
  serializeJson(doc, message);
  wsSend(message, clientId);
}

// ==================== WebSocket -> loop 命令队列 ====================
//
// AsyncWebSocket 回调运行在 async_tcp 任务里，而 remoteConfig、LittleFS 与
// HID 状态都归 loop() 独占。旧实现（WebSocketsServer）在 loop() 中处理命令，
// 迁移后若直接在 TCP 任务里改配置，会与 loop() 的按键分发/状态推送并发读写
// JsonDocument（保存按键时尤其容易崩），同时长时间 flash 写也会阻塞 HTTP。
// 这里回调只负责拷贝入队，实际处理在 loop() 里串行执行。

void pushState(int client = -1);
void handleWsCommand(uint32_t clientId, uint8_t* payload, size_t length);

struct WsCommand {
  uint32_t clientId;
  uint16_t length;
  char payload[1];
};
static const uint8_t kWsCommandDepth = 6;
static WsCommand* wsCommandQueue[kWsCommandDepth];
static uint8_t wsQueueHead = 0, wsQueueTail = 0;
static portMUX_TYPE wsQueueMux = portMUX_INITIALIZER_UNLOCKED;
static String wsInboundFrame;  // 仅在 async_tcp 任务内使用（分片重组）

static void queueWsCommand(uint32_t clientId, const uint8_t* data, size_t length) {
  if (length > 8192) return;
  WsCommand* command = static_cast<WsCommand*>(malloc(sizeof(WsCommand) + length));
  if (!command) {
    Serial.println("[WS] command alloc failed; dropping");
    return;
  }
  command->clientId = clientId;
  command->length = static_cast<uint16_t>(length);
  if (length) memcpy(command->payload, data, length);

  portENTER_CRITICAL(&wsQueueMux);
  uint8_t next = (wsQueueHead + 1) % kWsCommandDepth;
  bool queued = next != wsQueueTail;
  if (queued) {
    wsCommandQueue[wsQueueHead] = command;
    wsQueueHead = next;
  }
  portEXIT_CRITICAL(&wsQueueMux);
  if (!queued) {
    free(command);
    Serial.println("[WS] command queue full; dropping");
  }
}

// 在 loop() 中出队执行；避免把 JsonDocument 操作留在 TCP 任务。
void drainWsCommands() {
  while (true) {
    WsCommand* command = nullptr;
    portENTER_CRITICAL(&wsQueueMux);
    if (wsQueueTail != wsQueueHead) {
      command = wsCommandQueue[wsQueueTail];
      wsQueueTail = (wsQueueTail + 1) % kWsCommandDepth;
    }
    portEXIT_CRITICAL(&wsQueueMux);
    if (!command) return;
    handleWsCommand(command->clientId, reinterpret_cast<uint8_t*>(command->payload),
                    command->length);
    free(command);
  }
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

// ==================== remoteConfig 互斥 ====================
//
// WebSocket 命令已改到 loop() 执行，但 /api/config/*、/api/status、/api/export
// 仍在 AsyncWebServer 的 TCP 任务里读 remoteConfig。所有写操作（保存配置、
// 记录 BLE 主机、恢复出厂）持这把递归锁，避免 JsonDocument 读写并发。
static SemaphoreHandle_t configMutex = nullptr;

void configLock() {
  if (configMutex) xSemaphoreTakeRecursive(configMutex, portMAX_DELAY);
}

void configUnlock() {
  if (configMutex) xSemaphoreGiveRecursive(configMutex);
}

// 构造只含 status 的 state 消息：直接序列化进一块精确大小的共享缓冲。
// 配置数据不再随 WS 推送（改为 /api/config/* 分段拉取），单条消息始终很小。
static AsyncWebSocketSharedBuffer buildStatusMessage(JsonDocument& statusDoc) {
  static const char kHead[] = "{\"event\":\"state\",\"status\":";
  const size_t headLen = sizeof(kHead) - 1;
  size_t statusLen = measureJson(statusDoc);
  size_t total = headLen + statusLen + 3;

  // 预检最大可分配块：make_shared 失败会抛 bad_alloc 并 abort，不能靠运气。
  if (ESP.getMaxAllocHeap() < total + 4096) {
    Serial.printf("[WS] state skipped: need=%u maxAlloc=%u heap=%u\n",
                  static_cast<unsigned>(total), ESP.getMaxAllocHeap(), ESP.getFreeHeap());
    return nullptr;
  }

  auto buffer = std::make_shared<std::vector<uint8_t>>(total);
  char* out = reinterpret_cast<char*>(buffer->data());
  size_t used = 0;
  memcpy(out + used, kHead, headLen); used += headLen;
  used += serializeJson(statusDoc, out + used, total - used);
  out[used++] = '}';
  buffer->resize(used);
  return buffer;
}

void pushState(int client) {
  // 优化：无 WebSocket 客户端时不序列化
  if (client < 0 && ws.count() == 0) { stateDirty = false; return; }
  // 优化：节流，每 50ms 最多推送一次。被节流时保留 dirty 标记，
  // 否则一次快速保存的 config.changed 可能永远丢失（前端不再主动轮询）。
  static uint32_t lastPush = 0;
  uint32_t now = millis();
  if (client < 0 && now - lastPush < 50) { stateDirty = true; return; }
  lastPush = now;

  // 显式请求（client >= 0）也触发一次配置变更通知
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

  // 配置变化只发一个轻量通知，前端按段重新拉取；不再序列化整份配置。
  if (configChanged) {
    static const char kConfigChanged[] = "{\"event\":\"config.changed\"}";
    if (client < 0) ws.textAll(kConfigChanged);
    else ws.text(static_cast<uint32_t>(client), kConfigChanged);
  }
  if (!statusChanged && client < 0) return;

  // 状态消息一次精确分配，直接交给 WebSocket 队列（不再拷贝）。
  AsyncWebSocketSharedBuffer message = buildStatusMessage(statusDoc);
  if (!message) return;
  if (client < 0) ws.textAll(message);
  else ws.text(static_cast<uint32_t>(client), message);
}

// AsyncWebSocket 事件回调。
// 签名与原 WebSocketsServer 不同：接收 AsyncWebSocketClient* 而非编号。
void wsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
             AwsEventType type, void* arg, uint8_t* payload, size_t length) {
  switch (type) {
    case WS_EVT_CONNECT:
      // 旧实现开了 30s 心跳；AsyncWebSocket 默认关闭，这里按客户端重新启用，
      // 连接被 WiFi 静默丢弃时能尽快发现并让前端重连。
      client->keepAlivePeriod(30);
      // 不在连接时推送配置：前端 ws.onopen 会自己 GET /api/config/*。
      // 之前两条路径同时序列化整份配置，页面加载瞬间的堆峰值会直接 OOM。
      break;
    case WS_EVT_DATA: {
      AwsFrameInfo* info = reinterpret_cast<AwsFrameInfo*>(arg);
      if (info->message_opcode != WS_TEXT) break;
      // 一帧可能被 TCP 拆成多次回调（index 为帧内偏移）；浏览器也可能把
      // 一条消息拆成多个 frame（opcode=CONTINUATION）。必须凑齐再处理，
      // 否则 keys.batchSave 这类 >1.4KB 的命令会被静默丢弃。
      if (info->index == 0 && info->opcode != WS_CONTINUATION) wsInboundFrame = "";
      if (wsInboundFrame.length() + length > 8192) {
        wsInboundFrame = "";
        Serial.println("[WS] command exceeds 8KB; dropped");
        break;
      }
      wsInboundFrame.concat(reinterpret_cast<const char*>(payload), length);
      if (!info->final || info->index + length != info->len) break;
      queueWsCommand(client->id(), reinterpret_cast<const uint8_t*>(wsInboundFrame.c_str()),
                     wsInboundFrame.length());
      wsInboundFrame = "";
      break;
    }
    case WS_EVT_DISCONNECT:
      wsInboundFrame = "";
      break;
    case WS_EVT_ERROR:
    case WS_EVT_PONG:
      break;
  }
}

// WebSocket 命令路由：与旧 socketEvent 的 WStype_TEXT 分支等价。
// 仅在 loop() 中调用（见 drainWsCommands）。
void handleWsCommand(uint32_t clientId, uint8_t* payload, size_t length) {
  ConfigGuard configGuard;  // 与 HTTP 读取接口互斥
  JsonDocument request, response;
  String error;
  bool applyApAfter = false, applyBleNameAfter = false;
  if (length > 8192 || deserializeJson(request, payload, length)) error = "Invalid or oversized JSON";
  response["id"] = request["id"];
  String op = request["op"] | "";
  JsonObjectConst args = request["args"].as<JsonObjectConst>();
  if (error.isEmpty()) {
    if (op == "state.get") {
      pushState(static_cast<int>(clientId));
      response["ok"] = true;
      wsSendJson(response, static_cast<int>(clientId));
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
        wsSendJson(response, static_cast<int>(clientId));
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
  wsSendJson(response, static_cast<int>(clientId));
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
      wsSend(message);
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
    wsSend(message);
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
      wsSend(msg);
    }
    dispatchInput("ir", "", proto.c_str(), code, repeat);
  }
}

// ==================== 导出流式响应 ====================
//
// 采样文件每个最多 749 段脉冲、可达数 KB；一个遥控器几十个键就是上百 KB。
// 全部拼进一个 String 会在 ESP32-C3 上直接耗尽堆（迁移到 AsyncWebServer 时
// 曾把原来的 sendContent 流式输出改成整串拼接）。这里用 chunked 响应：
// 配置部分先序列化（≤32KB），captures 在 filler 里按需从 LittleFS 读取。
struct ExportCapture {
  String prefix;  // 前导逗号（除第一个）+ JSON 路径键 + ":"
  String path;
};

struct ExportStream {
  enum Phase { kHead, kPrefix, kFile, kTail, kDone } phase = kHead;
  String head, tail;
  std::vector<ExportCapture> captures;
  size_t captureIndex = 0;
  size_t headOffset = 0, prefixOffset = 0, tailOffset = 0;
  File file;

  void closeFile() {
    if (file) file.close();
  }

  // 跳到下一个采样；没有更多则进入收尾段。
  void nextCapture() {
    closeFile();
    captureIndex++;
    prefixOffset = 0;
    if (captureIndex >= captures.size()) {
      phase = kTail;
      tailOffset = 0;
    } else {
      phase = kPrefix;
    }
  }

  // 注意：做成成员函数而不是自由函数——Arduino 会为 .ino 里的自由函数生成
  // 前置声明，插入位置在结构体定义之前，ExportStream 类型此时还不存在。
  static size_t fill(ExportStream& stream, uint8_t* buffer, size_t maxLen) {
    size_t written = 0;
    while (written < maxLen) {
      if (stream.phase == kHead || stream.phase == kPrefix || stream.phase == kTail) {
        String* source = stream.phase == kHead ? &stream.head
                       : stream.phase == kPrefix
                           ? &stream.captures[stream.captureIndex].prefix
                           : &stream.tail;
        size_t* offset = stream.phase == kHead ? &stream.headOffset
                       : stream.phase == kPrefix
                           ? &stream.prefixOffset
                           : &stream.tailOffset;
        size_t count = source->length() - *offset;
        if (count > maxLen - written) count = maxLen - written;
        if (count) {
          memcpy(buffer + written, source->c_str() + *offset, count);
          *offset += count;
          written += count;
        }
        if (*offset < source->length()) break;  // 缓冲已写满，等待下一次回调
        if (stream.phase == kHead) {
          if (stream.captures.empty()) {
            stream.phase = kTail;
            stream.tailOffset = 0;
          } else {
            stream.phase = kPrefix;
            stream.prefixOffset = 0;
          }
        } else if (stream.phase == kPrefix) {
          stream.file = LittleFS.open(stream.captures[stream.captureIndex].path, "r");
          if (stream.file) stream.phase = kFile;
          else stream.nextCapture();  // 列表生成后被删除，跳过
        } else {
          stream.phase = kDone;
          break;
        }
      } else if (stream.phase == kFile) {
        size_t count = stream.file.read(buffer + written, maxLen - written);
        if (count) written += count;
        else stream.nextCapture();
      } else {
        break;
      }
    }
    return written;
  }
};

void exportData(AsyncWebServerRequest* request) {
  ConfigGuard configGuard;  // 快照配置，避免与 loop() 的保存并发
  String scope = request->arg("scope");
  String id = request->arg("id");
  if (scope == "remote" && id.isEmpty()) id = request->arg("remoteId");
  if (scope == "macro" && id.isEmpty()) id = request->arg("macroId");
  if (scope != "remotes" && scope != "macros" && scope != "all" && scope != "remote" && scope != "macro") {
    request->send(400, "application/json", "{\"error\":\"Invalid export scope\"}"); return;
  }
  if ((scope == "remote" || scope == "macro") && id.isEmpty()) {
    request->send(400, "application/json", "{\"error\":\"Missing id parameter\"}"); return;
  }
  if (!storageReady) {
    request->send(503, "application/json", "{\"error\":\"Configuration unavailable\"}"); return;
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
      request->send(404, "application/json", "{\"error\":\"Macro not found\"}"); return;
    }
  }

  String head = "{\"format\":\"esp32-remote\"";
  // 预留一次到位，避免 String 反复扩容在高碎片堆上产生额外峰值。
  head.reserve(measureJson(remoteConfig) + 256);

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
    for (JsonObjectConst r : remoteConfig["remotes"].as<JsonArrayConst>()) {
      bool added = false;
      for (JsonObjectConst k : remoteConfig["keys"].as<JsonArrayConst>()) {
        if (macroHasKeyId(k["id"].as<String>()) && k["remoteId"].as<String>() == r["id"].as<String>()) {
          remotesArr.add(r); added = true; break;
        }
      }
      (void)added;
    }
  } else if (scope != "macros") {
    for (JsonObjectConst r : remoteConfig["remotes"].as<JsonArrayConst>()) remotesArr.add(r);
  }
  String json; serializeJson(outRemotes, json);
  head += ",\"remotes\":" + json;

  // 2. keys 数组
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
  head += ",\"keys\":" + json;

  // 3. captures 对象：只在 head 里写开头，条目由 exportFiller 流式输出。
  head += ",\"captures\":{";
  auto stream = std::make_shared<ExportStream>();
  stream->captures.reserve(remoteConfig["keys"].as<JsonArrayConst>().size());
  for (JsonObjectConst k : remoteConfig["keys"].as<JsonArrayConst>()) {
    if (scope == "remote" && k["remoteId"].as<String>() != id) continue;
    if (scope == "macro" && !macroHasKeyId(k["id"].as<String>())) continue;
    String path = k["capturePath"] | "";
    if (path.isEmpty() || !LittleFS.exists(path)) continue;
    JsonDocument label; label.set(path);
    json = ""; serializeJson(label, json);
    ExportCapture capture;
    capture.prefix = (stream->captures.empty() ? "" : ",") + json + ":";
    capture.path = path;
    stream->captures.push_back(std::move(capture));
  }

  // 4. macros / mappings
  String tail = "}";
  if (scope == "macro") {
    tail += ",\"macros\":[";
    json = ""; serializeJson(exportedMacro, json);
    tail += json;
    tail += "]";
  } else if (scope != "remotes" && scope != "remote") {
    json = ""; serializeJson(remoteConfig["macros"], json);
    tail += ",\"macros\":" + json;
  }
  if (scope == "all") {
    json = ""; serializeJson(remoteConfig["mappings"], json);
    tail += ",\"mappings\":" + json;
  }
  tail += "}";

  stream->head = std::move(head);
  stream->tail = std::move(tail);
  AsyncWebServerResponse* response = request->beginChunkedResponse(
      "application/json; charset=utf-8",
      [stream](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
        (void)index;
        return ExportStream::fill(*stream, buffer, maxLen);
      });
  response->addHeader("Content-Disposition", "attachment; filename=\"ir-remote-export.json\"");
  request->send(response);
}

// ==================== 分段配置读取 ====================
//
// 整份配置（几十个按键 + 映射）一次序列化会给 C3 带来明显的堆压力。
// 这里按集合拆成多个接口，前端顺序拉取，任一时刻只占用一个集合的缓冲。

static AsyncWebSocketSharedBuffer allocJsonBody(size_t total, const char* what) {
  if (ESP.getMaxAllocHeap() < total + 2048) {
    Serial.printf("[WEB] %s skipped: need=%u maxAlloc=%u heap=%u\n",
                  what, static_cast<unsigned>(total), ESP.getMaxAllocHeap(), ESP.getFreeHeap());
    return nullptr;
  }
  return std::make_shared<std::vector<uint8_t>>(total);
}

static void sendJsonBody(AsyncWebServerRequest* request, AsyncWebSocketSharedBuffer body) {
  if (!body) {
    request->send(503, "application/json", "{\"error\":\"Insufficient memory\"}");
    return;
  }
  AsyncWebServerResponse* response = request->beginChunkedResponse(
      "application/json; charset=utf-8",
      [body](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
        size_t remaining = body->size() - index;
        size_t count = remaining < maxLen ? remaining : maxLen;
        if (count) memcpy(buffer, body->data() + index, count);
        return count;
      });
  request->send(response);
}

// GET /api/config/<集合> -> {"<集合>":[...]}
static void handleConfigSection(AsyncWebServerRequest* request, const char* key) {
  ConfigGuard configGuard;
  JsonVariantConst section = remoteConfig.as<JsonObjectConst>()[key];
  size_t keyLen = strlen(key);
  size_t total = 4 + keyLen + measureJson(section) + 3;  // {"key":<json>}
  auto body = allocJsonBody(total, key);
  if (!body) {
    request->send(503, "application/json", "{\"error\":\"Insufficient memory\"}");
    return;
  }
  char* out = reinterpret_cast<char*>(body->data());
  size_t used = 0;
  out[used++] = '{';
  out[used++] = '"';
  memcpy(out + used, key, keyLen); used += keyLen;
  out[used++] = '"';
  out[used++] = ':';
  used += serializeJson(section, out + used, total - used);
  out[used++] = '}';
  body->resize(used);
  sendJsonBody(request, body);
}

// GET /api/config/meta -> settings / hosts / nextId / lastHostId
static void handleConfigMeta(AsyncWebServerRequest* request) {
  ConfigGuard configGuard;
  JsonObjectConst root = remoteConfig.as<JsonObjectConst>();
  size_t settingsLen = measureJson(root["settings"]);
  size_t hostsLen = measureJson(root["hosts"]);
  size_t lastLen = measureJson(root["lastHostId"]);
  // {"settings":…,"hosts":…,"nextId":<≤10 位>,"lastHostId":…}
  size_t total = 12 + settingsLen + 9 + hostsLen + 10 + 10 + 14 + lastLen + 1 + 8;
  auto body = allocJsonBody(total, "meta");
  if (!body) {
    request->send(503, "application/json", "{\"error\":\"Insufficient memory\"}");
    return;
  }
  char* out = reinterpret_cast<char*>(body->data());
  size_t used = 0;
  memcpy(out + used, "{\"settings\":", 12); used += 12;
  used += serializeJson(root["settings"], out + used, total - used);
  memcpy(out + used, ",\"hosts\":", 9); used += 9;
  used += serializeJson(root["hosts"], out + used, total - used);
  used += snprintf(out + used, total - used, ",\"nextId\":%lu",
                   static_cast<unsigned long>(root["nextId"] | 1UL));
  memcpy(out + used, ",\"lastHostId\":", 14); used += 14;
  used += serializeJson(root["lastHostId"], out + used, total - used);
  out[used++] = '}';
  body->resize(used);
  sendJsonBody(request, body);
}

void setup() {
  Serial.begin(115200);
  configMutex = xSemaphoreCreateRecursiveMutex();
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
  server.on("/api/export", HTTP_GET, [](AsyncWebServerRequest* request) { exportData(request); });
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* request) {
    ConfigGuard configGuard;
    JsonDocument doc;
    doc["storageReady"] = storageReady;
    doc["filesystemMounted"] = filesystemMounted;
    doc["heap"] = ESP.getFreeHeap();
    uint32_t fsTotal, fsUsed;
    fsUsageCached(fsTotal, fsUsed);
    doc["fsTotal"] = fsTotal;
    doc["fsUsed"] = fsUsed;
    doc["learningRemote"] = learningRemote;
    doc["frames"] = frameCount;
    doc["hidRunning"] = hidRunning;
    // 便于在未部署前端时展示实际生效的热点与蓝牙名称。
    doc["apSsid"] = remoteConfig["settings"]["apSsid"] | kApSsid;
    doc["bleName"] = remoteConfig["settings"]["bleName"] | "Remote Master";
    doc["irTxMode"] = remoteConfig["settings"]["irTxMode"] | "off";
    doc["irTxRunning"] = irTxBusy();
    bleHostStatus(doc["ble"].to<JsonObject>());
    String message; serializeJson(doc, message);
    request->send(200, "application/json", message);
  });
  // 配置按集合分段下发：单次响应只序列化一个集合，避免整份配置的堆峰值。
  // 前端启动/收到 WS config.changed 后顺序拉取这些接口。
  server.on("/api/config/meta", HTTP_GET, [](AsyncWebServerRequest* request) { handleConfigMeta(request); });
  server.on("/api/config/remotes", HTTP_GET, [](AsyncWebServerRequest* request) { handleConfigSection(request, "remotes"); });
  server.on("/api/config/keys", HTTP_GET, [](AsyncWebServerRequest* request) { handleConfigSection(request, "keys"); });
  server.on("/api/config/macros", HTTP_GET, [](AsyncWebServerRequest* request) { handleConfigSection(request, "macros"); });
  server.on("/api/config/mappings", HTTP_GET, [](AsyncWebServerRequest* request) { handleConfigSection(request, "mappings"); });
  // 未命中任何显式路由时按 /frontend 静态资源处理（含 SPA 路由回退）。
  server.onNotFound([](AsyncWebServerRequest* request) { fileApiServeStatic(request); });
  // 挂载 WebSocket：/ws 端点
  ws.onEvent(wsEvent);
  server.addHandler(&ws);
  server.begin();
  Serial.printf("[WEB] http://%s; WebSocket /ws\n", WiFi.softAPIP().toString().c_str());
}

void loop() {
  // loop 分段计时遥测：单轮超过 150ms 时输出各环节耗时（ms），用于定位
  // "按键后设备几秒无响应"这类阻塞问题。正常迭代 <5ms，遥测零噪音。
  // 排查结束后此块可整体移除（标记 [LOOP]）。
  uint32_t t0 = micros();
  // system.reset 的实际执行点：响应已在上一轮的 handleWsCommand 里通过 AsyncWebSocket 发出，
  // 这里稍作延迟让 TCP 把回包推出去，再执行数秒级的格式化。
  if (pendingFactoryReset) {
    ConfigGuard configGuard;  // 重建配置期间禁止 HTTP 读取半成品
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
  // HTTP 由 AsyncWebServer 在底层 task 自动处理；WS 命令在 TCP 任务里只入队，
  // 这里串行执行，保证 remoteConfig / flash 写入与 loop() 的其他逻辑互斥。
  uint32_t tHttp = micros();
  drainWsCommands();
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
      wsSend(message);
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
