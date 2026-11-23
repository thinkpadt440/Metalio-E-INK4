#pragma once
#include "FreeRTOS.h"
using SemaphoreHandle_t = void*;
static inline void* xSemaphoreCreateMutex() { return malloc(1); }
static inline void* xSemaphoreCreateBinary() { return malloc(1); }
static inline int xSemaphoreTake(void*, int) { return 1; }
static inline void xSemaphoreGive(void*) {}
static inline void vSemaphoreDelete(void* p) { free(p); }
