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

#include "gc/shared/gc_globals.hpp"
#include "gc/shared/suspendibleThreadSet.hpp"
#include "gc/z/zAddress.inline.hpp"
#include "gc/z/zArray.inline.hpp"
#include "gc/z/zBitMap.inline.hpp"
#include "gc/z/zCPU.inline.hpp"
#include "gc/z/zDefer.inline.hpp"
#include "gc/z/zGeneration.inline.hpp"
#include "gc/z/zHeap.inline.hpp"
#include "gc/z/zLock.inline.hpp"
#include "gc/z/zNUMA.hpp"
#include "gc/z/zNUMA.inline.hpp"
#include "gc/z/zPageAge.inline.hpp"
#include "gc/z/zPageType.hpp"
#include "gc/z/zReferenceCounting.hpp"
#include "gc/z/zStat.hpp"
#include "gc/z/zTask.hpp"
#include "gc/z/zTree.inline.hpp"
#include "gc/z/zValue.inline.hpp"
#include "logging/log.hpp"
#include "memory/allocation.hpp"
#include "nmt/memTag.hpp"
#include "oops/access.inline.hpp"
#include "oops/markWord.inline.hpp"
#include "runtime/atomicAccess.hpp"
#include "utilities/bitMap.inline.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/integerCast.hpp"
#include "utilities/powerOfTwo.hpp"
#include "utilities/stack.inline.hpp"

static const ZStatSubPhase ZSubPhaseConcurrentDeathRow("Concurrent Death Row", ZGenerationId::young);
static const ZStatSubPhase ZSubPhaseConcurrentCoalesceFreeList("Concurrent Coalesce Free List", ZGenerationId::young);
static const ZStatSubPhase ZSubPhaseConcurrentDeathRowPage("Concurrent Death Row Page", ZGenerationId::young);
static const ZStatSubPhase ZSubPhaseConcurrentCoalesceFreeListPage("Concurrent Coalesce Free List Page", ZGenerationId::young);

// We use "crow reference counting". Crows can count 1, 2, 3, many. In other
// words, it can't really distinguish between 4 and 5. For us, the counts get
// blurred at 7, because we use 4 bits *signed* reference count. The signedness
// is required due to races between mutators and the GC, causing periodic
// instability in the reference counters. So reference count of over 7 means we
// don't really know. Once this number is reached, we never change it, and the
// object simply can not be reclaimed with reference counting. The reference
// counts are embedded in the 4 bit age bits of the markWord of objects.
struct ZHeaderRefCount : public AllStatic {
  static constexpr int RefCountBits = markWord::refc_bits;
  static constexpr int SignBit = RefCountBits - 1;
  static constexpr uint Mask = right_n_bits(RefCountBits);
  static constexpr uint SignMask = ~(uint)right_n_bits(SignBit);
  static constexpr int Uncertain = (int)(SignMask);
  static constexpr int Min = (int)SignMask;
  static constexpr int Max = -(Min + 1);

  static int count_impl(markWord mark) {
    uint age = mark.refc();
    int ref_count = (int)age;

    if (is_set_nth_bit(age, SignBit)) {
      // Sign extend the integer
      ref_count |= SignMask;
    }

    return ref_count;
  }

  static markWord set_count_impl(markWord mark, int ref_count) {
    uint age = (uint)ref_count & Mask;

    return mark.set_refc(age);
  }

  static int count(markWord mark) {
    int ref_count = count_impl(mark);
    assert(ref_count == Uncertain || (ref_count <= Max && ref_count >= Min),
           "incorrect bounds: %d", ref_count);
    return ref_count;
  }

  static markWord set_count(markWord mark, int ref_count) {
    markWord result = set_count_impl(mark, ref_count);

    assert(ref_count == Uncertain || (ref_count <= Max && ref_count >= Min),
           "incorrect bounds: %d", ref_count);
    assert(count_impl(result) == ref_count, "invalid ref count");

    return result;
  }
};

static BitMap::idx_t page_to_index(const ZPage* page) {
  return untype(page->start()) >> ZGranuleSizeShift;
}

static void set_pardoned(ZPage* page, zaddress addr) {
  if (!page->is_allocating()) {
    return;
  }

  if (ZGeneration::young()->is_phase_relocate()) {
    return;
  }

  page->set_pardoned(addr);
}

struct ZReferenceCounting::State : public CHeapObj<mtGC> {
  // Allocation Support
  template <ZPageType PageType>
  struct CPUAllocPages {
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

    static constexpr int SizeClasses = MaxAllocSizeShift - MinAllocSizeShift + 1;

    ZArray<ZPage*> _alloc_pages;
    const uint32_t _cpu_id;
    const uint32_t _numa_id;
    Atomic<int> _next_page_index[SizeClasses];

    CPUAllocPages(uint32_t cpu_id)
        : _alloc_pages(),
          _cpu_id(cpu_id),
          _numa_id(ZNUMA::cpu_id_to_numa_id(cpu_id)),
          _next_page_index() {}

    void reset() {
      _alloc_pages.clear();
      for (Atomic<int>& index : _next_page_index) {
        index.store_relaxed(0);
      }
    }

    void reset_and_deallocate() {
      _alloc_pages.clear_and_deallocate();
      for (Atomic<int>& index : _next_page_index) {
        index.store_relaxed(0);
      }
    }

    void reserve(int capacity) {
      _alloc_pages.reserve(capacity);
    }

    void push(ZPage* page) {
      _alloc_pages.push(page);
    }

    bool free_list_alloc_object(size_t size, FreeListAllocation* allocation) {
      const int size_class = log2i_ceil(size) - MinAllocSizeShift;
      precond(size_class < SizeClasses);
      precond(size_class >= 0);

      for (;;) {
        const int page_index = _next_page_index[size_class].load_relaxed();

        if (page_index == _alloc_pages.length()) {
          return false;
        }

        ZPage* const page = _alloc_pages.at(page_index);
        const zaddress addr = page->alloc_object_from_free_list(size);
        if (addr != zaddress::null) {
          *allocation = {page, addr};
          return true;
        }
        const bool success = _next_page_index[size_class].compare_set(page_index, page_index + 1, memory_order_relaxed);
        if (success && page_index + 1 == _alloc_pages.length()) {
          log_debug(gc, freelist)("{%02u, %01u} Exhausted " EXACTFMT " size class", _cpu_id, _numa_id, EXACTFMTARGS(size_t(1) << (size_class + MinAllocSizeShift)));
        }
      }
    }
  };

