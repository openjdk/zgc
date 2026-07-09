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

#include "gc/z/zAddress.inline.hpp"
#include "gc/z/zFreeList.hpp"
#include "gc/z/zLock.inline.hpp"
#include "runtime/atomic.hpp"
#include "utilities/debug.hpp"

#include <limits>

// TODO: Should live elsewhere and deal with crappy platforms
static size_t ffs(size_t number) {
  return __builtin_ffsl(number) - 1;
}

static size_t fls(size_t number) {
  return sizeof(size_t) * CHAR_BIT - __builtin_clzl(number);
}

static size_t ilog2(size_t number) {
  return fls(number) - 1;
}

ZFreeList::ZFreeList(zaddress page_start, size_t page_size, size_t alignment)
  : _page_start(untype(page_start)),
    _page_size(page_size),
    _alignment(alignment),
    _fl_bitmap(),
    _blocks() {
}

zaddress ZFreeList::allocate(size_t size) {
  BlockHeader* blk = find_block(size);

  if (blk == nullptr) {
    return zaddress::null;
  }

  uintptr_t blk_start = uintptr_t(blk);
  return zaddress(blk_start);
}

BlockHeader *ZFreeList::find_block(size_t size) {
  size_t aligned_size = align_size(size); // TODO: No need to align surely?
  size_t target_size = aligned_size + (1UL << (ilog2(aligned_size) - _sl_index_log2)) - 1;

  uint32_t ideal = ideal_list_index(target_size);
  assert(ideal <= _num_lists, "sanity");

  BlockHeader *blk = nullptr;
  while (blk == nullptr) {
    // If the first-level index is out of bounds, the request cannot be fulfilled
    uint64_t available = _fl_bitmap.load_relaxed() & (~UCONST64(0) << ideal);

    if (available == 0) {
      // Free lists exhausted
      return nullptr;
    }

    uint32_t list_index = ffs(available);
    blk = remove_block(list_index);
  }

  // If the block can be split, we split it in order to minimize internal fragmentation
  if ((blk->size - aligned_size) >= _mbs) {
    BlockHeader *remainder_blk = split_block(blk, aligned_size);
    insert_block(remainder_blk);
  }

  return blk;
}

BlockHeader *ZFreeList::split_block(BlockHeader *blk, size_t size) {
  size_t remainder_size = blk->size - size;

  // Shrink blk to size
  blk->size = size;

  // Use a portion of blk's memory for the new block
  BlockHeader *remainder_blk = reinterpret_cast<BlockHeader *>((uintptr_t)blk + blk->size);
  remainder_blk->size = remainder_size;

  return remainder_blk;
}

bool ZFreeList::ptr_in_pool(uintptr_t ptr) {
  return ptr >= _page_start && ptr < (_page_start + _page_size);
}

size_t ZFreeList::align_size(size_t size) {
  if(size == 0) {
    size = 1;
  }

  return align_up(size, _mbs);
}

static uint32_t calculate_offset(BlockHeader *blk, uintptr_t start) {
  if (blk == nullptr) {
    return std::numeric_limits<uint32_t>::max();
  }

  return reinterpret_cast<uintptr_t>(blk) - start;
}

static BlockHeader* calculate_block(uint32_t offset, uintptr_t start) {
  if (offset == std::numeric_limits<uint32_t>::max()) {
    return nullptr;
  }

  return reinterpret_cast<BlockHeader*>(static_cast<uintptr_t>(offset) + start);
}

BlockHeader* ZFreeList::blk_get_next(BlockHeader *blk) {
  return calculate_block(blk->next, _page_start);
}

void ZFreeList::blk_set_next(BlockHeader *blk, BlockHeader *next) {
  blk->next = calculate_offset(next, _page_start);
}

uint32_t ZFreeList::ideal_list_index(size_t size) {
  uint32_t fl = ilog2(size);
  uint32_t sl = size >> (fl - _sl_index_log2) ^ (1UL << _sl_index_log2);
  return ((fl - _min_alloc_size_log2) << _sl_index_log2) + sl;
}

void ZFreeList::insert_block(BlockHeader *blk) {
  uint32_t list_index = ideal_list_index(blk->size);

  for (;;) {
    BlockHeader* head = _blocks[list_index].load_acquire();
    blk_set_next(blk, head);
    if (_blocks[list_index].compare_set(head, blk, memory_order_release)) {
      break;
    }
  }

  // Update bitmap to indicate level has a free block
  for (;;) {
    uint64_t current_word = _fl_bitmap.load_relaxed();
    uint64_t new_word = current_word | UCONST64(1) << list_index;
    if (current_word == new_word ||
        _fl_bitmap.compare_set(current_word, new_word, memory_order_relaxed)) {
      break;
    }
  }
}

BlockHeader* ZFreeList::remove_block(uint32_t list_index) {
  for (;;) {
    BlockHeader* head = _blocks[list_index].load_acquire();

    if (head == nullptr) {
      break;
    }

    BlockHeader* next = blk_get_next(head);
    if (_blocks[list_index].compare_set(head, next)) {
      return head;
    }
  }

  for (;;) {
    uint64_t current_word = _fl_bitmap.load_relaxed();
    uint64_t new_word = current_word & ~(UCONST64(1) << list_index);
    if (current_word == new_word ||
        _fl_bitmap.compare_set(current_word, new_word, memory_order_relaxed)) {
      return nullptr;
    }
  }
}

void ZFreeList::free(zaddress ptr, size_t size) {
  assert(ptr != zaddress::null, "sanity");
  assert(ptr_in_pool(untype(ptr)), "sanity");

  BlockHeader *blk = reinterpret_cast<BlockHeader *>(ptr);
  blk->size = size;
  insert_block(blk);
}

//BlockHeader *ZFreeList::get_next_phys_block(BlockHeader *blk, std::map<void *, size_t> &allocmap) {
//  if(blk == nullptr) {
//    return nullptr;
//  }
//
//  size_t step;
//  if(allocmap.find(blk) == allocmap.end()) {
//    step = blk->size;
//  } else {
//    step = allocmap.find(blk)->second;
//  }
//
//  uintptr_t next = (uintptr_t)blk + step;
//  return ptr_in_pool(next)
//    ? (BlockHeader *)next
//    : nullptr;
//}

//void ZFreeList::coalesce(std::map<void *, size_t> &allocmap) {
//  // 1. Clear bitmap and free-lists.
//  _fl_bitmap = 0;
//  for(size_t i = 0; i < _num_lists + 1; i++) {
//    _blocks[i] = nullptr;
//  }
//
//  BlockHeader *current_blk = reinterpret_cast<BlockHeader *>(_block_start);
//
//  while(current_blk != nullptr) {
//    bool current_free = (allocmap.find(current_blk) == allocmap.end());
//    BlockHeader *next_blk = get_next_phys_block(current_blk, allocmap);
//
//    if(current_free) {
//      bool next_free = (next_blk != nullptr) && (allocmap.find(next_blk) == allocmap.end());
//      // Coalesce with all following blocks that are free.
//      while(next_free) {
//        current_blk->size += next_blk->size;
//        BlockHeader *new_next_blk = get_next_phys_block(next_blk, allocmap);
//
//        if(new_next_blk == next_blk) {
//          break;
//        }
//
//        next_blk = new_next_blk;
//        next_free = (next_blk != nullptr) && (allocmap.find(next_blk) == allocmap.end());
//      }
//
//      // Only insert the current block (which has been coalesced with all free
//      // next blocks) if it is free.
//      insert_block(current_blk);
//    }
//
//    current_blk = next_blk;
//  }
//}
