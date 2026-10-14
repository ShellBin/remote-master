#include "ble_hid.h"
#include "globals.h"
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include "remote_store.h"
#include <atomic>
#include "nimble/nimble/host/services/gap/include/services/gap/ble_svc_gap.h"

// ASCII -> HID 键码映射表 [ascii] = {hid_key, modifier}
// 仅定义可打印字符 (0x20-0x7E)
static const uint8_t kAsciiToHid[128][2] = {
  {0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},   // 0x00-0x07
  {0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},   // 0x08-0x0F
  {0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},   // 0x10-0x17
  {0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},   // 0x18-0x1F
  {44,0},{30,2},{52,2},{32,2},{33,2},{34,2},{36,2},{52,0},  //  !"#$%&'
  {38,2},{39,2},{37,2},{46,2},{54,0},{45,0},{55,0},{56,0},  // ()*+,-./
  {39,0},{30,0},{31,0},{32,0},{33,0},{34,0},{35,0},{36,0},  // 01234567
  {37,0},{38,0},{51,2},{51,0},{54,2},{46,0},{55,2},{56,2},  // 89:;<=>?
  {31,2},{4,2},{5,2},{6,2},{7,2},{8,2},{9,2},{10,2},  // @ABCDEFG
  {11,2},{12,2},{13,2},{14,2},{15,2},{16,2},{17,2},{18,2},  // HIJKLMNO
  {19,2},{20,2},{21,2},{22,2},{23,2},{24,2},{25,2},{26,2},  // PQRSTUVW
  {27,2},{28,2},{29,2},{47,0},{49,0},{48,0},{35,2},{45,2},  // XYZ[\]^_
  {53,0},{4,0},{5,0},{6,0},{7,0},{8,0},{9,0},{10,0},  // `abcdefg
  {11,0},{12,0},{13,0},{14,0},{15,0},{16,0},{17,0},{18,0},  // hijklmno
  {19,0},{20,0},{21,0},{22,0},{23,0},{24,0},{25,0},{26,0},  // pqrstuvw
  {27,0},{28,0},{29,0},{47,2},{49,2},{48,2},{53,2},{0,0},   // xyz{|}~ DEL
};

// ==================== HID Report Descriptor ====================
// 组合键盘(1) + 鼠标(2) + 消费者控制(3)

