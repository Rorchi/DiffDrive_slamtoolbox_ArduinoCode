#include <unity.h>

#include "../../src/wheel_protocol.h"

int send(WheelProtocol &parser, const char *text, int &left, int &right) {
  int result = 0;
  while (*text) result = parser.feed(*text++, left, right);
  return result;
}

void test_valid_commands_and_split_packet() {
  WheelProtocol parser;
  int left = 0;
  int right = 0;

  TEST_ASSERT_EQUAL(1, send(parser, "V1 -30 30\n", left, right));
  TEST_ASSERT_EQUAL(-30, left);
  TEST_ASSERT_EQUAL(30, right);
  TEST_ASSERT_EQUAL(0, send(parser, "V1 70 ", left, right));
  TEST_ASSERT_EQUAL(1, send(parser, "130\n", left, right));
  TEST_ASSERT_EQUAL(70, left);
  TEST_ASSERT_EQUAL(130, right);
  TEST_ASSERT_EQUAL(1, send(parser, "V1 150 -150\n", left, right));
}

void test_invalid_frames_stop_and_recover() {
  WheelProtocol parser;
  int left = 1;
  int right = 1;
  const char *invalid[] = {
      "V1 151 0\n",       "V1 -151 0\n",     "V1 nan 0\n",
      "V1 1\n",           "V1 1 2 extra\n", "V2 1 2\n",
      "V1 99999999999 0\n", "V1 1.5 2\n",   "V1 1 2\r\n",
      "\n",
  };

  for (const char *text : invalid) {
    TEST_ASSERT_EQUAL(-1, send(parser, text, left, right));
    TEST_ASSERT_EQUAL(0, left);
    TEST_ASSERT_EQUAL(0, right);
  }

  TEST_ASSERT_EQUAL(1, send(parser, "V1 10 20\n", left, right));
  TEST_ASSERT_EQUAL(10, left);
  TEST_ASSERT_EQUAL(20, right);
}

void test_immediate_stop_and_overflow() {
  WheelProtocol parser;
  int left = 0;
  int right = 0;

  TEST_ASSERT_EQUAL(0, send(parser, "V1 100 ", left, right));
  TEST_ASSERT_EQUAL(-1, send(parser, "S", left, right));
  TEST_ASSERT_EQUAL(0, left);
  TEST_ASSERT_EQUAL(0, right);
  TEST_ASSERT_EQUAL(
      -1,
      send(parser, "0123456789012345678901234567890123456789\n",
           left, right));
  TEST_ASSERT_EQUAL(1, send(parser, "V1 0 0\n", left, right));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_valid_commands_and_split_packet);
  RUN_TEST(test_invalid_frames_stop_and_recover);
  RUN_TEST(test_immediate_stop_and_overflow);
  return UNITY_END();
}
