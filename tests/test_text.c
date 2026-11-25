// Host tests for core/textutil.c (phone numbers, login codes, dates).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "textutil.h"

static int failures = 0;
#define CHECK(label, cond)                 \
  do {                                     \
    if (!(cond)) {                         \
      printf("FAIL %s\n", label);          \
      failures++;                          \
    }                                      \
  } while (0)
#define CHECK_STR(label, got, want)                                                 \
  do {                                                                              \
    if (strcmp((got), (want))) {                                                    \
      printf("FAIL %s\n  got:  [%s]\n  want: [%s]\n", label, (got), (want));        \
      failures++;                                                                   \
    }                                                                               \
  } while (0)

static void test_phone(void) {
  char out[40];
  phone_local_part("+852 9123 4567", "852", out, sizeof out);
  CHECK_STR("local from +cc", out, "91234567");
  phone_local_part("0085291234567", "852", out, sizeof out);
  CHECK_STR("local from 00cc", out, "91234567");
  phone_local_part("85291234567", "852", out, sizeof out);
  CHECK_STR("local from cc", out, "91234567");
  phone_local_part("8521234", "852", out, sizeof out);
  CHECK_STR("short number keeps leading digits", out, "8521234");
  phone_format("852", "91234567", "plus_cc", out, sizeof out);
  CHECK_STR("fmt plus_cc", out, "+85291234567");
  phone_format("852", "91234567", "00cc", out, sizeof out);
  CHECK_STR("fmt 00cc", out, "0085291234567");
  phone_format("852", "91234567", "local", out, sizeof out);
  CHECK_STR("fmt local", out, "91234567");
}

static void test_phone_match(void) {
  CHECK("same with country code", phone_same("+85291234567", "91234567"));
  CHECK("same with spaces and 00", phone_same("00852 9123 4567", "+852-9123-4567"));
  CHECK("different", !phone_same("+85291234567", "+85291234568"));
  CHECK("too short never matches", !phone_same("123", "123"));
  CHECK("empty never matches", !phone_same("", ""));
  CHECK("in list", phone_in_list("+85291234567", "61234567, 91234567"));
  CHECK("in list semicolon", phone_in_list("91234567", "+85261234567;+85291234567"));
  CHECK("not in list", !phone_in_list("+85291234567", "61234567,51234567"));
  CHECK("empty list", !phone_in_list("+85291234567", ""));
}

static void test_http_date(void) {
  CHECK("http date", http_date_parse("Tue, 24 Nov 2026 15:42:54 GMT") == 1795534974LL);
  CHECK("http date epoch", http_date_parse("Thu, 01 Jan 1970 00:00:00 GMT") == 0);
  CHECK("http date leap day", http_date_parse("Thu, 29 Feb 2024 12:00:00 GMT") == 1709208000LL);
  CHECK("http date junk", http_date_parse("yesterday") == -1);
  CHECK("http date bad month", http_date_parse("Tue, 24 Foo 2026 15:42:54 GMT") == -1);
  CHECK("http date null", http_date_parse(NULL) == -1);
}

static void test_otp(void) {
  char c[16];
  CHECK("otp found", otp_extract("Your code is 482913. Valid 5 min", 4, 8, c, sizeof c) && !strcmp(c, "482913"));
  CHECK("otp skips long numbers",
        otp_extract("Call 85291234567 then enter 7731", 4, 8, c, sizeof c) && !strcmp(c, "7731"));
  CHECK("otp none", !otp_extract("no digits 12 here", 4, 8, c, sizeof c));
  CHECK("otp chinese", otp_extract("\xE9\xA9\x97\xE8\xAD\x89\xE7\xA2\xBC\xEF\xBC\x9A""551177", 4, 8, c, sizeof c) &&
                           !strcmp(c, "551177"));
}

static void test_placeholders(void) {
  int n = 0;
  char *s = placeholder_replace("phone=%3Cphone%3E&x=<PHONE>&j={\"p\":\"\\u003cphone\\u003e\"}", "phone", "91234567", &n);
  CHECK_STR("placeholder variants", s, "phone=91234567&x=91234567&j={\"p\":\"91234567\"}");
  CHECK("placeholder count", n == 3);
  free(s);
  CHECK("placeholder present", placeholder_present("code=%3ccode%3e", "code"));
  CHECK("placeholder absent", !placeholder_present("code=1234", "code"));
}

int main(void) {
  test_phone();
  test_otp();
  test_phone_match();
  test_http_date();
  test_placeholders();
  if (failures) {
    printf("%d check(s) failed\n", failures);
    return 1;
  }
  printf("all core checks passed\n");
  return 0;
}
