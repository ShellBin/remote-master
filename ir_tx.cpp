#include "ir_tx.h"
#include "remote_store.h"
#include "globals.h"
#include <LittleFS.h>
#include <driver/gpio.h>
#include <driver/rmt_tx.h>
#include <vector>

namespace {
enum class Mode { Off, PushPull, Sink };
Mode mode = Mode::Off;
rmt_channel_handle_t channel = nullptr;
rmt_encoder_handle_t encoder = nullptr;
bool channelEnabled = false, running = false;
uint32_t deadline = 0, started = 0;
JsonDocument sequence;
size_t stepIndex = 0;
String lastError;

struct Frame {
  std::vector<rmt_symbol_word_t> symbols;
  uint32_t carrierHz = 38000;
  uint32_t durationUs = 0;
};
Frame frame;

void idlePin() {
  // Disconnect the peripheral matrix before restoring a static, inactive pin.
  gpio_reset_pin(static_cast<gpio_num_t>(kIrTxPin));
  gpio_set_level(static_cast<gpio_num_t>(kIrTxPin), mode == Mode::Sink ? 1 : 0);
  gpio_config_t config = {};
  config.pin_bit_mask = 1ULL << kIrTxPin;
  config.mode = mode == Mode::Off ? GPIO_MODE_INPUT :
                mode == Mode::Sink ? GPIO_MODE_OUTPUT_OD : GPIO_MODE_OUTPUT;
  config.pull_up_en = GPIO_PULLUP_DISABLE;
  config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  gpio_config(&config);
}

void releaseChannel() {
  if (channelEnabled) rmt_disable(channel);
  channelEnabled = false;
  if (channel) rmt_del_channel(channel);
  channel = nullptr;
  if (encoder) rmt_del_encoder(encoder);
  encoder = nullptr;
  idlePin();
}

uint32_t defaultCarrier(const String& protocol) {
  if (protocol == "Sony") return 40000;
  if (protocol == "RC5" || protocol == "RC6" || protocol == "RC6A") return 36000;
  if (protocol == "Panasonic" || protocol.startsWith("Kaseikyo")) return 37000;
  if (protocol == "Bang&Olufsen") return 455000;
  return 38000;
}

// 帧缓存：连击/长按时同一键会高频触发 loadFrame，每次都打开采样文件并
// 解析 JSON 会阻塞 loop 10~50ms。out（全局 frame）按 keyId + configRevision
// 缓存，配置保存（含重新学习采样）后 revision 自增，缓存自动失效。
String cachedFrameKeyId;
uint32_t cachedFrameRevision = 0;

bool loadFrame(const String& keyId, Frame& out, String& error) {
  if (keyId == cachedFrameKeyId && cachedFrameRevision == configRevision &&
      !out.symbols.empty()) {
    return true;
  }
  cachedFrameKeyId = "";  // 加载失败时保持失效，避免半帧残留
  JsonObjectConst key;
  for (JsonObjectConst item : remoteConfig["keys"].as<JsonArrayConst>())
    if (item["id"].as<String>() == keyId) { key = item; break; }
  if (key.isNull()) { error = "IR key not found"; return false; }
  String path = key["capturePath"] | "";
  if (path.isEmpty()) { error = "Key has no captured pulses; learn it again"; return false; }
  File file = LittleFS.open(path, "r");
  if (!file) { error = "IR capture file not found"; return false; }
  if (file.size() > 16384) { error = "IR capture file is too large"; return false; }
  JsonDocument capture;
  auto parseError = deserializeJson(capture, file, DeserializationOption::NestingLimit(4));
  file.close();
  JsonArrayConst ticks = capture["rawTicks"].as<JsonArrayConst>();
  if (parseError || capture["version"] != 1 ||
      strcmp(capture["encoding"] | "", "alternating-mark-space-ticks") != 0 ||
      strcmp(capture["startsWith"] | "", "mark") != 0 ||
      !capture["overflow"].is<bool>() || capture["overflow"].as<bool>() ||
      ticks.size() < 3 || ticks.size() > 749 ||
      !capture["tickMicros"].is<uint32_t>()) {
    error = "Invalid or incomplete IR capture"; return false;
  }
  uint32_t tickUs = capture["tickMicros"];
  if (tickUs == 0 || tickUs > 1000 ||
      (!capture["markExcessMicros"].isNull() && !capture["markExcessMicros"].is<int>())) {
    error = "Invalid IR capture timing"; return false;
  }
  int excess = capture["markExcessMicros"] | 0;
  if (excess < -1000 || excess > 1000) { error = "Invalid IR mark correction"; return false; }
  out.carrierHz = defaultCarrier(capture["protocol"] | "");
  if (!capture["carrierHz"].isNull()) {
    if (!capture["carrierHz"].is<uint32_t>()) { error = "Invalid IR carrier"; return false; }
    out.carrierHz = capture["carrierHz"];
  }
  if (out.carrierHz < 20000 || out.carrierHz > 500000) {
    error = "IR carrier must be 20000-500000 Hz"; return false;
  }
  out.symbols.clear();
  out.durationUs = 0;
  size_t half = 0;
  auto append = [&](uint16_t duration, bool mark) {
    if (!(half & 1)) {
      rmt_symbol_word_t symbol = {};
      symbol.duration0 = duration;
      symbol.level0 = mark;
      out.symbols.push_back(symbol);
    } else {
      out.symbols.back().duration1 = duration;
      out.symbols.back().level1 = mark;
    }
    ++half;
  };
  size_t index = 0;
  for (JsonVariantConst tick : ticks) {
    if (!tick.is<uint16_t>() || tick.as<uint16_t>() == 0) {
      error = "Invalid IR pulse length"; return false;
    }
    bool mark = (index++ & 1) == 0;
    // Demodulating receivers lengthen marks and shorten spaces.
    int64_t duration = static_cast<int64_t>(tick.as<uint16_t>()) * tickUs +
                       (mark ? -excess : excess);
    if (duration <= 0 || duration > 100000 || out.durationUs + duration > 1000000) {
      error = "IR frame exceeds timing limits"; return false;
    }
    out.durationUs += duration;
    // RMT duration fields are 15 bits. Split long pulses without changing level.
    while (duration > 32767) { append(32767, mark); duration -= 32767; }
    append(static_cast<uint16_t>(duration), mark);
  }
  if (half & 1) append(1, false);
  cachedFrameKeyId = keyId;
  cachedFrameRevision = configRevision;
  return true;
}

bool startFrame(String& error) {
  rmt_tx_channel_config_t config = {};
  config.gpio_num = static_cast<gpio_num_t>(kIrTxPin);
  config.clk_src = RMT_CLK_SRC_DEFAULT;
  config.resolution_hz = 1000000;
  config.mem_block_symbols = 48;
  config.trans_queue_depth = 1;
  config.flags.invert_out = mode == Mode::Sink;
  config.flags.io_od_mode = mode == Mode::Sink;
  esp_err_t result = rmt_new_tx_channel(&config, &channel);
  rmt_copy_encoder_config_t copyConfig = {};
  if (result == ESP_OK) result = rmt_new_copy_encoder(&copyConfig, &encoder);
  rmt_carrier_config_t carrier = {};
  carrier.frequency_hz = frame.carrierHz;
  carrier.duty_cycle = 0.33f;
  if (result == ESP_OK) result = rmt_apply_carrier(channel, &carrier);
  if (result == ESP_OK) {
    result = rmt_enable(channel);
    channelEnabled = result == ESP_OK;
  }
  rmt_transmit_config_t transmit = {};
  // Logical low is inactive; invert_out also inverts the carrier and EOT level.
  if (result == ESP_OK)
    result = rmt_transmit(channel, encoder, frame.symbols.data(),
                          frame.symbols.size() * sizeof(rmt_symbol_word_t), &transmit);
  if (result != ESP_OK) {
    error = String("IR transmitter: ") + esp_err_to_name(result);
    releaseChannel();
    return false;
  }
  deadline = millis() + (frame.durationUs + 999) / 1000 + 500;
  return true;
}

bool available(String& error) {
  if (mode == Mode::Off) error = "IR transmit is off; enable GPIO4 in settings";
  // 只互斥进行中的 IR 动作。不再检查 hidRunning：BLE 报告走射频、IR 走
  // RMT DMA，两者无共享资源，互斥只会让按住窗口吞掉 IR 触发。
  else if (running) error = "An IR action is already running";
  else return true;
  return false;
}

void beginAction() {
  running = true;
  started = millis();
  lastError = "";
  irTxPauseReceiver(true);
}

void failAction(const String& error) {
  irTxStop();
  lastError = error;
  Serial.printf("[IR] %s\n", error.c_str());
}
} // namespace

