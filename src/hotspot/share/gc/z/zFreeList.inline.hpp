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

#include "gc/z/zFreeList.hpp"

#include "gc/z/zAddress.hpp"
#include "gc/z/zAddress.inline.hpp"
#include "gc/z/zArray.hpp"
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

inline bool ZNextBlockDescriptor::is_null() const {
  return _size == 0;
}

inline ZNextBlockDescriptor ZNextBlockDescriptor::split_off_tail(uint32_t size) {
  precond(size <= _size);

  if (_size <= size) {
    return {};
  }

  _size -= size;

  return {size, ZPageLocalOffset(uint32_t(_next) + _size)};
}

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

  const ZNextBlockDescriptor blk = find_block(align_up(size, _alignment));

  if (blk.is_null()) {
    return zaddress::null;
  }

  postcond(blk._size >= size);
  // TODO: Cleanup usage of zaddress vs zaddress_unsafe vs new type for
  // dereferencable but not yet initalized heap memory.
  return safe(from_local_offset(blk._next));
}

template <ZPageType PageType>
ZNextBlockDescriptor ZFreeList<PageType>::find_block(size_t size) {
  precond(size >= MinAllocSize);
  precond(is_aligned(size, _alignment));

  // There maybe blocks in insertion_list_index which we can fit in, but we do not want to linear scan the list.
  uint32_t ideal = guaranteed_list_index(size);
  assert(ideal < ListCount, "sanity");

  ZNextBlockDescriptor blk;
  uint32_t list_index;
  while (blk.is_null()) {
    // If the first-level index is out of bounds, the request cannot be fulfilled
    uint64_t available = _bitmap.load_relaxed() & (~UCONST64(0) << ideal);

    if (available == 0) {
      // Free lists exhausted
      return blk;
    }

    list_index = count_trailing_zeros(available);
    blk = remove_block(list_index, size);
  }

  postcond(blk._size >= size);

  // If the block can be split, we split it in order to minimize internal fragmentation
  if (blk._size != size) {
    const ZNextBlockDescriptor remainder_blk = blk.split_off_tail(blk._size - size);
    precond(remainder_blk._size < MinAllocSize || list_index > insertion_list_index(remainder_blk._size));
    insert_block(remainder_blk);
  }

  return blk;
}

template <ZPageType PageType>
ZPageLocalOffset ZFreeList<PageType>::to_local_offset(zaddress_unsafe addr) const {
  return static_cast<ZPageLocalOffset>(_page.local_offset(addr));
}

template <ZPageType PageType>
zaddress_unsafe ZFreeList<PageType>::from_local_offset(ZPageLocalOffset offset) const {
  return ZOffset::address_unsafe(_page.global_offset(static_cast<uint32_t>(offset)));
}

template <ZPageType PageType>
ZNextBlockDescriptor ZFreeList<PageType>::blk_get_next(ZNextBlockDescriptor blk) const {
  // TODO: Cleanup types
  return *reinterpret_cast<ZNextBlockDescriptor*>(from_local_offset(blk._next));
}