  // Death Row Counters Support
  struct Counters {
    size_t _death_row_roots[ZPageAgeOldCount] = {};
    size_t _processed[ZPageAgeOldCount] = {};
    size_t _freed[ZPageAgeOldCount] = {};
    size_t _large_freed[ZPageAgeOldCount] = {};
    size_t _pardoned[ZPageAgeOldCount] = {};
    size_t _free_list_available[ZPageTypeCount] = {};
  };

  ZPerWorker<Counters> _per_worker_counters;

  void record_death_row_counters() {
    size_t death_row_roots[ZPageAgeOldCount] = {};
    size_t processed[ZPageAgeOldCount] = {};
    size_t freed[ZPageAgeOldCount] = {};
    size_t large_freed[ZPageAgeOldCount] = {};
    size_t pardoned[ZPageAgeOldCount] = {};

    ZPerWorkerIterator<State::Counters> counters_iter{&_per_worker_counters};
    for (State::Counters* counters; counters_iter.next(&counters);) {
      for (uint i = 0; i < ZPageAgeOldCount; i++) {
        death_row_roots[i] += counters->_death_row_roots[i];
        processed[i] += counters->_processed[i];
        freed[i] += counters->_freed[i];
        large_freed[i] += counters->_large_freed[i];
        pardoned[i] += counters->_pardoned[i];
      }
    }

    ZGeneration::young()->stat_reference_counting()->at_process_death_row(death_row_roots,
                                                                          processed,
                                                                          freed,
                                                                          large_freed,
                                                                          pardoned);
  }

  void record_free_list_available_counters() {
    size_t free_list_available[ZPageTypeCount] = {};

    ZPerWorkerIterator<State::Counters> counters_iter{&_per_worker_counters};
    for (State::Counters* counters; counters_iter.next(&counters);) {
      for (uint i = 0; i < ZPageTypeCount; i++) {
        free_list_available[i] += counters->_free_list_available[i];
      }
    }

    for (uint i = 0; i < ZPageTypeCount; i++) {
      ZGeneration::young()->set_freelist_available_at_start(static_cast<ZPageType>(i), free_list_available[i]);
    }
  }

  struct FreeListAllocator {
    // Free-list Construction Support
    ZPerCPU<CPUAllocPages<ZPageType::small>> _small_allocation_pages{ZValueIdTagType{}};
    ZPerCPU<CPUAllocPages<ZPageType::medium>> _medium_allocation_pages{ZValueIdTagType{}};

    struct PerNUMAData {
      ZArray<ZPage*> _allocating_pages[2]{};
    };

    ZPerWorker<ZPerNUMA<PerNUMAData>> _per_worker_allocating;


    CPUAllocPages<ZPageType::small>& small_allocation_pages(uint32_t cpu_id) {
      return _small_allocation_pages.get(cpu_id);
    }

    CPUAllocPages<ZPageType::medium>& medium_allocation_pages(uint32_t cpu_id) {
      return _medium_allocation_pages.get(cpu_id);
    }

    void construct_free_list_allocator() {
      // TODO: Parallize?
      const int numa_count = integer_cast<int>(ZNUMA::count());
      const int cpu_count = integer_cast<int>(ZCPU::count());
      const int cpu_per_numa = MAX2(1, cpu_count / numa_count);

      ZArray<int> per_numa_page_count[2]{{numa_count, numa_count, {}},
                                        {numa_count, numa_count, {}}};

      { // Accumelate workers page counts
        ZPerWorkerIterator<ZPerNUMA<PerNUMAData>> worker_iter(&_per_worker_allocating);
        for (ZPerNUMA<PerNUMAData>* worker_data; worker_iter.next(&worker_data);) {
          uint32_t numa_id;
          ZPerNUMAIterator<PerNUMAData> numa_iter(worker_data);
          for (PerNUMAData* numa_data; numa_iter.next(&numa_data, &numa_id);) {
            for (int i = 0; i < 2; i++) {
              per_numa_page_count[i].at(integer_cast<int>(numa_id)) += numa_data->_allocating_pages[i].length();
            }
          }
        }
      }

      { // Reset old allocators
        ZPerCPUIterator<CPUAllocPages<ZPageType::small>> small_iter(&_small_allocation_pages);
        for (CPUAllocPages<ZPageType::small>* alloc_pages; small_iter.next(&alloc_pages);) {
          const uint32_t numa_id = alloc_pages->_numa_id;
          const int small_pages_per_cpu = (per_numa_page_count[0].at(integer_cast<int>(numa_id)) + cpu_per_numa - 1) / cpu_per_numa;
          alloc_pages->reset();
          alloc_pages->reserve(small_pages_per_cpu);
        }

        ZPerCPUIterator<CPUAllocPages<ZPageType::medium>> medium_iter(&_medium_allocation_pages);
        for (CPUAllocPages<ZPageType::medium>* alloc_pages; medium_iter.next(&alloc_pages);) {
          const uint32_t numa_id = alloc_pages->_numa_id;
          const int medium_pages_per_cpu = (per_numa_page_count[1].at(integer_cast<int>(numa_id)) + cpu_per_numa - 1) / cpu_per_numa;
          alloc_pages->reset();
          alloc_pages->reserve(medium_pages_per_cpu);
        }
      }

      ZPerWorkerIterator<ZPerNUMA<PerNUMAData>> worker_iter(&_per_worker_allocating);
      for (ZPerNUMA<PerNUMAData>* worker_data; worker_iter.next(&worker_data);) {
        uint32_t numa_id;
        ZPerNUMAIterator<PerNUMAData> numa_iter(worker_data);
        for (PerNUMAData* numa_data; numa_iter.next(&numa_data, &numa_id);) {
          // Insert all small pages
          ZArray<ZPage*>& small_pages = numa_data->_allocating_pages[0];
          while (small_pages.is_nonempty()) {
            const int pre_length = small_pages.length();

            ZPerCPUIterator<CPUAllocPages<ZPageType::small>> small_iter(&_small_allocation_pages);
            for (CPUAllocPages<ZPageType::small>* alloc_pages; small_pages.is_nonempty() && small_iter.next(&alloc_pages);) {
              if (numa_id != alloc_pages->_numa_id) {
                // Wrong NUMA id
                continue;
              }
              alloc_pages->push(small_pages.pop());
            }

            if (pre_length == small_pages.length()) {
              assert(ZNUMA::is_faked(), "Something weird happened");
              _small_allocation_pages.addr(0)->push(small_pages.pop());
            }
          }

          // Insert all medium pages
          ZArray<ZPage*>& medium_pages = numa_data->_allocating_pages[1];
          while (medium_pages.is_nonempty()) {
            const int pre_length = medium_pages.length();

            ZPerCPUIterator<CPUAllocPages<ZPageType::medium>> medium_iter(&_medium_allocation_pages);
            for (CPUAllocPages<ZPageType::medium>* alloc_pages; medium_pages.is_nonempty() && medium_iter.next(&alloc_pages);) {
              if (numa_id != alloc_pages->_numa_id) {
                // Wrong NUMA id
                continue;
              }
              alloc_pages->push(medium_pages.pop());
            }

            if (pre_length == medium_pages.length()) {
              assert(ZNUMA::is_faked(), "Something weird happened");
              _medium_allocation_pages.addr(0)->push(medium_pages.pop());
            }
          }
        }
      }
    }

