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

class ZPage;
class outputStream;

// Free list based on a flattened two level segregated fit. It has been
// carefully adjusted specifically for the size classes of a ZPage.
template <ZPageType PageType>
class ZFreeList : public CHeapObj<mtGC> {
public:
  explicit ZFreeList(const ZPage& page);

  zaddress allocate(size_t size);
  void free(zaddress_unsafe addr, size_t size);

  static void print_size_classes();
private:
  enum class ZPageLocalOffset : uint32_t {
    invalid = -1u,
  };

  struct BlockHeader {
    uint32_t size;
    ZPageLocalOffset next;
  };

  // static constexpr ZPageType PageType = ZPageType::small;
  static_assert(PageType != ZPageType::large, "Unsupported ZPageType");
  static constexpr bool IsSmallPage = PageType == ZPageType::small;
  // Inclusive
  static constexpr int MinAllocSizeShift = IsSmallPage ? ZMinObjectAlignmentShift : ZMinObjectAlignmentMediumShift;
  // Exclusive, a fully free page is returned to the PageAllocator
  static constexpr int MaxFreeBlockSizeShift = IsSmallPage ? ZPageSizeSmallShift : ZPageSizeMediumMaxMaxShift;

  // TODO: This seems partially incorrect.
  static constexpr int AlignmentShift = MinAllocSizeShift;
  static constexpr int SecondLevelIndexCountShift = 2;
  static constexpr int FirstLevelIndexMax = MaxFreeBlockSizeShift - 1;
  static constexpr int SecondLevelIndexCount = 1 << SecondLevelIndexCountShift;
  static constexpr int FirstLevelIndexShift = SecondLevelIndexCountShift + AlignmentShift;
  static constexpr int FirstLevelIndexCount = FirstLevelIndexMax - FirstLevelIndexShift + 1;
  static constexpr size_t SmallBlockSize = size_t(1) << FirstLevelIndexShift;

  static constexpr int ListCount = FirstLevelIndexCount * SecondLevelIndexCount;

  const ZPage& _page;

  Atomic<uint64_t> _fl_bitmap;
  Atomic<BlockHeader*> _blocks[ListCount];

  static_assert(ListCount <= sizeof(_fl_bitmap) * 8);

  void insert_block(BlockHeader* blk);

  BlockHeader* find_block(size_t size);

  // If blk is not nullptr, blk is removed, otherwise the head of the free-list
  // corresponding to mapping is removed.
  BlockHeader* remove_block(uint32_t list_index);

  // size is the number of bytes that should remain in blk. blk is shrinked to
  // size and a new block with the remaining blk->size - size is returned.
  BlockHeader* split_block(BlockHeader* blk, size_t size);

  // The following methods are calculated differently depending on the configuration.
  inline BlockHeader* blk_get_next(BlockHeader* blk);
  inline void blk_set_next(BlockHeader* blk, BlockHeader* next);

  uint32_t guaranteed_list_index(size_t size) const;
  uint32_t insertion_list_index(size_t size) const;

  ZPageLocalOffset calculate_offset(BlockHeader* blk) const;
  BlockHeader* calculate_block(ZPageLocalOffset offset) const;

  void print_on(outputStream* st) const;
};

#endif // SHARE_GC_Z_ZFREELIST_HPP
