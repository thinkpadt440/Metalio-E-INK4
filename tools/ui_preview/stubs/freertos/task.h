#pragma once
#include "FreeRTOS.h"
static inline int xTaskCreate(void (*f)(void*), const char*, int, void* p, int, void*) { f(p); return 1; }
static inline void vTaskDelete(void*) {}