    void reset_per_worker_state() {
      ZPerWorkerIterator<ZPerNUMA<PerNUMAData>> iter(&_per_worker_allocating);
      for (ZPerNUMA<PerNUMAData>* allocating; iter.next(&allocating);) {
        // Clear and dealocate arrays
        ZPerNUMAIterator<PerNUMAData> numa_iter(allocating);
        for (PerNUMAData* numa_data; numa_iter.next(&numa_data);) {
          for (auto& array : numa_data->_allocating_pages) {
            array.clear_and_deallocate();
          }
        }
      }
    }

    void reset_allocator() {
      ZPerCPUIterator<CPUAllocPages<ZPageType::small>> small_iter(&_small_allocation_pages);
      for (CPUAllocPages<ZPageType::small>* alloc_pages; small_iter.next(&alloc_pages);) {
        alloc_pages->reset_and_deallocate();
      }

      ZPerCPUIterator<CPUAllocPages<ZPageType::medium>> medium_iter(&_medium_allocation_pages);
      for (CPUAllocPages<ZPageType::medium>* alloc_pages; medium_iter.next(&alloc_pages);) {
        alloc_pages->reset_and_deallocate();
      }
    }
  };

  FreeListAllocator _free_list_allocators[ZPageAgeOldCount];

  // TODO: Cleanup interface
  ZBitMap _to_coalesce{ZAddressOffsetMax >> ZGranuleSizeShift, mtGC, true /* clear */};

  CPUAllocPages<ZPageType::small>& small_allocation_pages(uint32_t cpu_id, ZPageAge to_age) {
    precond(ZPageAgeRangeOld.contains(to_age));
    return _free_list_allocators[ZPageAgeRangeOld.index(to_age)].small_allocation_pages(cpu_id);
  }

  CPUAllocPages<ZPageType::medium>& medium_allocation_pages(uint32_t cpu_id, ZPageAge to_age) {
    precond(ZPageAgeRangeOld.contains(to_age));
    return _free_list_allocators[ZPageAgeRangeOld.index(to_age)].medium_allocation_pages(cpu_id);
  }

  void construct_free_list_promotion_allocator() {
    _free_list_allocators[ZPageAgeRangeOld.index(ZPageAge::promotion)].construct_free_list_allocator();
  }

  void construct_free_list_old_allocator() {
    _free_list_allocators[ZPageAgeRangeOld.index(ZPageAge::old)].construct_free_list_allocator();
    _free_list_allocators[ZPageAgeRangeOld.index(ZPageAge::old)].reset_per_worker_state();
  }

  void reset_per_worker_state_promotion() {
    // TODO: Cleanup free list counters

    // Clear counters
    _per_worker_counters.set_all({});

    _free_list_allocators[ZPageAgeRangeOld.index(ZPageAge::promotion)].reset_per_worker_state();
  }

  void reset_old_allocator() {
    _free_list_allocators[ZPageAgeRangeOld.index(ZPageAge::old)].reset_allocator();
  }

  void clear_bitmap() {
    _to_coalesce.clear();
  }

  void register_free_page(const ZPage* page) {
    _to_coalesce.par_set_bit(page_to_index(page), memory_order_relaxed);
  }
};

ZReferenceCounting::FoundDeathRow::FoundDeathRow()
    // Array initialization requires copy constructors, which CHeapBitMap
    // doesn't provide. Instantiate two instances, and populate an array
    // with pointers to the two instances.
  : _bitmaps{{ZAddressOffsetMax >> ZGranuleSizeShift, mtGC, true /* clear */},
             {ZAddressOffsetMax >> ZGranuleSizeShift, mtGC, true /* clear */}},
    _current{0} {}

BitMap& ZReferenceCounting::FoundDeathRow::current_bitmap() {
  return _bitmaps[_current];
}

const BitMap& ZReferenceCounting::FoundDeathRow::current_bitmap() const {
  return _bitmaps[_current];
}

BitMap& ZReferenceCounting::FoundDeathRow::previous_bitmap() {
  return _bitmaps[_current ^ 1];
}

const BitMap& ZReferenceCounting::FoundDeathRow::previous_bitmap() const {
  return _bitmaps[_current ^ 1];
}

void ZReferenceCounting::FoundDeathRow::flip() {
  _current ^= 1;
}

void ZReferenceCounting::FoundDeathRow::register_page(ZPage* page) {
  current_bitmap().par_set_bit(page_to_index(page), memory_order_relaxed);
}

void ZReferenceCounting::FoundDeathRow::verify_previous() const {
  // Should all have been claimed
  postcond(previous_bitmap().find_first_set_bit(0u) == previous_bitmap().size());
}

