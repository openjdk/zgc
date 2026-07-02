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

// TODO: Pick better death row data structure?
ZReferenceCounting::ZReferenceCounting()
  : _lock(),
    _next_death_row(new (mtGC) ZAddressTable(8, 0x3fffffff)),
    _curr_death_row(new (mtGC) ZAddressTable(8, 0x3fffffff)) {
}

void ZReferenceCounting::on_young_mark_start() {
  delete _curr_death_row; // TODO: Looks expensive for safepoint; defer?
  _curr_death_row = _next_death_row;
  _next_death_row = new (mtGC) ZAddressTable(8, 0x3fffffff);
  log_info(gc)("[OYMS %d:%d]", _curr_death_row->number_of_entries(), _next_death_row->number_of_entries()); // TODO: REMOVE
}

void ZReferenceCounting::on_old_mark_start() {
  delete _curr_death_row; // TODO: Looks expensive for safepoint; defer?
  delete _next_death_row; // TODO: Looks expensive for safepoint; defer?
  _curr_death_row = new (mtGC) ZAddressTable(8, 0x3fffffff);
  _next_death_row = new (mtGC) ZAddressTable(8, 0x3fffffff);
  log_info(gc)("[OOMS %d:%d]", _curr_death_row->number_of_entries(), _next_death_row->number_of_entries()); // TODO: REMOVE
}

void ZReferenceCounting::increment(zaddress addr) {
  oop obj = to_oop(addr);

  if (!ZHeap::heap()->page(addr)->is_allocating()) {
    // Only is_allocating old objects are candidates for eager reclamation.
    return;
  }

  for (;;) {
    markWord mark = obj->mark();
    int ref_count = ZRefCount::count(mark);

    if (ref_count == ZRefCount::Uncertain) {
      return;
    }

    int new_ref_count = ref_count == ZRefCount::Max ? ZRefCount::Uncertain : (ref_count + 1);
    markWord new_mark = ZRefCount::set_count(mark, new_ref_count);

    if (obj->cas_set_mark(new_mark, mark, memory_order_relaxed) == mark) {
      if (new_ref_count == 1) {
        log_info(gc)("[i%d %p %d:%d]", new_ref_count, (void*)addr, _curr_death_row->number_of_entries(), _next_death_row->number_of_entries()); // TODO: REMOVE
        // When transitioning from 0 to 1, we no longer need to be remembered.
        ZLocker<ZLock> locker(&_lock);
        _curr_death_row->remove(addr);
        _next_death_row->remove(addr); // TODO: Seems impossible, but lets be safe now
      } else {
        log_info(gc)("[i%d %p]", new_ref_count, (void*)addr); // TODO: REMOVE
      }

      return;
    }
  }
}

