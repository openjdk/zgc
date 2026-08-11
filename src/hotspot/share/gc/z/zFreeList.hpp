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
 *
 */

#ifndef SHARE_GC_Z_ZFREELIST_HPP
#define SHARE_GC_Z_ZFREELIST_HPP

#include "gc/z/zAddress.hpp"
#include "gc/z/zGlobals.hpp"
#include "gc/z/zPageType.hpp"
#include "memory/allocation.hpp"
#include "runtime/atomic.hpp"
#include "utilities/globalDefinitions.hpp"
#include <cstdint>

class ZPage;
class outputStream;

// TODO: Try to move these into the free list. But need to sort out PrimitiveConversions.
enum class ZPageLocalOffset : uint32_t {};

struct ZNextBlockDescriptor {
  uint32_t _size{};
  ZPageLocalOffset _next{};

  bool is_null() const;
  ZNextBlockDescriptor split_off_tail(uint32_t size);
};

template<>
struct PrimitiveConversions::Translate<ZNextBlockDescriptor> : public std::true_type {
  typedef ZNextBlockDescriptor Value;
  typedef uint64_t Decayed;

  static Decayed decay(Value v) {
    return (uint64_t(v._next) << 32) | uint64_t(v._size);
  }
  static Value recover(Decayed d) {
    const Value value{uint32_t(d & 0xFFFFFFFF), ZPageLocalOffset(d >> 32)};
    postcond(d == decay(value));
    return value;
  }
};


// Free list based on a flattened two level segregated fit. It has been
// carefully adjusted specifically for the size classes of a ZPage.
template <ZPageType PageType>
class ZFreeList : public CHeapObj<mtGC> {
public:
  explicit ZFreeList(const ZPage& page);

  zaddress allocate(size_t size);
  void free(zaddress_unsafe addr, size_t size);
  // TODO: Cleanup interface.
  void free_tail(zaddress_unsafe addr, size_t size);
  void undo_allocate(zaddress_unsafe addr, size_t size);

  size_t coalesce_free_list();

  static void print_size_classes();
private:

  // static constexpr ZPageType PageType = ZPageType::small;
  static_assert(PageType != ZPageType::large, "Unsupported ZPageType");
  static constexpr bool IsSmallPage = PageType == ZPageType::small;
  // Inclusive
  static constexpr int MinAllocSizeShift = IsSmallPage ? ZMinMinObjectSizeSmallShift : ZMinMinObjectSizeMediumShift;
  static constexpr size_t MinAllocSize = size_t(1) << MinAllocSizeShift;

  // Exclusive, a fully free page is returned to the PageAllocator
  static constexpr int MaxFreeBlockSizeShift = IsSmallPage ? ZPageSizeSmallShift : ZPageSizeMediumMaxMaxShift;
  static constexpr int MaxAllocSizeShift = MaxFreeBlockSizeShift - 3;
  static constexpr size_t MaxAllocSize = size_t(1) << MaxAllocSizeShift;

  // TODO: Should we make this dynamic? It would affect all the calculates and
  //       which list we create.
  static constexpr int MinAlignmentShift = IsSmallPage ? ZMinObjectAlignmentSmallShift: ZMinObjectAlignmentMediumShift;

  static constexpr size_t MinAlignment = size_t(1) << MinAlignmentShift;

  static constexpr int SecondLevelIndexCountShift = IsSmallPage ? 2 : 3;

  static constexpr int SecondLevelIndexCount = 1 << SecondLevelIndexCountShift;

  // First Level Min allocatable index, if smaller free blocks are possible they are put on the _non_alloc_blocks list.
  static constexpr int MinFirstLevelIndex = MinAllocSizeShift;

  // First Level Max allocatable index, we currently put all free blocks covering the largets allocation size in the final _blocks[ListCount - 1] list.
  static constexpr int MaxFirstLevelIndex = MaxAllocSizeShift;

  static constexpr int FirstLevelIndexCount = MaxFirstLevelIndex - MinFirstLevelIndex;

  static constexpr int SpecialFirstLevelIndexCount = MAX2(SecondLevelIndexCountShift + MinAlignmentShift - MinAllocSizeShift, 0);

  static constexpr int SpecialFirstLevelIndex = MinFirstLevelIndex + SpecialFirstLevelIndexCount;

  static constexpr int SpecialIndexCount = (1 << SpecialFirstLevelIndexCount) - 1;

  static constexpr size_t NonSpecialFirstLevelSize = MinAlignment * (SpecialIndexCount + 1);

  // Lists [ SpecialIndex i * aligmnent, ..., FL+SL, ..., Largest Allocation Size ]
  static constexpr int ListCount = SpecialIndexCount + ((MaxFirstLevelIndex - SpecialFirstLevelIndex) << SecondLevelIndexCountShift) + 1 /* MaxFirstLevelIndex list */;

  const ZPage& _page;
  const size_t _alignment;

  // Tracking pressense of allocatable size class lists.
  Atomic<uint64_t> _bitmap;

  // This is the list used for blocks that are smaller than the smallest allocation, or have been returned via undo.
  Atomic<ZNextBlockDescriptor> _non_alloc_blocks;

  // All the _fl_bitmap tracket lists
  Atomic<ZNextBlockDescriptor> _blocks[ListCount];

  static_assert(ListCount <= sizeof(_bitmap) * 8);

  void insert_block(ZNextBlockDescriptor blk);
  void insert_non_alloc_block(ZNextBlockDescriptor blk);

  ZNextBlockDescriptor find_block(size_t size);

  // If blk is not nullptr, blk is removed, otherwise the head of the free-list
  // corresponding to mapping is removed.
  ZNextBlockDescriptor remove_block(uint32_t list_index, uint32_t size);

  // The following methods are calculated differently depending on the configuration.
  inline ZNextBlockDescriptor blk_get_next(ZNextBlockDescriptor blk) const;
  inline void blk_set_next(ZNextBlockDescriptor blk, ZNextBlockDescriptor next);

  uint32_t guaranteed_list_index(size_t size) const;
  uint32_t insertion_list_index(size_t size) const;

  ZPageLocalOffset to_local_offset(zaddress_unsafe addr) const;
  zaddress_unsafe from_local_offset(ZPageLocalOffset offset) const;

  void print_on_impl(outputStream* st, bool on_error) const;
  void error_print_on(outputStream* st) const;

public:
  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_Z_ZFREELIST_HPP
