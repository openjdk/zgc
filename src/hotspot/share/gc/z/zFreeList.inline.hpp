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
#include "gc/z/zArray.hpp"
#include "gc/z/zFreeList.hpp"
#include "gc/z/zGlobals.hpp"
#include "gc/z/zPage.inline.hpp"
#include "gc/z/zPageType.hpp"
#include "logging/log.hpp"
#include "logging/logStream.hpp"
#include "runtime/atomic.hpp"
#include "runtime/atomicAccess.hpp"
#include "runtime/globals.hpp"
#include "utilities/align.hpp"
#include "utilities/count_trailing_zeros.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/integerCast.hpp"
#include "utilities/powerOfTwo.hpp"
#include "utilities/vmError.hpp"
#include <cstdint>

template <ZPageType PageType>
void ZFreeList<PageType>::print_size_classes() {
  if (!ZOldRefCount) {
    return;
  }

  LogTarget(Info, gc, freelist) lt;
  LogStream ls(lt);

  ls.print_cr("======= ZFreeList (%s) =======", IsSmallPage ? "Small" : "Medium");

  if (MinAllocSize > MinAlignment) {
    ls.print_cr("_non_alloc_blocks: <" EXACTFMT " and Undo Blocks", EXACTFMTARGS(MinAllocSize));
  } else {
    ls.print_cr("_non_alloc_blocks: Only Undo Blocks");
  }

  for (int fl = MinFirstLevelIndex; fl <= MaxFirstLevelIndex; fl++) {
    const size_t fl_size = size_t(1) << fl;
    const size_t fl_size_next = (fl_size << 1);

    ls.print("%02d: %4zu%s { ", fl, EXACTFMTARGS(fl_size));

    if (fl < SpecialFirstLevelIndex) {
      for (size_t size = fl_size; size < fl_size_next; size += MinAlignment) {
        ls.print("%4zu%s ", EXACTFMTARGS(size));
      }
    } else if (fl < MaxFirstLevelIndex) {
      const size_t sl_portion = (fl_size_next - fl_size) >> SecondLevelIndexCountShift;
      for (int sl = 0; sl < SecondLevelIndexCount; sl++) {
        const size_t sl_size = fl_size + sl_portion * integer_cast<size_t>(sl);
        ls.print("%4zu%s ", EXACTFMTARGS(sl_size));
      }
    } else {
      precond(fl == MaxFirstLevelIndex);
      ls.print("%4zu%s+ ", EXACTFMTARGS(fl_size));
    }
    ls.print_cr("}");
  }

  ls.print_cr("======= ZFreeList (%s) =======", IsSmallPage ? "Small" : "Medium");
}

template <ZPageType PageType>
ZFreeList<PageType>::ZFreeList(const ZPage& page)
  : _page(page),
    _alignment(size_t(1) << (IsSmallPage ? ZObjectAlignmentSmallShift : ZObjectAlignmentMediumShift)),
    _bitmap(),
    _non_alloc_blocks(),
    _blocks() {
}

template <ZPageType PageType>
zaddress ZFreeList<PageType>::allocate(size_t size) {
  auto on_vm_error = OnVMError([&](outputStream* st) {
    const auto page_type_str = PageType == ZPageType::small ? "ZPageType::small" : "ZPageType::medium";
    st->print_cr("ZFreeList<%s>::allocate(size: 0x%zX)", page_type_str, size);
    error_print_on(st);
  });
  precond(ZOldRefCount);
  precond(ZAllocateInFreeList);

  BlockHeader* blk = find_block(align_up(size, _alignment));

  if (blk == nullptr) {
    return zaddress::null;
  }

  uintptr_t blk_start = uintptr_t(blk);

  postcond(is_aligned(blk_start, _alignment));
  return zaddress(blk_start);
}

