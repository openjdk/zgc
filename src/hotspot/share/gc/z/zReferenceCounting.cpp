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
#include "gc/z/zCPU.inline.hpp"
#include "gc/z/zGeneration.inline.hpp"
#include "gc/z/zHeap.inline.hpp"
#include "gc/z/zLock.inline.hpp"
#include "gc/z/zReferenceCounting.hpp"
#include "gc/z/zNUMA.hpp"
#include "gc/z/zNUMA.inline.hpp"
#include "gc/z/zPageType.hpp"
#include "gc/z/zTask.hpp"
#include "gc/z/zValue.inline.hpp"
#include "logging/log.hpp"
#include "memory/allocation.hpp"
#include "nmt/memTag.hpp"
#include "oops/access.inline.hpp"
#include "runtime/atomicAccess.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/integerCast.hpp"
#include "utilities/powerOfTwo.hpp"
#include "utilities/stack.inline.hpp"

// We use "crow reference counting". Crows can count 1, 2, 3, many. In other
// words, it can't really distinguish between 4 and 5. For us, the counts get
// blurred at 7, because we use 4 bits *signed* reference count. The signedness
// is required due to races between mutators and the GC, causing periodic
// instability in the reference counters. So reference count of over 7 means we
// don't really know. Once this number is reached, we never change it, and the
// object simply can not be reclaimed with reference counting. The reference
// counts are embedded in the 4 bit age bits of the markWord of objects.
struct ZRefCount : public AllStatic {
  static constexpr int Min = -7;
  static constexpr int Max = 7;
  static constexpr int Uncertain = -8;

  static int count_impl(markWord mark) {
    int age = mark.age();
    int ref_count = age;

    if (age & 0b1000) {
      // Sign extend the integer
      ref_count |= 0xFFFFFFF0;
    }

    return ref_count;
  }

