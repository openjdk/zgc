/*
 * Copyright (c) 2015, 2026, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/z/zAbort.inline.hpp"
#include "gc/z/zAddress.inline.hpp"
#include "gc/z/zBarrier.inline.hpp"
#include "gc/z/zCollectedHeap.hpp"
#include "gc/z/zForwarding.inline.hpp"
#include "gc/z/zGeneration.inline.hpp"
#include "gc/z/zHeap.inline.hpp"
#include "gc/z/zIndexDistributor.inline.hpp"
#include "gc/z/zIterator.inline.hpp"
#include "gc/z/zNUMA.inline.hpp"
#include "gc/z/zObjectAllocator.hpp"
#include "gc/z/zPage.inline.hpp"
#include "gc/z/zPageAge.inline.hpp"
#include "gc/z/zPageAllocator.hpp"
#include "gc/z/zRelocate.hpp"
#include "gc/z/zRelocationSet.inline.hpp"
#include "gc/z/zRootsIterator.hpp"
#include "gc/z/zStackWatermark.hpp"
#include "gc/z/zStat.hpp"
#include "gc/z/zStringDedup.inline.hpp"
#include "gc/z/zTask.hpp"
#include "gc/z/zUncoloredRoot.inline.hpp"
#include "gc/z/zUtils.inline.hpp"
#include "gc/z/zValue.inline.hpp"
#include "gc/z/zVerify.hpp"
#include "gc/z/zWorkers.hpp"
#include "prims/jvmtiTagMap.hpp"
#include "runtime/atomicAccess.hpp"
#include "utilities/align.hpp"
#include "utilities/debug.hpp"

static const ZStatCriticalPhase ZCriticalPhaseRelocationStall("Relocation Stall");
static const ZStatSubPhase ZSubPhaseConcurrentRelocateRememberedSetFlipPromotedYoung("Concurrent Relocate Remset FP", ZGenerationId::young);
static const ZStatSubPhase ZSubPhaseConcurrentFreeListPageYoung("Concurrent FP Free-List Page", ZGenerationId::young);
static const ZStatSubPhase ZSubPhaseConcurrentFreeListPageOld("Concurrent FP Free-List Page", ZGenerationId::old);

ZRelocateQueue::ZRelocateQueue()
  : _lock(),
    _queue(),
    _nworkers(0),
    _nsynchronized(0),
    _synchronize(false),
    _is_active(false),
    _needs_attention(0) {}

bool ZRelocateQueue::needs_attention() const {
  return _needs_attention.load_relaxed() != 0;
}

void ZRelocateQueue::inc_needs_attention() {
  const int needs_attention = _needs_attention.add_then_fetch(1);
  assert(needs_attention == 1 || needs_attention == 2, "Invalid state");
}

void ZRelocateQueue::dec_needs_attention() {
  const int needs_attention = _needs_attention.sub_then_fetch(1);
  assert(needs_attention == 0 || needs_attention == 1, "Invalid state");
}

void ZRelocateQueue::activate(uint nworkers) {
  _is_active.store_relaxed(true);
  join(nworkers);
}

void ZRelocateQueue::deactivate() {
  _is_active.store_relaxed(false);
  clear();
}

bool ZRelocateQueue::is_active() const {
  return _is_active.load_relaxed();
}

void ZRelocateQueue::join(uint nworkers) {
  assert(nworkers != 0, "Must request at least one worker");
  assert(_nworkers == 0, "Invalid state");
  assert(_nsynchronized == 0, "Invalid state");

  log_debug(gc, reloc)("Joining workers: %u", nworkers);

  _nworkers = nworkers;
}

void ZRelocateQueue::resize_workers(uint nworkers) {
  assert(nworkers != 0, "Must request at least one worker");
  assert(_nworkers == 0, "Invalid state");
  assert(_nsynchronized == 0, "Invalid state");

  log_debug(gc, reloc)("Resize workers: %u", nworkers);

  ZLocker<ZConditionLock> locker(&_lock);
  _nworkers = nworkers;
}

void ZRelocateQueue::leave() {
  ZLocker<ZConditionLock> locker(&_lock);
  _nworkers--;

  assert(_nsynchronized <= _nworkers, "_nsynchronized: %u _nworkers: %u", _nsynchronized, _nworkers);

  log_debug(gc, reloc)("Leaving workers: left: %u _synchronize: %d _nsynchronized: %u", _nworkers, _synchronize, _nsynchronized);

  // Prune done forwardings
  const bool forwardings_done = prune();

  // Check if all workers synchronized
  const bool last_synchronized = _synchronize && _nworkers == _nsynchronized;

  if (forwardings_done || last_synchronized) {
    _lock.notify_all();
  }
}

void ZRelocateQueue::add_and_wait(ZForwarding* forwarding) {
  ZStatTimer timer(ZCriticalPhaseRelocationStall);
  ZLocker<ZConditionLock> locker(&_lock);

  if (forwarding->is_done()) {
    return;
  }

  _queue.append(forwarding);
  if (_queue.length() == 1) {
    // Queue became non-empty
    inc_needs_attention();
    _lock.notify_all();
  }

  while (!forwarding->is_done()) {
    _lock.wait();
  }
}

bool ZRelocateQueue::prune() {
  if (_queue.is_empty()) {
    return false;
  }

  bool done = false;

  for (int i = 0; i < _queue.length();) {
    const ZForwarding* const forwarding = _queue.at(i);
    if (forwarding->is_done()) {
      done = true;

      _queue.delete_at(i);
    } else {
      i++;
    }
  }

  if (_queue.is_empty()) {
    dec_needs_attention();
  }

  return done;
}

ZForwarding* ZRelocateQueue::prune_and_claim() {
  if (prune()) {
    _lock.notify_all();
  }

  for (int i = 0; i < _queue.length(); i++) {
    ZForwarding* const forwarding = _queue.at(i);
    if (forwarding->claim()) {
      return forwarding;
    }
  }

  return nullptr;
}

class ZRelocateQueueSynchronizeThread {
private:
  ZRelocateQueue* const _queue;

public:
  ZRelocateQueueSynchronizeThread(ZRelocateQueue* queue)
    : _queue(queue) {
    _queue->synchronize_thread();
  }

  ~ZRelocateQueueSynchronizeThread() {
    _queue->desynchronize_thread();
  }
};

void ZRelocateQueue::synchronize_thread() {
  _nsynchronized++;

  log_debug(gc, reloc)("Synchronize worker _nsynchronized %u", _nsynchronized);

  assert(_nsynchronized <= _nworkers, "_nsynchronized: %u _nworkers: %u", _nsynchronized, _nworkers);
  if (_nsynchronized == _nworkers) {
    // All workers synchronized
    _lock.notify_all();
  }
}

void ZRelocateQueue::desynchronize_thread() {
  _nsynchronized--;

  log_debug(gc, reloc)("Desynchronize worker _nsynchronized %u", _nsynchronized);

  assert(_nsynchronized < _nworkers, "_nsynchronized: %u _nworkers: %u", _nsynchronized, _nworkers);
}

ZForwarding* ZRelocateQueue::synchronize_poll() {
  // Fast path avoids locking
  if (!needs_attention()) {
    return nullptr;
  }

  // Slow path to get the next forwarding and/or synchronize
  ZLocker<ZConditionLock> locker(&_lock);

  {
    ZForwarding* const forwarding = prune_and_claim();
    if (forwarding != nullptr) {
      // Don't become synchronized while there are elements in the queue
      return forwarding;
    }
  }

  if (!_synchronize) {
    return nullptr;
  }

  ZRelocateQueueSynchronizeThread rqst(this);

  do {
    _lock.wait();

    ZForwarding* const forwarding = prune_and_claim();
    if (forwarding != nullptr) {
      return forwarding;
    }
  } while (_synchronize);

  return nullptr;
}

void ZRelocateQueue::clear() {
  assert(_nworkers == 0, "Invalid state");

  if (_queue.is_empty()) {
    return;
  }

  ZArrayIterator<ZForwarding*> iter(&_queue);
  for (ZForwarding* forwarding; iter.next(&forwarding);) {
    assert(forwarding->is_done(), "All should be done");
  }

  assert(false, "Clear was not empty");

  _queue.clear();
  dec_needs_attention();
}

void ZRelocateQueue::synchronize() {
  ZLocker<ZConditionLock> locker(&_lock);
  _synchronize = true;

  inc_needs_attention();

  log_debug(gc, reloc)("Synchronize all workers 1 _nworkers: %u _nsynchronized: %u", _nworkers, _nsynchronized);

  while (_nworkers != _nsynchronized) {
    _lock.wait();
    log_debug(gc, reloc)("Synchronize all workers 2 _nworkers: %u _nsynchronized: %u", _nworkers, _nsynchronized);
  }
}

void ZRelocateQueue::desynchronize() {
  ZLocker<ZConditionLock> locker(&_lock);
  _synchronize = false;

  log_debug(gc, reloc)("Desynchronize all workers _nworkers: %u _nsynchronized: %u", _nworkers, _nsynchronized);

  assert(_nsynchronized <= _nworkers, "_nsynchronized: %u _nworkers: %u", _nsynchronized, _nworkers);

  dec_needs_attention();

  _lock.notify_all();
}

ZRelocationTargets::ZRelocationTargets()
  : _targets() {}

ZPage* ZRelocationTargets::get(uint32_t partition_id, ZPageAge age) {
  return _targets.get(partition_id)[untype(age) - 1];
}

void ZRelocationTargets::set(uint32_t partition_id, ZPageAge age, ZPage* page) {
  _targets.get(partition_id)[untype(age) - 1] = page;
}

template <typename Function>
void ZRelocationTargets::apply_and_clear_targets(Function function) {
  ZPerNUMAIterator<TargetArray> iter(&_targets);
  for (TargetArray* targets; iter.next(&targets);) {
    for (size_t i = 0; i < ZNumRelocationAges; i++) {
      // Apply function
      function((*targets)[i]);

      // Clear target
      (*targets)[i] = nullptr;
    }
  }
}

ZRelocate::ZRelocate(ZGeneration* generation)
  : _generation(generation),
    _queue(),
    _iters(),
    _small_targets(),
    _medium_targets(),
    _shared_medium_targets() {}

ZWorkers* ZRelocate::workers() const {
  return _generation->workers();
}

void ZRelocate::start() {
  _queue.activate(workers()->active_workers());
}

void ZRelocate::add_remset(volatile zpointer* p) {
  ZGeneration::young()->remember(p);
}

static zaddress relocate_object_inner(ZForwarding* forwarding, zaddress from_addr, ZForwardingCursor* cursor) {
  assert(ZHeap::heap()->is_object_live(from_addr), "Should be live");

  // Allocate object
  const size_t size = ZUtils::object_size(from_addr);

  // TODO: Refactor everything
  zaddress to_addr = zaddress::null;
  ZReferenceCounting::FreeListAllocation free_list_allocation;

  if (is_old(forwarding->to_age())) {
    free_list_allocation = ZGeneration::young()->free_list_alloc_object(size, forwarding->type(), forwarding->to_age());
    to_addr = free_list_allocation._address;
  }

  if (to_addr == zaddress::null) {
    const ZPageAge to_age = forwarding->to_age();

    to_addr = ZHeap::heap()->alloc_object_for_relocation(size, to_age);
  }


  if (is_null(to_addr)) {
    // Allocation failed
    return zaddress::null;
  }


  // Copy object
  ZUtils::object_copy_disjoint(from_addr, to_addr, size);

  ZPage* to_page = ZHeap::heap()->page(to_addr); // TODO: optimize
  if (forwarding->is_old_to_old()) {
    ZPage* const page = free_list_allocation._address == zaddress::null ? to_page : free_list_allocation._page;
    ZGeneration::young()->on_old_to_space_alloc(page, to_addr, true);
  }

  // Insert forwarding
  const zaddress to_addr_final = forwarding->insert(from_addr, to_addr, cursor);

  if (to_addr_final != to_addr) {
    // Already relocated, try undo allocation
    if (free_list_allocation._address != zaddress::null) {
      // TODO: Change this so be symmtric, alloc gets FreeListAllocation, undo consumes it.
      free_list_allocation._page->undo_alloc_object_from_free_list(unsafe(to_addr), size);
    } else {
      ZHeap::heap()->undo_alloc_object_for_relocation(to_addr, size);
    }
  } else {
    if (free_list_allocation._address != zaddress::null) {
      if (forwarding->is_promotion()) {
        ZGeneration::young()->increase_mutator_freelist_promoted(free_list_allocation._page->type(), size);
      } else {
        ZGeneration::old()->increase_mutator_freelist_compacted(free_list_allocation._page->type(), size);
      }
    }

    if (forwarding->is_old_to_old()) {
      ZGeneration::young()->on_mutator_old_to_old(forwarding, from_addr, to_addr);
    }
  }

  return to_addr_final;
}

zaddress ZRelocate::relocate_object(ZForwarding* forwarding, zaddress_unsafe from_addr) {
  ZForwardingCursor cursor;

  // Lookup forwarding
  zaddress to_addr = forwarding->find(from_addr, &cursor);
  if (!is_null(to_addr)) {
    // Already relocated
    return to_addr;
  }

  // Relocate object
  if (forwarding->retain_page(&_queue)) {
    assert(_generation->is_phase_relocate(), "Must be");
    to_addr = relocate_object_inner(forwarding, safe(from_addr), &cursor);
    forwarding->release_page();

    if (!is_null(to_addr)) {
      // Success
      return to_addr;
    }

    // Failed to relocate object. Signal and wait for a worker thread to
    // complete relocation of this page, and then forward the object.
    _queue.add_and_wait(forwarding);
  }

  // Forward object
  return forward_object(forwarding, from_addr);
}

zaddress ZRelocate::forward_object(ZForwarding* forwarding, zaddress_unsafe from_addr) {
  const zaddress to_addr = forwarding->find(from_addr);
  assert(!is_null(to_addr), "Should be forwarded: " PTR_FORMAT, untype(from_addr));
  return to_addr;
}

static ZPage* alloc_page(ZForwarding* forwarding) {
  if (ZStressRelocateInPlace) {
    // Simulate failure to allocate a new page. This will
    // cause the page being relocated to be relocated in-place.
    return nullptr;
  }

  const ZPageType type = forwarding->type();
  const size_t size = forwarding->size();
  const ZPageAge age = forwarding->to_age();
  const uint32_t preferred_partition = forwarding->partition_id();

  ZAllocationFlags flags;
  flags.set_non_blocking();
  flags.set_gc_relocation();

  return ZHeap::heap()->alloc_page(type, size, flags, age, preferred_partition);
}

static void retire_target_page(ZGeneration* generation, ZPage* page) {
  if (generation->is_young() && page->is_old()) {
    generation->increase_uncompensated_promoted(page->type(), page->used());
  } else {
    generation->increase_uncompensated_compacted(page->type(), page->used());
  }

  // Free target page if it is empty. We can end up with an empty target
  // page if we allocated a new target page, and then lost the race to
  // relocate the remaining objects, leaving the target page empty when
  // relocation completed.
  if (page->used() == 0) {
    ZHeap::heap()->free_page(page);
  } else if (ZOldRefCount && page->is_old()) {
    // Put the remainder in the freelist.
    // TODO: For shared pages this has the same race as the used statistics
    //       above, need the same last reference does the retire fix so we do
    //       not allocate when the page is still useful. After that we can
    //       remove atomicity and the loop.
    for (size_t size = page->remaining(); size != 0; size = page->remaining()) {
      zaddress addr = page->alloc_object_atomic(size);
      if (addr != zaddress::null) {
        page->free_tail_to_free_list(unsafe(addr), size);
      }
    }
  }
}

class ZRelocateSmallAllocator {
private:
  ZGeneration* const _generation;
  Atomic<size_t>     _in_place_count;

public:
  ZRelocateSmallAllocator(ZGeneration* generation)
    : _generation(generation),
      _in_place_count(0) {}

  ZPage* alloc_and_retire_target_page(ZForwarding* forwarding, ZPage* target) {
    ZPage* const page = alloc_page(forwarding);
    if (page == nullptr) {
      _in_place_count.add_then_fetch(1u);
    }

    if (target != nullptr) {
      // Retire the old target page
      retire_target_page(_generation, target);
    }

    return page;
  }

  void share_target_page(ZPage* page, uint32_t partition_id) {
    // Does nothing
  }

  void free_target_page(ZPage* page) {
    if (page != nullptr) {
      retire_target_page(_generation, page);
    }
  }

  zaddress alloc_object(ZPage* page, size_t size) const {
    return (page != nullptr) ? page->alloc_object(size) : zaddress::null;
  }

  void undo_alloc_object(ZPage* page, zaddress addr, size_t size) const {
    page->undo_alloc_object(addr, size);
  }

  size_t in_place_count() const {
    return _in_place_count.load_relaxed();
  }
};

class ZRelocateMediumAllocator {
private:
  ZGeneration* const  _generation;
  ZConditionLock      _lock;
  ZRelocationTargets* _shared_targets;
  bool                _in_place;
  Atomic<size_t>      _in_place_count;

public:
  ZRelocateMediumAllocator(ZGeneration* generation, ZRelocationTargets* shared_targets)
    : _generation(generation),
      _lock(),
      _shared_targets(shared_targets),
      _in_place(false),
      _in_place_count(0) {}

  ~ZRelocateMediumAllocator() {
    _shared_targets->apply_and_clear_targets([&](ZPage* page) {
      if (page != nullptr) {
        retire_target_page(_generation, page);
      }
    });
  }

  ZPage* alloc_and_retire_target_page(ZForwarding* forwarding, ZPage* target) {
    ZLocker<ZConditionLock> locker(&_lock);

    // Wait for any ongoing in-place relocation to complete
    while (_in_place) {
      _lock.wait();
    }

    // Allocate a new page only if the shared page is the same as the
    // current target page. The shared page will be different from the
    // current target page if another thread shared a page, or allocated
    // a new page.
    const ZPageAge to_age = forwarding->to_age();
    const uint32_t partition_id = forwarding->partition_id();
    if (_shared_targets->get(partition_id, to_age) == target) {
      ZPage* const to_page = alloc_page(forwarding);
      _shared_targets->set(partition_id, to_age, to_page);
      if (to_page == nullptr) {
        _in_place_count.add_then_fetch(1u);
        _in_place = true;
      }

      // This thread is responsible for retiring the shared target page
      if (target != nullptr) {
        retire_target_page(_generation, target);
      }
    }

    return _shared_targets->get(partition_id, to_age);
  }

  void share_target_page(ZPage* page, uint32_t partition_id) {
    const ZPageAge age = page->age();

    ZLocker<ZConditionLock> locker(&_lock);
    assert(_in_place, "Invalid state");
    assert(_shared_targets->get(partition_id, age) == nullptr, "Invalid state");
    assert(page != nullptr, "Invalid page");

    _shared_targets->set(partition_id, age, page);
    _in_place = false;

    _lock.notify_all();
  }

  void free_target_page(ZPage* page) {
    // Does nothing
  }

  zaddress alloc_object(ZPage* page, size_t size) const {
    return (page != nullptr) ? page->alloc_object_atomic(size) : zaddress::null;
  }

  void undo_alloc_object(ZPage* page, zaddress addr, size_t size) const {
    page->undo_alloc_object_atomic(addr, size);
  }

  size_t in_place_count() const {
    return _in_place_count.load_relaxed();
  }
};

template <typename Allocator>
class ZRelocateWork : public StackObj {
private:
  Allocator* const    _allocator;
  ZForwarding*        _forwarding;
  ZRelocationTargets* _targets;
  ZGeneration* const  _generation;
  size_t              _freelist_promoted[ZPageTypeCount];
  size_t              _freelist_compacted[ZPageTypeCount];
  size_t              _mutator_promoted[ZPageTypeCount];
  size_t              _mutator_compacted[ZPageTypeCount];
  ZStringDedupContext _string_dedup_context;

  size_t object_alignment() const {
    return (size_t)1 << _forwarding->object_alignment_shift();
  }

  void increase_mutator_forwarded(size_t unaligned_object_size) {
    const size_t aligned_size = align_up(unaligned_object_size, object_alignment());
    const uint type_index = untype(_forwarding->type());
    if (_forwarding->is_promotion()) {
      _mutator_promoted[type_index] += aligned_size;
    } else {
      _mutator_compacted[type_index] += aligned_size;
    }
  }

  zaddress try_relocate_object_inner(zaddress from_addr, uint32_t partition_id) {
    ZForwardingCursor cursor;

    const size_t size = ZUtils::object_size(from_addr);

    // Lookup forwarding
    {
      const zaddress to_addr = _forwarding->find(from_addr, &cursor);
      if (!is_null(to_addr)) {
        // Already relocated
        increase_mutator_forwarded(size);
        update_remset_for_fields(from_addr, to_addr, true /* was_mutator */);
        return to_addr;
      }
    }

    // TODO: Refactor everthing.
    zaddress allocated_addr = zaddress::null;
    ZReferenceCounting::FreeListAllocation free_list_allocation;

    if (is_old(_forwarding->to_age())) {
      free_list_allocation = ZGeneration::young()->free_list_alloc_object(size, _forwarding->type(), _forwarding->to_age());
      allocated_addr = free_list_allocation._address;
    }

    ZPage* const to_page = _targets->get(partition_id, _forwarding->to_age());

    if (allocated_addr == zaddress::null) {
      // Allocate object
      allocated_addr = _allocator->alloc_object(to_page, size);
      if (is_null(allocated_addr)) {
        // Allocation failed
        return zaddress::null;
      }
    }

    bool in_place = _forwarding->in_place_relocation();

    // Copy object. Use conjoint copying if we are relocating
    // in-place and the new object overlaps with the old object.
    if (in_place && allocated_addr + size > from_addr) {
      ZUtils::object_copy_conjoint(from_addr, allocated_addr, size);
    } else {
      ZUtils::object_copy_disjoint(from_addr, allocated_addr, size);
    }

    if (_forwarding->is_old_to_old()) {
      ZPage* const page = free_list_allocation._address == zaddress::null ? to_page : free_list_allocation._page;
      ZGeneration::young()->on_old_to_space_alloc(page, allocated_addr, false);
    }

    if (in_place) {
      // In-place relocation has to update remembered sets before mutators can
      // clobber the memory. Otherwise, we lose information necessary to process
      // reference counts for the old generation.
      update_remset_for_fields(from_addr, allocated_addr, false /* was_mutator */);
    }

    // Insert forwarding
    const zaddress to_addr = _forwarding->insert(from_addr, allocated_addr, &cursor);

    if (to_addr != allocated_addr) {
      // Already relocated, undo allocation
      assert(!in_place, "sanity");

      if (free_list_allocation._address != zaddress::null) {
        // TODO: Change this so be symmtric, alloc gets FreeListAllocation, undo consumes it.
        free_list_allocation._page->undo_alloc_object_from_free_list(unsafe(allocated_addr), size);
      } else {
        // TODO: Fix wrong address undo.
        // _allocator->undo_alloc_object(to_page, to_addr, size);
      }

      increase_mutator_forwarded(size);
    } else if (free_list_allocation._address != zaddress::null) {
      if (_forwarding->is_promotion()) {
        _freelist_promoted[untype(free_list_allocation._page->type())] += size;
      } else {
        _freelist_compacted[untype(free_list_allocation._page->type())] += size;
      }
    }

    if (!in_place) {
      update_remset_for_fields(from_addr, to_addr, to_addr != allocated_addr);
    }

    return to_addr;
  }

  void update_remset_old_to_old(zaddress from_addr, zaddress to_addr, bool was_mutator) const {
    // Old-to-old relocation - move existing remset bits

    // If a young generation collection started while the old generation
    // relocated  objects, the remember set bits were flipped from "current"
    // to "previous".
    //
    // We need to select the correct remembered sets bitmap to ensure that the
    // old remset bits are found.
    //
    // Note that if the young generation marking (remset scanning) finishes
    // before the old generation relocation has relocated this page, then the
    // young generation will visit this page's previous remembered set bits and
    // moved them over to the current bitmap.
    //
    // If the young generation runs multiple cycles while the old generation is
    // relocating, then the first cycle will have consumed the old remset,
    // bits and moved associated objects to a new old page. The old relocation
    // could find either of the two bitmaps. So, either it will find the original
    // remset bits for the page, or it will find an empty bitmap for the page. It
    // doesn't matter for correctness, because the young generation marking has
    // already taken care of the bits.

    const uint32_t young_marks = ZGeneration::old()->young_marks_since_old_reloc_start();

    const bool before_young_mark = young_marks == 0;
    const bool during_young_mark = young_marks == 1 && ZGeneration::young()->is_phase_mark();
    const bool after_young_mark = !before_young_mark && !during_young_mark;

    if (after_young_mark) {
      // After the first young mark, all remembered sets of forwardings have been
      // processed, and processing them again is dangerous for reference counting.
      return;
    }

    if (before_young_mark && was_mutator) {
      // Before young mark start, mutators help relocating remembered set entries,
      // so we shouldn't do it again as that might conflict our counters.
      return;
    }

    // If this is called for an in-place relocated page, then this code has the
    // responsibility to clear the old remset bits. Extra care is needed because:
    //
    // 1) The to-object copy can overlap with the from-object copy
    // 2) Remset bits of old objects need to be cleared
    //
    // A watermark is used to keep track of how far the old remset bits have been removed.

    ZPage* const from_page = _forwarding->page();
    const uintptr_t from_local_offset = from_page->local_offset(from_addr);

    // Note: even with in-place relocation, the to_page could be another page
    ZPage* const to_page = ZHeap::heap()->page(to_addr);

    const bool in_place = from_page->start() == to_page->start();

    // Uses _relaxed version to handle that in-place relocation resets _top
    assert(ZHeap::heap()->is_in_page_relaxed(from_page, from_addr), "Must be");
    assert(to_page->is_in(to_addr), "Must be");

    // Read the size from the to-object, since the from-object
    // could have been overwritten during in-place relocation.
    const size_t size = ZUtils::object_size(to_addr);

    // When in-place relocation is done and the old remset bits are located in
    // the bitmap that is going to be used for the new remset bits, then we
    // need to clear the old bits before the new bits are inserted.
    const bool iterate_current_remset = before_young_mark;

    BitMap::Iterator iter = before_young_mark
        ? from_page->remset_iterator_limited_current(from_local_offset, size)
        : from_page->remset_iterator_limited_previous(from_local_offset, size);

    for (BitMap::idx_t field_bit : iter) {
      const uintptr_t field_local_offset = ZRememberedSet::to_offset(field_bit);

      // Add remset entry in the to-page
      const uintptr_t offset = field_local_offset - from_local_offset;
      const zaddress to_field = to_addr + offset;
      const zaddress from_field = from_addr + offset;
      log_trace(gc, reloc)("Remember: from: " PTR_FORMAT " to: " PTR_FORMAT " marking: %d page: " PTR_FORMAT " remset: " PTR_FORMAT,
                           untype(from_page->start() + field_local_offset), untype(to_field), ZGeneration::young()->is_phase_mark(), p2i(to_page), p2i(to_page->remset_current()));

      volatile zpointer* const to_p = (volatile zpointer*)to_field;
      volatile zpointer* const from_p = (volatile zpointer*)from_field;

      if (during_young_mark) {
        // When we have in-place relocation the fromspace object might have been conjointly copied
        // to to-space, meaning that the from-space view is garbage. However, when we do have in-place
        // relocated objects, we perform the remset maintenance before exposing the object in the
        // forwarding table. Therefore, the to-space field will contain exactly the same value that
        // our from-space snapshot would have.
        zpointer prev = AtomicAccess::load(in_place ? to_p : from_p);

        // Young generation remembered set scanning needs to know about this
        // field. It will take responsibility to add a new remember set entry if needed.
        _forwarding->relocated_remembered_fields_register(to_p, prev); // TODO: Doesn't respect the flag right now
      } else {
        assert(before_young_mark, "must be");

        // Forget current so subsequent remset scanning doesn't process the prev bits from fromspace.
        from_page->forget_current(from_p);

        if (!ZGeneration::young()->remember(to_p)) { // TODO: Put into shared function?
          assert(!in_place, "impossible scenario");
          // During in-place relocation, we can only fail inserting the remembered set due to mutator
          // relocations. This means that from and to objects are disjoint, and we can easily fish out
          // the initial value from the from-space.
          zpointer prev = AtomicAccess::load(from_p);

          // It is impossible for the below load barrier to require relocation. If the mutator beat
          // us to it with setting the current bit with its store barrier, then the forwarding table
          // for the initial previous value that we have in prev, is guaranteed to have been relocated
          // already by the mutator.
          const zaddress addr = ZBarrier::load_barrier_on_oop_field_preloaded(nullptr, prev);
          ZGeneration::young()->on_failed_remember(addr);
        }
        if (in_place) {
          assert(to_page->is_remembered(to_p), "p: " PTR_FORMAT, p2i(to_p));
        }
      }
    }
  }

  static bool add_remset_if_young(volatile zpointer* p, zaddress addr) {
    if (ZHeap::heap()->is_young(addr)) {
      ZRelocate::add_remset(p);
      return true;
    }

    return false;
  }

  static void update_remset_promoted_filter_and_remap_per_field(volatile zpointer* p) {
    const zpointer ptr = AtomicAccess::load(p);

    assert(ZPointer::is_old_load_good(ptr), "Should be at least old load good: " PTR_FORMAT, untype(ptr));

    if (ZPointer::is_store_good(ptr)) {
      // Already has a remset entry
      return;
    }

    if (ZPointer::is_load_good(ptr)) {
      if (!is_null_any(ptr)) {
        const zaddress addr = ZPointer::uncolor(ptr);
        add_remset_if_young(p, addr);
      }
      // No need to remap it is already load good
      return;
    }

    if (is_null_any(ptr)) {
      // Eagerly remap to skip adding a remset entry just to get deferred remapping
      ZBarrier::remap_young_relocated(p, ptr);
      return;
    }

    const zaddress_unsafe addr_unsafe = ZPointer::uncolor_unsafe(ptr);
    ZForwarding* const forwarding = ZGeneration::young()->forwarding(addr_unsafe);

    if (forwarding == nullptr) {
      // Object isn't being relocated
      const zaddress addr = safe(addr_unsafe);
      if (!add_remset_if_young(p, addr)) {
        // Not young - eagerly remap to skip adding a remset entry just to get deferred remapping
        ZBarrier::remap_young_relocated(p, ptr);
      }
      return;
    }

    const zaddress addr = forwarding->find(addr_unsafe);

    if (!is_null(addr)) {
      // Object has already been relocated
      if (!add_remset_if_young(p, addr)) {
        // Not young - eagerly remap to skip adding a remset entry just to get deferred remapping
        ZBarrier::remap_young_relocated(p, ptr);
      }
      return;
    }

    // Object has not been relocated yet
    // Don't want to eagerly relocate objects, so just add a remset
    ZRelocate::add_remset(p);
    return;
  }

  void update_remset_promoted(zaddress from_addr, zaddress to_addr) const {
    if (ZOldRefCount) {
      // TODO: Bad smell
      auto doit = [&](volatile zpointer* p) {
        if (!ZGeneration::young()->remember(p)) {
          uintptr_t offset = uintptr_t(p) - untype(to_addr);
          volatile zpointer *from_p = (volatile zpointer*)untype(from_addr + offset);
          const zpointer prev = AtomicAccess::load(from_p);
          // It is impossible for the below load barrier to require relocation. If the mutator beat
          // us to it with setting the current bit with its store barrier, then the forwarding table
          // for the initial previous value that we have in prev, is guaranteed to have been relocated
          // already by the mutator.
          const zaddress addr = ZBarrier::load_barrier_on_oop_field_preloaded(nullptr, prev);
          ZGeneration::young()->on_failed_remember(addr);
        }
      };
      // TODO: This Pair-wise oop iteration is something we could have a general iterator for,
      //       we added something similar in ZClonerOopClousre.
      ZIterator::basic_oop_iterate(to_oop(to_addr), doit);
      ZGeneration::young()->on_promotion(to_addr);
    } else {
      ZIterator::basic_oop_iterate(to_oop(to_addr), update_remset_promoted_filter_and_remap_per_field);
    }
  }

  void update_remset_for_fields(zaddress from_addr, zaddress to_addr, bool was_mutator) const {
    if (_forwarding->is_young_to_young()) {
      // No remembered set in young pages
      return;
    }

    // Need to deal with remset when moving objects to the old generation
    if (_forwarding->is_old_to_old()) {
      update_remset_old_to_old(from_addr, to_addr, was_mutator);
      ZGeneration::young()->on_old_to_old(to_addr, was_mutator);
      return;
    }

    // Normal promotion
    update_remset_promoted(from_addr, to_addr);
  }

  void maybe_string_dedup(zaddress to_addr) {
    if (_forwarding->is_promotion()) {
      // Only deduplicate promoted objects, and let short-lived strings simply die instead.
      _string_dedup_context.request(to_oop(to_addr));
    }
  }

  bool try_relocate_object(zaddress from_addr, uint32_t partition_id) {
    const zaddress to_addr = try_relocate_object_inner(from_addr, partition_id);

    if (is_null(to_addr)) {
      return false;
    }

    maybe_string_dedup(to_addr);

    return true;
  }

  ZPage* start_in_place_relocation(zoffset relocated_watermark) {
    _forwarding->in_place_relocation_claim_page();
    _forwarding->in_place_relocation_start(relocated_watermark);

    ZPage* const from_page = _forwarding->page();
    const bool promotion = _forwarding->is_promotion();

    // Old In-place relocation always happens through a new cloned page
    ZPage* const to_page = from_page->inplace_relocate_page();
    postcond(_forwarding->is_young_to_young() || to_page != from_page);

    // Reset page for in-place relocation
    to_page->reset_top_for_allocation();

    // Verify that the inactive remset is clear when resetting the page for
    // in-place relocation.
    if (from_page->is_old()) {
      const bool young_marks = ZGeneration::old()->active_remset_is_current();

      if (ZGeneration::old()->active_remset_is_current()) {
        to_page->verify_remset_cleared_previous();
      } else {
        to_page->verify_remset_cleared_current();
      }
    }

    if (to_page->is_old()) {
      // TODO: IMPORTANT ensure we don't have a memory leak for the previous page
      ZHeap::heap()->in_place_replace_page(from_page, to_page);
    }

    if (promotion) {
      // Register the promotion
      ZGeneration::young()->in_place_relocate_promote(from_page, to_page);
      ZGeneration::young()->register_in_place_relocate_promoted(from_page);
    }

    return to_page;
  }

  void relocate_object(oop obj) {
    const zaddress addr = to_zaddress(obj);
    assert(ZHeap::heap()->is_object_live(addr), "Should be live");

    const ZPageAge to_age = _forwarding->to_age();
    const uint32_t partition_id = _forwarding->partition_id();

    while (!try_relocate_object(addr, partition_id)) {
      // Failed to relocate object, try to allocate a new target page,
      // or if that fails, use the page being relocated as the new target,
      // which will cause it to be relocated in-place.
      ZPage* const target_page = _targets->get(partition_id, to_age);
      ZPage* to_page = _allocator->alloc_and_retire_target_page(_forwarding, target_page);
      _targets->set(partition_id, to_age, to_page);

      // We got a new page, retry relocation
      if (to_page != nullptr) {
        continue;
      }

      // Start in-place relocation to block other threads from accessing
      // the page, or its forwarding table, until it has been released
      // (relocation completed).
      to_page = start_in_place_relocation(ZAddress::offset(addr));
      _targets->set(partition_id, to_age, to_page);
    }
  }