static uint8_t kHidReportMap[] = {
  // ---------- 键盘 ----------
  0x05, 0x01,        // Usage Page (Generic Desktop)
  0x09, 0x06,        // Usage (Keyboard)
  0xA1, 0x01,        // Collection (Application)
  0x85, 0x01,        //   Report ID (1)
  0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
  0x19, 0xE0,        //   Usage Minimum (0xE0)
  0x29, 0xE7,        //   Usage Maximum (0xE7)
  0x15, 0x00,        //   Logical Minimum (0)
  0x25, 0x01,        //   Logical Maximum (1)
  0x95, 0x08,        //   Report Count (8)
  0x75, 0x01,        //   Report Size (1)
  0x81, 0x02,        //   Input (Data,Var,Abs) - 修饰键
  0x95, 0x01,        //   Report Count (1)
  0x75, 0x08,        //   Report Size (8)
  0x81, 0x03,        //   Input (Const,Var,Abs) - 保留
  0x95, 0x06,        //   Report Count (6)
  0x75, 0x08,        //   Report Size (8)
  0x15, 0x00,        //   Logical Minimum (0)
  0x25, 0x73,        //   Logical Maximum (115)
  0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
  0x19, 0x00,        //   Usage Minimum (0)
  0x29, 0x73,        //   Usage Maximum (115)
  0x81, 0x00,        //   Input (Data,Array,Abs) - 按键
  0x05, 0x08,        //   Usage Page (LEDs)
  0x19, 0x01, 0x29, 0x05,
  0x15, 0x00, 0x25, 0x01,
  0x95, 0x05, 0x75, 0x01,
  0x91, 0x02,        //   Output (5 LED bits)
  0x95, 0x01, 0x75, 0x03,
  0x91, 0x03,        //   Output (padding)
  0xC0,              // End Collection

  // ---------- 鼠标 ----------
  0x05, 0x01,        // Usage Page (Generic Desktop)
  0x09, 0x02,        // Usage (Mouse)
  0xA1, 0x01,        // Collection (Application)
  0x85, 0x02,        //   Report ID (2)
  0x09, 0x01,        //   Usage (Pointer)
  0xA1, 0x00,        //   Collection (Physical)
  0x05, 0x09,        //     Usage Page (Button)
  0x19, 0x01,        //     Usage Minimum (1)
  0x29, 0x03,        //     Usage Maximum (3)
  0x15, 0x00,        //     Logical Minimum (0)
  0x25, 0x01,        //     Logical Maximum (1)
  0x95, 0x03,        //     Report Count (3)
  0x75, 0x01,        //     Report Size (1)
  0x81, 0x02,        //     Input (Data,Var,Abs) - 按钮
  0x95, 0x01,        //     Report Count (1)
  0x75, 0x05,        //     Report Size (5)
  0x81, 0x03,        //     Input (Const,Var,Abs) - 填充
  0x05, 0x01,        //     Usage Page (Generic Desktop)
  0x09, 0x30,        //     Usage (X)
  0x09, 0x31,        //     Usage (Y)
  0x09, 0x38,        //     Usage (Wheel)
  0x15, 0x81,        //     Logical Minimum (-127)
  0x25, 0x7F,        //     Logical Maximum (127)
  0x75, 0x08,        //     Report Size (8)
  0x95, 0x03,        //     Report Count (3)
  0x81, 0x06,        //     Input (Data,Var,Rel)
  0xC0,              //   End Collection
  0xC0,              // End Collection

  // ---------- 消费者控制（媒体键） ----------
  0x05, 0x0C,        // Usage Page (Consumer)
  0x09, 0x01,        // Usage (Consumer Control)
  0xA1, 0x01,        // Collection (Application)
  0x85, 0x03,        //   Report ID (3)
  0x05, 0x0C,        //   Usage Page (Consumer)
  0x15, 0x00,        //   Logical Minimum (0)
  0x26, 0xFF, 0x03,  //   Logical Maximum (1023)
  0x19, 0x00,        //   Usage Minimum (0)
  0x2A, 0xFF, 0x03,  //   Usage Maximum (1023)
  0x75, 0x10,        //   Report Size (16)
  0x95, 0x01,        //   Report Count (1)
  0x81, 0x00,        //   Input (Data,Array,Abs)
  0xC0               // End Collection
};

// ==================== BLE 对象 ====================

static NimBLEHIDDevice* hid = nullptr;
static NimBLEServer* hidServer = nullptr;
static NimBLECharacteristic* inputKeyboard = nullptr;
static NimBLECharacteristic* inputMouse = nullptr;
static NimBLECharacteristic* inputConsumer = nullptr;
static std::atomic<bool> bleConnected{false};
static std::atomic<bool> restartAdvertising{false};
static std::atomic<uint16_t> peerHandle{BLE_HS_CONN_HANDLE_NONE};
static uint32_t pairingUntil = 0, hostDeadline = 0;
static uint8_t hostSlot = 0;
static String preferredHost;
static String currentHost;
static std::atomic<uint8_t> reportSubscriptions{0};
static std::atomic<int> lastDisconnectReason{0};
static bool advertisingStarted = false;
static std::atomic<bool> newHostPairing{false};
static std::atomic<uint16_t> forgettingHandle{BLE_HS_CONN_HANDLE_NONE};

class HidReportCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic* characteristic, NimBLEConnInfo& info,
                   uint16_t value) override {
    uint8_t bit = characteristic == inputKeyboard ? 1 : characteristic == inputMouse ? 2 : 4;
    if (value & 1) reportSubscriptions.fetch_or(bit);
    else reportSubscriptions.fetch_and(static_cast<uint8_t>(~bit));
    Serial.printf("[BLE] report=%u subscribed=%u handle=%u\n", bit, value, info.getConnHandle());
  }
};
static HidReportCallbacks reportCallbacks;

class HidServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override {
    uint16_t expected = BLE_HS_CONN_HANDLE_NONE;
    if (!peerHandle.compare_exchange_strong(expected, info.getConnHandle())) {
      server->disconnect(info.getConnHandle());
      return;
    }
    // During an explicit add-host window, bonded hosts must not win the race.
    if (newHostPairing.load() && NimBLEDevice::isBonded(info.getIdAddress())) {
      server->disconnect(info.getConnHandle());
      return;
    }
    Serial.printf("[BLE] connected address=%s handle=%u\n",
                  info.getAddress().toString().c_str(), info.getConnHandle());
    server->updateConnParams(info.getConnHandle(), 12, 24, 0, 400);
    int rc = 0;
    bool started = NimBLEDevice::startSecurity(info.getConnHandle(), &rc);
    Serial.printf("[BLE] security started=%u rc=%d\n", started, rc);
  }
  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    Serial.printf("[BLE] encrypted=%u bonded=%u authenticated=%u keyBytes=%u\n",
                  info.isEncrypted(), info.isBonded(), info.isAuthenticated(), info.getSecKeySize());
    if (!info.isEncrypted() || info.getConnHandle() == forgettingHandle.load()) {
      hidServer->disconnect(info.getConnHandle()); return;
    }
    bleConnected.store(true);
  }
  void onIdentity(NimBLEConnInfo& info) override {
    if (newHostPairing.load() && NimBLEDevice::isBonded(info.getIdAddress()))
      hidServer->disconnect(info.getConnHandle());
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int reason) override {
    if (info.getConnHandle() != peerHandle.load()) return;
    bleConnected.store(false);
    reportSubscriptions.store(0);
    lastDisconnectReason.store(reason);
    peerHandle.store(BLE_HS_CONN_HANDLE_NONE);
    restartAdvertising.store(true);
    Serial.printf("[BLE] disconnected reason=%d\n", reason);
  }
};

// ==================== 初始化 ====================

// 广播负载 31 字节：flags(3) + appearance(4) + 16 位 UUID(4) + 头(2)，
// 主广播包内名称最多 18 字节，超出时只放进扫描响应（上限 29 字节）。
static void configureAdvertisementData(const String& name) {
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->setConnectableMode(BLE_GAP_CONN_MODE_UND);
  advertising->setDiscoverableMode(BLE_GAP_DISC_MODE_GEN);
  NimBLEAdvertisementData advertisement, scanResponse;
  advertisement.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  advertisement.setAppearance(HID_KEYBOARD);
  advertisement.setCompleteServices(NimBLEUUID(static_cast<uint16_t>(0x1812)));
  if (name.length() <= 18) advertisement.setName(name.c_str());
  scanResponse.setName(name.c_str());
  if (!advertising->setAdvertisementData(advertisement) ||
      !advertising->setScanResponseData(scanResponse))
    Serial.println("[BLE] advertising data configuration failed");
  advertising->enableScanResponse(true);
}

static String configuredBleName() {
  return String(remoteConfig["settings"]["bleName"] | "ESP32-C3 IR Remote");
}

void bleHidBegin() {
  String name = configuredBleName();
  NimBLEDevice::init(name.c_str());
  ble_svc_gap_device_appearance_set(HID_KEYBOARD);
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  hidServer = NimBLEDevice::createServer();
  hidServer->setCallbacks(new HidServerCallbacks());
  hidServer->advertiseOnDisconnect(false);
  hid = new NimBLEHIDDevice(hidServer);
  hid->setManufacturer("ShellBin");
  hid->setPnp(0x02, 0xE502, 0xA111, 0x0210);
  hid->setHidInfo(0x00, 0x02);
  inputKeyboard = hid->getInputReport(1);
  hid->getOutputReport(1);
  inputMouse = hid->getInputReport(2);
  inputConsumer = hid->getInputReport(3);
  inputKeyboard->setCallbacks(&reportCallbacks);
  inputMouse->setCallbacks(&reportCallbacks);
  inputConsumer->setCallbacks(&reportCallbacks);
  hid->setReportMap(kHidReportMap, sizeof(kHidReportMap));
  hid->setBatteryLevel(100);
  if (!hidServer->start()) {
    Serial.println("[BLE] GATT server start failed");
    return;
  }
  configureAdvertisementData(name);
  bleReconnectHosts();

  Serial.printf("[OK] BLE HID 已启动：%s\n", name.c_str());
}

void bleApplyName() {
  if (!hidServer) return;
  String name = configuredBleName();
  NimBLEDevice::setDeviceName(name.c_str());
  configureAdvertisementData(name);
  bleReconnectHosts();
  Serial.printf("[BLE] device name applied: %s\n", name.c_str());
}

