#pragma once
#include "bindings.h"

// Stable IDs keep transports, learned inputs and output actions independent.
extern JsonDocument remoteConfig;
// 配置内容版本号：saveRemoteConfig 每次成功落盘 +1。pushState 用它判断
// config 是否变化，避免每次推送都对 32KB 配置做全文序列化比较。
extern uint32_t configRevision;
bool remoteStoreBegin();
// 恢复初始状态：格式化整个文件系统——前端、配置、红外采样与出厂绑定全部清空，
// 页面需重新部署（回到内置 fallback 页）。BLE 配对由调用方（bleClearAllBonds）清除。
bool remoteStoreFactoryReset();
// 配置损坏时的自愈：旧配置改名 /remote-config.bad 留档后重建默认配置。
// 不格式化 FS，前端与红外采样保留。仅配置解析/校验失败时由 setup() 调用。
bool remoteStoreRebuild();
bool remoteStoreCommand(const String& op, JsonObjectConst args, String& error);
bool resolveRemoteInput(const char* transport, const char* device,
                        const char* proto, const char* code, Binding& action);
bool remoteAction(JsonObjectConst mapping, Binding& action);
bool saveRemoteConfig(String* error = nullptr);
JsonObjectConst resolveInputMapping(const char* transport, const char* device,
                                   const char* proto, const char* code);
void writeConfigSummary(JsonObject obj);
// 确保 settings 对象存在且字段合法（缺失或非法时回填默认值）。
void ensureDefaultSettings();