public:
  ZRelocateWork(Allocator* allocator, ZRelocationTargets* targets, ZGeneration* generation)
    : _allocator(allocator),
      _forwarding(nullptr),
      _targets(targets),
      _generation(generation),
      _freelist_promoted(),
      _freelist_compacted(),
      _mutator_promoted(),
      _mutator_compacted() {}

  ~ZRelocateWork() {
    _targets->apply_and_clear_targets([&](ZPage* page) {
        _allocator->free_target_page(page);
    });

    // Report statistics on-behalf of non-worker threads
    for (uint i = 0; i < ZPageTypeCount; i++) {
      const ZPageType type = static_cast<ZPageType>(i);
      _generation->increase_freelist_promoted(type, _freelist_promoted[i]);
      _generation->increase_freelist_compacted(type, _freelist_compacted[i]);
      _generation->increase_mutator_promoted(type, _mutator_promoted[i]);
      _generation->increase_mutator_compacted(type, _mutator_compacted[i]);
    }
  }

  void finish_in_place_relocation() {
    // We are done with the from_space copy of the page
    _forwarding->in_place_relocation_finish();
  }

  void do_forwarding(ZForwarding* forwarding) {
    _forwarding = forwarding;

    _forwarding->page()->log_msg(" (relocate page)");

    ZVerify::before_relocation(_forwarding);

    // Relocate objects
    _forwarding->object_iterate([&](oop obj) { relocate_object(obj); });

    ZVerify::after_relocation(_forwarding);

    // Verify
    if (ZVerifyForwarding) {
      _forwarding->verify();
    }

    _generation->increase_freed(_forwarding->page()->size());

    // Deal with in-place relocation
    const bool in_place = _forwarding->in_place_relocation();
    if (in_place) {
      finish_in_place_relocation();
    }

    // Old from-space pages need to deal with remset bits
    if (_forwarding->is_old_to_old()) {
      _forwarding->relocated_remembered_fields_after_relocate();
    }

    // Release relocated page
    _forwarding->release_page();

    if (in_place) {
      // Wait for all other threads to call release_page
      ZPage* const page = _forwarding->detach_page();
      // TODO: Check that we don't leak the from-space page during
      // in-place relocation.

      page->log_msg(" (relocate page done in-place)");

      // Different pages when promoting
      const uint32_t target_partition = _forwarding->partition_id();
      ZPage* const target_page = _targets->get(target_partition, _forwarding->to_age());
      _allocator->share_target_page(target_page, target_partition);
    } else {
      // Wait for all other threads to call release_page
      ZPage* const page = _forwarding->detach_page();

      page->log_msg(" (relocate page done normal)");

      // Free page
      ZHeap::heap()->free_page(page);
    }
  }
};

