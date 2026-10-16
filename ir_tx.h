#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

constexpr uint8_t kIrTxPin = 4;
bool validIrTxMode(const char* mode);
void irTxApplyMode(const char* mode);
bool irTxBusy();
bool irTxSendKey(const String& keyId, String& error);
bool irTxExecute(JsonObjectConst descriptor, String& error);
void irTxTick();
void irTxStop();
const String& irTxLastError();

// Implemented beside IrReceiver, whose configuration lives in the sketch.
void irTxPauseReceiver(bool paused);
