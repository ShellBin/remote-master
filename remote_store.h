#pragma once
#include "bindings.h"

// Stable IDs keep transports, learned inputs and output actions independent.
extern JsonDocument remoteConfig;
bool remoteStoreBegin();
bool remoteStoreCommand(const String& op, JsonObjectConst args, String& error);
bool resolveRemoteInput(const char* transport, const char* device,
                        const char* proto, const char* code, Binding& action);
bool remoteAction(JsonObjectConst mapping, Binding& action);
bool saveRemoteConfig();
JsonObjectConst resolveInputMapping(const char* transport, const char* device,
                                   const char* proto, const char* code);
void writeConfigSummary(JsonObject obj);
// 确保 settings 对象存在且字段合法（缺失或非法时回填默认值）。
void ensureDefaultSettings();