class ZRelocateTask : public ZRestartableTask {
private:
  ZGeneration* const                        _generation;
  ZRelocateQueue* const                     _queue;
  ZPerNUMA<ZRelocationSetParallelIterator>* _iters;
  ZPerWorker<ZRelocationTargets>*           _small_targets;
  ZPerWorker<ZRelocationTargets>*           _medium_targets;
  ZRelocateSmallAllocator                   _small_allocator;
  ZRelocateMediumAllocator                  _medium_allocator;
  const size_t                              _total_forwardings;

public:
  ZRelocateTask(ZRelocationSet* relocation_set,
                ZRelocateQueue* queue,
                ZPerNUMA<ZRelocationSetParallelIterator>* iters,
                ZPerWorker<ZRelocationTargets>* small_targets,
                ZPerWorker<ZRelocationTargets>* medium_targets,
                ZRelocationTargets* shared_medium_targets)
    : ZRestartableTask("ZRelocateTask"),
      _generation(relocation_set->generation()),
      _queue(queue),
      _iters(iters),
      _small_targets(small_targets),
      _medium_targets(medium_targets),
      _small_allocator(_generation),
      _medium_allocator(_generation, shared_medium_targets),
      _total_forwardings(relocation_set->nforwardings()) {

    for (uint32_t i = 0; i < ZNUMA::count(); i++) {
      ZRelocationSetParallelIterator* const iter = _iters->addr(i);

      // Destruct the iterator from the previous GC-cycle, which is a temporary
      // iterator if this is the first GC-cycle.
      iter->~ZRelocationSetParallelIterator();

      // In-place construct the iterator with the current relocation set
      ::new (iter) ZRelocationSetParallelIterator(relocation_set);
    }
  }

