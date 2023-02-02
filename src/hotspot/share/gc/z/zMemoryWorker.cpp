/*
 * Copyright (c) 2025, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/z/zAdaptiveHeap.inline.hpp"
#include "gc/z/zGlobals.hpp"
#include "gc/z/zLock.inline.hpp"
#include "gc/z/zMemoryWorker.hpp"
#include "gc/z/zNUMA.inline.hpp"
#include "gc/z/zPage.inline.hpp"
#include "gc/z/zPageAllocator.hpp"
#include "gc/z/zVirtualMemory.inline.hpp"
#include "runtime/init.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/rbTree.inline.hpp"

RBTreeOrdering ZHeatingRequestTreeComparator::cmp(zoffset first, zoffset second) {
  if (first < second) {
    // Start before second
    return RBTreeOrdering::LT;
  }

  if (first > second) {
    // Start after second
    return RBTreeOrdering::GT;
  }

  // Same position
  return RBTreeOrdering::EQ;
}

bool ZMemoryWorker::is_enabled() {
  return ZAdaptiveHeap::can_adapt() || ZMemoryHeating;
}

ZMemoryWorker::ZMemoryWorker(uint32_t id, ZPartition* partition)
  : _id(id),
    _partition(partition),
    _lock(),
    _heating_requests(),
    _target_commit_capacity(0),
    _target_uncommit_capacity(0),
    _uncommit_request_start(Ticks::now()),
    _stop(false),
    _currently_heating() {
  if (!is_enabled()) {
    // Disabled, do not start.
    _stop = true;
    return;
  }
  set_name("ZMemoryWorker#%u", _id);
  create_and_start();

  if (ZNUMA::is_enabled()) {
    // If NUMA is enabled we set the affinity of the thread to CPUs associated
    // with the partition that the ZMemoryWorker will work on.
    os::numa_set_thread_affinity(this, ZNUMA::numa_id_to_node(_id));
  }
}

bool ZMemoryWorker::is_stop_requested() {
  ZLocker<ZConditionLock> locker(&_lock);
  return _stop;
}

size_t ZMemoryWorker::commit_granule(size_t capacity, size_t target_capacity) {
  const size_t smallest_granule = ZGranuleSize;
  const size_t largest_granule = MAX2(ZPageSizeMediumMax, smallest_granule);

  return clamp(align_up(target_capacity / 128, ZGranuleSize), smallest_granule, largest_granule);
}

size_t ZMemoryWorker::uncommit_granule() {
  const size_t smallest_granule = ZGranuleSize;
  const size_t largest_granule = MAX2(ZPageSizeMediumMax, smallest_granule);

  // Don't allocate things that are larger than the largest medium page size, in the lower address space
  return largest_granule;
}

bool ZMemoryWorker::should_heat() {
  ZLocker<ZConditionLock> locker(&_lock);
  return has_heating_request();
}

bool ZMemoryWorker::has_heating_request() {
  return _heating_requests.size() != 0;
}

bool ZMemoryWorker::peek() {
  for (;;) {
    uint64_t uncommit_delay = ZAdaptiveHeap::uncommit_delay();
    Ticks now = Ticks::now();

    ZLocker<ZConditionLock> locker(&_lock);
    if (_stop) {
      return false;
    }

    if (ZAdaptiveHeap::can_adapt()) {
      const size_t target_commit_capacity = _target_commit_capacity.load_relaxed();
      const size_t target_uncommit_capacity = _target_uncommit_capacity.load_relaxed();

      if (target_commit_capacity != 0) {
        // Request to commit memory
        return true;
      }

      if (has_uncommit_matured(now, uncommit_delay, target_uncommit_capacity)) {
        // Matured request to uncommit memory
        return true;
      }
    }

    if (ZMemoryHeating && has_heating_request()) {
      return true;
    }

    _lock.wait();
  }
}

void ZMemoryWorker::stop_heap_resizing() {
  // Remove requests to increase the capacity
  ZLocker<ZConditionLock> locker(&_lock);
  _target_commit_capacity.store_relaxed(0u);
  _target_uncommit_capacity.store_relaxed(0u);
}

void ZMemoryWorker::request_grow_capacity(size_t requested_capacity) {
  precond(ZAdaptiveHeap::can_adapt());

  ZLocker<ZConditionLock> locker(&_lock);
  _target_commit_capacity.store_relaxed(requested_capacity);
  _target_uncommit_capacity.store_relaxed(0u);
  _lock.notify_all();
}

void ZMemoryWorker::request_shrink_capacity(size_t requested_capacity) {
  precond(ZAdaptiveHeap::can_adapt());

  Ticks now = Ticks::now();

  ZLocker<ZConditionLock> locker(&_lock);
  _target_uncommit_capacity.store_relaxed(requested_capacity);
  _target_commit_capacity.store_relaxed(0u);
  if (_uncommit_request_start.value() == 0) {
    _uncommit_request_start = now;
  }
  _lock.notify_all();
}

void ZMemoryWorker::request_shrink_capacity_granule() {
  precond(ZAdaptiveHeap::can_adapt());

  Ticks now = Ticks::now();

  ZLocker<ZConditionLock> locker(&_lock);
  _target_commit_capacity.store_relaxed(0u);
  if (_uncommit_request_start.value() == 0) {
    _uncommit_request_start = now;
  }
  _lock.notify_all();
}

void ZMemoryWorker::consume_grow_request(size_t new_capacity) {
  ZLocker<ZConditionLock> locker(&_lock);

  const size_t target_capacity = _target_uncommit_capacity.load_relaxed();

  if (new_capacity >= target_capacity) {
    _target_commit_capacity.store_relaxed(0);
  }
}

bool ZMemoryWorker::consume_shrink_request(size_t new_capacity, uint64_t uncommit_delay) {
  ZLocker<ZConditionLock> locker(&_lock);

  // Shrinking request has been satisfied for now.
  // The director will remind us to shrink more if necessary.
  const size_t target_capacity = _target_uncommit_capacity.load_relaxed();

  if (target_capacity == 0) {
    _uncommit_request_start = Ticks();
  } else if (new_capacity <= target_capacity) {
    _uncommit_request_start = Ticks();
    _target_uncommit_capacity.store_relaxed(0);
  }

  // Trigger more urgent uncommitting if necessary
  return uncommit_delay <= ZAdaptiveHeap::urgent_uncommit_delay();
}

bool ZMemoryWorker::has_uncommit_matured(Ticks now, uint64_t uncommit_delay, uint64_t requested_capacity) {
  if (_uncommit_request_start.value() == 0) {
    // No current uncommit request.
    return false;
  }

  const uint64_t elapsed = (now - _uncommit_request_start).milliseconds();

  if (requested_capacity != 0) {
    // Long term shrinking as requested by a GC should shrink fairly quickly
    // regardless of memory pressure.
    return elapsed >= MIN2(uint64_t(10), uncommit_delay);
  }

  // Short term shrinking as requested by the director should shrink according
  // to the memory pressure based uncommit delay schedule.
  return elapsed >= uncommit_delay;
}

void ZMemoryWorker::wake_up_if_uncommit_matured() {
  Ticks now = Ticks::now();
  const uint64_t uncommit_delay = ZAdaptiveHeap::uncommit_delay();

  ZLocker<ZConditionLock> locker(&_lock);
  if (has_uncommit_matured(now, uncommit_delay, _target_uncommit_capacity.load_relaxed())) {
    _lock.notify_all();
  }
}

void ZMemoryWorker::remove_heating_request_range(const ZVirtualMemory& vmem) {
  ZArray<ZHeatingRequestNode*> to_remove;

  const zoffset start_inclusive = vmem.start();
  const zoffset end_inclusive = vmem.start() + (vmem.size() - ZGranuleSize);
  _heating_requests.visit_range_in_order(start_inclusive, end_inclusive, [&](const ZHeatingRequestNode* node) {
    // Const cast the node, we only use it to modify the tree after
    // visit_range_in_order is completed.
    to_remove.push(const_cast<ZHeatingRequestNode*>(node));
    return true;
  });

  for (ZHeatingRequestNode* node: to_remove) {
    _heating_requests.remove(node);
  }
}

void ZMemoryWorker::register_heating_request(const ZVirtualMemory& vmem) {
  precond(ZMemoryHeating);

  ZLocker<ZConditionLock> locker(&_lock);
  if (_stop) {
    // Don't add more requests during termination
    return;
  }

  while (!_currently_heating.is_null() && vmem.overlaps(_currently_heating)) {
    // Wait while currently heating overlaps with this vmem
    _lock.wait();
  }

  zoffset insert_start = vmem.start();
  zoffset_end insert_end = vmem.end();

  // Merge adjacent on the left
  ZHeatingRequestNode* left_node = _heating_requests.closest_leq(vmem.start());
  if (left_node != nullptr) {
    ZVirtualMemory closest_left(left_node->key(), left_node->val());
    if (closest_left.end() == vmem.start()) {
      insert_start = closest_left.start();
      _heating_requests.remove(left_node);
    }
  }

  // Merge adjacent on the right
  if (size_t(vmem.end()) < ZAddressOffsetMax) {
    ZHeatingRequestNode* right_node = _heating_requests.find_node(vmem.start() + vmem.size());
    if (right_node != nullptr) {
      insert_end = zoffset_end(right_node->key()) + right_node->val();
      _heating_requests.remove(right_node);
    }
  }

  ZHeatingRequestNode* const new_node = _heating_requests.allocate_node(insert_start, size_t(insert_end) - size_t(insert_start));
  _heating_requests.insert(insert_start, new_node);
}

ZVirtualMemory ZMemoryWorker::pop_heating_request() {
  assert(has_heating_request(), "precondition");

  const size_t max_heating = 16 * ZGranuleSize;

  ZHeatingRequestNode* const node = _heating_requests.leftmost();
  const size_t size = node->val();

  // If the node is small enough, we remove it entirely from the tree. Otherwise,
  // we update the node's size in-place without removing and inserting it.

  if (size <= max_heating) {
    const ZVirtualMemory vmem(node->key(), size);
    _heating_requests.remove(node);
    return vmem;
  }

  const size_t size_remainder = size - max_heating;
  const ZVirtualMemory vmem(node->key() + size_remainder, max_heating);

  // Update the value of the node to the new size.
  node->set_val(size_remainder);

  return vmem;
}

void ZMemoryWorker::remove_heating_request(const ZVirtualMemory& vmem) {
  precond(ZMemoryHeating);

  ZLocker<ZConditionLock> locker(&_lock);

  while (!_currently_heating.is_null() && vmem.overlaps(_currently_heating)) {
    // Wait while currently heating overlaps with this vmem
    _lock.wait();
  }

  const uintptr_t vmem_start = (uintptr_t)vmem.start();
  const uintptr_t vmem_end = vmem_start + vmem.size();

  // Cut off overlap on the left
  if (vmem.start() > zoffset(0)) {
    ZHeatingRequestNode* left_node = _heating_requests.closest_leq(vmem.start() - ZGranuleSize);
    if (left_node != nullptr) {
      const uintptr_t left_start = (uintptr_t)left_node->key();
      const uintptr_t left_end = left_start + left_node->val();

      if (left_end > vmem_start) {
        // There is an intersection on the left side
        const size_t left_leading = vmem_start - left_start;
        _heating_requests.upsert(zoffset(left_start), left_leading);

        if (left_end > vmem_end) {
          // Intersection continues to the right side
          const size_t left_trailing = left_end - vmem_end;
          ZHeatingRequestNode* const new_node = _heating_requests.allocate_node(zoffset(vmem_end), left_trailing);
          _heating_requests.insert(zoffset(vmem_end), new_node);
        }
      }
    }
  }

  // Cut off overlap on the right
  ZHeatingRequestNode* right_node = _heating_requests.closest_leq(vmem.start() + vmem.size());
  if (right_node != nullptr) {
    const uintptr_t right_start = (uintptr_t)right_node->key();
    const uintptr_t right_end = right_start + right_node->val();

    if (right_start < vmem_end && right_end > vmem_end) {
      // There is an intersection on the right side
      _heating_requests.remove(right_node);
      const size_t right_trailing = right_end - vmem_end;
      ZHeatingRequestNode* const new_node = _heating_requests.allocate_node(zoffset(vmem_end), right_trailing);
      _heating_requests.insert(zoffset(vmem_end), new_node);
    }
  }

  remove_heating_request_range(vmem);
}

size_t ZMemoryWorker::process_heating_request() {
  SuspendibleThreadSetJoiner sts_joiner;
  ZVirtualMemory vmem;
  {
    ZLocker<ZConditionLock> locker(&_lock);
    if (!has_heating_request()) {
      // Unmapping removed the request; bail
      return 0;
    }

    assert(has_heating_request(), "who else processed it?");

    vmem = pop_heating_request();

    assert(_currently_heating.is_null(), "must be");
    _currently_heating = vmem;
  }

  _partition->heat_memory(vmem);

  {
    ZLocker<ZConditionLock> locker(&_lock);
    _currently_heating = {};
    _lock.notify_all();
  }

  return vmem.size();
}

void ZMemoryWorker::await_start() {
  for (;;) {
    ZLocker<ZConditionLock> locker(&_lock);
    if (_stop) {
      return;
    }

    if (is_init_completed()) {
      return;
    }

    // Don't start working until JVM is bootstrapped
    _lock.wait();
  }
}

void ZMemoryWorker::run_thread() {
  // Wait until started
  await_start();

  for (;;) {
    if (!peek()) {
      // Stop
      return;
    }

    size_t committed = 0;
    size_t uncommitted = 0;
    size_t heated = 0;
    size_t last_target_commit_capacity = 0;
    size_t last_target_uncommit_capacity = 0;
    size_t capacity = 0;

    for (;;) {

      if (is_stop_requested()) {
        return;
      }

      if (ZAdaptiveHeap::can_adapt()) {
        capacity = _partition->capacity();
        const size_t curr_max_capacity = _partition->current_max_capacity();
        const size_t target_commit_capacity = _target_commit_capacity.load_relaxed();
        const size_t target_uncommit_capacity = _target_uncommit_capacity.load_relaxed();
        const size_t maybe_commit = commit_granule(capacity, target_commit_capacity);
        const size_t maybe_uncommit = uncommit_granule();
        const ZMemoryPressureMetrics metrics = ZAdaptiveHeap::memory_pressure_metrics();

        if (last_target_commit_capacity != 0 && last_target_commit_capacity != target_commit_capacity) {
          // Printouts look better when flushing across target commit capacity changes
          break;
        }

        if (last_target_uncommit_capacity != 0 && last_target_uncommit_capacity != target_uncommit_capacity) {
          // Printouts look better when flushing across target uncommit capacity changes
          break;
        }

        last_target_commit_capacity = target_commit_capacity;
        last_target_uncommit_capacity = target_uncommit_capacity;

        // Prioritize committing memory if requested
        if (uncommitted == 0 && target_commit_capacity != 0 && target_commit_capacity > capacity) {
          const size_t processed = _partition->increase_and_commit_capacity(maybe_commit, target_commit_capacity);
          const size_t new_capacity = capacity + processed;

          committed += processed;

          // Growing request has potentially been satisfied
          consume_grow_request(new_capacity);

          if (processed > 0) {
            continue;
          }
        }

        // The secondary priority is uncommitting memory if requested
        bool should_uncommit;
        const uint64_t uncommit_delay = ZAdaptiveHeap::uncommit_delay();
        const Ticks now = Ticks::now();
        {
          ZLocker<ZConditionLock> locker(&_lock);
          should_uncommit = committed == 0 && has_uncommit_matured(now, uncommit_delay, target_uncommit_capacity);
        }

        if (should_uncommit) {
          const size_t processed = uncommit(maybe_uncommit);
          const size_t new_capacity = capacity - processed;

          uncommitted += processed;

          if (processed > 0) {
            if (consume_shrink_request(new_capacity, uncommit_delay)) {
              request_shrink_capacity_granule();
            }
            continue;
          }
        }
      }

      // Last priority is to heat pages to optimize access speed
      if (ZMemoryHeating && should_heat()) {
        const size_t processed = process_heating_request();

        heated += process_heating_request();

        if (processed > 0) {
          continue;
        }
      }

      break;
    }

    if (committed > 0) {
      log_info(gc, heap)("Memory Worker (%d) Committed: %zuM(%.0f%%)",
                         _id, committed / M, percent_of(committed, last_target_commit_capacity));
    }

    if (uncommitted > 0) {
      log_info(gc, heap)("Memory Worker (%d) Uncommitted: %zuM(%.0f%%)",
                         _id, uncommitted / M, percent_of(uncommitted, last_target_uncommit_capacity));
    }

    if (heated > 0) {
      log_info(gc, heap)("Memory Worker (%d) Heated: %zuM(%.0f%%)",
                         _id, heated / M, percent_of(heated, capacity));
    }
  }
}

size_t ZMemoryWorker::uncommit(size_t to_uncommit) {
  ZArray<ZVirtualMemory> flushed_vmems;
  size_t flushed = 0;

  {
    // We need to join the suspendible thread set while manipulating capacity
    // and used, to make sure GC safepoints will have a consistent view.
    SuspendibleThreadSetJoiner sts_joiner;
    ZLocker<ZLock> locker(&_partition->_page_allocator->_lock);

    ZMappedCache& cache = _partition->_cache;

    // Never uncommit below min capacity.
    const size_t retain = MAX2(_partition->_used, _partition->_min_capacity);
    const size_t release = _partition->_capacity - retain;
    const size_t flush = MIN2(release, to_uncommit);

    // Flush memory from the mapped cache for uncommit
    flushed = cache.remove_for_uncommit(flush, &flushed_vmems);
    if (flushed == 0) {
      // Nothing flushed
      return 0;
    }

    // Record flushed memory as claimed and how much we've flushed for this partition
    _partition->increase_claimed(flushed);
  }

  // Unmap and uncommit flushed memory
  for (const ZVirtualMemory vmem : flushed_vmems) {
    _partition->unmap_virtual(vmem);
    _partition->uncommit_physical(vmem);
    _partition->free_physical(vmem);
    _partition->free_virtual(vmem);
  }

  {
    SuspendibleThreadSetJoiner sts_joiner;
    ZLocker<ZLock> locker(&_partition->_page_allocator->_lock);

    // Adjust claimed and capacity to reflect the uncommit
    _partition->decrease_claimed(flushed);
    _partition->decrease_capacity(flushed);
  }

  if (flushed > 0) {
    _partition->_page_allocator->truncate_heuristic_max_after_capacity_decrease();
  }

  return flushed;
}

void ZMemoryWorker::terminate() {
  ZLocker<ZConditionLock> locker(&_lock);
  _stop = true;
  _lock.notify_all();

  _heating_requests.remove_all();

  while (!_currently_heating.is_null()) {
    // Trying to unmap what's currently being heated; calm down!
    _lock.wait();
  }
}
