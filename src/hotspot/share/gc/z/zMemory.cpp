/*
 * Copyright (c) 2015, 2025, Oracle and/or its affiliates. All rights reserved.
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

#include "gc/z/zAddress.inline.hpp"
#include "gc/z/zArray.inline.hpp"
#include "gc/z/zList.inline.hpp"
#include "gc/z/zLock.inline.hpp"
#include "gc/z/zMemory.inline.hpp"

template <typename Range>
class ZRangeNode : public CHeapObj<mtGC> {
  friend class ZList<ZRangeNode>;

private:
  using offset     = typename Range::offset;
  using offset_end = typename Range::offset_end;

  Range                 _range;
  ZListNode<ZRangeNode> _node;

public:
  ZRangeNode(offset start, size_t size)
    : _range(start, size),
      _node() {}

  ZRangeNode(const Range& other)
    : ZRangeNode(other.start(), other.size()) {}

  Range* range() {
    return &_range;
  }

  offset start() const {
    return _range.start();
  }

  offset_end end() const {
    return _range.end();
  }

  size_t size() const {
    return _range.size();
  }
};

template <typename Range>
void ZMemoryManagerImpl<Range>::move_into(const Range& range) {
  assert(!range.is_null(), "Invalid range");
  assert(check_limits(range), "Range outside limits");

  const offset start = range.start();
  const offset_end end = range.end();
  const size_t size = range.size();

  ZListIterator<ZMemory> iter(&_list);
  for (ZMemory* area; iter.next(&area);) {
    if (area->start() < start) {
      continue;
    }

    ZMemory* const prev = _list.prev(area);
    if (prev != nullptr && start == prev->end()) {
      if (end == area->start()) {
        // Merge with prev and current area
        grow_from_back(prev, size);
        grow_from_back(prev, area->size());
        _list.remove(area);
        delete area;
      } else {
        // Merge with prev area
        grow_from_back(prev, size);
      }
    } else if (end == area->start()) {
      // Merge with current area
      grow_from_front(area, size);
    } else {
      // Insert new area before current area
      assert(end < area->start(), "Areas must not overlap");
      ZMemory* const new_area = new ZMemory(start, size);
      _list.insert_before(area, new_area);
    }

    // Done
    return;
  }

  // Insert last
  ZMemory* const last = _list.last();
  if (last != nullptr && start == last->end()) {
    // Merge with last area
    grow_from_back(last, size);
  } else {
    // Insert new area last
    ZMemory* const new_area = new ZMemory(start, size);
    _list.insert_last(new_area);
  }
}

template <typename Range>
void ZMemoryManagerImpl<Range>::insert_inner(const Range& range) {
  if (_callbacks._insert != nullptr) {
    _callbacks._insert(range);
  }
  move_into(range);
}

template <typename Range>
void ZMemoryManagerImpl<Range>::register_inner(const Range& range) {
  move_into(range);
}

template <typename Range>
void ZMemoryManagerImpl<Range>::grow_from_front(ZMemory* area, size_t size) {
  if (_callbacks._grow != nullptr) {
    const Range from = *area->range();
    const Range to = Range(from.start() - size, from.size() + size);
    _callbacks._grow(from, to);
  }
  area->range()->grow_from_front(size);
}

template <typename Range>
void ZMemoryManagerImpl<Range>::grow_from_back(ZMemory* area, size_t size) {
  if (_callbacks._grow != nullptr) {
    const Range from = *area->range();
    const Range to = Range(from.start(), from.size() + size);
    _callbacks._grow(from, to);
  }
  area->range()->grow_from_back(size);
}

template <typename Range>
Range ZMemoryManagerImpl<Range>::shrink_from_front(ZMemory* area, size_t size) {
  if (_callbacks._shrink != nullptr) {
    const Range from = *area->range();
    const Range to = from.last_part(size);
    _callbacks._shrink(from, to);
  }
  return area->range()->shrink_from_front(size);
}

template <typename Range>
Range ZMemoryManagerImpl<Range>::shrink_from_back(ZMemory* area, size_t size) {
  if (_callbacks._shrink != nullptr) {
    const Range from = *area->range();
    const Range to = from.first_part(from.size() - size);
    _callbacks._shrink(from, to);
  }
  return area->range()->shrink_from_back(size);
}

template <typename Range>
Range ZMemoryManagerImpl<Range>::remove_from_low_inner(size_t size) {
  ZListIterator<ZMemory> iter(&_list);
  for (ZMemory* area; iter.next(&area);) {
    if (area->size() >= size) {
      // Found a match

      Range range;

      if (area->size() == size) {
        // Exact match, remove area
        _list.remove(area);
        range = *area->range();
        delete area;
      } else {
        // Larger than requested, shrink area
        range = shrink_from_front(area, size);
      }

      if (_callbacks._remove != nullptr) {
        _callbacks._remove(range);
      }

      return range;
    }
  }

  // Out of memory
  return Range();
}

template <typename Range>
Range ZMemoryManagerImpl<Range>::remove_from_low_at_most_inner(size_t size) {
  ZMemory* const area = _list.first();
  if (area != nullptr) {
    Range range;

    if (area->size() <= size) {
      // Smaller than or equal to requested, remove area
      _list.remove(area);
      range = *area->range();
      delete area;
    } else {
      // Larger than requested, shrink area
      range = shrink_from_front(area, size);
    }

    if (_callbacks._remove) {
      _callbacks._remove(range);
    }

    return range;
  }

  return Range();
}

template <typename Range>
size_t ZMemoryManagerImpl<Range>::remove_from_low_many_at_most_inner(size_t size, ZArray<Range>* out) {
  size_t to_remove = size;

  while (to_remove > 0) {
    const Range range = remove_from_low_at_most_inner(to_remove);

    if (range.is_null()) {
      // The requested amount is not available
      return size - to_remove;
    }

    to_remove -= range.size();
    out->append(range);
  }

  return size;
}

template <typename Range>
ZMemoryManagerImpl<Range>::Callbacks::Callbacks()
  : _insert(nullptr),
    _remove(nullptr),
    _grow(nullptr),
    _shrink(nullptr) {}

template <typename Range>
ZMemoryManagerImpl<Range>::ZMemoryManagerImpl()
  : _list(),
    _callbacks(),
    _limits() {}

template <typename Range>
void ZMemoryManagerImpl<Range>::register_callbacks(const Callbacks& callbacks) {
  _callbacks = callbacks;
}

template <typename Range>
void ZMemoryManagerImpl<Range>::register_range(const Range& range) {
  ZLocker<ZLock> locker(&_lock);
  register_inner(range);
}

template <typename Range>
bool ZMemoryManagerImpl<Range>::unregister_first(Range* out) {
  // This intentionally does not call the "remove" callback.
  // This call is typically used to unregister memory before unreserving a surplus.

  ZLocker<ZLock> locker(&_lock);

  if (_list.is_empty()) {
    return false;
  }

  // Don't invoke the "remove" callback

  ZMemory* const area = _list.remove_first();

  // Return the range
  *out = *area->range();

  delete area;

  return true;
}

template <typename Range>
void ZMemoryManagerImpl<Range>::anchor_limits() {
  assert(_limits.is_null(), "Should only anchor limits once");

  if (_list.is_empty()) {
    return;
  }

  const offset start = _list.first()->start();
  const size_t size = _list.last()->end() - start;

  _limits = Range(start, size);
}

template <typename Range>
bool ZMemoryManagerImpl<Range>::check_limits(const Range& range) const {
  if (_limits.is_null()) {
    // Limits not anchored
    return true;
  }

  // Otherwise, check that other is within the limits
  return limits_contain(range);
}

template <typename Range>
typename ZMemoryManagerImpl<Range>::offset ZMemoryManagerImpl<Range>::peek_low_address() const {
  ZLocker<ZLock> locker(&_lock);

  const ZMemory* const area = _list.first();
  if (area != nullptr) {
    return area->start();
  }

  // Out of memory
  return offset::invalid;
}

template <typename Range>
typename ZMemoryManagerImpl<Range>::offset_end ZMemoryManagerImpl<Range>::peak_high_address_end() const {
  ZLocker<ZLock> locker(&_lock);

  const ZMemory* const area = _list.last();
  if (area != nullptr) {
    return area->end();
  }

  // Out of memory
  return offset_end::invalid;
}

template <typename Range>
void ZMemoryManagerImpl<Range>::insert(const Range& range) {
  ZLocker<ZLock> locker(&_lock);
  insert_inner(range);
}

template <typename Range>
void ZMemoryManagerImpl<Range>::insert_and_remove_from_low_many(const Range& range, ZArray<Range>* out) {
  ZLocker<ZLock> locker(&_lock);

  const size_t size = range.size();

  // Insert the range
  insert_inner(range);

  // Remove (hopefully) at a lower address
  const size_t removed = remove_from_low_many_at_most_inner(size, out);

  // This should always succeed since we freed the same amount.
  assert(removed == size, "must succeed");
}

template <typename Range>
Range ZMemoryManagerImpl<Range>::insert_and_remove_from_low_exact_or_many(size_t size, ZArray<Range>* in_out) {
  ZLocker<ZLock> locker(&_lock);

  size_t inserted = 0;

  // Insert everything
  ZArrayIterator<Range> iter(in_out);
  for (Range mem; iter.next(&mem);) {
    insert_inner(mem);
    inserted += mem.size();
  }

  // Clear stored memory so that we can populate it below
  in_out->clear();

  // Try to find and remove a contiguous chunk
  Range range = remove_from_low_inner(size);
  if (!range.is_null()) {
    return range;
  }

  // Failed to find a contiguous chunk, split it up into smaller chunks and
  // only remove up to as much that has been inserted.
  size_t removed = remove_from_low_many_at_most_inner(inserted, in_out);
  assert(removed == inserted, "Should be able to get back as much as we previously inserted");
  return Range();
}

template <typename Range>
Range ZMemoryManagerImpl<Range>::remove_from_low(size_t size) {
  ZLocker<ZLock> locker(&_lock);
  Range range = remove_from_low_inner(size);
  return range;
}

template <typename Range>
Range ZMemoryManagerImpl<Range>::remove_from_low_at_most(size_t size) {
  ZLocker<ZLock> lock(&_lock);
  Range range = remove_from_low_at_most_inner(size);
  return range;
}

template <typename Range>
size_t ZMemoryManagerImpl<Range>::remove_from_low_many_at_most(size_t size, ZArray<Range>* out) {
  ZLocker<ZLock> lock(&_lock);
  return remove_from_low_many_at_most_inner(size, out);
}

template <typename Range>
Range ZMemoryManagerImpl<Range>::remove_from_high(size_t size) {
  ZLocker<ZLock> locker(&_lock);

  ZListReverseIterator<ZMemory> iter(&_list);
  for (ZMemory* area; iter.next(&area);) {
    if (area->size() < size) {
      continue;
    }

    Range range;

    if (area->size() == size) {
      // Exact match, remove area
      _list.remove(area);
      range = *area->range();
      delete area;
    } else {
      // Larger than requested, shrink area
      range = shrink_from_back(area, size);
    }

    if (_callbacks._remove != nullptr) {
      _callbacks._remove(range);
    }

    return range;
  }

  // Out of memory
  return Range();
}

template <typename Range>
void ZMemoryManagerImpl<Range>::transfer_from_low(ZMemoryManagerImpl* other, size_t size) {
  assert(other->_list.is_empty(), "Should only be used for initialization");

  ZLocker<ZLock> locker(&_lock);
  size_t to_move = size;

  ZListIterator<ZMemory> iter(&_list);
  for (ZMemory* area; iter.next(&area);) {
    ZMemory* to_transfer;

    if (area->size() <= to_move) {
      // Smaller than or equal to requested, remove area
      _list.remove(area);
      to_transfer = area;
    } else {
      // Larger than requested, shrink area
      const Range range = shrink_from_front(area, to_move);
      to_transfer = new ZMemory(range);
    }

    // Insert into the other list
    //
    // The from list is sorted, the other list starts empty, and the inserts
    // come in sort order, so we can insert_last here.
    other->_list.insert_last(to_transfer);

    to_move -= to_transfer->size();
    if (to_move == 0) {
      break;
    }
  }

  assert(to_move == 0, "Should have transferred requested size");
}

// Instantiate the concrete classes
template class ZMemoryManagerImpl<ZVirtualMemory>;
template class ZMemoryManagerImpl<ZBackingIndexRange>;
