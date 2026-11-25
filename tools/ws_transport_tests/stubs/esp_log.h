#pragma once
inline void TestLog(const char*,const char*,...) {}
#define ESP_LOGE(...) TestLog(__VA_ARGS__)
#define ESP_LOGW(...) TestLog(__VA_ARGS__)
#define ESP_LOGI(...) TestLog(__VA_ARGS__)
#define ESP_LOGD(...) TestLog(__VA_ARGS__)
