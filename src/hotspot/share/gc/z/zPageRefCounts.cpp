/*
 * Copyright (c) 2026, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#include "gc/z/zAddress.inline.hpp"
#include "gc/z/zConcurrentTree.inline.hpp"
#include "gc/z/zPageRefCounts.hpp"
#include "runtime/orderAccess.hpp"

ZPageRefSmallCountEntry ZPageRefSmallCountEntry::empty() {
  return {0};
}

bool ZPageRefSmallCountEntry::is_empty(ZPageRefSmallCountEntry e) {
  return e._bits == 0;
}

ZPageRefSmallCountEntry ZPageRefSmallCountEntry::create(KeyT key, ValueT value) {
  assert(value >= CountMin && value <= CountMax, "stake out of range");
  assert(value != 0, "zero stake must not be stored");
  return {field_key::encode(key.object_index) |
          field_value::encode(uint32_t(value) & CountMask)};
}

ZPageRefSmallCountEntry::operator ValueT() const {
  const uint32_t bits = field_value::decode(_bits);
  int32_t value = checked_cast<int32_t>(bits);
  if ((bits & (uint32_t(1) << CountSignBit)) != 0) {
    value |= CountSignMask;
  }
  return value;
}

int64_t ZPageRefSmallCountEntry::clamp_delta(int64_t old, int64_t delta) {
  return clamp(delta, CountMin - old, CountMax - old);
}

int ZPageRefSmallCountEntry::cmp(ZPageRefSmallCountEntry a, KeyT b) {
  const uint32_t object_index = field_key::decode(a._bits);
  return object_index < b.object_index ? -1 : object_index > b.object_index ? 1 : 0;
}

int ZPageRefSmallCountEntry::cmp(ZPageRefSmallCountEntry a, ZPageRefSmallCountEntry b) {
  const uint32_t a_object_index = field_key::decode(a._bits);
  const uint32_t b_object_index = field_key::decode(b._bits);
  return a_object_index < b_object_index ? -1 : a_object_index > b_object_index ? 1 : 0;
}

ZPageRefBigCountEntry ZPageRefBigCountEntry::empty() {
  return {zaddress::null, 0};
}

bool ZPageRefBigCountEntry::is_empty(ZPageRefBigCountEntry entry) {
  return entry._key == zaddress::null;
}

ZPageRefBigCountEntry ZPageRefBigCountEntry::create(KeyT key, ValueT value) {
  return {key, value};
}

ZPageRefBigCountEntry::operator ValueT() const {
  return _value;
}

int ZPageRefBigCountEntry::cmp(ZPageRefBigCountEntry a, KeyT b) {
  return a._key < b ? -1 : a._key > b ? 1 : 0;
}

int ZPageRefBigCountEntry::cmp(ZPageRefBigCountEntry a, ZPageRefBigCountEntry b) {
  return cmp(a, b._key);
}

ZPageRefSmallCountEntry::KeyT ZPageRefCounts::key(zaddress addr) const {
  const zoffset offset = ZAddress::offset(addr);
  const uintptr_t local = offset - _start;
  const uintptr_t index = local >> _alignment_shift;

  assert(offset >= _start, "address before page");
  assert((local & ((uintptr_t(1) << _alignment_shift) - 1)) == 0, "misaligned object?");
  assert(index < ZPageRefSmallCountEntry::ObjectIndexMax, "page-local object index too wide");

  return {checked_cast<uint32_t>(index)};
}

int64_t ZPageRefCounts::read_small_stake(zaddress addr) {
  int64_t stake = 0;
  _small.find(key(addr), &stake);
  return stake;
}

int64_t ZPageRefCounts::read_big_stake(zaddress addr) {
  int64_t value = 0;
  _big.find(addr, &value);
  return value;
}

void ZPageRefCounts::add_big(zaddress addr, int64_t delta) {
  if (delta == 0) {
    return;
  }

  _big.update(addr, [&](int64_t* prev, int64_t** proposal) {
    const int64_t value = prev == nullptr ? delta : (*prev + delta);
    if (value == 0) {
      *proposal = nullptr;
    } else {
      **proposal = value;
    }
  });
}

void ZPageRefCounts::normalize(zaddress addr, int64_t pending) {
  for (;;) {
    add_big(addr, pending);
    OrderAccess::fence();
    const int64_t small = read_small_stake(addr);
    const int64_t big = read_big_stake(addr);
    if (big == 0 ||
        (big > 0 && small == ZPageRefSmallCountEntry::CountMax) ||
        (big < 0 && small == ZPageRefSmallCountEntry::CountMin)) {
      return;
    }

    _big.update(addr, [&](int64_t* prev, int64_t** proposal) {
      pending = prev == nullptr ? 0 : *prev;
      *proposal = nullptr;
    });

    const int64_t carry = pending;
    _small.update(key(addr), [&](int64_t* prev, int64_t** proposal) {
      const int64_t old = prev == nullptr ? 0 : *prev;
      const int64_t applied = ZPageRefSmallCountEntry::clamp_delta(old, carry);
      const int64_t value = old + applied;
      pending = carry - applied;
      if (applied == 0) {
        *proposal = prev;
      } else if (value == 0) {
        *proposal = nullptr;
      } else {
        **proposal = value;
      }
    });
  }
}

ZPageRefCounts::ZPageRefCounts(zoffset start, size_t alignment_shift)
  : _owners(1),
    _start(start),
    _alignment_shift(alignment_shift),
    _small(),
    _big() {
}

ZPageRefCounts::~ZPageRefCounts() = default;

void ZPageRefCounts::retain() {
  _owners.add_then_fetch(1u);
}

void ZPageRefCounts::release() {
  if (_owners.sub_then_fetch(1u) == 0) {
    delete this;
  }
}

int64_t ZPageRefCounts::add(zaddress addr, int64_t delta) {
  const int64_t big = read_big_stake(addr);
  int64_t previous;
  int64_t remainder;
  _small.update(key(addr), [&](int64_t* prev, int64_t** proposal) {
    const int64_t old = prev == nullptr ? 0 : *prev;
    previous = old + big;
    remainder = 0;
    if (delta == 0) {
      *proposal = prev;
      return;
    }

    const int64_t applied = ZPageRefSmallCountEntry::clamp_delta(old, delta);
    const int64_t value = old + applied;
    remainder = delta - applied;
    if (applied == 0) {
      *proposal = prev;
    } else if (value == 0) {
      *proposal = nullptr;
    } else {
      **proposal = value;
    }
  });
  normalize(addr, remainder);
  return previous;
}

int64_t ZPageRefCounts::decrement_monotonic(zaddress addr) {
  const auto small_key = key(addr);
  int64_t previous;
  _big.update(addr, [&](int64_t* prev, int64_t** proposal) {
    previous = prev == nullptr ? 0 : *prev;
    assert(previous >= 0, "negative big stake during monotonic drain: " INT64_FORMAT, previous);
    if (previous == 0) {
      *proposal = prev;
    } else if (previous == 1) {
      *proposal = nullptr;
    } else {
      **proposal = previous - 1;
    }
  });

  if (previous != 0) {
    return previous + ZPageRefSmallCountEntry::CountMax;
  }

  _small.update(small_key, [&](int64_t* prev, int64_t** proposal) {
    previous = prev == nullptr ? 0 : *prev;
    assert(previous >= 0, "negative small stake during monotonic decrement: " INT64_FORMAT, previous);
    if (previous == 0) {
      *proposal = prev;
    } else if (previous == 1) {
      *proposal = nullptr;
    } else {
      **proposal = previous - 1;
    }
  });
  return previous;
}

int64_t ZPageRefCounts::small_stake(zaddress addr) {
  return read_small_stake(addr);
}

int64_t ZPageRefCounts::take_small_stake(zaddress addr) {
  int64_t taken;
  _small.update(key(addr), [&](int64_t* prev, int64_t** proposal) {
    taken = prev == nullptr ? 0 : *prev;
    *proposal = nullptr;
  });
  return taken;
}

bool ZPageRefCounts::find(zaddress addr, int64_t* result) {
  *result = read_small_stake(addr) + read_big_stake(addr);
  return *result != 0;
}

void ZPageRefCounts::remove(zaddress addr) {
  _small.try_remove(key(addr));
  _big.try_remove(addr);
}

