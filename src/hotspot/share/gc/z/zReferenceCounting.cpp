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

#include "gc/shared/suspendibleThreadSet.hpp"
#include "gc/z/zAddress.hpp"
#include "gc/z/zAddress.inline.hpp"
#include "gc/z/zGeneration.hpp"
#include "gc/z/zGeneration.inline.hpp"
#include "gc/z/zHeap.hpp"
#include "gc/z/zHeap.inline.hpp"
#include "gc/z/zLock.inline.hpp"
#include "gc/z/zReferenceCounting.hpp"
#include "gc/z/zPageType.hpp"
#include "oops/access.inline.hpp"
#include "runtime/atomicAccess.hpp"
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

        if (new_ref_count == 1) {
          // When transitioning from 0 to 1, we no longer need to be remembered.
          page->unset_death_row(addr);
        }
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
      if (page->is_allocating()) {
        // Maintain death rows
        if (new_ref_count == ZRefCount::Uncertain) {
          page->unset_death_row(addr);
        } else if (new_ref_count == 0) {
          // When transitioning from 1 to 0, we need to remember the object so it
          // can be freed later. However, it can not be doned this GC cycle; we
          // have to wait until the next GC cycle. So we pardon the object from
          // death row for this GC cycle.
          page->set_death_row(addr);
        }
      }
      return;
    }
  }
}

