#pragma once
#include <cstddef>
#include <cstdint>
inline uint32_t esp_random() { return 42; }
inline uint32_t millis() { return 1000; }
inline void esp_fill_random(void* dest, size_t length) {
  static uint8_t request = 0;
  ++request;
  auto* bytes = static_cast<uint8_t*>(dest);
  for (size_t i = 0; i < length; ++i) bytes[i] = static_cast<uint8_t>(i + request);
}