template <typename Function>
void ZReferenceCounting::FoundDeathRow::par_iterate_death_row_pages(ZPageTable* page_table, Function function) {
  previous_bitmap().iterate([&](BitMap::idx_t index) {
    if (previous_bitmap().par_clear_bit(index, memory_order_relaxed)) {
      return function(page_table->at(index));
    }

    return true;
  });
}

ZReferenceCounting::State* ZReferenceCounting::state() {
  precond(ZOldRefCount);
  return _state;
}

const ZReferenceCounting::State* ZReferenceCounting::state() const {
  precond(ZOldRefCount);
  return _state;
}

int64_t ZReferenceCounting::increment(zaddress addr, ZPage* page) {
  oop obj = to_oop(addr);

  for (;;) {
    markWord mark = obj->mark();
    int ref_count = ZHeaderRefCount::count(mark);

    if (ref_count == ZHeaderRefCount::Max) {
      int64_t table_stake;
      page->_overflow_ref_counts.update(addr, [&](int64_t* prev, int64_t** updated) {
        table_stake = prev == nullptr ? 0 : *prev;
        **updated = table_stake + 1;
      });

      OrderAccess::fence();

      markWord mark_reloaded = obj->mark();
      int ref_count_reloaded = ZHeaderRefCount::count(mark_reloaded);

      if (ref_count == ref_count_reloaded) {
        // Mark word still maxed out after populating table; table entry provably not redundant
        return table_stake + ref_count;
      }

      page->_overflow_ref_counts.update(addr, [&](int64_t* prev, int64_t** updated) {
        const int64_t table_stake = prev == nullptr ? 0 : *prev;
        const int64_t new_table_stake = table_stake - 1;
        if (new_table_stake == 0) {
          *updated = nullptr;
        } else {
          **updated = new_table_stake;
        }
      });

      continue;
    }

    if (ref_count == ZHeaderRefCount::Min) {
      int64_t table_stake;
      page->_overflow_ref_counts.update(addr, [&](int64_t* prev, int64_t** updated) {
        table_stake = prev == nullptr ? 0 : *prev;
        if (table_stake == 0 || table_stake == -1) {
          *updated = nullptr;
        } else {
          **updated = table_stake + 1;
        }
      });

      if (table_stake != 0) {
        OrderAccess::fence();

        markWord mark_reloaded = obj->mark();
        int ref_count_reloaded = ZHeaderRefCount::count(mark_reloaded);

        if (ref_count_reloaded == ref_count) {
          // Mark word counters look the stable across table stake increment; return
          return table_stake + ref_count;
        }

        page->_overflow_ref_counts.update(addr, [&](int64_t* prev, int64_t** updated) {
          const int64_t table_stake = prev == nullptr ? 0 : *prev;
          const int64_t new_table_stake = table_stake - 1;
          if (new_table_stake == 0) {
            *updated = nullptr;
          } else {
            **updated = new_table_stake;
          }
        });

        continue;
      }
    }

    int new_ref_count = ref_count == ZHeaderRefCount::Max ? ZHeaderRefCount::Uncertain : (ref_count + 1);
    markWord new_mark = ZHeaderRefCount::set_count(mark, new_ref_count);

    if (obj->cas_set_mark(new_mark, mark, memory_order_relaxed) == mark) {
      return ref_count;
    }
  }
}

int64_t ZReferenceCounting::decrement(zaddress addr, ZPage* page) {
  oop obj = to_oop(addr);

  for (;;) {
    markWord mark = obj->mark();
    int ref_count = ZHeaderRefCount::count(mark);

    if (ref_count == ZHeaderRefCount::Min) {
      int64_t table_stake;
      page->_overflow_ref_counts.update(addr, [&](int64_t* prev, int64_t** updated) {
        table_stake = prev == nullptr ? 0 : *prev;
        **updated = table_stake - 1;
      });

      OrderAccess::fence();

      markWord mark_reloaded = obj->mark();
      int ref_count_reloaded = ZHeaderRefCount::count(mark_reloaded);

      if (ref_count == ref_count_reloaded) {
        // Mark word still maxed out after populating table; table entry provably not redundant
        return table_stake + ref_count;
      }

      page->_overflow_ref_counts.update(addr, [&](int64_t* prev, int64_t** updated) {
        const int64_t table_stake = prev == nullptr ? 0 : *prev;
        const int64_t new_table_stake = table_stake + 1;
        if (new_table_stake == 0) {
          *updated = nullptr;
        } else {
          **updated = new_table_stake;
        }
      });

      continue;
    }

    if (ref_count == ZHeaderRefCount::Max) {
      int64_t table_stake;
      page->_overflow_ref_counts.update(addr, [&](int64_t* prev, int64_t** updated) {
        table_stake = prev == nullptr ? 0 : *prev;
        if (table_stake == 0 || table_stake == 1) {
          *updated = nullptr;
        } else {
          **updated = table_stake - 1;
        }
      });

      if (table_stake != 0) {
        OrderAccess::fence();

        markWord mark_reloaded = obj->mark();
        int ref_count_reloaded = ZHeaderRefCount::count(mark_reloaded);

        if (ref_count_reloaded == ref_count) {
          // Mark word counters look the stable across table stake increment; return
          return table_stake + ref_count;
        }

        page->_overflow_ref_counts.update(addr, [&](int64_t* prev, int64_t** updated) {
          const int64_t table_stake = prev == nullptr ? 0 : *prev;
          const int64_t new_table_stake = table_stake + 1;
          if (new_table_stake == 0) {
            *updated = nullptr;
          } else {
            **updated = new_table_stake;
          }
        });

        continue;
      }
    }

    int new_ref_count = ref_count - 1;
    markWord new_mark = ZHeaderRefCount::set_count(mark, new_ref_count);

    if (obj->cas_set_mark(new_mark, mark, memory_order_relaxed) == mark) {
      return ref_count;
    }
  }
}

ZReferenceCounting::ZReferenceCounting()
  : _found_death_row(),
    _state(ZOldRefCount ? new State() : nullptr) {}