  static markWord set_count_impl(markWord mark, int ref_count) {
    int age = ref_count & 0xF;

    return mark.set_age(age);
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

  ZPerCPU<CPUAllocPages<ZPageType::small>> _small_allocation_pages{ZValueIdTagType{}};
  ZPerCPU<CPUAllocPages<ZPageType::medium>> _medium_allocation_pages{ZValueIdTagType{}};

  CPUAllocPages<ZPageType::small>& small_allocation_pages(uint32_t cpu_id) {
    return _small_allocation_pages.get(cpu_id);
  }

  CPUAllocPages<ZPageType::medium>& medium_allocation_pages(uint32_t cpu_id) {
    return _medium_allocation_pages.get(cpu_id);
  }

  // Death Row Counters Support
  struct Counters {
    size_t _freed = 0;
    size_t _pardoned = 0;
    size_t _free_list_availiable = 0;
  };

  ZPerWorker<Counters> _per_worker_counters;

  void log_free_and_pardoned_counters() {
    size_t freed = 0;
    size_t pardoned = 0;

    ZPerWorkerIterator<State::Counters> counters_iter{&_per_worker_counters};
    for (State::Counters* counters; counters_iter.next(&counters);) {
      freed += counters->_freed;
      pardoned += counters->_pardoned;
    }

    log_info(gc)("Old Generation Reclaimed: " PROPERFMT, PROPERFMTARGS(freed));
    log_info(gc)("Old Generation Pardoned: " PROPERFMT, PROPERFMTARGS(pardoned));
  }

  void record_and_log_free_list_availiable_counters() {
    size_t free_list_availiable = 0;

    ZPerWorkerIterator<State::Counters> counters_iter{&_per_worker_counters};
    for (State::Counters* counters; counters_iter.next(&counters);) {
      free_list_availiable += counters->_free_list_availiable;
    }

    log_info(gc, freelist)("Old Generation Free-List Availiable: " PROPERFMT, PROPERFMTARGS(free_list_availiable));
    ZGeneration::young()->set_freelist_availiable(free_list_availiable);
  }

  // Free-list Construction Support
  struct AllocPair {
    ZPage* _page = nullptr;
    size_t _free = 0u;
  };

  struct PerNUMAData {
    ZArray<AllocPair> _allocating_pages[2]{};
  };

  ZPerWorker<ZPerNUMA<PerNUMAData>> _per_worker_allocating;

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

    // Create merged storage
    ZPerNUMA<ZArray<AllocPair>> per_numa_pages[2];
    for (int numa_id = 0; numa_id < numa_count; numa_id++) {
      for (int i = 0; i < 2; i++) {
        per_numa_pages[i].get(integer_cast<uint32_t>(numa_id)).reserve(per_numa_page_count[i].at(numa_id));
      }
    }

    { // Copy per worker
      ZPerWorkerIterator<ZPerNUMA<PerNUMAData>> worker_iter(&_per_worker_allocating);
      for (ZPerNUMA<PerNUMAData>* worker_data; worker_iter.next(&worker_data);) {
        uint32_t numa_id;
        ZPerNUMAIterator<PerNUMAData> numa_iter(worker_data);
        for (PerNUMAData* numa_data; numa_iter.next(&numa_data, &numa_id);) {
          for (int i = 0; i < 2; i++) {
            per_numa_pages[i].get(numa_id).appendAll(&numa_data->_allocating_pages[i]);
          }
        }
      }
    }

    for (int numa_id = 0; numa_id < numa_count; numa_id++) {
      // Sort page arrays
      for (int i = 0; i < 2; i++) {
        // Sort [small -> larger]
        per_numa_pages[i].get(integer_cast<uint32_t>(numa_id)).sort([](AllocPair* e1, AllocPair* e2) {
          return e1->_free > e2->_free ? 1 : -1;
        });
      }

      // Insert all small pages
      ZArray<AllocPair>& small_pages = per_numa_pages[0].get(integer_cast<uint32_t>(numa_id));
      while (small_pages.is_nonempty()) {
        const int pre_length = small_pages.length();

        ZPerCPUIterator<CPUAllocPages<ZPageType::small>> small_iter(&_small_allocation_pages);
        for (CPUAllocPages<ZPageType::small>* alloc_pages; small_pages.is_nonempty() && small_iter.next(&alloc_pages);) {
          if (integer_cast<uint32_t>(numa_id) != alloc_pages->_numa_id) {
            // Wrong NUMA id
            continue;
          }
          alloc_pages->push(small_pages.pop()._page);
        }

        if (pre_length == small_pages.length()) {
          assert(false, "Something weird happened");
          _small_allocation_pages.addr(0)->_alloc_pages.push(small_pages.pop()._page);
        }
      }

      // Insert all medium pages
      ZArray<AllocPair>& medium_pages = per_numa_pages[1].get(integer_cast<uint32_t>(numa_id));
      while (medium_pages.is_nonempty()) {
        const int pre_length = medium_pages.length();

        ZPerCPUIterator<CPUAllocPages<ZPageType::medium>> medium_iter(&_medium_allocation_pages);
        for (CPUAllocPages<ZPageType::medium>* alloc_pages; medium_pages.is_nonempty() && medium_iter.next(&alloc_pages);) {
          if (integer_cast<uint32_t>(numa_id) != alloc_pages->_numa_id) {
            // Wrong NUMA id
            continue;
          }
          alloc_pages->push(medium_pages.pop()._page);
        }

        if (pre_length == medium_pages.length()) {
          assert(false, "Something weird happened");
          _medium_allocation_pages.addr(0)->_alloc_pages.push(medium_pages.pop()._page);
        }
      }

      // Clear and dealocate
      small_pages.clear_and_deallocate();
      medium_pages.clear_and_deallocate();
    }
  }