  ~ZRelocateTask() {
    _generation->stat_relocation()->at_relocate_end(_small_allocator.in_place_count(), _medium_allocator.in_place_count());

    // Signal that we're not using the queue anymore. Used mostly for asserts.
    _queue->deactivate();
  }

  virtual void work() {
    ZRelocateWork<ZRelocateSmallAllocator> small(&_small_allocator, _small_targets->addr(), _generation);
    ZRelocateWork<ZRelocateMediumAllocator> medium(&_medium_allocator, _medium_targets->addr(), _generation);

    const uint32_t num_nodes = ZNUMA::count();
    const uint32_t start_node = ZNUMA::id();
    uint32_t current_node = start_node;
    bool has_affinity = false;
    bool has_affinity_current_node = false;

    const auto do_forwarding = [&](ZForwarding* forwarding) {
      ZPage* const page = forwarding->page();
      if (page->is_small()) {
        small.do_forwarding(forwarding);
      } else {
        medium.do_forwarding(forwarding);
      }

      // Absolute last thing done while relocating a page.
      //
      // We don't use the SuspendibleThreadSet when relocating pages.
      // Instead the ZRelocateQueue is used as a pseudo STS joiner/leaver.
      //
      // After the mark_done call a safepointing could be completed and a
      // new GC phase could be entered.
      forwarding->mark_done();
    };

    const auto claim_and_do_forwarding = [&](ZForwarding* forwarding) {
      if (forwarding->claim()) {
        do_forwarding(forwarding);
      }
    };

    const auto check_numa_local = [&](ZForwarding* forwarding, uint32_t numa_id) {
      return forwarding->partition_id() == numa_id;
    };

    const auto do_forwarding_one_from_iter = [&]() {
      ZForwarding* forwarding;

      for (;;) {
        if (_iters->get(current_node).next_if(&forwarding, check_numa_local, current_node)) {
          // Set thread affinity for NUMA-local processing (if needed)
          if (UseNUMA && !has_affinity_current_node) {
            os::numa_set_thread_affinity(Thread::current(), ZNUMA::numa_id_to_node(current_node));
            has_affinity = true;
            has_affinity_current_node = true;
          }

          // Perform the forwarding task
          claim_and_do_forwarding(forwarding);
          return true;
        }

        // No work found on the current node, move to the next node
        current_node = (current_node + 1) % num_nodes;
        has_affinity_current_node = false;

        // If we've looped back to the starting node there's no more work to do
        if (current_node == start_node) {
          return false;
        }
      }
    };

    for (;;) {
      // As long as there are requests in the relocate queue, there are threads
      // waiting in a VM state that does not allow them to be blocked. The
      // worker thread needs to finish relocate these pages, and allow the
      // other threads to continue and proceed to a blocking state. After that,
      // the worker threads are allowed to safepoint synchronize.
      for (ZForwarding* forwarding; (forwarding = _queue->synchronize_poll()) != nullptr;) {
        do_forwarding(forwarding);
      }

      if (!do_forwarding_one_from_iter()) {
        // No more work
        break;
      }

      if (_generation->should_worker_resize()) {
        break;
      }
    }

    _queue->leave();

    if (UseNUMA && has_affinity) {
      // Restore the affinity of the thread so that it isn't bound to a specific
      // node any more
      os::numa_set_thread_affinity(Thread::current(), -1);
    }
  }

