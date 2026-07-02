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
#include "memory/allocation.hpp"
#include "runtime/atomic.hpp"
#include "utilities/globalDefinitions.hpp"

struct BlockHeader {
  uint32_t size;
  uint32_t next;
};

// Free list based on a flattened two level segregated fit. It has been
// carefully adjusted specifically for the size classes of a ZPage.
class ZFreeList : public CHeapObj<mtGC> {
public:
  ZFreeList(zaddress page_start, size_t page_size, size_t alignment);

  zaddress allocate(size_t size);
  void free(zaddress addr, size_t size);

private:
  static const size_t _min_alloc_size_log2 = 4;
  static const size_t _min_alloc_size = 2 << _min_alloc_size_log2;

  static const size_t _fl_index = 15;
  static const size_t _sl_index_log2 = 2;
  static const size_t _sl_index = (1 << _sl_index_log2);
  static const size_t _num_lists = _fl_index * _sl_index;
  static const size_t _mbs = 8;

  const uintptr_t _page_start;
  const size_t _page_size;
  const size_t _alignment;

  Atomic<uint64_t> _fl_bitmap;
  Atomic<BlockHeader*> _blocks[_num_lists];

  void insert_block(BlockHeader *blk);

  BlockHeader *find_block(size_t size);

  // If blk is not nullptr, blk is removed, otherwise the head of the free-list
  // corresponding to mapping is removed.
  BlockHeader *remove_block(uint32_t list_index);

  // size is the number of bytes that should remain in blk. blk is shrinked to
  // size and a new block with the remaining blk->size - size is returned.
  BlockHeader *split_block(BlockHeader *blk, size_t size);

  bool ptr_in_pool(uintptr_t ptr);

  size_t align_size(size_t size);

  // The following methods are calculated differently depending on the configuration.
  inline BlockHeader *blk_get_next(BlockHeader *blk);
  inline void blk_set_next(BlockHeader *blk, BlockHeader *next);

  uint32_t ideal_list_index(size_t size);

  //// Manually trigger block coalescing.
  //void coalesce(std::map<void *, size_t> &allocmap);

  //BlockHeader *get_next_phys_block(BlockHeader *blk, std::map<void *, size_t> &allocmap);
};

#endif // SHARE_GC_Z_ZFREELIST_HPP
