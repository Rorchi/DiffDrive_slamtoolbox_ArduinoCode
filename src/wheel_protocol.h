#pragma once

#include <stdlib.h>
#include <string.h>

// V1 <left_mm_s> <right_mm_s>\n, range +/-150 mm/s.
// Only a complete valid frame refreshes the command watchdog. A single S byte
// is an immediate stop. Invalid input is reported as -1 so the caller can stop.
struct WheelProtocol {
  static const int kMaxSpeedMmS = 150;

  char buffer[32] = {};
  unsigned char size = 0;
  bool discard = false;

  static bool number(char *&cursor, int &value) {
    char *start = cursor;
    if (*cursor == '-' || *cursor == '+') ++cursor;
    char *digits = cursor;
    long result = 0;
    while (*cursor >= '0' && *cursor <= '9') {
      result = result * 10 + (*cursor++ - '0');
      if (result > kMaxSpeedMmS) return false;
    }
    if (cursor == digits) return false;
    value = static_cast<int>((*start == '-') ? -result : result);
    return true;
  }

  // 0: incomplete, 1: valid V1 command, -1: stop or invalid input.
  int feed(char c, int &left, int &right) {
    if (c == 'S') {
      reset();
      left = right = 0;
      return -1;
    }

    if (c == '\n') {
      buffer[size] = '\0';
      bool valid = !discard && strncmp(buffer, "V1 ", 3) == 0;
      char *cursor = buffer + 3;
      if (valid) valid = number(cursor, left);
      if (valid) valid = *cursor++ == ' ';
      if (valid) valid = number(cursor, right);
      if (valid) valid = *cursor == '\0';
      reset();
      if (!valid) left = right = 0;
      return valid ? 1 : -1;
    }

    if (discard) return 0;
    if (size >= sizeof(buffer) - 1 || c < ' ' || c > '~') {
      discard = true;
      left = right = 0;
      return -1;
    }
    buffer[size++] = c;
    return 0;
  }

  void reset() {
    size = 0;
    discard = false;
  }
};
