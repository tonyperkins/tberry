// Host-side test for the reaction parser. Build and run:  g++ -std=c++17 -o /tmp/rt tests/reaction_test.cpp && /tmp/rt
#include <cstdio>
#include <cstdlib>
#include "../src/reaction.h"
static int fails = 0;
static void t(const char* s, bool want) {
  char a[40], e[24], c[12];
  bool got = parseReaction(s, a, sizeof a, e, sizeof e, c, sizeof c);
  if (got != want) { printf("FAIL  want=%d got=%d  [%s]\n", want, got, s); fails++; }
  else if (got) printf("ok    author=[%s] emoji=%s code=%s\n", a, e, c);
  else printf("ok    (not a reaction) [%s]\n", s);
}
int main() {
  // reactions as seen in the sample
  t("@[WRMattil]\xF0\x9F\x8E\x89 59pv3qap", true);                       // 🎉
  t("@[Wintermute]\xF0\x9F\x98\x82 0nr3ajmf", true);                     // 😂
  t("@[Tommy - AustinMesh.org]\xE2\x9D\xA4\xEF\xB8\x8F qqrs66j4", true); // ❤ + variation selector
  t("@[Vasuul]\xF0\x9F\x91\x8B 6bhzfn10", true);                         // 👋
  t("@[FEAR_RADIO_CAR (Aries)\xF0\x9F\x90\xBA]\xF0\x9F\x91\x8D 12ab34cd\n", true);  // name with emoji, trailing newline
  // look-alikes that must stay as ordinary messages
  t("@[Vasuul] \xF0\x9F\x91\x8B 6bhzfn10", false);                       // a space before the emoji
  t("@[WRMattil]\xF0\x9F\x8E\x89 59pv3qap and more", false);             // text after the code
  t("@[WRMattil]\xF0\x9F\x8E\x89 59pv3qa", false);                       // 7 characters
  t("@[WRMattil]\xF0\x9F\x8E\x89 59pv3qapx", false);                     // 9 characters
  t("@[WRMattil]\xF0\x9F\x8E\x89 59PV3QAP", false);                      // upper case
  t("@[EF 1] good morning", false);
  t("@[\xF0\x9F\xA4\xB7\xE2\x80\x8D\xE2\x99\x82\xEF\xB8\x8FNBDY] \xF0\x9F\x91\x8B", false);   // a plain wave back
  t("@[Greg] morning from Trenton TX!", false);
  t("morning everyone", false);
  t("@[WRMattil]59pv3qap", false);                                       // no emoji
  printf(fails ? "\n%d FAILURES\n" : "\nall passed\n", fails);
  return fails ? 1 : 0;
}