bool validIrTxMode(const char* value) {
  return value && (!strcmp(value, "off") || !strcmp(value, "push-pull") || !strcmp(value, "sink"));
}

void irTxApplyMode(const char* value) {
  irTxStop();
  mode = value && !strcmp(value, "push-pull") ? Mode::PushPull :
         value && !strcmp(value, "sink") ? Mode::Sink : Mode::Off;
  idlePin();
}

bool irTxBusy() { return running; }
const String& irTxLastError() { return lastError; }

void irTxStop() {
  releaseChannel();
  sequence.clear();
  stepIndex = 0;
  // 注意：不清 frame.symbols——loadFrame 的帧缓存依赖它（按 keyId +
  // configRevision 校验有效性，重新加载前会自行 clear）。
  if (running) irTxPauseReceiver(false);
  running = false;
}

bool irTxSendKey(const String& keyId, String& error) {
  if (!available(error) || !loadFrame(keyId, frame, error)) return false;
  sequence.clear();
  stepIndex = 0;
  beginAction();
  if (startFrame(error)) return true;
  failAction(error);
  return false;
}

bool irTxExecute(JsonObjectConst descriptor, String& error) {
  if (descriptor["type"] == 4) return irTxSendKey(descriptor["outKeyId"] | "", error);
  if (!available(error)) return false;
  JsonArrayConst steps;
  for (JsonObjectConst macro : remoteConfig["macros"].as<JsonArrayConst>())
    if (macro["id"].as<String>() == descriptor["macroId"].as<String>())
      steps = macro["steps"].as<JsonArrayConst>();
  if (descriptor["type"] != kTypeMacro || !validMacro(steps)) {
    error = "Invalid IR sequence"; return false;
  }
  uint32_t durationMs = 0;
  for (JsonObjectConst step : steps) {
    const char* kind = step["kind"] | "";
    if (!strcmp(kind, "wait")) durationMs += step["ms"].as<uint32_t>();
    else if (!strcmp(kind, "ir")) {
      if (!loadFrame(step["keyId"] | "", frame, error)) return false;
      durationMs += (frame.durationUs + 999) / 1000 + 20;
    } else { error = "IR sequences may only contain IR replay and wait steps"; return false; }
  }
  if (durationMs > kMaxMacroDurationMs) { error = "IR sequence exceeds 60 seconds"; return false; }
  sequence.set(steps);
  if (sequence.overflowed()) { error = "Insufficient memory for IR sequence"; return false; }
  stepIndex = 0;
  deadline = millis();
  beginAction();
  irTxTick();
  if (!running && !lastError.isEmpty()) { error = lastError; return false; }
  return true;
}

void irTxTick() {
  if (!running) return;
  uint32_t now = millis();
  if (now - started > kMaxMacroDurationMs + 1000) {
    failAction("IR action timed out"); return;
  }
  if (channel) {
    esp_err_t result = rmt_tx_wait_all_done(channel, 0);
    if (result == ESP_ERR_TIMEOUT && static_cast<int32_t>(now - deadline) < 0) return;
    if (result != ESP_OK) { failAction("IR transmission failed or timed out"); return; }
    releaseChannel();
    // Let the demodulator settle before accepting any input (including learning).
    deadline = now + 20;
    return;
  }
  if (static_cast<int32_t>(now - deadline) < 0) return;
  if (stepIndex >= sequence.size()) { irTxStop(); return; }
  JsonObjectConst step = sequence[stepIndex++].as<JsonObjectConst>();
  if (!strcmp(step["kind"] | "", "wait")) {
    deadline = now + step["ms"].as<uint32_t>();
    return;
  }
  String error;
  if (!loadFrame(step["keyId"] | "", frame, error) || !startFrame(error)) failAction(error);
}
