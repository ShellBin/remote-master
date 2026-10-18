#ifndef GLOBALS_H
#define GLOBALS_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "bindings.h"

// ---------- 绑定表 ----------
extern Binding bindings[];
extern uint8_t bindingCount;

// ---------- 学习模式 ----------
extern bool learningMode;

// ---------- 帧计数 ----------
extern uint32_t frameCount;

// ---------- 重复帧（长按复用上次帧） ----------
extern char lastProto[16];
extern char lastCode[32];

// ---------- HID 执行状态 ----------
extern bool hidRunning;
extern bool macroRunning;
extern bool hidNeedsRelease;
extern uint32_t hidDeadline;
extern uint32_t hidStarted;
extern size_t macroStep;
extern size_t macroTextOffset;
extern JsonDocument runningSteps;

// ---------- 配置并发保护 ----------
// remoteConfig 的读取可能发生在 AsyncWebServer 的 TCP 任务（/api/config/* 等），
// 写入发生在 loop()。写操作与这些读取用同一把递归锁串行化。
void configLock();
void configUnlock();
struct ConfigGuard {
  ConfigGuard() { configLock(); }
  ~ConfigGuard() { configUnlock(); }
};

#endif // GLOBALS_H
