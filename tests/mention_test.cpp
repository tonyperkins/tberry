// Host-side test for the mention parser:  g++ -std=c++17 -o /tmp/mt tests/mention_test.cpp && /tmp/mt
#include <cstdio>
#include "../src/mention.h"
static int fails = 0;
static void t(const char* text, bool want, const char* wantName = "") {
  char n[40];
  bool got = leadingMention(text, n, sizeof n);
  bool ok = got == want && (!want || strcmp(n, wantName) == 0);
  printf("%s  [%s] -> %s%s%s\n", ok ? "ok  " : "FAIL", text, got ? "[" : "no", got ? n : "", got ? "]" : "");
  if (!ok) fails++;
}
int main() {
  t("@[Wy-Bo] Bush, LA", true, "Wy-Bo");
  t("@[FEAR_RADIO_CAR (Aries)\xF0\x9F\x90\xBA] Fort Worth, TX here.", true, "FEAR_RADIO_CAR (Aries)\xF0\x9F\x90\xBA");
  t("  @[Greg] morning", true, "Greg");
  t("@[IneffableAwe]: Mobile, al here", true, "IneffableAwe");
  t("@[N5LRQ ( Jimmy)] morning brah!", true, "N5LRQ ( Jimmy)");
  t("good morning @[Greg]", false);
  t("@ AJ4TW Where are you located", false);
  t("@[] empty", false);
  t("@[unterminated", false);
  t("plain text", false);
  printf(fails ? "\n%d FAILURES\n" : "\nall passed\n", fails);
  return fails ? 1 : 0;
}
