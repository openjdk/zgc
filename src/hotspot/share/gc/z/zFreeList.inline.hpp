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

#include "gc/z/zAddress.hpp"
#include "gc/z/zAddress.inline.hpp"
#include "gc/z/zFreeList.hpp"
#include "gc/z/zPage.inline.hpp"
#include "gc/z/zPageType.hpp"
#include "logging/log.hpp"
#include "logging/logStream.hpp"
#include "runtime/atomic.hpp"
#include "runtime/globals.hpp"
#include "utilities/align.hpp"
#include "utilities/count_trailing_zeros.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/integerCast.hpp"
#include "utilities/powerOfTwo.hpp"
#include "utilities/vmError.hpp"

template <ZPageType PageType>
void ZFreeList<PageType>::print_size_classes() {
  if (!UseNewCode) {
    return;
  }

  LogTarget(Info, gc) lt;
  LogStream ls(lt);

  ls.print_cr("======= ZFreeList =======");

  for (int fl = 0; fl < FirstLevelIndexCount; fl++) {
    const size_t fl_size = size_t(1) << (fl + AlignmentShift);
    const size_t fl_size_next = (fl_size << 1);
    const size_t sl_portion = (fl_size_next - fl_size) >> SecondLevelIndexCountShift;
    if (UseNewCode2) {
      ls.print("%02d: " EXACTFMT " { ", fl, EXACTFMTARGS(fl_size));
    } else {
      ls.print("%02d: 0x%08zx { ", fl, fl_size);
    }
    for (int sl = 0; sl < SecondLevelIndexCount; sl++) {
      const size_t sl_size = fl_size + sl_portion * integer_cast<size_t>(sl);
      if (UseNewCode2) {
        ls.print(EXACTFMT " ", EXACTFMTARGS(sl_size));
      } else {
        ls.print("0x%08zx ", sl_size);
      }
    }
    ls.print_cr("}");
  }

  ls.print_cr("======= ZFreeList =======");
}

template <ZPageType PageType>
ZFreeList<PageType>::ZFreeList(const ZPage& page)
  : _page(page),
    _fl_bitmap(),
    _blocks() {
}

template <ZPageType PageType>
zaddress ZFreeList<PageType>::allocate(size_t size) {
  auto on_vm_error = OnVMError([&](outputStream* st) {
    const auto page_type_str = PageType == ZPageType::small ? "ZPageType::small" : "ZPageType::medium";
    st->print_cr("ZFreeList<%s>::allocate(size: 0x%zX)", page_type_str, size);
    print_on(st);
  });
  BlockHeader* blk = find_block(size);

  if (blk == nullptr) {
    return zaddress::null;
  }

  uintptr_t blk_start = uintptr_t(blk);
  return zaddress(blk_start);
}

template <ZPageType PageType>
typename ZFreeList<PageType>::BlockHeader* ZFreeList<PageType>::find_block(size_t size) {
  precond(size >= (size_t(1) << MinAllocSizeShift));
  precond(is_aligned(size, size_t(1) << AlignmentShift));

  // There maybe blocks in insertion_list_index which we can fit in, but we do not want to linear scan the list.
  uint32_t ideal = guaranteed_list_index(size);
  assert(ideal < ListCount, "sanity");

  BlockHeader* blk = nullptr;
  while (blk == nullptr) {
    // If the first-level index is out of bounds, the request cannot be fulfilled
    uint64_t available = _fl_bitmap.load_relaxed() & (~UCONST64(0) << ideal);

    if (available == 0) {
      // Free lists exhausted
      return nullptr;
    }

    uint32_t list_index = count_trailing_zeros(available);
    blk = remove_block(list_index);
  }

  // If the block can be split, we split it in order to minimize internal fragmentation
  if (blk->size != size) {
    BlockHeader* remainder_blk = split_block(blk, size);
    insert_block(remainder_blk);
  }

  return blk;
}

template <ZPageType PageType>
typename ZFreeList<PageType>::BlockHeader* ZFreeList<PageType>::split_block(BlockHeader* blk, size_t size) {
  size_t remainder_size = blk->size - size;

  // Shrink blk to size
  blk->size = integer_cast<uint32_t>(size);

  // Use a portion of blk's memory for the new block
  BlockHeader* remainder_blk = reinterpret_cast<BlockHeader*>((uintptr_t)blk + blk->size);
  remainder_blk->size = integer_cast<uint32_t>(remainder_size);

  return remainder_blk;
}

template <ZPageType PageType>
typename ZFreeList<PageType>::ZPageLocalOffset ZFreeList<PageType>::calculate_offset(BlockHeader* blk) const {
  if (blk == nullptr) {
    return ZPageLocalOffset::invalid;
  }

  return static_cast<ZPageLocalOffset>(_page.local_offset(to_zaddress_unsafe(reinterpret_cast<uintptr_t>(blk))));
}

