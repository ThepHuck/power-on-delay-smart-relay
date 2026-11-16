#pragma once

#include <stdint.h>

constexpr bool parseApiUnsigned(const char* input, uint32_t maximum, uint32_t& value) {
  value = 0;
  if (!input || !input[0]) return false;
  for (unsigned int i = 0; input[i]; ++i) {
    if (input[i] < '0' || input[i] > '9') return false;
    const uint32_t digit = input[i] - '0';
    if (digit > maximum || value > (maximum - digit) / 10) return false;
    value = value * 10 + digit;
  }
  return true;
}

// ArduinoJson performs the syntax check. This additionally rejects data after
// the root object, which a streaming JSON parser may otherwise leave unread.
constexpr bool apiJsonObjectEnvelope(const char* input, unsigned int length) {
  bool inString = false, escaped = false, started = false, ended = false;
  unsigned int depth = 0;
  for (unsigned int i = 0; i < length; ++i) {
    const char c = input[i];
    if (c == '\0') return false;
    const bool whitespace = c == ' ' || c == '\t' || c == '\r' || c == '\n';
    if (!started) {
      if (whitespace) continue;
      if (c != '{') return false;
      started = true; depth = 1; continue;
    }
    if (ended) { if (!whitespace) return false; continue; }
    if (inString) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == '"') inString = false;
    } else if (c == '"') inString = true;
    else if (c == '{' || c == '[') ++depth;
    else if (c == '}' || c == ']') { if (!depth) return false; ended = --depth == 0; }
  }
  return started && ended && !inString;
}
