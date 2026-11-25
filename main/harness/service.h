#pragma once
#include "client.h"
namespace harness {
void StartService();
void StopService();
ConnectionSnapshot ServiceStatus();
const char* ConnectionStatusLabel();
}  // namespace harness