template <ZPageType PageType>
typename ZFreeList<PageType>::BlockHeader* ZFreeList<PageType>::calculate_block(ZPageLocalOffset offset) const {
  if (offset == ZPageLocalOffset::invalid) {
    return nullptr;
  }

  return reinterpret_cast<BlockHeader*>(ZOffset::address_unsafe(_page.global_offset(static_cast<uint32_t>(offset))));
}

template <ZPageType PageType>
typename ZFreeList<PageType>::BlockHeader* ZFreeList<PageType>::blk_get_next(BlockHeader* blk) {
  return calculate_block(blk->next);
}

template <ZPageType PageType>
void ZFreeList<PageType>::blk_set_next(BlockHeader* blk, BlockHeader* next) {
  blk->next = calculate_offset(next);
}

template <ZPageType PageType>
uint32_t ZFreeList<PageType>::guaranteed_list_index(size_t size) const {
  precond(size <= (size_t(1) << (FirstLevelIndexCount + AlignmentShift - 1)));
  //       9876543210
  // Eg: 0b0010101000, AlignmentShift=3, SecondLevelIndexCountShift=2
  //         FSS
  //  size_shift   = 7;
  //  aligned_size = (0b0010101000 + 0b000011111) & ~0b000011111 = 0b0011000000
  const int size_shift = log2i(size);
  const size_t aligned_size = align_up(size, size_t(1) << (size_shift - SecondLevelIndexCountShift));
  return insertion_list_index(aligned_size);
}

template <ZPageType PageType>
uint32_t ZFreeList<PageType>::insertion_list_index(size_t size) const {
  //       9876543210
  // Eg: 0b0010101000, AlignmentShift=3, SecondLevelIndexCountShift=2
  //         FSS
  //  size_shift   = 7;
  //  first_level  = 7 - 3 = 4;
  //  second_level = (0b10101000 >> 5) ^ 0b100 = 0b101 ^ 0b100 = 0b01
  const int size_shift = log2i(size);
  const uint32_t first_level = integer_cast<uint32_t>(size_shift - AlignmentShift);
  const uint32_t second_level = integer_cast<uint32_t>(size >> (size_shift - SecondLevelIndexCountShift) ^ (size_t(1) << SecondLevelIndexCountShift));

  // The largest list contain larger size classes. Only exposed when coalesing.
  return MIN2((first_level << SecondLevelIndexCountShift) + second_level, integer_cast<uint>(ListCount) - 1);
}

template <ZPageType PageType>
void ZFreeList<PageType>::insert_block(BlockHeader* blk) {
  uint32_t list_index = insertion_list_index(blk->size);

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

template <ZPageType PageType>
typename ZFreeList<PageType>::BlockHeader* ZFreeList<PageType>::remove_block(uint32_t list_index) {
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

template <ZPageType PageType>
void ZFreeList<PageType>::free(zaddress_unsafe ptr, size_t size) {
  auto on_vm_error = OnVMError([&](outputStream* st) {
    const auto page_type_str = PageType == ZPageType::small ? "ZPageType::small" : "ZPageType::medium";
    st->print_cr("ZFreeList<%s>::free(ptr: " PTR_FORMAT ", size: 0x%zX)", page_type_str, untype(ptr), size);
    print_on(st);
  });

  assert(ptr != zaddress_unsafe::null, "sanity");

  BlockHeader* blk = reinterpret_cast<BlockHeader*>(ptr);
  blk->size = integer_cast<uint32_t>(size);
  insert_block(blk);
}


template <ZPageType PageType>
void ZFreeList<PageType>::print_on(outputStream* st) const {
  st->print_cr("bitmap: 0x%08zX", _fl_bitmap.load_relaxed());
  st->print("page: "); _page.print_on(st);

  for (int fl = 0; fl < FirstLevelIndexCount; fl++) {
    for (int sl = 0; sl < SecondLevelIndexCount; sl++) {
      const int index = fl * SecondLevelIndexCount + sl;
      const BlockHeader* block = _blocks[index].load_acquire();
      auto print_block = [&](const BlockHeader* block) {
        st->print(PTR_FORMAT "@", p2i(block));
        if (UseNewCode2) {
          st->print("{ size: " EXACTFMT ", next: 0x%08X }", EXACTFMTARGS(block->size), static_cast<uint32_t>(block->next));
        } else {
          st->print("{ size: 0x%08x, next: 0x%08X }", block->size, static_cast<uint32_t>(block->next));
        }
      };
      if (block != nullptr) {
        st->print("[%02d, %02d]: ", fl, sl);
        print_block(block);
        for (block = calculate_block(block->next); block != nullptr; block = calculate_block(block->next)) {
          st->print(" -> ");
          print_block(block);
        }
        st->cr();
      }
    }
  }
}
