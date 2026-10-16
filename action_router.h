#pragma once
#include "bindings.h"

// Input matching yields an action description; output adapters own execution.
bool dispatchAction(JsonObjectConst descriptor);
bool dispatchAction(JsonObjectConst descriptor, String& error);
void dispatchInput(const char* transport, const char* device,
                   const char* proto, const char* code, bool repeat);