template <ZPageType PageType>
typename ZFreeList<PageType>::BlockHeader* ZFreeList<PageType>::find_block(size_t size) {
  precond(size >= MinAllocSize);
  precond(is_aligned(size, _alignment));

  // There maybe blocks in insertion_list_index which we can fit in, but we do not want to linear scan the list.
  uint32_t ideal = guaranteed_list_index(size);
  assert(ideal < ListCount, "sanity");

  BlockHeader* blk = nullptr;
  while (blk == nullptr) {
    // If the first-level index is out of bounds, the request cannot be fulfilled
    uint64_t available = _bitmap.load_relaxed() & (~UCONST64(0) << ideal);

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
  if (size < NonSpecialFirstLevelSize) {
    return insertion_list_index(size);
  }

  precond(size <= MaxAllocSize);

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
  if (size < NonSpecialFirstLevelSize) {
    const uint32_t index = integer_cast<uint32_t>(size / MinAlignment) - 1;
    postcond(index < SpecialIndexCount);
    return index;
  }

  //       9876543210
  // Eg: 0b0010101000, AlignmentShift=3, SecondLevelIndexCountShift=2
  //         FSS
  //  size_shift   = 7;
  //  first_level  = 7 - 3 = 4;
  //  second_level = (0b10101000 >> 5) ^ 0b100 = 0b101 ^ 0b100 = 0b01
  const int size_shift = log2i(size);
  const uint32_t first_level = integer_cast<uint32_t>(size_shift - SpecialFirstLevelIndex);
  const uint32_t second_level = integer_cast<uint32_t>(size >> (size_shift - SecondLevelIndexCountShift) ^ (size_t(1) << SecondLevelIndexCountShift));

  // The largest list contain larger size classes. Only exposed when coalesing.
  return MIN2(SpecialIndexCount + (first_level << SecondLevelIndexCountShift) + second_level, integer_cast<uint32_t>(ListCount - 1));
}

template <ZPageType PageType>
void ZFreeList<PageType>::insert_block(BlockHeader* blk) {
  precond(is_aligned(blk, _alignment));

  const size_t size = blk->size;

  if (size < MinAllocSize) {
    // Add non alloc free block
    insert_non_alloc_block(blk);
    return;
  }


  uint32_t list_index = insertion_list_index(size);

  for (;;) {
    BlockHeader* head = _blocks[list_index].load_acquire();
    blk_set_next(blk, head);
    if (_blocks[list_index].compare_set(head, blk, memory_order_release)) {
      break;
    }
  }

  // Update bitmap to indicate level has a free block
  for (;;) {
    uint64_t current_word = _bitmap.load_relaxed();
    uint64_t new_word = current_word | UCONST64(1) << list_index;
    if (current_word == new_word ||
        _bitmap.compare_set(current_word, new_word, memory_order_relaxed)) {
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
    uint64_t current_word = _bitmap.load_relaxed();
    uint64_t new_word = current_word & ~(UCONST64(1) << list_index);
    if (current_word == new_word ||
        _bitmap.compare_set(current_word, new_word, memory_order_relaxed)) {
      return nullptr;
    }
  }
}

template <ZPageType PageType>
void ZFreeList<PageType>::insert_non_alloc_block(BlockHeader* blk) {
  // Add non alloc free block
  for (;;) {
    BlockHeader* head = _non_alloc_blocks.load_acquire();
    blk_set_next(blk, head);
    if (_non_alloc_blocks.compare_set(head, blk, memory_order_release)) {
      return;
    }
  }
}

template <ZPageType PageType>
void ZFreeList<PageType>::free(zaddress_unsafe ptr, size_t size) {
  auto on_vm_error = OnVMError([&](outputStream* st) {
    const auto page_type_str = PageType == ZPageType::small ? "ZPageType::small" : "ZPageType::medium";
    st->print_cr("ZFreeList<%s>::free(ptr: " PTR_FORMAT ", size: 0x%zX)", page_type_str, untype(ptr), size);
    error_print_on(st);
  });
  precond(ZOldRefCount);
  precond(ptr != zaddress_unsafe::null);
  precond(is_aligned(untype(ptr), _alignment));
  precond(_page.is_in({ZAddress::offset(ptr), size}));

  BlockHeader* blk = reinterpret_cast<BlockHeader*>(ptr);
  blk->size = integer_cast<uint32_t>(align_up(size, _alignment));
  insert_block(blk);
}

template <ZPageType PageType>
void ZFreeList<PageType>::undo_allocate(zaddress_unsafe ptr, size_t size) {
  auto on_vm_error = OnVMError([&](outputStream* st) {
    const auto page_type_str = PageType == ZPageType::small ? "ZPageType::small" : "ZPageType::medium";
    st->print_cr("ZFreeList<%s>::undo_allocate(ptr: " PTR_FORMAT ", size: 0x%zX)", page_type_str, untype(ptr), size);
    error_print_on(st);
  });
  precond(ZOldRefCount);
  precond(ptr != zaddress_unsafe::null);
  precond(is_aligned(untype(ptr), _alignment));
  precond(_page.is_in({ZAddress::offset(ptr), size}));

  BlockHeader* blk = reinterpret_cast<BlockHeader*>(ptr);
  blk->size = integer_cast<uint32_t>(align_up(size, _alignment));
  insert_non_alloc_block(blk);
}

template <ZPageType PageType>
size_t ZFreeList<PageType>::coalesce_free_list() {
  auto on_vm_error = OnVMError([&](outputStream* st) {
    const auto page_type_str = PageType == ZPageType::small ? "ZPageType::small" : "ZPageType::medium";
    st->print_cr("ZFreeList<%s>::coalesce_free_list()", page_type_str);
    error_print_on(st);
  });

  ZArray<BlockHeader*> blocks;

  const auto push_list_blocks= [&](BlockHeader* head) {
    for (BlockHeader* block = head; block != nullptr; block = calculate_block(block->next)) {
      blocks.push(block);
    }
  };

  // Acquire all the free blocks, clear the free-list
  push_list_blocks(_non_alloc_blocks.exchange(nullptr, memory_order_acquire));
  for (auto& head : _blocks) {
    push_list_blocks(head.exchange(nullptr, memory_order_acquire));
  }
  _bitmap.store_relaxed(0u);

  if (blocks.is_empty()) {
    return 0;
  }

  blocks.sort([](BlockHeader** e1, BlockHeader** e2) {
    precond(*e1 != *e2);
    return reinterpret_cast<uintptr_t>(*e1) < reinterpret_cast<uintptr_t>(*e2) ? -1 : 1;
  });


  size_t total_free_size = 0;
  BlockHeader* last_block = nullptr;
  for (BlockHeader* block : blocks) {
    if (last_block == nullptr) {
      last_block = block;
    } else {
      const uintptr_t last_block_end_addr = reinterpret_cast<uintptr_t>(last_block) + last_block->size;
      const uintptr_t block_start_addr = reinterpret_cast<uintptr_t>(block);
      if (last_block_end_addr == block_start_addr) {
        last_block->size += block->size;
      } else {
        // Insert the last block
        insert_block(last_block);
        total_free_size += last_block->size;
        last_block = block;
      }
    }
  }
  postcond(last_block != nullptr);
  insert_block(last_block);
  total_free_size += last_block->size;

  return total_free_size;
}

template <ZPageType PageType>
void ZFreeList<PageType>::free_tail(zaddress_unsafe ptr, size_t size) {
  auto on_vm_error = OnVMError([&](outputStream* st) {
    const auto page_type_str = PageType == ZPageType::small ? "ZPageType::small" : "ZPageType::medium";
    st->print_cr("ZFreeList<%s>::free_tail(ptr: " PTR_FORMAT ", size: 0x%zX)", page_type_str, untype(ptr), size);
    error_print_on(st);
  });
  precond(ZOldRefCount);
  precond(ptr != zaddress_unsafe::null);
  precond(is_aligned(untype(ptr), _alignment));
  precond(_page.is_in({ZAddress::offset(ptr), size}));
  precond(_page.end() == to_end_type(ZAddress::offset(ptr), size));

  BlockHeader* blk = reinterpret_cast<BlockHeader*>(ptr);
  blk->size = integer_cast<uint32_t>(align_up(size, _alignment));
  insert_non_alloc_block(blk);
}

template <ZPageType PageType>
void ZFreeList<PageType>::print_on_impl(outputStream* st, bool on_error) const {
  const auto print_list = [&](const BlockHeader* head) {
    const auto print_block = [&](const BlockHeader* block) {
      precond(block != nullptr);

      st->print(PTR_FORMAT "@", p2i(block));
      st->print("{ size: " EXACTFMT ", next: 0x%08X }", EXACTFMTARGS(block->size), static_cast<uint32_t>(block->next));
    };

    print_block(head);

    int max_print_blocks = 10;
    for (const BlockHeader* block = calculate_block(head->next); block != nullptr; block = calculate_block(block->next)) {
      st->print(" -> ");
      print_block(block);
      if (max_print_blocks-- == 0) {
        break;
      }
    }
    st->cr();
  };

  {
    const BlockHeader* const head = _non_alloc_blocks.load_acquire();

    const auto bitmap = _bitmap.load_relaxed();
    if (!on_error && bitmap == 0 && head == nullptr) {
      return;
    }

    st->print_cr("bitmap: 0x%08zX", _bitmap.load_relaxed());
    st->print("page: "); _page.print_on(st);

    if (head != nullptr) {
      st->print("_non_alloc_blocks: "); print_list(head);
    }
  }

  for (int fl = MinFirstLevelIndex; fl <= MaxFirstLevelIndex; fl++) {
    if (fl < SpecialFirstLevelIndex) {
      const size_t fl_size = size_t(1) << fl;
      const size_t fl_size_next = (fl_size << 1);
      for (size_t size = fl_size; size < fl_size_next; size += MinAlignment) {
        const int index = integer_cast<int>(size / MinAlignment) - 1;
        const BlockHeader* const head = _blocks[index].load_acquire();
        if (head != nullptr) {
          st->print("[%02d](%02d, %2zu%s): ", index, fl, EXACTFMTARGS(size)); print_list(head);
        }
      }
    } else if (fl < MaxFirstLevelIndex) {
      for (int sl = 0; sl < SecondLevelIndexCount; sl++) {
        const int index = SpecialIndexCount + (fl - SpecialFirstLevelIndex) * SecondLevelIndexCount + sl;
        precond(index < ListCount);
        const BlockHeader* const head = _blocks[index].load_acquire();
        if (head != nullptr) {
          st->print("[%02d](%02d, %02d): ", index, fl, sl); print_list(head);
        }
      }
    } else {
        const BlockHeader* const head = _blocks[ListCount - 1].load_acquire();
        if (head != nullptr) {
          st->print("[%02d](%02d, " EXACTFMT "): ", ListCount - 1, fl, EXACTFMTARGS(MaxAllocSize)); print_list(head);
        }
    }
  }
}


template <ZPageType PageType>
void ZFreeList<PageType>::error_print_on(outputStream* st) const {
  print_on_impl(st, true /* on_error */);
}

template <ZPageType PageType>
void ZFreeList<PageType>::print_on(outputStream* st) const {
  print_on_impl(st, false /* on_error */);
}
