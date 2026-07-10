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
#include "gc/z/zAddress.inline.hpp"
#include "gc/z/zGeneration.inline.hpp"
#include "gc/z/zHeap.inline.hpp"
#include "gc/z/zLock.inline.hpp"
#include "gc/z/zReferenceCounting.hpp"
#include "oops/access.inline.hpp"
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

    if (page->is_allocating()) {
      // Increments imply that the last GC cycle still had a reference to the object.
      // That means it could have escaped into roots before or after root scanning.
      // So we have to conservatively pardon these objects from the death row.
      page->set_pardoned(addr);
    }

    int new_ref_count = ref_count == ZRefCount::Max ? ZRefCount::Uncertain : (ref_count + 1);
    markWord new_mark = ZRefCount::set_count(mark, new_ref_count);

    if (obj->cas_set_mark(new_mark, mark, memory_order_relaxed) == mark) {
      if (new_ref_count == 1 && page->is_allocating()) {
        // When transitioning from 0 to 1, we no longer need to be remembered.
        page->unset_death_row(addr);
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

    if (ref_count == 0 && !ZGeneration::young()->is_phase_mark()) {
      // Only during young generation concurrent marking can an old-to-old
      // reference count become negative, due to increments from the last
      // phase not being processed yet.
      // During promotion to the old generation, we do not want the decrements
      // to do anything, as we are adding edges to the current remembered set,
      // so that the entire edge snapshot may be processed during the next
      // young generation marking phase.
      return;
    }

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
          page->set_pardoned(addr);
        }
      }
      return;
    }
  }
}

void ZReferenceCounting::on_remember(volatile zpointer* p, zaddress addr) {
  bool forgotten = ZGeneration::young()->forget_previous(p);

  if (is_null(addr) || ZHeap::heap()->is_young(addr)) {
    // Only count old-to-old edges.
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

  if (forgotten) {
    // TODO: Could return here instead. Now I explicitly both call increment and
    // decrement, purely to handle the death rows right.
    increment(addr);
  }

  decrement(addr);
  log_info(gc)("[re %d %lx]", ZRefCount::count(to_oop(addr)->mark()), untype(addr));
}

void ZReferenceCounting::on_forget(volatile zpointer* p, zaddress addr) {
  // When we have an old-to-old pointer that is about to become forgotten,
  // it means that it was written to by a mutator in the last marking epoch.
  // Therefore, we have to account for the last increment of the last cycle.
  increment(addr);
  log_info(gc)("[fo %d %lx]", ZRefCount::count(to_oop(addr)->mark()), untype(addr));
}

void ZReferenceCounting::on_promotion(zaddress addr) {
  assert(ZHeap::heap()->is_old(addr), "must be old");
  assert(ZHeap::heap()->page(addr)->is_allocating(), "must be allocating");
  assert(ZRefCount::count(to_oop(addr)->mark()) == 0,
         "invalid promotion ref count: %d", ZRefCount::count(to_oop(addr)->mark()));

  ZPage* page = ZHeap::heap()->page(addr);
  page->set_death_row(addr);
  log_info(gc)("[pr %d %lx]", ZRefCount::count(to_oop(addr)->mark()), untype(addr));
}

void ZReferenceCounting::on_old_to_old(zaddress addr) {
  assert(ZHeap::heap()->is_old(addr), "must be old");
  assert(ZHeap::heap()->page(addr)->is_allocating(), "must be allocating");

  if (ZRefCount::count(to_oop(addr)->mark()) == 0) {
    ZPage* page = ZHeap::heap()->page(addr);
    page->set_pardoned(addr);
    OrderAccess::release(); // TODO: Embed in death row set?
    page->set_death_row(addr);
  }
  log_info(gc)("[oo %d %lx]", ZRefCount::count(to_oop(addr)->mark()), untype(addr));
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
  log_info(gc)("[ro %d %lx]", ZRefCount::count(to_oop(addr)->mark()), untype(addr));
}

// TODO: Make parallel
// TODO: Deal better with large arrays
// TODO: Figure out when we can SuspendibleThreadSet::yield()
void ZReferenceCounting::process_death_row(ZPageTable* page_table, ZPageAllocator* page_allocator) {
  Stack<oop, mtGC> dfs_stack;
  SuspendibleThreadSetJoiner sts;

  ZGenerationPagesIterator pt_iter(page_table, ZGenerationId::old, page_allocator);
  for (ZPage* page; pt_iter.next(&page);) {
    if (!page->is_allocating()) {
      // TODO: Construct iterator bitmap for faster iteration instead of filtering
      continue;
    }

    page->iterate_death_row([&](zaddress addr) {
      OrderAccess::acquire(); // TODO: put acquire into iterator?
      if (page->is_pardoned(addr)) {
        return;
      }

      oop obj = to_oop(addr);
      int ref_count = ZRefCount::count(obj->mark());

      assert(ref_count == 0, "must be zero: %d: %p", ref_count, (void*)addr);
      dfs_stack.push(obj);
    });

    while (!dfs_stack.is_empty()) {
      oop obj = dfs_stack.pop();
      zaddress addr = to_zaddress(obj);
      int ref_count = ZRefCount::count(obj->mark());
      ZPage* const page = ZHeap::heap()->page(addr);
      log_info(gc)("[dr %d %lx]", ZRefCount::count(to_oop(addr)->mark()), untype(addr));

      assert(ref_count == 0, "must be zero: %d: %p", ref_count, cast_from_oop<void*>(obj));
      assert(page->is_allocating(), "must be allocating: %p", cast_from_oop<void*>(obj));

      ZIterator::basic_oop_iterate_safe(obj, [&](volatile zpointer* p){
        size_t field_offset = pointer_delta(p, obj, sizeof(char));
        oop o = HeapAccess<ON_UNKNOWN_OOP_REF>::oop_load_at(obj, field_offset); // TODO: Feel happy about this?
        zaddress a = to_zaddress(o);

        if (a == zaddress::null) {
          return;
        }

        if (ZHeap::heap()->is_young(a)) {
          return;
        }

        ZPage* page = ZHeap::heap()->page(a);

        if (!page->is_allocating()) {
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
            if (ref_count == 1) {
              if (page->is_pardoned(a)) {
                page->set_death_row(a);
              } else {
                dfs_stack.push(o);
              }
            }
            break;
          }
        }
      });

      // Unlink current remset entries
      const uintptr_t from_local_offset = page->local_offset(addr);
      BitMap::Iterator iter = page->remset_iterator_limited_current(from_local_offset, ZUtils::object_size(addr));

      for (BitMap::idx_t field_bit : iter) {
        const uintptr_t field_local_offset = ZRememberedSet::to_offset(field_bit);
        const uintptr_t field_offset = field_local_offset - from_local_offset;
        const zaddress field_addr = addr + field_offset;
        volatile zpointer* const p = (volatile zpointer*)field_addr;

        page->forget_current(p);
      }

      page->unset_death_row(addr);

      log_info(gc)("Freeing object: %p", (void*)p2i(obj));
      page->free_object_to_free_list(addr);
    }
  }

  // Clear all the pardoned bits to prepare for next GC cycle.
  ZGenerationPagesIterator pt_iter2(page_table, ZGenerationId::old, page_allocator);
  for (ZPage* page; pt_iter2.next(&page);) {
    if (!page->is_allocating()) {
      // TODO: Construct iterator bitmap for faster iteration instead of filtering
      continue;
    }

    page->iterate_pardoned([&](zaddress addr) {
      page->unset_pardoned(addr);
    });
  }
}
