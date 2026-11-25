#pragma once
#include "FreeRTOS.h"
struct TestEventGroup { EventBits_t bits=0; };
using EventGroupHandle_t = TestEventGroup*;
inline EventGroupHandle_t xEventGroupCreate() { return new TestEventGroup; }
inline void vEventGroupDelete(EventGroupHandle_t e) { delete e; }
inline EventBits_t xEventGroupSetBits(EventGroupHandle_t e,EventBits_t bits) { return e->bits |= bits; }
inline EventBits_t xEventGroupClearBits(EventGroupHandle_t e,EventBits_t bits) { auto old=e->bits;e->bits &= ~bits;return old; }
inline EventBits_t xEventGroupWaitBits(EventGroupHandle_t e,EventBits_t bits,int clear,int,unsigned) { auto result=e->bits;if(clear)e->bits &= ~bits;return result; }