  virtual void resize_workers(uint nworkers) {
    _queue->resize_workers(nworkers);
  }
};

static void remap_and_maybe_add_remset(volatile zpointer* p) {
  const zpointer ptr = AtomicAccess::load(p);

  if (ZPointer::is_store_good(ptr)) {
    // Already has a remset entry
    return;
  }

  // Remset entries are used for two reasons:
  // 1) Young marking old-to-young pointer roots
  // 2) Deferred remapping of stale old-to-young pointers
  //
  // This load barrier will up-front perform the remapping of (2),
  // and the code below only has to make sure we register up-to-date
  // old-to-young pointers for (1).
  const zaddress addr = ZBarrier::load_barrier_on_oop_field_preloaded(p, ptr);

  if (is_null(addr)) {
    // No need for remset entries for null pointers
    return;
  }

  if (ZHeap::heap()->is_old(addr)) {
    // No need for remset entries for pointers to old gen
    return;
  }

  ZRelocate::add_remset(p);
}

class ZRelocateAddRemsetForFlipPromoted : public ZRestartableTask {
private:
  ZStatTimerYoung                _timer;
  ZArrayParallelIterator<ZPage*> _iter;

public:
  ZRelocateAddRemsetForFlipPromoted(ZArray<ZPage*>* pages)
    : ZRestartableTask("ZRelocateAddRemsetForFlipPromoted"),
      _timer(ZSubPhaseConcurrentRelocateRememberedSetFlipPromotedYoung),
      _iter(pages) {}

