#include "ble_hid.h"
#include "globals.h"
#include "ir_tx.h"
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
static uint32_t pairingUntil = 0;
static String connectedHost;    // 当前已加密连接的主机地址
static String tryingHost;       // 正在尝试连接的主机地址（白名单过滤目标）
static std::atomic<uint8_t> reportSubscriptions{0};
static std::atomic<int> lastDisconnectReason{0};
static bool advertisingStarted = false;
static std::atomic<bool> newHostPairing{false};
static std::atomic<uint16_t> forgettingHandle{BLE_HS_CONN_HANDLE_NONE};
static uint32_t forgettingSince = 0;  // forgettingHandle 的兜底超时起点

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
    server->updateConnParams(info.getConnHandle(), 9, 12, 0, 1000);
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
  return String(remoteConfig["settings"]["bleName"] | "Remote Master");
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
  restartAdvertising.store(true);

  Serial.printf("[OK] BLE HID 已启动：%s\n", name.c_str());
}

void bleApplyName() {
  if (!hidServer) return;
  String name = configuredBleName();
  NimBLEDevice::setDeviceName(name.c_str());
  configureAdvertisementData(name);
  restartAdvertising.store(true);
  Serial.printf("[BLE] device name applied: %s\n", name.c_str());
}

bool bleHidConnected() {
  return bleConnected.load();
}

// 尝试重连指定主机：断开当前连接，将该地址设为白名单目标，触发广播
bool bleReconnectToHost(const String& address, uint8_t addressType) {
  if (hidServer && peerHandle.load() != BLE_HS_CONN_HANDLE_NONE)
    hidServer->disconnect(peerHandle.load());
  tryingHost = address;
  restartAdvertising.store(true);
  Serial.printf("[BLE] reconnect requested to %s type=%u\n", address.c_str(), addressType);
  return true;
}

void blePairingOpen() {
  stopHid("pair-new-host");
  // 主动断开当前主机：只打 forgettingHandle 标记等认证回调是踢不掉已加密
  // 连接的（旧主机不会再触发 onAuthenticationComplete），会导致 tickBleHosts
  // 永远提前 return、广播无法重启——新主机永远搜不到设备。
  uint16_t handle = peerHandle.load();
  if (handle != BLE_HS_CONN_HANDLE_NONE && hidServer) {
    forgettingHandle.store(handle);
    forgettingSince = millis();
    hidServer->disconnect(handle);
  }
  bleConnected.store(false);
  reportSubscriptions.store(0);
  newHostPairing.store(NimBLEDevice::getNumBonds() > 0);
  tryingHost = "";
  pairingUntil = millis() + 120000;
  restartAdvertising.store(true);
}

bool bleHostForget(const char* address, uint8_t addressType) {
  NimBLEAddress identity(std::string(address), addressType);
  uint16_t handle = peerHandle.load();
  bool active = handle != BLE_HS_CONN_HANDLE_NONE &&
                hidServer->getPeerInfoByHandle(handle).getIdAddress() == identity;
  if (active) stopHid("forget-host");
  bool existed = NimBLEDevice::isBonded(identity);
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
    forgettingSince = millis();
    bleConnected.store(false);
    reportSubscriptions.store(0);
    hidServer->disconnect(handle);
  }
  // If we forgot the lastHost, clear it
  String lastHostId = remoteConfig["lastHostId"] | "";
  if (!lastHostId.isEmpty()) {
    for (JsonObject host : remoteConfig["hosts"].as<JsonArray>()) {
      if (host["id"].as<String>() == lastHostId &&
          host["address"].as<String>() == address &&
          (host["addressType"] | 0) == addressType) {
        remoteConfig.remove("lastHostId");
        saveRemoteConfig();
        break;
      }
    }
  }
  tryingHost = "";
  pairingUntil = 0;
  newHostPairing.store(false);
  restartAdvertising.store(true);
  Serial.printf("[BLE] forgotten address=%s type=%u bondExisted=%u\n",
                address, addressType, existed);
  return true;
}

// 恢复初始状态：断开当前连接并清除所有 BLE 绑定，使设备回到可配对的出厂状态。
void bleClearAllBonds() {
  stopHid("factory-reset");
  uint16_t handle = peerHandle.load();
  if (handle != BLE_HS_CONN_HANDLE_NONE && hidServer) {
    hidServer->disconnect(handle);
  }
  bleConnected.store(false);
  reportSubscriptions.store(0);
  tryingHost = "";
  pairingUntil = 0;
  newHostPairing.store(false);
  NimBLEDevice::deleteAllBonds();
  restartAdvertising.store(true);
  Serial.println("[BLE] all bonds cleared");
}