  void reset_per_worker_state() {
    // Clear counters
    _per_worker_counters.set_all({});


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
};

ZReferenceCounting::State* ZReferenceCounting::state() {
  precond(ZOldRefCount);
  return _state;
}

const ZReferenceCounting::State* ZReferenceCounting::state() const {
  precond(ZOldRefCount);
  return _state;
}

void ZReferenceCounting::increment(zaddress addr) {
  oop obj = to_oop(addr);
  ZPage* page = ZHeap::heap()->page(addr);

  for (;;) {
    markWord mark = obj->mark();
    int ref_count = ZRefCount::count(mark);

    if (ref_count == ZRefCount::Uncertain) {
      return;
    }

    int new_ref_count = ref_count == ZRefCount::Max ? ZRefCount::Uncertain : (ref_count + 1);
    markWord new_mark = ZRefCount::set_count(mark, new_ref_count);

    if (obj->cas_set_mark(new_mark, mark, memory_order_relaxed) == mark) {
      if (page->is_allocating()) {
        // Increments imply that the last GC cycle still had a reference to the object.
        // That means it could have escaped into roots before or after root scanning.
        // So we have to conservatively pardon these objects from the death row.
        page->set_pardoned(addr);
      }

      return;
    }
  }
}

void ZReferenceCounting::decrement(zaddress addr) {
  oop obj = to_oop(addr);
  ZPage* page = ZHeap::heap()->page(addr);

  for (;;) {
    markWord mark = obj->mark();
    int ref_count = ZRefCount::count(mark);

    if (ref_count == ZRefCount::Uncertain) {
      // We can not reliably decrement from the uncertain count using
      // crow reference counting. Leave it in place.
      return;
    }

    int new_ref_count = ref_count == ZRefCount::Min ? ZRefCount::Uncertain : (ref_count - 1);
    markWord new_mark = ZRefCount::set_count(mark, new_ref_count);

    if (obj->cas_set_mark(new_mark, mark, memory_order_relaxed) == mark) {
      return;
    }
  }
}

ZReferenceCounting::ZReferenceCounting()
  : _state(ZOldRefCount ? new State() : nullptr) {}

void ZReferenceCounting::on_remember(volatile zpointer* p, zaddress addr, bool remembered) {
  if (is_null(addr)) {
    // Only count old-to-old edges.
    return;
  }

  ZPage* p_page = ZHeap::heap()->page(p);
  ZPage* addr_page = ZHeap::heap()->page(addr);

  if (!addr_page->is_old()) {
    return;
  }

  // The fact that the mutator had a path to access the previous value means that
  // it could have been loaded concurrently and become a root, after root processing
  // has finished. Therefore, we must pardon the object from any death row processing.
  if (addr_page->is_allocating()) {
    addr_page->set_pardoned(addr);
  }

  if (!p_page->is_old()) {
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
                                   ZRefCount::count(to_oop(addr)->mark()) == 0;

  const bool suppressed_promoting_edge = p_flip_promoted || addr_flip_promoted || addr_reloc_promoted;

  const bool forgotten = ZGeneration::young()->forget_previous(p);

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
    if (addr_page->is_allocating()) {
      addr_page->set_death_row(addr);
    }
    OrderAccess::release();
    decrement(addr);
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

  increment(addr);
}

void ZReferenceCounting::on_forget(volatile zpointer* p, zaddress addr) {
  // When we have an old-to-old pointer that is about to become forgotten,
  // it means that it was written to by a mutator in the last marking epoch.
  // Therefore, we have to account for the last increment of the last cycle.
  increment(addr);
}

void ZReferenceCounting::on_promotion(zaddress addr) {
  assert(ZHeap::heap()->is_old(addr), "must be old");
  assert(ZHeap::heap()->page(addr)->is_allocating(), "must be allocating");

  ZPage* page = ZHeap::heap()->page(addr);
  page->set_death_row(addr);
}

void ZReferenceCounting::on_old_to_space_alloc(ZPage* to_page, zaddress to_addr, bool mutator) { // TODO: Completeness for pardoning
  to_page->set_pardoned(to_addr);

  if (mutator) {
    // Maintain one stake in the mutator old-to-old relocation until the GC gets to
    // process the to-space object and add it to the right pardon/deathrow sets. It
    // will then decrement the counter.
    markWord mark = to_oop(to_addr)->mark();
    int ref_count = ZRefCount::count(mark);

    if (ref_count == ZRefCount::Uncertain) {
      return;
    }

    int new_ref_count = ref_count == ZRefCount::Max ? ZRefCount::Uncertain : ref_count + 1;
    markWord new_mark = ZRefCount::set_count(mark, new_ref_count);

    to_oop(to_addr)->set_mark(new_mark);
  }
}

// TODO: More helpful arguments
void ZReferenceCounting::on_old_to_old(zaddress addr, bool was_mutator) {
  assert(ZHeap::heap()->is_old(addr), "must be old");
  assert(ZHeap::heap()->page(addr)->is_allocating(), "must be allocating");

  ZPage* page = ZHeap::heap()->page(addr);

  if (was_mutator) {
    decrement(addr);
  }
}

void ZReferenceCounting::on_mutator_old_to_old(ZForwarding* forwarding, zaddress from_addr, zaddress to_addr) {
  const uint32_t young_marks = ZGeneration::old()->young_marks_since_old_reloc_start();
  const bool before_young_mark = young_marks == 0;

  if (!before_young_mark) {
    // TODO: Comments
    return;
  }

  ZPage* const from_page = forwarding->page();
  const uintptr_t from_local_offset = from_page->local_offset(from_addr);

  // Note: even with in-place relocation, the to_page could be another page
  ZPage* const to_page = ZHeap::heap()->page(to_addr);

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

void ZReferenceCounting::on_root(zaddress addr) {
  // Roots are always pardoned from the current YC. A full YC without any pending
  // increments nor roots is required before having zero old-to-old pointers is
  // a sufficient condition for freeing the object.
  ZPage* page = ZHeap::heap()->page(addr);

  if (!page->is_old()) {
    return;
  }

  if (!page->is_allocating()) {
    return;
  }

  page->set_pardoned(addr);
}

static bool can_kill(zaddress addr) {
  return ZRefCount::count(to_oop(addr)->mark()) == 0;
}

static bool try_kill(ZPage* page, zaddress addr, size_t& pardoned) {
  assert(page->is_old(), "must be old");
  assert(page->is_allocating(), "must be allocating");

  // Unset might race with pardoned setting in on_remembered.
  page->unset_death_row(addr);

  if (!can_kill(addr)) {
    return false;
  }

  // Acquire the pardon after the ref count read
  OrderAccess::acquire();

  if (!page->is_pardoned(addr)) {
    return true;
  }

  pardoned += ZUtils::object_size(addr);

  // When the object is pardoned, we have to set the death row back. At this point,
  // we are no longer racing with the death row setting of the mutator; it has
  // already been set.
  page->set_death_row(addr);
  return false;
}

class ZProcessDeathRowTask final : public ZTask {
  ZReferenceCounting::State* _state;
  ZGenerationPagesParallelIterator _pt_iter;

public:
  ZProcessDeathRowTask(ZReferenceCounting::State* state, ZPageTable* page_table, ZPageAllocator* page_allocator)
    : ZTask("ZProcessDeathRowTask"),
      _state(state),
      _pt_iter(page_table, ZGenerationId::old, page_allocator) {}

  void work() final;
};
class ZClearPardonAndCoalesceFreeListsTask final : public ZTask {
  ZReferenceCounting::State* _state;
  ZGenerationPagesParallelIterator _pt_iter;

public:
  ZClearPardonAndCoalesceFreeListsTask(ZReferenceCounting::State* state, ZPageTable* page_table, ZPageAllocator* page_allocator)
    : ZTask("ZClearPardonAndCoalesceFreeListsTask"),
      _state(state),
      _pt_iter(page_table, ZGenerationId::old, page_allocator) {}

  void work() final;
};

void ZReferenceCounting::process_death_row(ZPageTable* page_table, ZPageAllocator* page_allocator) {
  SuspendibleThreadSetJoiner sts;

  ZProcessDeathRowTask process_death_row_task(state(), page_table, page_allocator);
  ZGeneration::young()->workers()->run(&process_death_row_task);

  state()->log_free_and_pardoned_counters();

  ZClearPardonAndCoalesceFreeListsTask clear_and_coalese_task(state(), page_table, page_allocator);
  ZGeneration::young()->workers()->run(&clear_and_coalese_task);

  state()->construct_free_list_allocator();
  state()->reset_per_worker_state();
}


void ZProcessDeathRowTask::work() {
  Stack<oop, mtGC> dfs_stack;

  _pt_iter.do_pages([&](ZPage* page) {
    precond(dfs_stack.is_empty());

    if (!page->is_allocating()) {
      // TODO: Construct iterator bitmap for faster iteration instead of filtering
      return true;
    }

    // TODO: Break out functionality, we have so many nested function / lambda scopes with returns.
    // TODO: Clean up interface
    auto& pardoned = _state->_per_worker_counters.get()._pardoned;
    auto& freed = _state->_per_worker_counters.get()._freed;

    // Acquire the death-row/pardon view of a potentially concurrently
    // flip-surviving page. This also acquires the is_allocating() check with
    // respect to the death-row and pardon bits.
    OrderAccess::acquire();

    // Push all objects in page to be reclaimed
    page->iterate_death_row([&](zaddress addr) {
      if (try_kill(page, addr, pardoned)) {
        oop obj = to_oop(addr);
        dfs_stack.push(obj);
      }
    });

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

          ZPage* const field_page = ZHeap::heap()->page(a);
          assert(field_page->is_in(a), "why you no in?");

          if (!field_page->is_old()) {
            return;
          }

          for (;;) {
            markWord mark = o->mark();
            int ref_count = ZRefCount::count(mark);

            if (ref_count == ZRefCount::Uncertain) {
              break;
            }

            assert(ref_count > 0, "should be positive: %d", ref_count);

            markWord new_mark = ZRefCount::set_count(mark, ref_count - 1);

            if (o->cas_set_mark(new_mark, mark, memory_order_relaxed) == mark) {
              // If we decrement an edge to zero, we traverse through more garbage.
              if (field_page->is_allocating() && try_kill(field_page, a, pardoned)) {
                dfs_stack.push(o);
              }
              break;
            }
          }
        });
      }

      // Reclaim the object
      zaddress addr = to_zaddress(obj);
      int ref_count = ZRefCount::count(obj->mark());
      assert(ref_count == 0, "must be zero: %d: %p", ref_count, cast_from_oop<void*>(obj));

      ZPage* const obj_page = ZHeap::heap()->page(addr);

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

      freed += ZUtils::object_size(addr);
      if (obj_page->is_large()) {
        ZHeap::heap()->free_page(obj_page);
      } else {
        obj_page->free_object_to_free_list(addr);
      }
    }

    return true;
  });
}