void ZReferenceCounting::on_remember(volatile zpointer* p, zaddress addr, bool remembered) {
  ZPage* p_page = ZHeap::heap()->page(p);
  const bool p_is_old = p_page->is_old();

  bool forgotten = false;

  if (p_is_old && remembered) {
    forgotten = ZGeneration::young()->forget_previous(p);
  }

  if (is_null(addr)) {
    // Only count old-to-old edges.
    return;
  }

  ZPage* addr_page = ZHeap::heap()->page(addr);

  if (!addr_page->is_old()) {
    return;
  }

  // The fact that the mutator had a path to access the previous value means that
  // it could have been loaded concurrently and become a root, after root processing
  // has finished. Therefore, we must pardon the object from any death row processing.
  set_pardoned(addr_page, addr);

  if (!p_is_old) {
    // Young-to-old edges are not reference counted, but their previous old
    // value must be protected for the current death-row pass.
    return;
  }

  if (!remembered) {
    // Not the first old-to-old edge mutation; bail from accounting ref counts
    return;
  }

  const bool p_flip_promoted = p_page->is_flip_promoted_current_young_collection();
  const bool addr_flip_promoted = addr_page->is_flip_promoted_current_young_collection();
  const bool addr_reloc_promoted = !addr_flip_promoted &&
                                   addr_page->is_promoted() &&
                                   !ZGeneration::young()->is_phase_mark() &&
                                   ZHeaderRefCount::count(to_oop(addr)->mark()) == 0;

  const bool suppressed_promoting_edge = p_flip_promoted || addr_flip_promoted || addr_reloc_promoted;

  if (suppressed_promoting_edge) {
    assert(!forgotten, "why is there a prev bit on promoting edges?");
    return;
  }

  // The first store after young generation marking starts always needs to perform
  // the first decrement of the previously referred to object (i.e. addr). However,
  // if the remembered set entry from the previous bits was set, that means that we
  // must also compete with remembered set scanning to claim the responsibility for
  // performing the last increment of the previous marking epoch. This is done by
  // using atomics to clear the previous bits. The winner gets to account for the
  // last increment. In the first store path, that means that the last increment
  // of the previous epoch and the first decrement of the current epoch effectively
  // cancel out, leaving there to be no need to update the old-to-old reference count.
  if (!forgotten) {
    // This release orders setting pardon before setting death row, as well as setting
    // pardon before decrementing.
    OrderAccess::release();

    if (decrement(addr, addr_page) == 1 && addr_page->is_allocating()) {
      // A decrement to zero requires a death row request
      addr_page->set_death_row(addr);
      _found_death_row.register_page(addr_page);
    }
  }
}

void ZReferenceCounting::on_failed_remember(zaddress addr) {
  if (is_null(addr) || ZHeap::heap()->is_young(addr)) {
    // Only count old-to-old edges.
    return;
  }

  // When promoting objects and old-to-old moving objects between old relocate start
  // and young mark start, we have to compensate for failed remset inserts, because
  // the mutator incorrectly decremented due to winning with setting the current
  // remembered set. This ASCII art shows what strategy we have for different types
  // of object movement in different phases:
  //  ORS                      YMS                    YRS
  //                                                               y2o
  //                                                     young reloc compensates
  //                                                    inc on failed curr insert
  //
  //              o2o
  //    old reloc compensates
  //  inc on failed curr insert
  //  and clears from-space prev
  //
  //                                      o2o
  //                                always inc prev

  ZPage* page = ZHeap::heap()->page(addr);
  // Increments imply that the last GC cycle still had a reference to the object.
  // That means it could have escaped into roots before or after root scanning.
  // So we have to conservatively pardon these objects from the death row.
  set_pardoned(page, addr);

  increment(addr, page);
}

void ZReferenceCounting::on_forget(volatile zpointer* p, zaddress addr) {
  ZPage* page = ZHeap::heap()->page(addr);
  // Increments imply that the last GC cycle still had a reference to the object.
  // That means it could have escaped into roots before or after root scanning.
  // So we have to conservatively pardon these objects from the death row.
  set_pardoned(page, addr);

  // When we have an old-to-old pointer that is about to become forgotten,
  // it means that it was written to by a mutator in the last marking epoch.
  // Therefore, we have to account for the last increment of the last cycle.
  increment(addr, page);
}

void ZReferenceCounting::on_promotion(zaddress addr) {
  assert(ZHeap::heap()->is_old(addr), "must be old");
  assert(ZHeap::heap()->page(addr)->is_allocating(), "must be allocating");

  ZPage* page = ZHeap::heap()->page(addr);
  page->set_death_row(addr);
  _found_death_row.register_page(page);
}

void ZReferenceCounting::on_old_to_space_alloc(ZPage* to_page, zaddress to_addr, bool mutator) { // TODO: Completeness for pardoning
  set_pardoned(to_page, to_addr);

  if (mutator) {
    // Maintain one stake in the mutator old-to-old relocation until the GC gets to
    // process the to-space object and add it to the right pardon/deathrow sets. It
    // will then decrement the counter.
    increment(to_addr, to_page);
  }
}

// TODO: More helpful arguments
void ZReferenceCounting::on_old_to_old(zaddress from_addr, ZPage* from_page, zaddress to_addr, ZPage* to_page, bool was_mutator) {
  assert(to_page->is_old(), "must be old");
  assert(from_page->is_old(), "must be old");
  assert(to_page->is_allocating(), "must be allocating");

  // Release to order setting pardon before the death row bit and decrement.
  OrderAccess::release();

  // Only dereference the to oop in case of in-place relocation
  int ref_count = ZHeaderRefCount::count(to_oop(to_addr)->mark());

  if (!was_mutator && (ref_count == ZHeaderRefCount::Min || ref_count == ZHeaderRefCount::Max)) {
    int64_t overflow_count;
    if (from_page->_overflow_ref_counts.find(from_addr, &overflow_count)) {
      to_page->_overflow_ref_counts.update(to_addr, [&](int64_t* prev, int64_t** update) {
        **update = overflow_count;
      });
    }
  }

  if (was_mutator) {
    if (decrement(to_addr, to_page) == 1) {
      // Decrement to zero; register death row request
      to_page->set_death_row(to_addr);
      _found_death_row.register_page(to_page);
    }
  } else if (ZHeaderRefCount::count(to_oop(to_addr)->mark()) == 0) {
    to_page->set_death_row(to_addr);
    _found_death_row.register_page(to_page);
  }
}