  virtual void work() {
    SuspendibleThreadSetJoiner sts_joiner;
    ZStringDedupContext        string_dedup_context;

    for (ZPage* page; _iter.next(&page);) {
      page->object_iterate([&](oop obj) {
        if (ZOldRefCount) {
          // Remap oops and add remset if needed
          auto doit = [&](volatile zpointer* p) {
            ZGeneration::young()->remember(p);
          };
          ZIterator::basic_oop_iterate_safe(obj, doit);
          ZGeneration::young()->on_promotion(to_zaddress(obj));
        } else {
          ZIterator::basic_oop_iterate_safe(obj, obj->klass(), remap_and_maybe_add_remset);
        }

        // String dedup
        string_dedup_context.request(obj);
      });

      SuspendibleThreadSet::yield();
      if (ZGeneration::young()->should_worker_resize()) {
        return;
      }
    }
  }
};

void ZRelocate::relocate(ZRelocationSet* relocation_set) {
  {
    ZRelocateTask relocate_task(relocation_set, &_queue, &_iters, &_small_targets, &_medium_targets, &_shared_medium_targets);
    workers()->run(&relocate_task);
  }

  if (relocation_set->generation()->is_young()) {
    ZRelocateAddRemsetForFlipPromoted task(relocation_set->flip_promoted_pages());
    workers()->run(&task);
  }
}

