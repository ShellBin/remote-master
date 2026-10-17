#ifndef BLE_HID_H
#define BLE_HID_H

#include <Arduino.h>
#include "bindings.h"

// 初始化 BLE HID 设备（键盘 + 鼠标 + 媒体键）
void bleHidBegin();

// 应用 remoteConfig["settings"]["bleName"]：更新 GAP 名称并重建广播数据
void bleApplyName();

// BLE 是否已连接
bool bleHidConnected();
void tickBleHosts();
void bleHostStatus(JsonObject obj);
void blePairingOpen();
bool bleHostForget(const char* address, uint8_t addressType);
void tickKeepAlive();

// 清除所有已配对主机的 BLE 绑定信息（恢复初始状态时调用）
void bleClearAllBonds();

// 尝试重连指定主机（address + addressType），用于用户从页面选择历史设备
bool bleReconnectToHost(const String& address, uint8_t addressType);

// 发送原始 HID 键码（不做 ASCII 转换）
void sendRawKey(uint8_t key, uint8_t modifiers);

// 释放所有 HID 设备（键盘/鼠标/媒体）
void releaseHid();

// 停止当前 HID 动作（含宏），释放所有按键
void stopHid(const char* reason);

// 执行绑定（键盘/鼠标/媒体/宏），返回 false 表示被拒绝
bool executeBinding(const Binding& b);

// loop() 中调用：逐步执行宏、处理释放/超时
void tickHid();

#endif // BLE_HID_H