void bleHostStatus(JsonObject obj) {
  obj["connected"] = bleHidConnected();
  obj["host"] = connectedHost;
  obj["tryingHost"] = tryingHost;
  bool unpaired = NimBLEDevice::getNumBonds() == 0;
  obj["pairing"] = unpaired || (pairingUntil && static_cast<int32_t>(pairingUntil - millis()) > 0);
  obj["pairingAlwaysOpen"] = unpaired;
  obj["bondCount"] = NimBLEDevice::getNumBonds();
  obj["pairingNewHost"] = newHostPairing.load();
  obj["linkConnected"] = peerHandle.load() != BLE_HS_CONN_HANDLE_NONE;
  obj["reportSubscriptions"] = reportSubscriptions.load();
  obj["advertising"] = advertisingStarted && NimBLEDevice::getAdvertising()->isAdvertising();
  obj["lastDisconnectReason"] = lastDisconnectReason.load();
  obj["lastHostId"] = remoteConfig["lastHostId"] | "";
}

// 从配置中查找 lastHostId 对应的地址和类型，未找到返回 false
static bool resolveLastHost(String& address, uint8_t& addressType) {
  String lastHostId = remoteConfig["lastHostId"] | "";
  if (lastHostId.isEmpty()) return false;
  for (JsonObjectConst host : remoteConfig["hosts"].as<JsonArrayConst>()) {
    if (host["id"].as<String>() == lastHostId) {
      address = host["address"].as<String>();
      addressType = host["addressType"] | 0;
      return true;
    }
  }
  return false;
}

// 从 hosts 配置中查找指定地址对应的 addressType
static bool resolveHostType(const String& address, uint8_t& addressType) {
  for (JsonObjectConst host : remoteConfig["hosts"].as<JsonArrayConst>()) {
    if (host["address"].as<String>() == address) {
      addressType = host["addressType"] | 0;
      return true;
    }
  }
  return false;
}

void tickBleHosts() {
  // BLE callbacks only set atomics. Filesystem and JSON work stay on loop().
  if (pairingUntil && static_cast<int32_t>(millis() - pairingUntil) >= 0) {
    pairingUntil = 0;
    newHostPairing.store(false);
    if (peerHandle.load() == BLE_HS_CONN_HANDLE_NONE)
      restartAdvertising.store(true);
  }
  uint16_t handle = peerHandle.load();
  if (handle == forgettingHandle && handle != BLE_HS_CONN_HANDLE_NONE) {
    // 等待主动断开完成；超过 5 秒还没断（回调丢失等异常）则放行，
    // 避免广播重启逻辑被永久卡死。
    if (millis() - forgettingSince < 5000) return;
    forgettingHandle = BLE_HS_CONN_HANDLE_NONE;
    Serial.println("[BLE] forgetting-handle timed out; resuming normal tick");
  }
  forgettingHandle = BLE_HS_CONN_HANDLE_NONE;

  // On successful connection: record host and persist as lastHost
  if (bleHidConnected() && handle != BLE_HS_CONN_HANDLE_NONE && connectedHost.isEmpty()) {
    auto info = hidServer->getPeerInfoByHandle(handle);
    if (info.isBonded()) {
      auto address = info.getIdAddress();
      connectedHost = address.toString().c_str();
      tryingHost = "";
      pairingUntil = 0;
      newHostPairing.store(false);

      // Find or create host entry
      JsonArray hosts = remoteConfig["hosts"].as<JsonArray>();
      JsonObject found;
      for (JsonObject host : hosts)
        if (host["address"].as<String>() == connectedHost &&
            host["addressType"].as<uint8_t>() == address.getType()) found = host;
      if (found.isNull() && hosts.size() < 8) {
        found = hosts.add<JsonObject>();
        uint32_t next = remoteConfig["nextId"] | 1U;
        remoteConfig["nextId"] = next + 1;
        found["id"] = String(next);
        found["name"] = String("Host ") + connectedHost;
        found["address"] = connectedHost;
        found["addressType"] = address.getType();
        if (!saveRemoteConfig()) Serial.println("[STORE] host save failed");
      }
      // Persist as lastHost
      if (!found.isNull()) {
        remoteConfig["lastHostId"] = found["id"].as<String>();
        if (!saveRemoteConfig()) Serial.println("[STORE] lastHost save failed");
      }
    }
  }

  // Disconnected: clear state, tick 会触发广播重启
  if (handle != BLE_HS_CONN_HANDLE_NONE) return;
  if (!connectedHost.isEmpty()) {
    connectedHost = "";
    tryingHost = "";
    stopHid("disconnected");
  }

  if (!restartAdvertising.exchange(false)) return;

  // 保存 reconnect 目标（bleReconnectToHost 设置），避免被清空后丢失
  String savedTryingHost = tryingHost;
  uint8_t savedTryingType = 0;
  if (!savedTryingHost.isEmpty()) {
    resolveHostType(savedTryingHost, savedTryingType);
  }

  // 配置广播：有目标则白名单过滤，否则开放可发现
  auto advertising = NimBLEDevice::getAdvertising();
  advertising->stop();
  while (NimBLEDevice::getWhiteListCount())
    NimBLEDevice::whiteListRemove(NimBLEDevice::getWhiteListAddress(0));
  tryingHost = "";

  bool unpaired = NimBLEDevice::getNumBonds() == 0;
  String targetAddress;
  uint8_t targetType = 0;

  // 优先使用 reconnect 指定的目标（bleReconnectToHost 设置的 tryingHost），
  // 否则回退到 lastHostId
  if (!savedTryingHost.isEmpty()) {
    targetAddress = savedTryingHost;
    targetType = savedTryingType;
  } else if (!unpaired && !pairingUntil) {
    resolveLastHost(targetAddress, targetType);
  }

  // 检查目标是否仍有 bond
  bool targetBonded = false;
  if (!targetAddress.isEmpty()) {
    NimBLEAddress addr(std::string(targetAddress.c_str()), targetType);
    for (int i = 0; i < NimBLEDevice::getNumBonds(); ++i)
      if (NimBLEDevice::getBondedAddress(i) == addr) targetBonded = true;
  }

  if (!unpaired && !pairingUntil && targetBonded) {
    tryingHost = targetAddress;
    NimBLEAddress addr(std::string(targetAddress.c_str()), targetType);
    NimBLEDevice::whiteListAdd(addr);
    advertising->setScanFilter(false, true);
  } else if (!unpaired && !pairingUntil) {
    // 有 bond 但目标不可达 — 开放可发现（fallback），避免设备假死
    advertising->setScanFilter(false, false);
    Serial.printf("[BLE] WARNING: target not bonded, advertising openly\n");
  } else {
    // 无配对主机或配对窗口开放：可被发现
    advertising->setScanFilter(false, false);
  }
  advertisingStarted = advertising->start();
  Serial.printf("[BLE] advertising started=%u pairing=%u whitelist=%u trying=%s\n",
                advertisingStarted, pairingUntil != 0,
                static_cast<unsigned>(NimBLEDevice::getWhiteListCount()), tryingHost.c_str());
}