bool bleHidConnected() {
  return bleConnected.load();
}

void bleReconnectHosts() {
  if (hidServer && peerHandle.load() != BLE_HS_CONN_HANDLE_NONE)
    hidServer->disconnect(peerHandle.load());
  hostSlot = 0;
  hostDeadline = 0;
  preferredHost = "";
  restartAdvertising.store(true);
}

void blePairingOpen() {
  stopHid("pair-new-host");
  forgettingHandle.store(peerHandle.load());
  bleConnected.store(false);
  reportSubscriptions.store(0);
  newHostPairing.store(NimBLEDevice::getNumBonds() > 0);
  pairingUntil = millis() + 120000;
  bleReconnectHosts();
}

bool bleHostForget(const char* address, uint8_t addressType) {
  NimBLEAddress identity(std::string(address), addressType);
  uint16_t handle = peerHandle.load();
  bool active = handle != BLE_HS_CONN_HANDLE_NONE &&
                hidServer->getPeerInfoByHandle(handle).getIdAddress() == identity;
  if (active) stopHid("forget-host");
  // Missing bonds are already forgotten; errors must not become success responses.
  bool existed = NimBLEDevice::isBonded(identity);
  // NimBLE rejects IRK removal while advertising is active (BLE_HS_EBUSY).
  auto advertising = NimBLEDevice::getAdvertising();
  bool stopped = !advertising->isAdvertising() || advertising->stop();
  int rc = existed && stopped ? ble_gap_unpair(identity.getBase())
                             : existed ? BLE_HS_EBUSY : 0;
  bool removed = rc == 0;
  if (!removed || NimBLEDevice::isBonded(identity)) {
    restartAdvertising.store(true);
    Serial.printf("[BLE] forget failed address=%s type=%u rc=%d advertisingStopped=%u\n",
                  address, addressType, rc, stopped);
    return false;
  }
  if (active) {
    forgettingHandle = handle;
    bleConnected.store(false);
    reportSubscriptions.store(0);
    hidServer->disconnect(handle);
  }
  pairingUntil = 0;
  newHostPairing.store(false);
  restartAdvertising.store(true);
  Serial.printf("[BLE] forgotten address=%s type=%u bondExisted=%u\n",
                address, addressType, existed);
  return true;
}

void bleHostStatus(JsonObject obj) {
  obj["connected"] = bleHidConnected();
  obj["host"] = currentHost;
  obj["preferredHost"] = preferredHost;
  bool unpaired = NimBLEDevice::getNumBonds() == 0;
  obj["pairing"] = unpaired || (pairingUntil && static_cast<int32_t>(pairingUntil - millis()) > 0);
  obj["pairingAlwaysOpen"] = unpaired;
  obj["bondCount"] = NimBLEDevice::getNumBonds();
  obj["pairingNewHost"] = newHostPairing.load();
  obj["linkConnected"] = peerHandle.load() != BLE_HS_CONN_HANDLE_NONE;
  obj["reportSubscriptions"] = reportSubscriptions.load();
  obj["advertising"] = advertisingStarted && NimBLEDevice::getAdvertising()->isAdvertising();
  obj["lastDisconnectReason"] = lastDisconnectReason.load();
}