void ZReferenceCounting::decrement(zaddress addr) {
  oop obj = to_oop(addr);

  if (!ZHeap::heap()->page(addr)->is_allocating()) {
    // Only is_allocating old objects are candidates for eager reclamation.
    return;
  }

  for (;;) {
    markWord mark = obj->mark();
    int ref_count = ZRefCount::count(mark);

    if (ref_count == 0 && !ZGeneration::young()->is_phase_mark()) {
      // TODO: Validate this code
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
      log_info(gc)("[d%d %p %d:%d]", new_ref_count, (void*)addr, _curr_death_row->number_of_entries(), _next_death_row->number_of_entries()); // TODO: REMOVE
      if (new_ref_count == ZRefCount::Uncertain) {
        ZLocker<ZLock> locker(&_lock);
        _curr_death_row->remove(addr);
        _next_death_row->remove(addr); // TODO: Seems impossible, but lets be safe now
      } else if (new_ref_count == 0) {
        // When transitioning from 1 to 0, we need to remember the object so it
        // can be freed later.
        ZLocker<ZLock> locker(&_lock);
        bool created;
        _next_death_row->put_if_absent(addr, &created);
        _next_death_row->maybe_grow();
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

  log_info(gc)("[re %p]", (void*)addr); // TODO: REMOVE

  {
    ZLocker<ZLock> locker(&_lock);
    _curr_death_row->remove(addr);
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

  // TODO: Ugly back pointer in architecture
  if (forgotten) {
    // TODO: Could return here instead. Now I explicitly both call increment and
    // decrement, purely to handle the death rows right.
    increment(addr);
  }

  decrement(addr);
}

void ZReferenceCounting::on_forget(volatile zpointer* p, zaddress addr) {
  // When we have an old-to-old pointer that is about to become forgotten,
  // it means that it was written to by a mutator in the last marking epoch.
  // Therefore, we have to account for the last increment of the last cycle.
  log_info(gc)("[fo %p]", (void*)addr); // TODO: REMOVE
  increment(addr);
}

void ZReferenceCounting::on_promotion(zaddress addr) {
  assert(ZHeap::heap()->is_old(addr), "must be old");
  assert(ZHeap::heap()->page(addr)->is_allocating(), "must be allocating");
  assert(ZRefCount::count(to_oop(addr)->mark()) == 0,
         "invalid promotion ref count: %d", ZRefCount::count(to_oop(addr)->mark()));
  ZLocker<ZLock> locker(&_lock);
  bool created;
  log_info(gc)("[p %p %d:%d]", (void*)addr, _curr_death_row->number_of_entries(), _next_death_row->number_of_entries()); // TODO: REMOVE
  _next_death_row->put_if_absent(addr, &created);
  _next_death_row->maybe_grow();
}

// TODO: Not called
void ZReferenceCounting::on_root(zaddress addr) {
  // The reference counters track the number of old-to-old references in
  // the object graph. That means roots to the old generation are not
  // accounted for. To make life easier, we simply ban objects ever referred
  // to from global roots from becoming eagerly collected. Then we only need
  // to deal with indeterminism w.r.t. the execution stacks and the
  // young-to-old roots found during young generation collection.

  oop obj = to_oop(addr);
  markWord mark = obj->mark();
  int ref_count = ZRefCount::count(mark);
  if (ref_count > 0) {
    return;
  }

  ZLocker<ZLock> locker(&_lock);
  if (_curr_death_row->remove(addr)) {
    // If a root blocks freeing, enqueue for processing next time again,
    // so that it will eventually get freed when the root is gone, while
    // old-to-old ref count stays zero.
    bool created;
    log_info(gc)("[ro %p %d:%d]", (void*)addr, _curr_death_row->number_of_entries(), _next_death_row->number_of_entries()); // TODO: REMOVE
    _next_death_row->put_if_absent(addr, &created);
    _next_death_row->maybe_grow();
  }
}

// TODO: not called
// TODO: Make parallel
// TODO: Deal better with large arrays
// TODO: Figure out when we can SuspendibleThreadSet::yield()
void ZReferenceCounting::process_death_row() {
  class ZFollowGarbageOopIterateClosure: public BasicOopIterateClosure {
    Stack<oop, mtGC>* _dfs_stack;
    oop _obj;
    ZPage* _page;

  public:
    ZFollowGarbageOopIterateClosure(Stack<oop, mtGC>* dfs_stack, oop obj, ZPage* page) :
      _dfs_stack(dfs_stack),
      _obj(obj),
      _page(page) {}

    void do_oop(narrowOop *p) { ShouldNotReachHere(); }
    void do_oop(oop *p) {
      size_t field_offset = pointer_delta(p, _obj, sizeof(char));
      oop obj = HeapAccess<ON_UNKNOWN_OOP_REF>::oop_load_at(_obj, field_offset); // TODO: Feel happy about this?

      if (obj != nullptr) {
        for (;;) {
          markWord mark = obj->mark();
          int ref_count = ZRefCount::count(mark);
          assert(ref_count > 0, "sanity");
          markWord new_mark = ZRefCount::set_count(mark, ref_count - 1);
          if (obj->cas_set_mark(new_mark, mark, memory_order_relaxed) == mark) {
            log_info(gc)("[drd %p]", cast_from_oop<void*>(obj)); // TODO: REMOVE
            // If we decrement an edge to zero, we traverse through more garbage.
            if (ref_count == 1) {
              _dfs_stack->push(obj);
            }
            break;
          }
        }
      }
    }
  };

  Stack<oop, mtGC> dfs_stack;
  SuspendibleThreadSetJoiner sts;

  _curr_death_row->iterate_all([&](zaddress addr, bool unused) {
    oop obj = to_oop(addr);
    int ref_count = ZRefCount::count(obj->mark());
    log_info(gc)("[drr %p]", (void*)addr); // TODO: REMOVE
    assert(ref_count == 0, "must be zero: %d: %p", ref_count, (void*)addr);
    dfs_stack.push(obj);
  });

  while (!dfs_stack.is_empty()) {
    oop obj = dfs_stack.pop();
    zaddress addr = to_zaddress(obj);
    int ref_count = ZRefCount::count(obj->mark());
    log_info(gc)("[drf %p]", cast_from_oop<void*>(obj)); // TODO: REMOVE
    assert(ref_count == 0, "must be zero: %d: %p", ref_count, cast_from_oop<void*>(obj));
    ZPage* const page = ZHeap::heap()->page(addr);
    assert(page->is_allocating(), "must be allocating: %p", cast_from_oop<void*>(obj));

    ZFollowGarbageOopIterateClosure cl(&dfs_stack, obj, page);
    obj->oop_iterate(&cl);

    // Unlink current remset entries
    const uintptr_t from_local_offset = page->local_offset(addr);
    BitMap::Iterator iter = page->remset_iterator_limited_current(from_local_offset, obj->size());

    for (BitMap::idx_t field_bit : iter) {
      const uintptr_t field_local_offset = ZRememberedSet::to_offset(field_bit);
      const uintptr_t field_offset = field_local_offset - from_local_offset;
      const zaddress field_addr = addr + field_offset;
      volatile zpointer* const p = (volatile zpointer*)field_addr;

      page->forget_current(p);
    }

    log_info(gc)("Freeing object: %p", (void*)p2i(obj));
    page->free_object_to_free_list(addr);
  }
}