// Fix: 空闲心跳，每 3 秒发送空报告防止 macOS 监管超时断连
static uint32_t lastKeepAlive = 0;
void tickKeepAlive() {
  if (!bleConnected.load()) { lastKeepAlive = millis(); return; }
  if (hidRunning) { lastKeepAlive = millis(); return; }
  if (millis() - lastKeepAlive < 3000) return;
  // 发送空报告维持链路
  uint8_t emptyReport[8] = {0};
  if (inputKeyboard) { inputKeyboard->setValue(emptyReport, 8); inputKeyboard->notify(); }
  lastKeepAlive = millis();
}

// ==================== HID 动作执行 ====================

// 追踪上次发送的报告类型，避免 releaseHid 发送多余报告导致 BLE 阻塞
static uint8_t activeReports = 0; // bit0=keyboard, bit1=mouse, bit2=consumer

// notify 耗时遥测：正常情况下 setValue+notify 是毫秒内的非阻塞调用。
// 若 BLE 栈 mbuf 耗尽或 ble_hs 锁被长操作持有，这里会阻塞 loop——
// 超过 50ms 即输出耗时，用于定位"按键后设备几秒无响应"类问题。
void sendRawKey(uint8_t key, uint8_t modifiers) {
  uint8_t report[8] = {modifiers, 0, key, 0, 0, 0, 0, 0};
  uint32_t started = micros();
  inputKeyboard->setValue(report, sizeof(report));
  inputKeyboard->notify();
  uint32_t elapsedUs = micros() - started;
  activeReports |= 1;
  Serial.printf("[HID] send key=%u mod=%u\n", key, modifiers);
  if (elapsedUs > 50000) Serial.printf("[HID] SLOW notify(press): %lums\n", (unsigned long)(elapsedUs / 1000));
}