void ZClearPardonAndCoalesceFreeListsTask::work() {
  auto& free_list_availiable = _state->_per_worker_counters.get()._free_list_availiable;
  auto& allocating = _state->_per_worker_allocating.get();

  // Clear all the pardoned bits to prepare for next GC cycle.
  _pt_iter.do_pages([&](ZPage* page) {
    if (!page->is_allocating()) {
      // TODO: Construct iterator bitmap for faster iteration instead of filtering
      return true;
    }

    page->iterate_pardoned([&](zaddress addr) {
      page->unset_pardoned(addr);
    });

    if (!page->is_large()) {
      const size_t free_size = page->coalesce_free_list();
      if (free_size != 0) {
        free_list_availiable += free_size;
        const uint32_t numa_id = page->is_multi_partition() ? 0 : page->single_partition_id();
        allocating.get(numa_id)._allocating_pages[static_cast<int>(page->type())].push({ page, free_size });
      }
    }
    return true;
  });
}

// TODO: Deal better with large arrays
// TODO: Figure out when we can SuspendibleThreadSet::yield()

ZReferenceCounting::FreeListAllocation ZReferenceCounting::free_list_alloc_object(size_t size, ZPageType type) {
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
      if (state()->small_allocation_pages(cpu_id).free_list_alloc_object(size, &allocation)) {
        return allocation;
      }
      break;
    }
    case ZPageType::medium: {
      if (state()->medium_allocation_pages(cpu_id).free_list_alloc_object(size, &allocation)) {
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
