#include "rangetest.h"
#include <Arduino.h>

static SigResult last;
static bool haveLast = false;
static bool pending = false;
static int total = 0;
static uint32_t version = 1;

void sig_begin(const char*, uint32_t) { pending = true; version++; }
bool sig_pending() { return pending; }
void sig_finish(const SigResult& r) { last = r; haveLast = true; pending = false; total++; version++; }
const SigResult* sig_last() { return haveLast ? &last : nullptr; }
int sig_total() { return total; }
uint32_t sig_version() { return version; }