void ZReferenceCounting::on_remember(volatile zpointer* p, zaddress addr) {
  bool forgotten = ZGeneration::young()->forget_previous(p);

  if (is_null(addr)) {
    // Only count old-to-old edges.
    return;
  }

  ZPage* p_page = ZHeap::heap()->page(p);

  if (!p_page->is_old()) {
    return;
  }

  ZPage* addr_page = ZHeap::heap()->page(addr);

  if (!addr_page->is_old() || addr_page->is_flip_promoted()) {
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

  if (addr_page->is_allocating()) {
    addr_page->set_pardoned(addr);
    OrderAccess::release();
  }

  if (forgotten) {
    // TODO: Could return here instead. Now I explicitly both call increment and
    // decrement, purely to handle the death rows right.
    increment(addr);
  }

  decrement(addr);
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

void ZReferenceCounting::on_old_to_space_alloc(ZPage* to_page, zaddress from_addr, zaddress to_addr, bool mutator) { // TODO: Completeness for pardoning
  to_page->set_pardoned(to_addr);

  if (mutator) {
    // TODO: No need for atomics
    // Maintain one stake in the mutator old-to-old relocation until the GC gets to
    // process the to-space object and add it to the right pardon/deathrow sets. It
    // will then decrement the counter.
    for (;;) {
      markWord mark = to_oop(from_addr)->mark();
      int ref_count = ZRefCount::count(mark);

      if (ref_count == ZRefCount::Uncertain) {
        break;
      }

      int new_ref_count = ref_count == ZRefCount::Max ? ZRefCount::Uncertain : ref_count + 1;
      markWord new_mark = ZRefCount::set_count(mark, new_ref_count);

      if (to_oop(to_addr)->cas_set_mark(new_mark, mark, memory_order_relaxed) == mark) {
        break;
      }
    }
  }
}

// TODO: More helpful arguments
void ZReferenceCounting::on_old_to_old(zaddress addr, bool was_mutator) {
  assert(ZHeap::heap()->is_old(addr), "must be old");
  assert(ZHeap::heap()->page(addr)->is_allocating(), "must be allocating");

  ZPage* page = ZHeap::heap()->page(addr);
  page->set_pardoned(addr);
  OrderAccess::release(); // TODO: Embed in death row set?

  // TODO: WARNING Ordering issue
  if (ZRefCount::count(to_oop(addr)->mark()) == 0) {
    page->set_death_row(addr);
  }

  if (was_mutator) {
    decrement(addr); // TODO: Dig up the street
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

// TODO: Make parallel
// TODO: Deal better with large arrays
// TODO: Figure out when we can SuspendibleThreadSet::yield()
void ZReferenceCounting::process_death_row(ZPageTable* page_table, ZPageAllocator* page_allocator) {
  if (!ZOldRefCount) {
    return;
  }

  Stack<oop, mtGC> dfs_stack;
  SuspendibleThreadSetJoiner sts;
  int allocating_page_count[2] = {};

  size_t freed = 0;
  size_t pardoned = 0;

  ZGenerationPagesIterator pt_iter(page_table, ZGenerationId::old, page_allocator);
  for (ZPage* page; pt_iter.next(&page);) {
    if (!page->is_allocating()) {
      // TODO: Construct iterator bitmap for faster iteration instead of filtering
      continue;
    }

    const bool page_is_large = page->is_large();

    page->iterate_death_row([&](zaddress addr) {
      oop obj = to_oop(addr);
      int ref_count = ZRefCount::count(obj->mark());

      // TODO too strong assert? assert(ref_count == 0, "must be zero: %d: %p", ref_count, (void*)addr);

      if (ref_count != 0) {
        return;
      }

      OrderAccess::acquire(); // TODO: put acquire into iterator?
                              // TODO: It's the ref count we care about though? Hmm.

      if (page->is_pardoned(addr)) {
        pardoned += ZUtils::object_size(addr);
        return;
      }

      dfs_stack.push(obj);
    });

    while (!dfs_stack.is_empty()) {
      oop obj = dfs_stack.pop();

      Klass* k = obj->klass();
      const bool is_reference = k->is_instance_klass() && InstanceKlass::cast(k)->reference_type() != REF_NONE;

      if (!is_reference) {
        ZIterator::basic_oop_iterate_safe(obj, [&](volatile zpointer* p){
          zaddress a = ZBarrier::load_barrier_on_oop_field(p);

          if (a == zaddress::null) {
            return;
          }

          oop o = to_oop(a);

          ZPage* page = ZHeap::heap()->page(a);
          assert(page->is_in(a), "why you no in?");

          if (!page->is_old()) {
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
              if (page->is_allocating() && ref_count == 1) {
                OrderAccess::acquire(); // Acquire between observing ref count 0 and reading pardoned
                if (page->is_pardoned(a)) {
                  pardoned += ZUtils::object_size(a);
                  page->set_death_row(a);
                } else {
                  dfs_stack.push(o);
                }
              }
              break;
            }
          }
        });
      }

      zaddress addr = to_zaddress(obj);
      int ref_count = ZRefCount::count(obj->mark());
      assert(ref_count == 0, "must be zero: %d: %p", ref_count, cast_from_oop<void*>(obj));

      ZPage* page = ZHeap::heap()->page(addr);

      // Unlink current remset entries before zapping the object.
      const uintptr_t from_local_offset = page->local_offset(addr);
      const size_t size = ZUtils::object_size(addr);
      BitMap::Iterator current_iter = page->remset_iterator_limited_current(from_local_offset, size);

      for (BitMap::idx_t field_bit : current_iter) {
        const uintptr_t field_local_offset = ZRememberedSet::to_offset(field_bit);
        const uintptr_t field_offset = field_local_offset - from_local_offset;
        const zaddress field_addr = addr + field_offset;
        volatile zpointer* const p = (volatile zpointer*)field_addr;

        page->forget_current(p);
      }

      page->unset_death_row(addr);

      freed += ZUtils::object_size(addr);
      if (page_is_large) {
        ZHeap::heap()->free_page(page);
      } else {
        page->free_object_to_free_list(addr);
      }
    }

    if (!page_is_large) {
     // TODO: Cleanup
     allocating_page_count[static_cast<int>(page->type())]++;
    }
  }

  log_info(gc)("Old Generation Reclaimed: %zu", freed);
  log_info(gc)("Old Generation Pardoned: %zu", pardoned);

  for (int i = 0; i < 2; i++) {
    _allocating[i].clear();
    _allocating[i].reserve(allocating_page_count[i]);
    _next_page_index[i].store_relaxed(0);
  }

  // Clear all the pardoned bits to prepare for next GC cycle.
  ZGenerationPagesIterator pt_iter2(page_table, ZGenerationId::old, page_allocator);
  size_t free_list_availiable = 0;
  for (ZPage* page; pt_iter2.next(&page);) {
    if (!page->is_allocating()) {
      // TODO: Construct iterator bitmap for faster iteration instead of filtering
      continue;
    }

    page->iterate_pardoned([&](zaddress addr) {
      page->unset_pardoned(addr);
    });

    if (!page->is_large()) {
      const size_t free_size = page->coalesce_free_list();
      if (free_size != 0) {
        free_list_availiable += free_size;
        _allocating[static_cast<int>(page->type())].push({ page, free_size });
      }
    }
  }

  for (int i = 0; i < 2; i++) {
    _allocating[i].sort([](AllocPair* e1, AllocPair* e2) {
      return e1->_free > e2->_free ? -1 : 1;
    });
  }

  log_info(gc, freelist)("Old Generation Free-List Availiable: %zu", free_list_availiable);
  ZGeneration::young()->set_freelist_availiable(free_list_availiable);
}

ZReferenceCounting::FreeListAllocation ZReferenceCounting::free_list_alloc_object(size_t size, ZPageType type) {
  precond(type != ZPageType::large);
  // TODO: Bound this? What is the trade-off with forcing bump-pointer alloc
  const int page_type_index = static_cast<int>(type);
  for (;;) {
    const int page_index = _next_page_index[page_type_index].load_relaxed();

    if (page_index == _allocating[page_type_index].length()) {
      return {};
    }
    ZPage* const page = _allocating[page_type_index].at(page_index)._page;
    const zaddress addr = page->alloc_object_from_free_list(size);
    if (addr != zaddress::null) {
      return {page, addr};
    }

    _next_page_index[page_type_index].compare_set(page_index, page_index + 1, memory_order_relaxed);
  }
}

void ZReferenceCounting::print_free_lists_on(outputStream* st) const {
  for (int type = 0; type < 2; type++) {
    for (const AllocPair& pair : _allocating[type]) {
      pair._page->print_free_list_on(st);
    }
  }
}