void releaseHid() {
  if (!bleConnected.load()) { activeReports = 0; return; }
  uint8_t empty[8] = {0};
  uint32_t started = micros();
  if (activeReports & 1) { if (inputKeyboard) { inputKeyboard->setValue(empty, 8); inputKeyboard->notify(); } }
  if (activeReports & 2) { if (inputMouse) { inputMouse->setValue(empty, 4); inputMouse->notify(); } }
  if (activeReports & 4) { if (inputConsumer) { inputConsumer->setValue(empty, 2); inputConsumer->notify(); } }
  uint32_t elapsedUs = micros() - started;
  activeReports = 0;
  Serial.println("[HID] release");
  if (elapsedUs > 50000) Serial.printf("[HID] SLOW notify(release): %lums\n", (unsigned long)(elapsedUs / 1000));
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

// 追踪上次按下的键盘键，用于"按住"模型
static uint8_t lastHidKey = 0;
static uint8_t lastHidModifiers = 0;
static uint32_t lastKeyInputAt = 0;  // 上次同键输入到达的时刻（连击/长按判别用）

// 按住窗口：NEC 家族 repeat 帧约 108ms 一帧，Sony 等协议长按会每 ~45ms
// 重发完整帧。取 250ms 覆盖两者并容忍 loop 抖动；仍低于主机自动重复
// 触发阈值（macOS ~500ms），单击不受影响。
static const uint32_t kHoldWindowMs = 250;
// 同键完整帧间隔低于该值视为长按重发（Sony ~45ms），而不是用户重新点击。
// 人手连击很难稳定低于 120ms；NEC 家族长按走专用 repeat 帧，不受影响。
static const uint32_t kRetriggerMinMs = 120;

bool executeBinding(const Binding& b, bool repeat) {
  if (!bleConnected) return false;
  // 注意：不检查 irTxBusy()。IR 发射走 RMT DMA，与 BLE notify 无共享资源，
  // 交叉阻塞会让 ~90ms 的发射窗口吞掉 BLE 按键。
  uint8_t required = b.type == kTypeMouse ? 2 : b.type == kTypeMedia ? 4 : 1;
  if (!(reportSubscriptions.load() & required)) {
    Serial.printf("[HID] report not subscribed: %u\n", required);
    return false;
  }
  uint32_t now = millis();
  if (b.type == kTypeMacro) {
    if (hidRunning) return false;  // 宏不抢占进行中的动作
    if (deserializeJson(runningSteps, b.macro) ||
        !validMacro(runningSteps.as<JsonArrayConst>())) {
      runningSteps.clear(); return false;
    }
    macroStep = macroTextOffset = 0;
    macroRunning = true;
  } else {
    // 键盘/鼠标/媒体键的同键重复分两种：
    // 1) 长按延续：repeat 帧（NEC 家族）或间隔 <120ms 的同键完整帧
    //    （Sony 等协议长按 = 高频重发完整帧）——只续按住时长，不发新击键；
    // 2) 用户重新点击（间隔 >=120ms 的非重复输入）——先释放再按下，
    //    主机才能注册第二次击键。此前所有同键输入都被当成情况 1 吞掉，
    //    表现为"第一次快、第二次起全卡"。
    bool sameKey = (hidRunning && !macroRunning && b.type == kTypeKeyboard &&
                    b.key == lastHidKey && b.modifiers == lastHidModifiers);
    if (sameKey && (repeat || now - lastKeyInputAt < kRetriggerMinMs)) {
      lastKeyInputAt = now;
      hidDeadline = now + kHoldWindowMs;
      return true;
    }
  }
  // 新击键或首次按下：先释放进行中的按键（同键连击也在这里完成 release）。
  if (hidRunning) {
    if (macroRunning) return false;
    stopHid("overwritten");
  }
  hidRunning = true;
  hidStarted = now;
  hidDeadline = now + kHoldWindowMs;
  hidNeedsRelease = b.type != kTypeMacro;
  switch (b.type) {
    case kTypeKeyboard:
      lastHidKey = b.key;
      lastHidModifiers = b.modifiers;
      lastKeyInputAt = now;
      sendRawKey(b.key, b.modifiers);
      break;
    case kTypeMouse:
      {
        uint8_t mouseReport[4] = {b.mouseButton, (uint8_t)b.mouseX, (uint8_t)b.mouseY, (uint8_t)b.mouseWheel};
        inputMouse->setValue(mouseReport, 4);
        inputMouse->notify();
        activeReports |= 2;
      }
      break;
    case kTypeMedia:
      if (b.mediaCode) {
        uint8_t consumerReport[2] = {(uint8_t)(b.mediaCode & 0xFF), (uint8_t)(b.mediaCode >> 8)};
        inputConsumer->setValue(consumerReport, 2);
        inputConsumer->notify();
        activeReports |= 4;
      }
      break;
  }
  // hidDeadline 已在上面设为 kHoldWindowMs：repeat 帧/同键续帧在松开前
  // 到达即可形成连续按住；单击时 250ms 的按住对主机无感。
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
    // 非宏按键 release 后立即完成：BLE notify 可靠有序，无需再留 20ms
    // 尾巴。宏保留 20ms 步间隔，给主机留出处理 release 的时间。
    if (!macroRunning) { stopHid("completed"); return; }
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
    hidDeadline = millis() + 200;
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
