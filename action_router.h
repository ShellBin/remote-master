#pragma once
#include "bindings.h"

// Input matching yields an action description; output adapters own execution.
// Future ir.send / mixed sequence adapters can be added without changing input capture.
bool dispatchAction(JsonObjectConst descriptor);
void dispatchInput(const char* transport, const char* device,
                   const char* proto, const char* code, bool repeat);