ZPageAge ZRelocate::compute_to_age(ZPageAge from_age) {
  if (is_old(from_age)) {
    return ZPageAge::old;
  }

  const uint age = untype(from_age);
  if (age >= ZGeneration::young()->tenuring_threshold()) {
    return ZPageAge::promotion;
  }

  return to_zpageage(age + 1);
}

class ZFlipAgeYoungPagesTask : public ZTask {
private:
  ZArrayParallelIterator<ZPage*> _iter;

public:
  ZFlipAgeYoungPagesTask(const ZArray<ZPage*>* pages)
    : ZTask("ZFlipAgeYoungPagesTask"),
      _iter(pages) {}

  virtual void work() {
    SuspendibleThreadSetJoiner sts_joiner;
    ZArray<ZPage*> promoted_pages;

    for (ZPage* prev_page; _iter.next(&prev_page);) {
      const ZPageAge to_age = ZRelocate::compute_to_age(prev_page->age());
      postcond(to_age != ZPageAge::old);

      // Figure out if this is proper promotion
      const bool promotion = to_age == ZPageAge::promotion;

      // Logging
      prev_page->log_msg(promotion ? " (flip promoted)" : " (flip survived)");

      // Setup to-space page
      ZPage* const new_page = prev_page->flip_age();

      // Reset page for flip aging
      new_page->reset_livemap();

      if (new_page->is_flip_promoted_current_young_collection()) {
        ZGeneration::young()->flip_promote(prev_page, new_page);
        // Defer promoted page registration
        promoted_pages.push(prev_page);
      }

      SuspendibleThreadSet::yield();
    }

    if (promoted_pages.length() != 0) {
      ZGeneration::young()->register_flip_promoted(promoted_pages);
    }
  }
};

template <typename Function>
static size_t object_iterate_and_construct_free_list(const ZPage* prev_page, ZPage* aged_page, Function function) {
  precond(aged_page->virtual_memory() == prev_page->virtual_memory());
  precond(aged_page->is_flip_aged());
  precond(aged_page->is_old());
  precond(aged_page->age() != ZPageAge::old || ZMaintainOldFreeLists);
  precond(aged_page->age() != ZPageAge::promotion || ZFlipPromotionFreeLists);

  size_t free_list_available = 0;

  const size_t page_object_alignment = aged_page->object_alignment();
  zoffset_end next_potential_free_start = to_zoffset_end(aged_page->start());

  prev_page->object_iterate([&](oop obj) {
    // Invoke iteration callback
    function(obj);

    if (prev_page->is_large()) {
      // No free lists for large pages
      return;
    }

    // Build free-list
    precond(is_aligned(untype(to_zaddress(obj)), page_object_alignment));

    const zoffset obj_offset = ZAddress::offset(to_zaddress(obj));
    const size_t aligned_object_size = align_up(ZUtils::object_size(to_zaddress(obj)), page_object_alignment);

    if (next_potential_free_start != obj_offset) {
      const zoffset free_start = to_zoffset(next_potential_free_start);
      const size_t free_size = obj_offset - free_start;
      aged_page->free_object_to_free_list(ZOffset::address_unsafe(free_start), free_size);
      free_list_available += free_size;
    }

    // Set next potential start
    next_potential_free_start = to_zoffset_end(obj_offset, aligned_object_size);
  });

  if (prev_page->is_large()) {
    // No free lists for large pages
    postcond(free_list_available == 0);
    return 0;
  }

  postcond(next_potential_free_start != to_zoffset_end(aged_page->start()));
  postcond(next_potential_free_start <= aged_page->end());

  if (next_potential_free_start != aged_page->end()) {
    if (aged_page->end() != aged_page->top()) {
      // Alloc the tail
      [[maybe_unused]] zaddress addr = aged_page->alloc_object(aged_page->remaining());
    }
    // Free tail
    const zoffset free_start = to_zoffset(next_potential_free_start);
    const size_t free_size = aged_page->end() - free_start;
    aged_page->free_object_to_free_list(ZOffset::address_unsafe(free_start), free_size);
    free_list_available += free_size;
  }

  if (ZAllocateInOldFreeList && aged_page->age() == ZPageAge::old) {
    // Register the page
    ZGeneration::young()->register_old_alloction_page(aged_page);
  }

  return free_list_available;
}

class ZFlipAgeOldPagesTask : public ZRestartableTask {
private:
  ZArrayParallelIterator<ZPage*> _not_selected_iter;
  ZArrayParallelIterator<ZPage*> _selected_iter;
  Atomic<size_t>                 _free_list_available[ZPageTypeCount];

public:
  ZFlipAgeOldPagesTask(const ZArray<ZPage*>* not_selected_pages, const ZArray<ZPage*>* selected_pages)
    : ZRestartableTask("ZFlipAgeOldPagesTask"),
      _not_selected_iter(not_selected_pages),
      _selected_iter(selected_pages),
      _free_list_available() {}

  ~ZFlipAgeOldPagesTask() {
    for (uint i = 0; i < ZPageTypeCount; i++) {
      ZGeneration::old()->increase_freelist_available_at_start(static_cast<ZPageType>(i), _free_list_available[i].load_relaxed());
    }
  }