template <ZPageType PageType>
void ZFreeList<PageType>::blk_set_next(ZNextBlockDescriptor blk, ZNextBlockDescriptor next) {
  // TODO: Cleanup types
  *reinterpret_cast<ZNextBlockDescriptor*>(from_local_offset(blk._next)) = next;
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
void ZFreeList<PageType>::insert_block(ZNextBlockDescriptor blk) {
  precond(is_aligned(uint32_t(blk._next), _alignment));

  const size_t size = blk._size;

  if (size < MinAllocSize) {
    // Add non alloc free block
    insert_non_alloc_block(blk);
    return;
  }

  uint32_t list_index = insertion_list_index(size);

  for (;;) {
    ZNextBlockDescriptor head = _blocks[list_index].load_acquire();
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
ZNextBlockDescriptor ZFreeList<PageType>::remove_block(uint32_t list_index, uint32_t size) {
  for (;;) {
    const ZNextBlockDescriptor head = _blocks[list_index].load_acquire();

    if (head.is_null()) {
      break;
    }

    const uint32_t remaining_size = head._size - size;
    if (remaining_size >= MinAllocSize && insertion_list_index(remaining_size) == list_index) {
      // Same index
      ZNextBlockDescriptor next = head;
      const ZNextBlockDescriptor tail = next.split_off_tail(size);
      postcond(next._size == remaining_size);

      if (_blocks[list_index].compare_set(head, next)) {
        return tail;
      }
    } else {
      // New list index, remove whole block
      ZNextBlockDescriptor next = blk_get_next(head);
      if (_blocks[list_index].compare_set(head, next)) {
        return head;
      }
    }

  }

  // TODO: Clean up clearing semantics
  for (;;) {
    uint64_t current_word = _bitmap.load_relaxed();
    uint64_t new_word = current_word & ~(UCONST64(1) << list_index);
    if (current_word == new_word ||
        _bitmap.compare_set(current_word, new_word, memory_order_relaxed)) {
      return {};
    }
  }
}

template <ZPageType PageType>
void ZFreeList<PageType>::insert_non_alloc_block(ZNextBlockDescriptor blk) {
  // Add non alloc free block
  for (;;) {
    ZNextBlockDescriptor head = _non_alloc_blocks.load_acquire();
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

  insert_block({integer_cast<uint32_t>(align_up(size, _alignment)), to_local_offset(ptr)});
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

  insert_non_alloc_block({integer_cast<uint32_t>(align_up(size, _alignment)), to_local_offset(ptr)});
}

template <ZPageType PageType>
size_t ZFreeList<PageType>::coalesce_free_list() {
  auto on_vm_error = OnVMError([&](outputStream* st) {
    const auto page_type_str = PageType == ZPageType::small ? "ZPageType::small" : "ZPageType::medium";
    st->print_cr("ZFreeList<%s>::coalesce_free_list()", page_type_str);
    error_print_on(st);
  });

  ZArray<ZNextBlockDescriptor> blocks;

  const auto push_list_blocks= [&](ZNextBlockDescriptor head) {
    for (ZNextBlockDescriptor block = head; !block.is_null(); block = blk_get_next(block)) {
      blocks.push(block);
    }
  };

  // Acquire all the free blocks, clear the free-list
  push_list_blocks(_non_alloc_blocks.exchange({}, memory_order_acquire));
  for (auto& head : _blocks) {
    push_list_blocks(head.exchange({}, memory_order_acquire));
  }
  _bitmap.store_relaxed(0u);

  if (blocks.is_empty()) {
    return 0;
  }

  blocks.sort([](ZNextBlockDescriptor* e1, ZNextBlockDescriptor* e2) {
    precond(e1->_next != e2->_next);
    return e1->_next < e2->_next ? -1 : 1;
  });


  size_t total_free_size = 0;
  ZNextBlockDescriptor last_block{};
  for (ZNextBlockDescriptor block : blocks) {
    if (last_block.is_null()) {
      last_block = block;
    } else {
      const uintptr_t last_block_end_addr = untype(from_local_offset(last_block._next)) + last_block._size;
      const uintptr_t block_start_addr = untype(from_local_offset(block._next));
      if (last_block_end_addr == block_start_addr) {
        last_block._size += block._size;
      } else {
        // Insert the last block
        insert_block(last_block);
        total_free_size += last_block._size;
        last_block = block;
      }
    }
  }
  postcond(!last_block.is_null());
  insert_block(last_block);
  total_free_size += last_block._size;

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
  precond(is_aligned(size, _alignment));
  precond(_page.is_in({ZAddress::offset(ptr), size}));
  precond(_page.end() == to_end_type(ZAddress::offset(ptr), size));

  insert_non_alloc_block({integer_cast<uint32_t>(size), to_local_offset(ptr)});
}

template <ZPageType PageType>
void ZFreeList<PageType>::print_on_impl(outputStream* st, bool on_error) const {
  const auto print_list = [&](const ZNextBlockDescriptor head) {
    const auto print_block = [&](const ZNextBlockDescriptor block) {
      precond(!block.is_null());

      st->print(PTR_FORMAT "@", untype(from_local_offset(block._next)));
      st->print("{ size: " EXACTFMT ", next: 0x%08X }", EXACTFMTARGS(block._size), static_cast<uint32_t>(block._next));
    };

    print_block(head);

    int max_print_blocks = 10;
    for (ZNextBlockDescriptor block = blk_get_next(head); !block.is_null(); block = blk_get_next(block)) {
      st->print(" -> ");
      print_block(block);
      if (max_print_blocks-- == 0) {
        break;
      }
    }
    st->cr();
  };

  {
    const ZNextBlockDescriptor head = _non_alloc_blocks.load_acquire();

    const auto bitmap = _bitmap.load_relaxed();
    if (!on_error && bitmap == 0 && head.is_null()) {
      return;
    }

    st->print_cr("bitmap: 0x%08zX", _bitmap.load_relaxed());
    st->print("page: "); _page.print_on(st);

    if (!head.is_null()) {
      st->print("_non_alloc_blocks: "); print_list(head);
    }
  }

  for (int fl = MinFirstLevelIndex; fl <= MaxFirstLevelIndex; fl++) {
    if (fl < SpecialFirstLevelIndex) {
      const size_t fl_size = size_t(1) << fl;
      const size_t fl_size_next = (fl_size << 1);
      for (size_t size = fl_size; size < fl_size_next; size += MinAlignment) {
        const int index = integer_cast<int>(size / MinAlignment) - 1;
        const ZNextBlockDescriptor head = _blocks[index].load_acquire();
        if (!head.is_null()) {
          st->print("[%02d](%02d, %2zu%s): ", index, fl, EXACTFMTARGS(size)); print_list(head);
        }
      }
    } else if (fl < MaxFirstLevelIndex) {
      for (int sl = 0; sl < SecondLevelIndexCount; sl++) {
        const int index = SpecialIndexCount + (fl - SpecialFirstLevelIndex) * SecondLevelIndexCount + sl;
        precond(index < ListCount);
        const ZNextBlockDescriptor head = _blocks[index].load_acquire();
        if (!head.is_null()) {
          st->print("[%02d](%02d, %02d): ", index, fl, sl); print_list(head);
        }
      }
    } else {
        const ZNextBlockDescriptor head = _blocks[ListCount - 1].load_acquire();
        if (!head.is_null()) {
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