void ZReferenceCounting::on_mutator_old_to_old(ZForwarding* forwarding, zaddress from_addr, zaddress to_addr) {
  const uint32_t young_marks = ZGeneration::old()->young_marks_since_old_reloc_start();
  const bool before_young_mark = young_marks == 0;

  ZPage* const from_page = forwarding->page();
  // Note: even with in-place relocation, the to_page could be another page
  ZPage* const to_page = ZHeap::heap()->page(to_addr);

  // Move the overflow ref count stake to the new table
  int ref_count = ZHeaderRefCount::count(to_oop(from_addr)->mark());
  if (ref_count == ZHeaderRefCount::Min || ref_count == ZHeaderRefCount::Max) {
    int64_t overflow_count;
    if (from_page->_overflow_ref_counts.find(from_addr, &overflow_count)) {
      to_page->_overflow_ref_counts.update(to_addr, [&](int64_t* prev, int64_t** update) {
        **update = overflow_count;
      });
    }
  }

  if (!before_young_mark) {
    // TODO: Comments
    return;
  }

  const uintptr_t from_local_offset = from_page->local_offset(from_addr);


  // Uses _relaxed version to handle that in-place relocation resets _top
  assert(ZHeap::heap()->is_in_page_relaxed(from_page, from_addr), "Must be");
  assert(to_page->is_in(to_addr), "Must be");

  const size_t size = ZUtils::object_size(to_addr);

  BitMap::Iterator iter = from_page->remset_iterator_limited_current(from_local_offset, size);

  for (BitMap::idx_t field_bit : iter) {
    const uintptr_t field_local_offset = ZRememberedSet::to_offset(field_bit);

    // Add remset entry in the to-page
    const uintptr_t offset = field_local_offset - from_local_offset;
    const zaddress to_field = to_addr + offset;
    const zaddress from_field = from_addr + offset;
    volatile zpointer* const to_p = (volatile zpointer*)to_field;
    volatile zpointer* const from_p = (volatile zpointer*)from_field;

    // Forget current so subsequent remset scanning doesn't process the prev bits from fromspace.
    from_page->forget_current(from_p);

    if (!to_page->remember(to_p)) {
      // It is impossible for the below load barrier to require relocation. If the mutator beat
      // us to it with setting the current bit with its store barrier, then the forwarding table
      // for the initial previous value that we have in prev, is guaranteed to have been relocated
      // already by the mutator.
      // When the remembering of to-space current bits fails, it's because another mutator set the
      // bit, mistakenly assuming that this reference location had its first mutation since the
      // last youg mark start. However, we know better that it didn't, we just hadn't moved the
      // current bit over to to-space yet. So we compensate for this by incrementing the counter.
      zpointer prev = AtomicAccess::load(from_p);
      const zaddress addr = ZBarrier::load_barrier_on_oop_field_preloaded(nullptr, prev);
      ZGeneration::young()->on_failed_remember(addr);
    }
  }
}

void ZReferenceCounting::on_undo(zaddress addr, ZPage* page) {
  int ref_count = ZHeaderRefCount::count(to_oop(addr)->mark());
  if (ref_count == ZHeaderRefCount::Min || ref_count == ZHeaderRefCount::Max) {
    page->_overflow_ref_counts.update(addr, [&](int64_t* prev, int64_t** update) {
      *update = nullptr;
    });
  }
}

void ZReferenceCounting::on_root(zaddress addr) {
  // Roots are always pardoned from the current YC. A full YC without any pending
  // increments nor roots is required before having zero old-to-old pointers is
  // a sufficient condition for freeing the object.
  ZPage* page = ZHeap::heap()->page(addr);

  if (!page->is_old()) {
    return;
  }

  set_pardoned(page, addr);
}

bool ZReferenceCounting::try_kill_root(ZPage* page, zaddress addr, size_t& pardoned) {
  assert(page->is_old(), "must be old");
  assert(page->is_allocating(), "must be allocating");

  // Check if the ref count has remained at zero since the 1 -> 0 transition. It
  // might have gotten incremented since then. If so, there will be pardon bits.

  if (ZHeaderRefCount::count(to_oop(addr)->mark_acquire()) != 0) {
    // When the observed count is not zero, we should try to remove the death row
    // bit. But we have to verify after if it should be concurrently set again
    // due to racing store barriers.
    page->set_pardoned(addr);
    OrderAccess::release();
    page->unset_death_row(addr);

    // Ensure the death row clearing happens-before the ref count reload.
    OrderAccess::fence();

    // Check for concurrent decrements after clearing the death row bit, in order
    // to make sure we don't lose track of objects that are dead.
    if (ZHeaderRefCount::count(to_oop(addr)->mark_acquire()) == 0) {
      page->set_death_row(addr);
      _found_death_row.register_page(page);
    }

    return false;
  }

  // The mark_acquire() ensures checking for pardon bits is acquired across the
  // zero ref count observation. This ensures that a mutator decrementing to zero
  // will be caught as always having a pardon bit.

  if (!page->is_pardoned(addr)) {
    return true;
  }

  pardoned += ZUtils::object_size(addr);

  // When the object is pardoned, we have to set the death row back. At this point,
  // we are no longer racing with the death row setting of the mutator; it has
  // already been set.
  page->set_death_row(addr);
  _found_death_row.register_page(page);
  return false;
}

bool ZReferenceCounting::try_kill_followed(ZPage* page, zaddress addr, size_t& pardoned, int64_t observed_count) {
  assert(page->is_old(), "must be old");
  assert(page->is_allocating(), "must be allocating");

  if (observed_count != 0) {
    // Not even a candidate for freeing
    return false;
  }

  if (page->is_in_death_row(addr)) {
    return false;
  }

  // A root worker publishes its pardon before clearing the death-row bit.
  // Acquire that publication after observing the bit clear.
  OrderAccess::acquire();

  if (page->is_pardoned(addr)) {
    // Can't kill if death row owned or pardoned
    OrderAccess::release();
    // When a followed object is pardoned, there is no need to re-insert death row
    // row bits as none were cleared.
    pardoned += ZUtils::object_size(addr);
    page->set_death_row(addr);
    _found_death_row.register_page(page);
    return false;
  }

  return true;
}

