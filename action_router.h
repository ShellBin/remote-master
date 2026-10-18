#pragma once
#include "bindings.h"

// Input matching yields an action description; output adapters own execution.
// repeat 仅对键盘类绑定有意义：true 表示长按延续帧（不触发新击键）。
bool dispatchAction(JsonObjectConst descriptor);
bool dispatchAction(JsonObjectConst descriptor, String& error, bool repeat = false);
void dispatchInput(const char* transport, const char* device,
                   const char* proto, const char* code, bool repeat);
