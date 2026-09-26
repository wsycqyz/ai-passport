#pragma once
#include <stdio.h>
// Type-check the format arguments like the real macros, without printing.
#define ESP_LOG_STUB(tag, ...) do { if (0) { (void)(tag); printf(__VA_ARGS__); } } while (0)
#define ESP_LOGE(tag, ...) ESP_LOG_STUB(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) ESP_LOG_STUB(tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) ESP_LOG_STUB(tag, __VA_ARGS__)
