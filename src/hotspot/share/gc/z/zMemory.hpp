/*
 * Copyright (c) 2015, 2025, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_Z_ZMEMORY_HPP
#define SHARE_GC_Z_ZMEMORY_HPP

#include "gc/z/zAddress.hpp"
#include "gc/z/zArray.hpp"
#include "gc/z/zList.hpp"
#include "gc/z/zLock.hpp"
#include "memory/allocation.hpp"

template <typename Range>
class ZRangeNode;

template <typename Start, typename End>
class ZRange {
  friend class VMStructs;

public:
  using offset     = Start;
  using offset_end = End;

private:
  End _start;
  End _end;

  // Used internally to create a ZRange.
  // The size parameter is only used for verification and to distinguish
  // the constructors if End = size_t
  ZRange(End start, End end, size_t size);

public:
  ZRange();
  ZRange(Start start, size_t size);

  bool is_null() const;

  Start start() const;
  End end() const;

  size_t size() const;

  bool operator==(const ZRange& other) const;
  bool operator!=(const ZRange& other) const;

  bool contains(const ZRange& other) const;

  void grow_from_front(size_t size);
  void grow_from_back(size_t size);

  ZRange shrink_from_front(size_t size);
  ZRange shrink_from_back(size_t size);

  ZRange partition(size_t offset, size_t partition_size) const;
  ZRange first_part(size_t split_offset) const;
  ZRange last_part(size_t split_offset) const;

  bool adjacent_to(const ZRange& other) const;
};

class ZVirtualMemory : public ZRange<zoffset, zoffset_end> {
public:
  ZVirtualMemory();
  ZVirtualMemory(zoffset start, size_t size);
  ZVirtualMemory(const ZRange<zoffset, zoffset_end>& range);

  int granule_count() const;
};

using ZBackingIndexRange = ZRange<zbacking_index, zbacking_index_end>;

template <typename Range>
class ZMemoryManagerImpl {
private:
  using ZMemory = ZRangeNode<Range>;

public:
  using offset     = typename Range::offset;
  using offset_end = typename Range::offset_end;
  typedef void (*CallbackInsert)(const Range& range);
  typedef void (*CallbackRemove)(const Range& range);
  typedef void (*CallbackGrow)(const Range& from, const Range& to);
  typedef void (*CallbackShrink)(const Range& extracted, const Range& origin);

  struct Callbacks {
    CallbackInsert _insert;
    CallbackRemove _remove;
    CallbackGrow   _grow;
    CallbackShrink _shrink;

    Callbacks();
  };

private:
  mutable ZLock  _lock;
  ZList<ZMemory> _list;
  Callbacks      _callbacks;
  Range          _limits;

  void move_into(const Range& range);

  void insert_inner(const Range& range);
  void register_inner(const Range& range);

  void grow_from_front(ZMemory* area, size_t size);
  void grow_from_back(ZMemory* area, size_t size);

  Range shrink_from_front(ZMemory* area, size_t size);
  Range shrink_from_back(ZMemory* area, size_t size);

  Range remove_from_low_inner(size_t size);
  Range remove_from_low_at_most_inner(size_t size);

  size_t remove_from_low_many_at_most_inner(size_t size, ZArray<Range>* out);

  bool check_limits(const Range& range) const;

public:
  ZMemoryManagerImpl();

  void register_callbacks(const Callbacks& callbacks);

  void register_range(const Range& range);
  bool unregister_first(Range* out);

  bool is_empty() const;
  bool is_contiguous() const;

  void anchor_limits();
  bool limits_contain(const Range& range) const;

  offset peek_low_address() const;
  offset_end peak_high_address_end() const;

  void insert_and_remove_from_low_many(const Range& range, ZArray<Range>* out);
  Range insert_and_remove_from_low_exact_or_many(size_t size, ZArray<Range>* in_out);

  void insert(const Range& range);

  Range remove_from_low(size_t size);
  Range remove_from_low_at_most(size_t size);
  size_t remove_from_low_many_at_most(size_t size, ZArray<Range>* out);
  Range remove_from_high(size_t size);

  void transfer_from_low(ZMemoryManagerImpl* other, size_t size);
};

#endif // SHARE_GC_Z_ZMEMORY_HPP