void tickBleHosts() {
  // BLE callbacks only set atomics. Filesystem and JSON work stay on loop().
  if (pairingUntil && static_cast<int32_t>(millis() - pairingUntil) >= 0) {
    pairingUntil = 0;
    newHostPairing.store(false);
    if (peerHandle.load() == BLE_HS_CONN_HANDLE_NONE) bleReconnectHosts();
  }
  uint16_t handle = peerHandle.load();
  if (handle == forgettingHandle && handle != BLE_HS_CONN_HANDLE_NONE) return;
  forgettingHandle = BLE_HS_CONN_HANDLE_NONE;
  if (bleHidConnected() && handle != BLE_HS_CONN_HANDLE_NONE && currentHost.isEmpty()) {
    auto info = hidServer->getPeerInfoByHandle(handle);
    if (info.isBonded()) {
      auto address = info.getIdAddress();
      currentHost = address.toString().c_str();
      pairingUntil = 0;
      newHostPairing.store(false);
      hostDeadline = 0;
      JsonArray hosts = remoteConfig["hosts"].as<JsonArray>();
      JsonObject found;
      for (JsonObject host : hosts)
        if (host["address"].as<String>() == currentHost &&
            host["addressType"].as<uint8_t>() == address.getType()) found = host;
      if (found.isNull() && hosts.size() < 8) {
        found = hosts.add<JsonObject>();
        uint32_t next = remoteConfig["nextId"] | 1U;
        remoteConfig["nextId"] = next + 1;
        found["id"] = String(next);
        found["name"] = String("Host ") + currentHost;
        found["address"] = currentHost;
        found["addressType"] = address.getType();
        found["priority"] = hosts.size() - 1;
        found["enabled"] = true;
        if (!saveRemoteConfig()) Serial.println("[STORE] host save failed");
      }
    }
  }
  if (handle != BLE_HS_CONN_HANDLE_NONE) return;
  if (!currentHost.isEmpty()) {
    currentHost = "";
    stopHid("disconnected");
    bleReconnectHosts();
  }
  uint32_t now = millis();
  if (!restartAdvertising.exchange(false) &&
      (hostDeadline == 0 || static_cast<int32_t>(now - hostDeadline) < 0)) return;
  auto advertising = NimBLEDevice::getAdvertising();
  advertising->stop();
  while (NimBLEDevice::getWhiteListCount())
    NimBLEDevice::whiteListRemove(NimBLEDevice::getWhiteListAddress(0));
  preferredHost = "";
  JsonObject ordered[8];
  uint8_t count = 0;
  for (JsonObject host : remoteConfig["hosts"].as<JsonArray>()) {
    if (!(host["enabled"] | true)) continue;
    NimBLEAddress address(host["address"].as<std::string>(), host["addressType"] | 0);
    bool bonded = false;
    for (int i = 0; i < NimBLEDevice::getNumBonds(); ++i)
      if (NimBLEDevice::getBondedAddress(i) == address) bonded = true;
    if (bonded && count < 8) ordered[count++] = host;
  }
  for (uint8_t i = 0; i < count; ++i)
    for (uint8_t j = i + 1; j < count; ++j)
      if ((ordered[j]["priority"] | 99) < (ordered[i]["priority"] | 99)) {
        JsonObject swap = ordered[i]; ordered[i] = ordered[j]; ordered[j] = swap;
      }
  bool unpaired = NimBLEDevice::getNumBonds() == 0;
  if (!unpaired && !pairingUntil && count) {
    JsonObject host = ordered[hostSlot++ % count];
    preferredHost = host["address"].as<String>();
    if (!NimBLEDevice::whiteListAdd(NimBLEAddress(
        host["address"].as<std::string>(), host["addressType"] | 0)))
      Serial.println("[BLE] whitelist update failed");
    advertising->setScanFilter(false, true);
    hostDeadline = now + 10000;
  } else {
    // An empty device starts discoverable; a configured device requires pairing mode.
    advertising->setScanFilter(false, !unpaired && !pairingUntil);
    hostDeadline = 0;
  }
  advertisingStarted = advertising->start();
  Serial.printf("[BLE] advertising started=%u pairing=%u whitelist=%u preferred=%s\n",
                advertisingStarted, pairingUntil != 0,
                static_cast<unsigned>(NimBLEDevice::getWhiteListCount()), preferredHost.c_str());
}

// ==================== HID 动作执行 ====================

void sendRawKey(uint8_t key, uint8_t modifiers) {
  uint8_t report[8] = {modifiers, 0, key, 0, 0, 0, 0, 0};
  inputKeyboard->setValue(report, sizeof(report));
  inputKeyboard->notify();
}