  virtual void work() {
    SuspendibleThreadSetJoiner sts_joiner;

    for (ZPage* prev_page; _not_selected_iter.next(&prev_page);) {
      prev_page->log_msg(" (flip survived)");
      precond(prev_page->is_old());
      precond(prev_page->is_relocatable());

      const uint32_t young_marks = ZGeneration::old()->young_marks_since_old_mark_end();

      if (young_marks <= 1) {
        // At thnot_selected_is point, there might be dead remembered set entries. We must prune them
        // befornot_selected_e flipping the page to become is_allocating, so that concurrent remembered
        // set snot_selected_canning doesn't scan dead remembered set entries.
        prev_page->prune_dead_remset_entries();
      }

      ZPage* const aged_page = prev_page->flip_age();

      ZGeneration::old()->flip_survive(prev_page, aged_page);

      auto function = [&](oop obj) {
        ZGeneration::young()->on_old_to_old(to_zaddress(obj), false /* was_mutator */); // TODO: More naming issues
      };

      if (ZOldRefCount && ZMaintainOldFreeLists) {
        ZStatTimerWorker timer(ZSubPhaseConcurrentFreeListPageOld);

        _free_list_available[untype(aged_page->type())].add_then_fetch(object_iterate_and_construct_free_list(prev_page, aged_page, function), memory_order_relaxed);
      } else {
        prev_page->object_iterate(function);
      }

      if (ZGeneration::old()->should_worker_resize()) {
        return;
      }

      SuspendibleThreadSet::yield();
    }

    for (ZPage* page; _selected_iter.next(&page);) {
      const uint32_t young_marks = ZGeneration::old()->young_marks_since_old_mark_end();

      if (young_marks <= 1) {
        // At thnot_selected_is point, there might be dead remembered set entries. We must prune them
        // befornot_selected_e flipping the page to become is_allocating, so that concurrent remembered
        // set snot_selected_canning doesn't scan dead remembered set entries.
        page->prune_dead_remset_entries();
      }

      if (ZGeneration::old()->should_worker_resize()) {
        return;
      }

      SuspendibleThreadSet::yield();
    }
  }
};

class ZPromoteBarrierTask : public ZRestartableTask {
private:
  ZArrayParallelIterator<ZPage*> _flip_promoted_iter;
  ZArrayParallelIterator<ZPage*> _relocate_promoted_iter;
  Atomic<size_t>                 _free_list_available[ZPageTypeCount];

public:
  ZPromoteBarrierTask(const ZArray<ZPage*>* flip_promoted_pages,
                      const ZArray<ZPage*>* relocate_promoted_pages)
    : ZRestartableTask("ZPromoteBarrierTask"),
      _flip_promoted_iter(flip_promoted_pages),
      _relocate_promoted_iter(relocate_promoted_pages),
      _free_list_available() {}

  ~ZPromoteBarrierTask() {
    for (uint i = 0; i < ZPageTypeCount; i++) {
      ZGeneration::young()->increase_freelist_available_at_start(static_cast<ZPageType>(i), _free_list_available[i].load_relaxed());
    }
  }

  virtual void work() {
    SuspendibleThreadSetJoiner sts_joiner;

    auto promote_barriers = [&](ZArrayParallelIterator<ZPage*>* iter) {
      for (ZPage* page; iter->next(&page);) {
        // When promoting an object (and before relocate start), we must ensure that all
        // contained zpointers are store good. The marking code ensures that for non-null
        // pointers, but null pointers are ignored. This code ensures that even null pointers
        // are made store good, for the promoted objects.
        auto promote = [&](oop obj) {
          ZIterator::basic_oop_iterate_safe(obj, ZBarrier::promote_barrier_on_young_oop_field);
        };

        if (ZOldRefCount && ZFlipPromotionFreeLists && iter == &_flip_promoted_iter) {
          ZStatTimerWorker timer(ZSubPhaseConcurrentFreeListPageYoung);

          ZPage* const aged_page = ZHeap::heap()->page(ZOffset::address(page->start()));

          _free_list_available[untype(aged_page->type())].add_then_fetch(object_iterate_and_construct_free_list(page, aged_page, promote), memory_order_relaxed);
        } else {
          page->object_iterate(promote);
        }

        if (ZGeneration::young()->should_worker_resize()) {
          return;
        }

        SuspendibleThreadSet::yield();
      }
    };

    promote_barriers(&_flip_promoted_iter);
    if (ZGeneration::young()->should_worker_resize()) {
      return;
    }
    promote_barriers(&_relocate_promoted_iter);
  }
};

// TODO: Move to separate file, merging with the ZGeneration rendezvous code?
class ZRendezvousHandshakeClosure : public HandshakeClosure {
public:
  ZRendezvousHandshakeClosure()
    : HandshakeClosure("ZRendezvous") {}

  void do_thread(Thread* thread) {
    // Does nothing
  }
};

class ZRendezvousGCThreads: public VM_Operation {
 public:
  VMOp_Type type() const { return VMOp_ZRendezvousGCThreads; }

  virtual bool evaluate_at_safepoint() const {
    // We only care about synchronizing the GC threads.
    // Leave the Java threads running.
    return false;
  }

  virtual bool skip_thread_oop_barriers() const {
    fatal("Concurrent VMOps should not call this");
    return true;
  }

  virtual bool is_gc_operation() const {
    return true;
  }

  void doit() {
    // Light weight "handshake" of the GC threads
    SuspendibleThreadSet::synchronize();
    SuspendibleThreadSet::desynchronize();
  };
};

void ZRelocate::flip_age_old_pages(ZPageAllocator* page_allocator, const ZArray<ZPage*>* not_selected_pages, const ZArray<ZPage*>* selected_pages) {
  ZFlipAgeOldPagesTask flip_age_task(not_selected_pages, selected_pages);
  workers()->run(&flip_age_task);
  // TODO: Remove rendezvous code when we have better ZPage SMR.

  // Perform a handshake to make sure concurrent threads are not operating on stale
  // pages from before flip aging before we destroy them.
  ZRendezvousHandshakeClosure cl;
  Handshake::execute(&cl);

  // GC threads are not part of the handshake above.
  // Explicitly "handshake" them.
  ZRendezvousGCThreads op;
  VMThread::execute(&op);

  for (int i = 0; i < not_selected_pages->length(); i++) {
    // Delete non-relocating promoted pages from last cycle
    ZPage* const page = not_selected_pages->at(i);
    page_allocator->safe_destroy_page(page);
  }
}

void ZRelocate::flip_age_young_pages(const ZArray<ZPage*>* pages) {
  ZFlipAgeYoungPagesTask flip_age_task(pages);
  workers()->run(&flip_age_task);
}

void ZRelocate::barrier_promoted_pages(const ZArray<ZPage*>* flip_promoted_pages,
                                       const ZArray<ZPage*>* relocate_promoted_pages) {
  ZPromoteBarrierTask promote_barrier_task(flip_promoted_pages, relocate_promoted_pages);
  workers()->run(&promote_barrier_task);
}

void ZRelocate::synchronize() {
  _queue.synchronize();
}

void ZRelocate::desynchronize() {
  _queue.desynchronize();
}

ZRelocateQueue* ZRelocate::queue() {
  return &_queue;
}

bool ZRelocate::is_queue_active() const {
  return _queue.is_active();
}
