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

#ifndef SHARE_GC_Z_ZPAGEREFCOUNTS_HPP
#define SHARE_GC_Z_ZPAGEREFCOUNTS_HPP

#include "gc/z/zAddress.hpp"
#include "gc/z/zBitField.hpp"
#include "gc/z/zConcurrentTree.hpp"
#include "gc/z/zGlobals.hpp"
#include "memory/allocation.hpp"
#include "runtime/atomic.hpp"
#include "utilities/globalDefinitions.hpp"

struct ZPageRefSmallCountEntry {
  // A small page has the most object slots and consequently leaves the
  // smallest count field. Use that fixed layout for every page type.
  static constexpr uint32_t ObjectIndexBits =
      ZPageSizeSmallShift - ZMinObjectAlignmentSmallShift;
  static constexpr uint32_t CountBits = 32 - ObjectIndexBits;
  static constexpr uint32_t ObjectIndexMax = uint32_t(1) << ObjectIndexBits;
  // A zero word is the empty-entry sentinel. A zero stake is never stored:
  // its mapping is removed instead.
  static constexpr uint32_t CountMask = right_n_bits(CountBits);
  static constexpr uint32_t CountSignBit = CountBits - 1;
  static constexpr uint32_t CountSignMask = ~right_n_bits(CountSignBit);
  static constexpr int64_t CountMin =
      -(int64_t(1) << CountSignBit);
  static constexpr int64_t CountMax =
      (int64_t(1) << CountSignBit) - 1;

  typedef ZBitField<uint32_t, uint32_t, 0, CountBits> field_value;
  typedef ZBitField<uint32_t, uint32_t, CountBits, ObjectIndexBits> field_key;

  struct KeyT {
    uint32_t object_index;
  };

  using ValueT = int64_t;

  uint32_t _bits;

  static ZPageRefSmallCountEntry empty();
  static bool is_empty(ZPageRefSmallCountEntry e);
  static ZPageRefSmallCountEntry create(KeyT key, ValueT value);
  operator ValueT() const;
  static int64_t clamp_delta(int64_t old, int64_t delta);
  static int cmp(ZPageRefSmallCountEntry a, KeyT b);
  static int cmp(ZPageRefSmallCountEntry a, ZPageRefSmallCountEntry b);
};

static_assert(sizeof(ZPageRefSmallCountEntry) == 4, "this better be small");

struct ZPageRefBigCountEntry {
  using KeyT = zaddress;
  using ValueT = int64_t;

  zaddress _key;
  int64_t _value;

  static ZPageRefBigCountEntry empty();
  static bool is_empty(ZPageRefBigCountEntry entry);
  static ZPageRefBigCountEntry create(KeyT key, ValueT value);
  operator ValueT() const;
  static int cmp(ZPageRefBigCountEntry a, KeyT b);
  static int cmp(ZPageRefBigCountEntry a, ZPageRefBigCountEntry b);
};

class ZPageRefCounts : public CHeapObj<mtGC> {
  friend class ZPageRefCountsTestAccess;

  // Flipped page descriptors share stakes until their safe destruction.
  Atomic<uint> _owners;
  const zoffset _start;
  const size_t _alignment_shift;
  ZConcurrentTree<ZPageRefSmallCountEntry> _small;
  ZConcurrentTree<ZPageRefBigCountEntry> _big;

  ZPageRefSmallCountEntry::KeyT key(zaddress addr) const;

  int64_t read_small_stake(zaddress addr);
  int64_t read_big_stake(zaddress addr);
  void add_big(zaddress addr, int64_t delta);
  void normalize(zaddress addr, int64_t pending);

public:
  ZPageRefCounts(zoffset start, size_t alignment_shift);
  ~ZPageRefCounts();

  void retain();
  void release();
  int64_t add(zaddress addr, int64_t delta);
  int64_t decrement_monotonic(zaddress addr);
  int64_t small_stake(zaddress addr);
  int64_t take_small_stake(zaddress addr);
  bool find(zaddress addr, int64_t* result);
  void remove(zaddress addr);
};

#endif // SHARE_GC_Z_ZPAGEREFCOUNTS_HPP