void releaseHid() {
  if (!bleConnected.load()) return;
  // 释放键盘
  uint8_t keyReport[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  if (inputKeyboard) { inputKeyboard->setValue(keyReport, 8); inputKeyboard->notify(); }
  // 释放鼠标
  uint8_t mouseReport[4] = {0, 0, 0, 0};
  if (inputMouse) { inputMouse->setValue(mouseReport, 4); inputMouse->notify(); }
  // 释放媒体键
  uint8_t consumerReport[2] = {0, 0};
  if (inputConsumer) { inputConsumer->setValue(consumerReport, 2); inputConsumer->notify(); }
}

void stopHid(const char* reason) {
  bool wasMacro = macroRunning;
  releaseHid();
  hidRunning = macroRunning = hidNeedsRelease = false;
  runningSteps.clear();
  if (wasMacro) {
    Serial.print(F("[HID] 宏已停止: "));
    Serial.println(reason);
  }
}

bool executeBinding(const Binding& b) {
  if (hidRunning || !bleConnected) return false;
  uint8_t required = b.type == kTypeMouse ? 2 : b.type == kTypeMedia ? 4 : 1;
  if (!(reportSubscriptions.load() & required)) {
    Serial.printf("[HID] report not subscribed: %u\n", required);
    return false;
  }
  if (b.type == kTypeMacro) {
    if (deserializeJson(runningSteps, b.macro) ||
        !validMacro(runningSteps.as<JsonArrayConst>())) {
      runningSteps.clear(); return false;
    }
    macroStep = macroTextOffset = 0;
    macroRunning = true;
  }
  hidRunning = true;
  hidStarted = millis();
  hidDeadline = millis();
  hidNeedsRelease = b.type != kTypeMacro;
  switch (b.type) {
    case kTypeKeyboard:
      sendRawKey(b.key, b.modifiers);
      break;
    case kTypeMouse:
      if (b.mouseX || b.mouseY || b.mouseWheel) {
        uint8_t mouseReport[4] = {0, (uint8_t)b.mouseX, (uint8_t)b.mouseY, (uint8_t)b.mouseWheel};
        inputMouse->setValue(mouseReport, 4);
        inputMouse->notify();
      }
      if (b.mouseButton) {
        uint8_t mouseReport[4] = {b.mouseButton, 0, 0, 0};
        inputMouse->setValue(mouseReport, 4);
        inputMouse->notify();
      }
      break;
    case kTypeMedia:
      if (b.mediaCode) {
        uint8_t consumerReport[2] = {(uint8_t)(b.mediaCode & 0xFF), (uint8_t)(b.mediaCode >> 8)};
        inputConsumer->setValue(consumerReport, 2);
        inputConsumer->notify();
      }
      break;
  }
  if (hidNeedsRelease) hidDeadline = millis() + 50;
  return true;
}

void tickHid() {
  if (!hidRunning) return;
  if (!bleConnected) { stopHid("disconnected"); return; }
  if (millis() - hidStarted > 120000) { stopHid("timeout"); return; }
  if ((int32_t)(millis() - hidDeadline) < 0) return;
  if (hidNeedsRelease) {
    releaseHid();
    hidNeedsRelease = false;
    hidDeadline = millis() + 20;
    return;
  }
  if (!macroRunning || macroStep >= runningSteps.size()) {
    stopHid("completed");
    return;
  }
  JsonObjectConst step = runningSteps[macroStep];
  const char* kind = step["kind"] | "";
  if (strcmp(kind, "wait") == 0) {
    hidDeadline = millis() + step["ms"].as<uint32_t>();
    macroStep++;
  } else if (strcmp(kind, "key") == 0) {
    sendRawKey(step["key"], step["modifiers"]);
    macroStep++;
    hidNeedsRelease = true;
    hidDeadline = millis() + 50;
  } else if (strcmp(kind, "text") == 0) {
    const char* text = step["text"];
    if (!text) { stopHid("bad-step"); return; }
    uint8_t c = text[macroTextOffset++];
    // ASCII 转 HID 键码（仅支持可打印字符）
    uint8_t hidKey = 0, hidMod = 0;
    if (c < 128) {
      hidKey = kAsciiToHid[c][0];
      hidMod = kAsciiToHid[c][1];
    }
    if (hidKey == 0 && c != ' ') { stopHid("textError"); return; }
    sendRawKey(hidKey, hidMod);
    if (!text[macroTextOffset]) {
      macroTextOffset = 0;
      macroStep++;
    }
    hidNeedsRelease = true;
    hidDeadline = millis() + 10;
  } else if (strcmp(kind, "ir") == 0) {
    // 防御：含红外步骤的宏不允许通过蓝牙目标执行（保存层已拦截）。
    stopHid("ir-unsupported");
  } else stopHid("bad-step");
}
