#pragma once
// MeshCore drops a packet it has already seen (a flood arrives once per route). This table subclass
// does the same and also counts those copies per packet, so a message can say "heard 4 times".
#include <helpers/SimpleMeshTables.h>

class CountingTables : public SimpleMeshTables {
  struct Dup { uint8_t hash4[4]; uint8_t copies; };
  static constexpr int N = 48;
  Dup dups_[N] = {};
  int next_ = 0;

public:
  bool wasSeen(const mesh::Packet* packet) override {
    bool seen = SimpleMeshTables::wasSeen(packet);
    if (seen) {
      uint8_t h[MAX_HASH_SIZE];
      packet->calculatePacketHash(h);
      for (auto& d : dups_) {
        if (memcmp(d.hash4, h, 4) == 0) { if (d.copies < 250) d.copies++; return true; }
      }
      Dup& d = dups_[next_];
      next_ = (next_ + 1) % N;
      memcpy(d.hash4, h, 4);
      d.copies = 1;
    }
    return seen;
  }

  // Extra copies heard after the first (0 if none, or if the packet has aged out of the table).
  uint8_t extraCopies(const uint8_t hash4[4]) const {
    for (auto& d : dups_) if (memcmp(d.hash4, hash4, 4) == 0 && d.copies) return d.copies;
    return 0;
  }
};

extern CountingTables tables;
