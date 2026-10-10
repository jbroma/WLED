// What travels between the controllers on UDP port kPort. This header and wire.cpp are the only
// places that know the byte layout; everything else deals in Scores.
//
//   conductor -> everyone (multicast, 1 Hz)   beacon: "ORNB" + epoch
//   follower  -> conductor (unicast, 1 Hz)    join:   "ORNJ"
//   conductor -> each subscriber (unicast)    Score:  "ORN1" + version + the fields of types.h
#pragma once
#include "types.h"

namespace orion {

constexpr uint16_t kPort = 11989;
constexpr Ms kBeaconMs = 1000;
constexpr Ms kJoinMs = 1000;
constexpr Ms kSubscriberExpiryMs = 6000;               // conductor drops a follower this long after its last join (rides out 4 lost joins)
constexpr int kMaxSubscribers = 8;

constexpr size_t kMaxPacket = 168;

enum class Packet : uint8_t { Unknown, Score, Beacon, Join };
Packet classify(const uint8_t* in, size_t len);        // by magic only; the decoders do the validating

// Bytes written, or 0 when `cap` is too small.
size_t encode(const Score& s, uint8_t* out, size_t cap);
size_t encodeBeacon(uint8_t epoch, uint8_t* out, size_t cap);
size_t encodeJoin(uint8_t* out, size_t cap);

// Return false and leave `out` untouched on a wrong magic, version or size, or a field outside its range.
bool decode(const uint8_t* in, size_t len, Score& out);
bool decodeBeacon(const uint8_t* in, size_t len, uint8_t& epoch);

}  // namespace orion
