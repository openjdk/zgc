#ifndef SHARE_GC_Z_ZEPOCH_HPP
#define SHARE_GC_Z_ZEPOCH_HPP

#include "gc/z/zBitField.hpp"
#include "metaprogramming/primitiveConversions.hpp"
#include "runtime/atomic.hpp"
#include "utilities/globalDefinitions.hpp"

// Slot state:
//   bits 63..1: Readers (63 bits)
//   bit      0: Open for new holds
//
// Admission atomically tests open and increments readers.
class ZEpochEntry {
  friend struct PrimitiveConversions::Translate<ZEpochEntry>;

  typedef ZBitField<uint64_t, bool,     0,  1> field_open;
  typedef ZBitField<uint64_t, uint64_t, 1, 63> field_readers;

  uint64_t _entry;

public:
  ZEpochEntry() : _entry(0) {}
  ZEpochEntry(bool open, uint64_t readers)
    : _entry(field_open::encode(open) | field_readers::encode(readers)) {}

  bool open() const { return field_open::decode(_entry); }
  uint64_t readers() const { return field_readers::decode(_entry); }
};

// Support for Atomic<ZEpochEntry>.
template<>
struct PrimitiveConversions::Translate<ZEpochEntry> : public std::true_type {
  using Value = ZEpochEntry;
  using Decayed = uint64_t;

  static Decayed decay(Value value) { return value._entry; }
  static Value recover(Decayed value) {
    ZEpochEntry entry;
    entry._entry = value;
    return entry;
  }
};

// A class used to manage grace periods in lock-free code. An important
// invariant of this class is that only a single grace period may be
// active at a time; concurrent maintenance calls are not supported.
class ZEpoch {
public:
  // Identifies an epoch
  using ZEpochId = uint64_t;

  // Identifies the admitted slot. It must be released exactly once.
  using ZHold = uint8_t;

private:
  Atomic<ZEpochEntry> _slots[2];
  Atomic<uint8_t>  _current_slot;

  // Current epoch.
  ZEpochId _current_epoch;

  // Greatest epoch for which all earlier holds have been released.
  ZEpochId _quiescent_epoch;

public:
  ZEpoch();

  // Advance the current epoch and return the new epoch. Concurrent calls
  // to advance are not allowed.
  ZEpochId advance();

  // Return true when all holds from before epoch advancement have been released.
  bool is_quiescent();

  // Concurrent advisory observation of both permanent slots, not an atomic
  // population snapshot or a substitute for is_quiescent().
  bool has_holders() const;

  // Hold prevents a subsequent epoch advancement from quiescing. Release removes
  // the stake in that hold.
  ZHold hold();
  void release(ZHold hold);
};

#endif // SHARE_GC_Z_ZEPOCH_HPP