class ZReferenceCounting::ZProcessDeathRowTask final : public ZRestartableTask {
  ZReferenceCounting::State* const _state;
  ZPageTable* const _page_table;
  ZPageAllocator* const _page_allocator;
  ZReferenceCounting* const _reference_counting;

public:
  ZProcessDeathRowTask(ZReferenceCounting::State* state, ZPageTable* page_table, ZPageAllocator* page_allocator, ZReferenceCounting* reference_counting)
    : ZRestartableTask("ZProcessDeathRowTask"),
      _state(state),
      _page_table(page_table),
      _page_allocator(page_allocator),
      _reference_counting(reference_counting) {
    _page_allocator->enable_safe_destroy();
    if (ZGeneration::old()->young_marks_since_old_mark_start() == 0) {
      _state->clear_bitmap();
    }
  }

  ~ZProcessDeathRowTask() {
    _page_allocator->disable_safe_destroy();
  }

  void work() final;
};

class ZCoalesceFreeListsTask final : public ZRestartableTask {
  ZReferenceCounting::State* const _state;
  ZPageTable* const _page_table;
  ZBitMap _to_coalesce;

  template <typename Function>
  void par_iterate_to_coalesce_pages(ZPageTable* page_table, Function function) {
    _to_coalesce.iterate([&](BitMap::idx_t index) {
      if (_to_coalesce.par_clear_bit(index, memory_order_relaxed)) {
        return function(page_table->at(index));
      }

      return true;
    });
  }

public:
  ZCoalesceFreeListsTask(ZReferenceCounting::State* state, ZPageTable* page_table)
    : ZRestartableTask("ZCoalesceFreeListsTask"),
      _state(state),
      _page_table(page_table),
      _to_coalesce(_state->_to_coalesce) {}

  void work() final;
};

void ZReferenceCounting::process_death_row(ZPageTable* page_table, ZPageAllocator* page_allocator) {
  // TODO: The task's page table iterators hold all deleted ZPage* via safe_delete.
  //       Maybe we should add some hazard pointer style mechanism instead so we
  //       only have to keep the ZPages we are interested in. This is also relevant
  //       for all our promotion pages during relocation and selection.

  {
    ZStatTimerYoung timer(ZSubPhaseConcurrentDeathRow);

    ZProcessDeathRowTask process_death_row_task(state(), page_table, page_allocator, this);
    ZGeneration::young()->workers()->run(&process_death_row_task);
  }

  state()->record_death_row_counters();

  {
    ZStatTimerYoung timer(ZSubPhaseConcurrentCoalesceFreeList);

    ZCoalesceFreeListsTask coalese_task(state(), page_table);
    ZGeneration::young()->workers()->run(&coalese_task);
  }

  state()->record_free_list_available_counters();
  state()->construct_free_list_promotion_allocator();
  state()->reset_per_worker_state_promotion();

  _found_death_row.verify_previous();
}

template <typename Function>
void ZReferenceCounting::par_iterate_death_row_pages(ZPageTable* page_table, Function function) {
  _found_death_row.par_iterate_death_row_pages(page_table, function);
}

void ZReferenceCounting::flip_found_death_row() {
  _found_death_row.flip();
}

void ZReferenceCounting::ZProcessDeathRowTask::work() {
  SuspendibleThreadSetJoiner sts;

  Stack<oop, mtGC> dfs_stack;
  _reference_counting->par_iterate_death_row_pages(_page_table, [&](ZPage* page) {
    ZStatTimerWorker timer(ZSubPhaseConcurrentDeathRowPage);

    ZDefer deferred_yield{[&]() {
      SuspendibleThreadSet::yield();
    }};

    precond(dfs_stack.is_empty());

    if (!page->is_old() || !page->is_allocating()) {
      return !ZGeneration::young()->should_worker_resize();
    }

    // TODO: Maybe the state should contain _found_death_row
    // TODO: Should pages be pushed here for coalscing. To avoid a page table walk.
    // TODO: Break out functionality, we have so many nested function / lambda scopes with returns.
    // TODO: Clean up interface
    auto& pardoned = _state->_per_worker_counters.get()._pardoned;
    auto& death_row_roots = _state->_per_worker_counters.get()._death_row_roots;
    auto& processed = _state->_per_worker_counters.get()._processed;
    auto& freed = _state->_per_worker_counters.get()._freed;
    auto& large_freed = _state->_per_worker_counters.get()._large_freed;

    // Acquire the death-row/pardon view of a potentially concurrently
    // flip-surviving page. This also acquires the is_allocating() check with
    // respect to the death-row and pardon bits.
    OrderAccess::acquire();

    {
      const size_t counter_index = ZPageAgeRangeOld.index(page->age());
      // Push all objects in page to be reclaimed
      page->iterate_death_row([&](zaddress addr) {
        death_row_roots[counter_index] += ZUtils::object_size(addr);
        if (_reference_counting->try_kill_root(page, addr, pardoned[counter_index])) {
          oop obj = to_oop(addr);
          dfs_stack.push(obj);
        }
      });
    }

    // Drain the stack, push all object field which should be reclaimed and reclaim the object.
    while (!dfs_stack.is_empty()) {
      oop obj = dfs_stack.pop();

      Klass* k = obj->klass();
      const bool is_reference = k->is_instance_klass() && InstanceKlass::cast(k)->reference_type() != REF_NONE;

      if (!is_reference) {
        // Decrement all old object edges, push those to be reclaimed.
        ZIterator::basic_oop_iterate_safe(obj, [&](volatile zpointer* p){
          zaddress a = ZBarrier::load_barrier_on_oop_field(p);

          if (a == zaddress::null) {
            return;
          }

          oop o = to_oop(a);

          ZPage* const obj_page = ZHeap::heap()->page(a);
          assert(obj_page->is_in(a), "why you no in?");

          if (!obj_page->is_old()) {
            return;
          }

          int64_t ref_count = _reference_counting->decrement(a, obj_page);
          assert(ref_count > 0, "should be positive: " INT64_FORMAT, ref_count);
          const size_t counter_index = ZPageAgeRangeOld.index(obj_page->age());
          // If we decrement an edge to zero, we traverse through more garbage.
          if (obj_page->is_allocating() &&
              _reference_counting->try_kill_followed(obj_page, a, pardoned[counter_index], ref_count - 1)) {
            dfs_stack.push(o);
          }
        });
      }

      // Reclaim the object
      zaddress addr = to_zaddress(obj);
      int ref_count = ZHeaderRefCount::count(obj->mark());
      assert(ref_count == 0, "must be zero: %d: %p", ref_count, cast_from_oop<void*>(obj));

      ZPage* const obj_page = ZHeap::heap()->page(addr);
      const size_t counter_index = ZPageAgeRangeOld.index(obj_page->age());

      // Unlink current remset entries before zapping the object.
      const uintptr_t from_local_offset = obj_page->local_offset(addr);
      const size_t size = ZUtils::object_size(addr);
      BitMap::Iterator current_iter = obj_page->remset_iterator_limited_current(from_local_offset, size);

      for (BitMap::idx_t field_bit : current_iter) {
        const uintptr_t field_local_offset = ZRememberedSet::to_offset(field_bit);
        const uintptr_t field_offset = field_local_offset - from_local_offset;
        const zaddress field_addr = addr + field_offset;
        volatile zpointer* const p = (volatile zpointer*)field_addr;

        obj_page->forget_current(p);
      }

      processed[counter_index] += size;
      if (obj_page->is_large()) {
        large_freed[counter_index] += size;
        // TODO: Only really relevant for Old generation stats during an OC.
        //       Not sure how we feel about this outside the OC.
        ZGeneration::old()->increase_freed(obj_page->size());
        ZHeap::heap()->free_page(obj_page);
      } else {
        freed[counter_index] += size;
        if (obj_page->is_promoted()) {
          obj_page->free_object_to_free_list(addr);
        } else if(ZMaintainOldFreeLists) {
          // TODO: For now we just add these to the undo list so we can use them
          //       after the old relocation is over. However we might be able to
          //       insert these blocks if we are during old relocation to make
          //       them also availiable for old-to-old relocations, there is no
          //       ABA, the only thing is that we might be to late and the old
          //       allocator has already discarded this page as not eligable.
          //       However if we do this we need to fix our accoutning which does
          //       not expect the freelist availiable to grow.
          obj_page->undo_alloc_object_from_free_list(unsafe(addr), size);
        }
      }

      obj_page->unset_death_row(addr);
    }

    return !ZGeneration::young()->should_worker_resize();
  });
}

void ZCoalesceFreeListsTask::work() {
  SuspendibleThreadSetJoiner sts;

  auto& free_list_available = _state->_per_worker_counters.get()._free_list_available;
  auto& allocating = _state->_free_list_allocators[ZPageAgeRangeOld.index(ZPageAge::promotion)]._per_worker_allocating.get();

  // Clear all the pardoned bits to prepare for next GC cycle.
  par_iterate_to_coalesce_pages(_page_table, [&](ZPage* page) {
    ZStatTimerWorker timer(ZSubPhaseConcurrentCoalesceFreeListPage);
    precond(page->is_old());
    precond(page->is_allocating());
    precond(!page->is_large());

    const size_t free_size = page->coalesce_free_list();
    if (free_size != 0) {
      free_list_available[untype(page->type())] += free_size;
      const uint32_t numa_id = page->is_multi_partition() ? 0 : page->single_partition_id();
      allocating.get(numa_id)._allocating_pages[static_cast<int>(page->type())].push(page);
    }

    // Yield once per page
    SuspendibleThreadSet::yield();
    return !ZGeneration::young()->should_worker_resize();
  });
}

// TODO: Deal better with large arrays

ZReferenceCounting::FreeListAllocation ZReferenceCounting::free_list_alloc_object(size_t size, ZPageType type, ZPageAge to_age) {
  precond(type != ZPageType::large);
  if (type == ZPageType::large) {
    return {};
  }

  const uint32_t start_cpu_id = ZCPU::id();
  const uint32_t num_cpu_id = ZCPU::count();

  for (uint32_t i = 0; i < num_cpu_id; ++i) {
    const uint32_t cpu_id = (start_cpu_id + i) % num_cpu_id;

    ZReferenceCounting::FreeListAllocation allocation;
    switch (type) {
    case ZPageType::small: {
      if (state()->small_allocation_pages(cpu_id, to_age).free_list_alloc_object(size, &allocation)) {
        return allocation;
      }
      break;
    }
    case ZPageType::medium: {
      if (state()->medium_allocation_pages(cpu_id, to_age).free_list_alloc_object(size, &allocation)) {
        return allocation;
      }
      break;
    }
    default:
      ShouldNotReachHere();
    }
  }

  return {};
}

void ZReferenceCounting::on_free_list_insert(const ZPage* page) {
  if (page->age() == ZPageAge::promotion) {
    _state->register_free_page(page);
  }
}

void ZReferenceCounting::register_old_alloction_page(ZPage* page) {
  precond(!page->is_large());
  precond(page->age() == ZPageAge::old);
  precond(ZMaintainOldFreeLists);
  precond(ZAllocateInOldFreeList);

  const int type_i = page->is_small() ? 0 : 1;
  const uint32_t numa_id = page->is_multi_partition() ? 0 : page->single_partition_id();

  // Insert page
  state()->_free_list_allocators[ZPageAgeRangeOld.index(ZPageAge::old)]._per_worker_allocating.get().get(numa_id)._allocating_pages[type_i].push(page);
}

void ZReferenceCounting::construct_old_allocator() {
  precond(ZMaintainOldFreeLists);
  precond(ZAllocateInOldFreeList);
  state()->construct_free_list_old_allocator();
}

void ZReferenceCounting::reset_old_allocator() {
  precond(ZMaintainOldFreeLists);
  precond(ZAllocateInOldFreeList);
  state()->reset_old_allocator();
}
