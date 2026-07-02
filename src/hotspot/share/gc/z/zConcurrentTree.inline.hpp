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

#ifndef SHARE_GC_Z_ZCONCURRENTTREE_INLINE_HPP
#define SHARE_GC_Z_ZCONCURRENTTREE_INLINE_HPP

#include "gc/z/zConcurrentTree.hpp"

#include "gc/z/zArray.inline.hpp"
#include "runtime/orderAccess.hpp"
#include "utilities/debug.hpp"
#include "utilities/powerOfTwo.hpp"

template <typename EntryT>
ZConcurrentTree<EntryT>::Layer::Layer(LayerKind kind, Layer* prev, size_t capacity, size_t size)
  : _kind(kind),
    _prev(prev),
    _capacity(capacity),
    _size(size) {}

template <typename EntryT>
ZConcurrentTree<EntryT>::Layer::~Layer() {}

template <typename EntryT>
size_t ZConcurrentTree<EntryT>::PayloadLayer::occupancy_words(size_t capacity) {
  const size_t floor_size = capacity / 2;
  return floor_size / 64 + (floor_size % 64 != 0 ? 1 : 0);
}

template <typename EntryT>
ZConcurrentTree<EntryT>::PayloadLayer::PayloadLayer(size_t capacity)
  : Layer(LayerKind::Payload, nullptr, capacity, 0),
    _entries(NEW_C_HEAP_ARRAY(EntryT, capacity, mtGC)),
    _occupied(NEW_C_HEAP_ARRAY(Atomic<uint64_t>, occupancy_words(capacity), mtGC)),
    _population(NEW_C_HEAP_ARRAY(size_t, population_slots(capacity), mtGC)),
    _credits(NEW_C_HEAP_ARRAY(uint64_t, credit_slots(capacity), mtGC)) {
  for (size_t i = 0; i < capacity; ++i) {
    _entries[i] = EntryT::empty();
  }
  for (size_t i = 0; i < occupancy_words(capacity); ++i) {
    ::new (&_occupied[i]) Atomic<uint64_t>();
  }
  for (size_t i = 0; i < population_slots(capacity); ++i) {
    _population[i] = 0;
  }
  for (size_t i = 0; i < credit_slots(capacity); ++i) {
    _credits[i] = 0;
  }
}

template <typename EntryT>
ZConcurrentTree<EntryT>::PayloadLayer::~PayloadLayer() {
  FREE_C_HEAP_ARRAY(_entries);
  FREE_C_HEAP_ARRAY(_occupied);
  FREE_C_HEAP_ARRAY(_population);
  FREE_C_HEAP_ARRAY(_credits);
}

template <typename EntryT>
void ZConcurrentTree<EntryT>::PayloadLayer::put_entry(idx_t index, EntryT entry) {
  _entries[index] = entry;
  if (index < this->_capacity / 2) {
    // Internal nodes don't need to update the floor occupancy bitmap
    return;
  }
  // At the floor, we update a bitmap so we can quickly check if a
  // terminal height subtree is full or not. Now only the bit position
  // is protected by the overlaid patch layer, so we have to use atomics
  // to protect the bits around this.
  const idx_t floor_index = index - this->_capacity / 2;
  const uint64_t mask = UCONST64(1) << (floor_index % 64);
  Atomic<uint64_t>& word = _occupied[floor_index / 64];
  const uint64_t bits = word.load_relaxed();
  word.store_relaxed(EntryT::is_empty(entry) ? bits & ~mask : bits | mask);
}

template <typename EntryT>
bool ZConcurrentTree<EntryT>::PayloadLayer::is_full(idx_t index) const {
  assert(index >= population_slots(this->_capacity), "terminal subtree required");
  // Published trees have no occupied node below an empty ancestor, so
  // a subtree is full iff its entire interval on the floor is occupied.
  const size_t shift = log2i(this->_capacity - 1) - log2i(index);
  idx_t cursor = (index << shift) - this->_capacity / 2;
  const idx_t end = cursor + (size_t(1) << shift);
  while (cursor < end) {
    const size_t offset = cursor % 64;
    const size_t length = MIN2(size_t(64 - offset), end - cursor);
    const uint64_t mask = (~uint64_t(0) >> (64 - length)) << offset;
    // We need to use atomic load because a patching layer with an adjacent
    // entry of the same bitmap word could be concurrently sequestered.
    if ((_occupied[cursor / 64].load_relaxed() & mask) != mask) {
      return false;
    }
    cursor += length;
  }
  return true;
}

template <typename EntryT>
uint64_t ZConcurrentTree<EntryT>::PayloadLayer::credits(idx_t index) const {
  return index < credit_slots(this->_capacity) ? _credits[index] : 0;
}

template <typename EntryT>
ZConcurrentTree<EntryT>::PatchLayer::PatchLayer(Layer* prev)
  : Layer(LayerKind::Patch, prev, prev->_capacity, prev->_size),
    _patches() {}

template <typename EntryT>
int ZConcurrentTree<EntryT>::PatchLayer::lower_bound(idx_t index) const {
  int low = 0;
  int high = _patches.length();
  while (low < high) {
    const int mid = low + (high - low) / 2;
    if (_patches.at(mid)._index < index) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }
  return low;
}

template <typename EntryT>
ZConcurrentTree<EntryT>::ZEpochHoldScope::ZEpochHoldScope(ZConcurrentTree<EntryT>* tree)
  : _tree(tree),
    _hold(tree->_epoch.hold())
{}

template <typename EntryT>
ZConcurrentTree<EntryT>::ZEpochHoldScope::~ZEpochHoldScope() {
  _tree->_epoch.release(_hold);
  // Dekker duality between releasing the hold and checkign for maintenance
  // to flatten the chain of layers. This guarantees liveness/progress such
  // that when concurrency stops, the chain is guaranteed to be flat.
  OrderAccess::fence();
  _tree->do_pruning();
}

template <typename EntryT>
size_t ZConcurrentTree<EntryT>::population_slots(size_t capacity) {
  return MAX2(size_t(1), capacity >> (terminal_height + 1));
}

template <typename EntryT>
size_t ZConcurrentTree<EntryT>::credit_slots(size_t capacity) {
  return MAX2(size_t(1), capacity >> terminal_height);
}

template <typename EntryT>
typename ZConcurrentTree<EntryT>::idx_t ZConcurrentTree<EntryT>::root_idx() {
  return 1;
}

template <typename EntryT>
typename ZConcurrentTree<EntryT>::idx_t ZConcurrentTree<EntryT>::left_idx(idx_t node) {
  return node << 1;
}

template <typename EntryT>
typename ZConcurrentTree<EntryT>::idx_t ZConcurrentTree<EntryT>::right_idx(idx_t node) {
  return (node << 1) + 1;
}

template <typename EntryT>
typename ZConcurrentTree<EntryT>::idx_t ZConcurrentTree<EntryT>::parent_idx(idx_t node) {
  return node >> 1;
}

template <typename EntryT>
typename ZConcurrentTree<EntryT>::idx_t ZConcurrentTree<EntryT>::sibling_idx(idx_t node) {
  return node ^ 1;
}

template <typename EntryT>
size_t ZConcurrentTree<EntryT>::max_nodes(height_t height) {
  return (UCONST64(1) << (height + 1)) - 1;
}

template <typename EntryT>
typename ZConcurrentTree<EntryT>::height_t ZConcurrentTree<EntryT>::height(idx_t node) {
  assert(node > 0, "can't be empty");
  return height_t(log2i(node));
}

template <typename EntryT>
size_t ZConcurrentTree<EntryT>::capacity(idx_t node, size_t total_nodes) {
  height_t depth = static_cast<height_t>(log2i(node + 1));
  return (size_t(total_nodes + 1) >> depth) - 1;
}

template <typename EntryT>
size_t ZConcurrentTree<EntryT>::subtree_capacity(idx_t node, size_t tree_capacity) {
  if (node >= tree_capacity) {
    return 0;
  }

  const height_t tree_height = height(idx_t(tree_capacity - 1));
  const height_t node_height = height(node);
  return max_nodes(tree_height - node_height);
}

template <typename EntryT>
size_t ZConcurrentTree<EntryT>::grow_limit(size_t capacity) {
  return capacity - ceil_div(capacity, growth_reserve_denominator);
}

template <typename EntryT>
size_t ZConcurrentTree<EntryT>::shrink_limit(size_t capacity) {
  const size_t quotient = capacity / shrink_denominator;
  const auto remainder = capacity % shrink_denominator;
  return shrink_numerator * quotient + ceil_div(shrink_numerator * remainder, shrink_denominator);
}

template <typename EntryT>
typename ZConcurrentTree<EntryT>::Record ZConcurrentTree<EntryT>::record_at(Layer* snapshot, idx_t index) {
  if (index >= snapshot->_capacity) {
    return {index, EntryT::empty(), 0, 0, false};
  }
  for (Layer* curr = snapshot; ; curr = curr->_prev.load_acquire()) {
    if (curr->_kind == LayerKind::Patch) {
      PatchLayer* patch = static_cast<PatchLayer*>(curr);
      const int pos = patch->lower_bound(index);
      if (pos < patch->_patches.length() && patch->_patches.at(pos)._index == index) {
        return patch->_patches.at(pos);
      }
      continue;
    }
    PayloadLayer* base = static_cast<PayloadLayer*>(curr);
    const bool counted = index < population_slots(base->_capacity);
    return {index, base->_entries[index], counted ? base->_population[index] : 0,
            base->credits(index), counted ? false : base->is_full(index)};
  }
}

template <typename EntryT>
typename ZConcurrentTree<EntryT>::Record& ZConcurrentTree<EntryT>::writable_record_at(Layer* snapshot, idx_t index) {
  assert(snapshot->_kind == LayerKind::Patch, "private patch required");
  assert(index < snapshot->_capacity, "out of bounds");
  PatchLayer* patch = static_cast<PatchLayer*>(snapshot);
  const int pos = patch->lower_bound(index);
  if (pos == patch->_patches.length() || patch->_patches.at(pos)._index != index) {
    const Record record = record_at(snapshot, index);
    patch->_patches.insert_before(pos, record);
  }
  return patch->_patches.at(pos);
}

template <typename EntryT>
EntryT ZConcurrentTree<EntryT>::entry_at(Layer* snapshot, idx_t index) {
  // Lookups need no population metadata, especially no bitmap scans.
  if (index >= snapshot->_capacity) {
    return EntryT::empty();
  }
  for (Layer* curr = snapshot; ; curr = curr->_prev.load_acquire()) {
    if (curr->_kind == LayerKind::Patch) {
      PatchLayer* patch = static_cast<PatchLayer*>(curr);
      const int pos = patch->lower_bound(index);
      if (pos < patch->_patches.length() && patch->_patches.at(pos)._index == index) {
        return patch->_patches.at(pos)._entry;
      }
      continue;
    }
    return static_cast<PayloadLayer*>(curr)->_entries[index];
  }
}

template <typename EntryT>
void ZConcurrentTree<EntryT>::put_entry_at(Layer* snapshot, idx_t index, EntryT entry) {
  if (snapshot->_kind == LayerKind::Payload) {
    static_cast<PayloadLayer*>(snapshot)->put_entry(index, entry);
  } else {
    writable_record_at(snapshot, index)._entry = entry;
  }
}

template <typename EntryT>
uint64_t ZConcurrentTree<EntryT>::credits_at(Layer* snapshot, idx_t index) {
  if (index >= credit_slots(snapshot->_capacity)) {
    return 0;
  }
  return record_at(snapshot, index)._credits;
}

template <typename EntryT>
void ZConcurrentTree<EntryT>::put_credits_at(Layer* snapshot, idx_t index, uint64_t value) {
  if (index >= credit_slots(snapshot->_capacity)) {
    assert(value == 0, "no credit bank below terminal roots");
    return;
  }
  writable_record_at(snapshot, index)._credits = value;
}

template <typename EntryT>
size_t ZConcurrentTree<EntryT>::population_at(Layer* snapshot, idx_t index) {
  assert(index < population_slots(snapshot->_capacity), "explicit population required");
  return record_at(snapshot, index)._population;
}

template <typename EntryT>
bool ZConcurrentTree<EntryT>::is_full(Layer* snapshot, idx_t index) {
  return index >= snapshot->_capacity || record_at(snapshot, index)._full;
}

template <typename EntryT>
void ZConcurrentTree<EntryT>::put_population_at(Layer* snapshot, idx_t index, size_t value) {
  if (index < population_slots(snapshot->_capacity)) {
    if (snapshot->_kind == LayerKind::Payload) {
      static_cast<PayloadLayer*>(snapshot)->_population[index] = value;
    } else {
      writable_record_at(snapshot, index)._population = value;
    }
  } else if (snapshot->_kind == LayerKind::Patch) {
    // extract/inject explicitly describe the finished subtree, rather than
    // consulting floor bits during their temporarily disconnected builds.
    writable_record_at(snapshot, index)._full = value == subtree_capacity(index, snapshot->_capacity);
  }
}

template <typename EntryT>
size_t ZConcurrentTree<EntryT>::ceil_div(size_t value, size_t denominator) {
  return value / denominator + (value % denominator != 0U ? 1U : 0U);
}

template <typename EntryT>
template <typename Function>
void ZConcurrentTree<EntryT>::visit(Layer* snapshot, idx_t node, Function visitor) {
  if (node >= snapshot->_capacity) {
    return;
  }

  EntryT entry = entry_at(snapshot, node);

  if (EntryT::is_empty(entry)) {
    return;
  }

  visit(snapshot, left_idx(node), visitor);
  visitor(node);
  visit(snapshot, right_idx(node), visitor);
}

template <typename EntryT>
size_t ZConcurrentTree<EntryT>::inject(Layer* snapshot, ZArray<EntryT>* entries, idx_t start, idx_t end, idx_t cursor) {
  if (start >= end) {
    return 0;
  }

  const idx_t middle = start + (end - start) / 2;

  const size_t injected_left = inject(snapshot, entries, start, middle, left_idx(cursor));
  const size_t injected_right = inject(snapshot, entries, middle + 1, end, right_idx(cursor));
  const size_t population = injected_left + 1 + injected_right;

  put_entry_at(snapshot, cursor, entries->at(middle));
  put_population_at(snapshot, cursor, population);

  return population;
}

template <typename EntryT>
void ZConcurrentTree<EntryT>::extract(Layer* snapshot, ZArray<EntryT>* entries, EntryT new_entry, idx_t cursor, bool clear) {
  bool inserted = false;

  visit(snapshot, cursor, [&](idx_t visited) {
    EntryT entry = entry_at(snapshot, visited);

    // Shoehorn the new entry when it is no longer greater than previous entries
    if (!inserted) {
      int comparison = EntryT::cmp(new_entry, entry);
      assert(comparison != 0, "should never be equal");
      if (comparison < 0) {
        entries->append(new_entry);
        inserted = true;
      }
    }

    entries->append(entry);
    if (clear) {
      put_entry_at(snapshot, visited, EntryT::empty());
      put_population_at(snapshot, visited, 0);
    }
  });

  if (!inserted) {
    // Last entry
    entries->append(new_entry);
  }
}

template <typename EntryT>
void ZConcurrentTree<EntryT>::extract(Layer* snapshot, ZArray<EntryT>* entries, idx_t cursor, bool clear) {
  visit(snapshot, cursor, [&](idx_t visited) {
    EntryT entry = entry_at(snapshot, visited);
    if (clear) {
      put_entry_at(snapshot, visited, EntryT::empty());
      put_population_at(snapshot, visited, 0);
    }
    entries->append(entry);
  });
}

template <typename EntryT>
void ZConcurrentTree<EntryT>::rebuild_patch(Layer* snapshot, ZArray<EntryT>* entries, idx_t cursor) {
  // When rebuilding we essentially recursively insert the median entry into the tree which
  // causes it to be perfectly balanced. But that involves visiting the height of the tree for
  // every insert of the rebuild which is an unnecessary log. This optimized version performs
  // the recursive median insert without having to re-traverse the height for every entry.

  assert(snapshot->_kind == LayerKind::Patch, "private patch required");
  PatchLayer* patch = static_cast<PatchLayer*>(snapshot);

  struct Work {
    idx_t _index;
    int _start;
    int _end;
  };
  ZArray<Work> work;
  ZArray<Record> merged;
  work.append({cursor, 0, entries->length()});
  int previous = 0;

  for (int next = 0; next < work.length(); ++next) {
    const Work item = work.at(next);
    if (item._index >= snapshot->_capacity) {
      assert(item._start == item._end, "rebuilt subtree must fit");
      continue;
    }
    Record record = record_at(snapshot, item._index);
    const int population = item._end - item._start;
    if (EntryT::is_empty(record._entry) && population == 0) {
      continue;
    }

    const int middle = item._start + population / 2;
    record._entry = population == 0 ? EntryT::empty() : entries->at(middle);
    if (item._index < population_slots(snapshot->_capacity)) {
      record._population = population;
    } else {
      record._full = size_t(population) == subtree_capacity(item._index, snapshot->_capacity);
    }

    while (previous < patch->_patches.length() &&
           patch->_patches.at(previous)._index < item._index) {
      merged.append(patch->_patches.at(previous++));
    }

    if (previous < patch->_patches.length() &&
        patch->_patches.at(previous)._index == item._index) {
      ++previous;
    }

    merged.append(record);
    work.append({left_idx(item._index), item._start, middle});
    work.append({right_idx(item._index), population == 0 ? middle : middle + 1, item._end});
  }

  while (previous < patch->_patches.length()) {
    merged.append(patch->_patches.at(previous++));
  }

  patch->_patches.swap(&merged);
}

template <typename EntryT>
void ZConcurrentTree<EntryT>::rebuild_and_insert(Layer* source, Layer* target, idx_t cursor, EntryT new_entry) {
  // Extract a sorted list of the entries
  ZArray<EntryT> entries;
  extract(source, &entries, new_entry, cursor, false);

  if (source == target) {
    rebuild_patch(target, &entries, cursor);
  } else {
    inject(target, &entries, 0, entries.length(), cursor);
  }
}

template <typename EntryT>
void ZConcurrentTree<EntryT>::rebuild(Layer* source, Layer* target, idx_t cursor) {
  // Extract a sorted list of the entries
  ZArray<EntryT> entries;
  extract(source, &entries, cursor, false);

  if (source == target) {
    rebuild_patch(target, &entries, cursor);
  } else {
    inject(target, &entries, 0, entries.length(), cursor);
  }
}

template <typename EntryT>
void ZConcurrentTree<EntryT>::add_population_to_root(Layer* snapshot, idx_t node, int64_t diff) {
  // Every affected ancestor needs a record even if its fullness stays
  // unchanged: it shields readers from multi-write floor bitmap copyback.
  // Acquire the complete writable record once and carry the changed child's
  // fullness upward; only the unchanged sibling needs another logical lookup.
  const size_t populations = population_slots(snapshot->_capacity);
  const size_t credits = credit_slots(snapshot->_capacity);
  idx_t child = 0;
  bool child_full = false;
  for (idx_t curr = node; curr != 0; curr = parent_idx(curr)) {
    Record& record = writable_record_at(snapshot, curr);
    // is_full() only reads records, so it cannot invalidate this reference.
    // No reference survives the next iteration's writable_record_at() insertion.
    if (curr < populations) {
      record._population += diff;
    } else {
      record._full = !EntryT::is_empty(record._entry) &&
                     (child == 0
                       ? is_full(snapshot, left_idx(curr)) &&
                         is_full(snapshot, right_idx(curr))
                       : child_full && is_full(snapshot, sibling_idx(child)));
      child_full = record._full;
    }
    if (diff > 0 && curr < credits) {
      record._credits += diff;
    }
    child = curr;
  }
}

template <typename EntryT>
typename ZConcurrentTree<EntryT>::idx_t ZConcurrentTree<EntryT>::select_rebuild_subtree(Layer* snapshot, idx_t cursor, size_t* credits_spent, idx_t* charged_bank) {
  const height_t tree_height = height(idx_t(snapshot->_capacity - 1));
  const height_t small_height = MIN2(terminal_height, tree_height);
  idx_t prev = 0;

  idx_t candidate_node = 0;
  size_t candidate_credits = 0;
  idx_t candidate_child = 0;

  // find_node() stops at the first external child. Its parent is the full
  // leaf whose subtree must be rebuilt; the external cursor itself has no
  // population or credit slot.
  for (idx_t curr = parent_idx(cursor); curr != 0; curr = parent_idx(curr)) {
    const height_t curr_height = tree_height - height(curr);
    const size_t curr_capacity = subtree_capacity(curr, snapshot->_capacity);
    const idx_t curr_child = prev;
    prev = curr;

    if (curr_height <= small_height) {
      if (is_full(snapshot, curr)) {
        continue;
      }
      // For small subtrees, it's not worth rebuilding larger subtrees.
      *credits_spent = 0;
      *charged_bank = 0;
      return curr;
    }

    const size_t curr_population = population_at(snapshot, curr);
    if (curr_population >= curr_capacity) {
      continue;
    }
    const uint64_t credit = credits_at(snapshot, curr_child);
    const size_t population_after_insert = curr_population + 1;
    const size_t required_credit = MAX2(size_t(1), ceil_div(population_after_insert, credit_denominator));
    const size_t required_progress_holes = required_credit * 2;

    const size_t min_population = ceil_div(curr_capacity, 4);
    const size_t max_population = curr_capacity - required_progress_holes;

    if (population_after_insert < min_population ||
        population_after_insert > max_population ||
        credit < required_credit) {
      continue;
    }

    if (candidate_node == 0) {
      candidate_node = curr;
      candidate_child = curr_child;
      candidate_credits = required_credit;
    } else if (credit > 2 * required_credit) {
      candidate_node = curr;
      candidate_child = curr_child;
      candidate_credits = required_credit;
    }
  }

  // Global growth was checked before entering this function, so the root
  // always has room for a correct, though less localized, rebuild.
  if (candidate_node == 0) {
    *credits_spent = 0;
    *charged_bank = 0;
    return root_idx();
  }

  *credits_spent = candidate_credits;
  *charged_bank = candidate_child;

  return candidate_node;
}

template <typename EntryT>
bool ZConcurrentTree<EntryT>::find_node(Layer* snapshot, KeyT key, idx_t* result, EntryT* found_entry) {
  idx_t node = root_idx();

  while (node < snapshot->_capacity) {
    EntryT entry = entry_at(snapshot, node);

    if (EntryT::is_empty(entry)) {
      *result = node;
      return false;
    }

    int comparison = EntryT::cmp(entry, key);

    if (comparison < 0) {
      node = right_idx(node);
    } else if (comparison > 0) {
      node = left_idx(node);
    } else {
      *result = node;
      if (found_entry != nullptr) {
        *found_entry = entry;
      }
      return true;
    }
  }

  *result = node;
  return false;
}

template <typename EntryT>
bool ZConcurrentTree<EntryT>::insert_at(Layer*& snapshot, EntryT new_entry, idx_t found) {
  if (snapshot->_size == grow_limit(snapshot->_capacity)) {
    PayloadLayer* new_layer = new PayloadLayer(snapshot->_capacity * 2);
    rebuild_and_insert(snapshot, new_layer, root_idx(), new_entry);
    new_layer->_size = snapshot->_size + 1;
    replace_private_patch(snapshot, new_layer);
    return true;
  }

  if (found < snapshot->_capacity) {
    put_entry_at(snapshot, found, new_entry);
    add_population_to_root(snapshot, found, 1);
    snapshot->_size++;
    return true;
  }

  size_t paid_credits;
  idx_t charged_node;
  idx_t regrow_subtree = select_rebuild_subtree(snapshot, found, &paid_credits, &charged_node);
  if (charged_node == 0) {
    rebuild_and_insert(snapshot, snapshot, regrow_subtree, new_entry);
  } else {
    idx_t charged_sibling = sibling_idx(charged_node);
    size_t charged_credits = credits_at(snapshot, charged_node) - paid_credits;
    size_t charged_sibling_credits = credits_at(snapshot, charged_sibling);
    rebuild_and_insert(snapshot, snapshot, regrow_subtree, new_entry);
    put_credits_at(snapshot, charged_node, charged_credits);
    put_credits_at(snapshot, charged_sibling, charged_sibling_credits);
  }

  // inject() sets the rebuilt subtree's population, including the inserted
  // entry. Its ancestors were not rebuilt, so account for that entry there.
  add_population_to_root(snapshot, parent_idx(regrow_subtree), 1);

  snapshot->_size++;
  return true;
}

template <typename EntryT>
bool ZConcurrentTree<EntryT>::remove_at(Layer*& snapshot, idx_t removed) {
  // Recursively bubble up donors down to the leaf level
  for (;;) {
    idx_t left = left_idx(removed);
    EntryT left_entry = entry_at(snapshot, left);

    if (!EntryT::is_empty(left_entry)) {
      idx_t donor = left;
      // Find right corner of left subtree
      while (!EntryT::is_empty(entry_at(snapshot, right_idx(donor)))) {
        donor = right_idx(donor);
      }
      EntryT donor_entry = entry_at(snapshot, donor);
      put_entry_at(snapshot, removed, donor_entry);
      removed = donor;
      continue;
    }

    idx_t right = right_idx(removed);
    EntryT right_entry = entry_at(snapshot, right);

    if (!EntryT::is_empty(right_entry)) {
      idx_t donor = right;
      // Find left corner of right subtree
      while (!EntryT::is_empty(entry_at(snapshot, left_idx(donor)))) {
        donor = left_idx(donor);
      }
      EntryT donor_entry = entry_at(snapshot, donor);
      put_entry_at(snapshot, removed, donor_entry);
      removed = donor;
      continue;
    }

    break;
  }

  // Once we get to the floor level, delete the leaf.
  put_entry_at(snapshot, removed, EntryT::empty());
  snapshot->_size--;
  add_population_to_root(snapshot, removed, -1);

  if (snapshot->_capacity > 2 && snapshot->_size < shrink_limit(snapshot->_capacity)) {
    PayloadLayer* new_layer = new PayloadLayer(snapshot->_capacity / 2);
    rebuild(snapshot, new_layer, root_idx());
    new_layer->_size = snapshot->_size;
    replace_private_patch(snapshot, new_layer);
  }

  return true;
}

template <typename EntryT>
void ZConcurrentTree<EntryT>::replace_private_patch(Layer*& snapshot, PayloadLayer* replacement) {
  assert(snapshot->_kind == LayerKind::Patch, "private patch required");
  replacement->_prev.store_relaxed(snapshot->_prev.load_acquire());
  delete static_cast<PatchLayer*>(snapshot);
  snapshot = replacement;
}

template <typename EntryT>
typename ZConcurrentTree<EntryT>::PayloadLayer* ZConcurrentTree<EntryT>::sequester_patches() {
  ZArray<PatchLayer*> patches;
  Layer* curr = _pending;
  while (curr->_kind == LayerKind::Patch) {
    PatchLayer* patch = static_cast<PatchLayer*>(curr);
    patches.append(patch);
    curr = patch->_prev.load_acquire();
  }
  PayloadLayer* base = static_cast<PayloadLayer*>(curr);
  for (int i = patches.length(); i > 0; --i) {
    PatchLayer* patch = patches.at(i - 1);
    for (int j = 0; j < patch->_patches.length(); ++j) {
      const Record& r = patch->_patches.at(j);
      assert(r._index < base->_capacity, "batch capacity mismatch");
      base->put_entry(r._index, r._entry);
      if (r._index < population_slots(base->_capacity)) {
        base->_population[r._index] = r._population;
      }
      if (r._index < credit_slots(base->_capacity)) {
        base->_credits[r._index] = r._credits;
      }
    }
  }
  base->_size = _pending->_size;
  return base;
}

template <typename EntryT>
void ZConcurrentTree<EntryT>::purge(Layer* curr, Layer* stop) {
  while (curr != stop) {
    Layer* prev = curr->_prev.load_acquire();
    delete curr;
    curr = prev;
  }
}

template <typename EntryT>
void ZConcurrentTree<EntryT>::do_pruning() {
  for (;;) {
    PruningState phase = _pruning_state.load_acquire();
    if (phase == PruningState::Claimed || !_pruning_state.compare_set(phase, PruningState::Claimed)) {
      return;
    }
    Layer* idle_head = nullptr;
    if (phase == PruningState::Idle) {
      Layer* curr = _head.load_acquire();
      idle_head = curr;
      if (curr->_kind == LayerKind::Patch) {
        _pending = curr;
        // Publish the cutoff before advancing so quiescent maintenance sees it
        _epoch.advance();
        phase = PruningState::BeforeSequester;
      } else if (curr->_prev.load_acquire() != nullptr) {
        // A payload-only cutoff needs no copy shield: lookups already stop here
        _pending = curr;
        _retained_payload = static_cast<PayloadLayer*>(curr);
        _obsolete = _retained_payload->_prev.load_acquire();
        _retained_payload->_prev.release_store(nullptr);
        _epoch.advance();
        phase = PruningState::BeforePurge;
      }
    } else if (_epoch.is_quiescent()) {
      if (phase == PruningState::BeforeSequester) {
        _retained_payload = sequester_patches();
        Layer* replacement = _retained_payload;
        // Try swinging the head to the selected payload layer
        if (!_head.compare_set(_pending, replacement)) {
          Layer* prev = _head.load_acquire();
          while (prev->_prev.load_acquire() != _pending) {
            prev = prev->_prev.load_acquire();
          }
          prev->_prev.release_store(replacement);
        }
        _obsolete = _retained_payload->_prev.load_acquire();
        _retained_payload->_prev.release_store(nullptr);
        _epoch.advance();
        phase = PruningState::BeforePurge;
      } else {
        purge(_pending, _retained_payload);
        purge(_obsolete, nullptr);
        _obsolete = nullptr;
        _pending = nullptr;
        _retained_payload = nullptr;
        phase = PruningState::Idle;
      }
    }
    // Transition to the next step.
    _pruning_state.release_store(phase);

    // Check for more maintenance such that without interference, we always
    // finish the pruning. And when there is concurrent interference, we will
    // let the interfering thread(s) deal with pruning when they are done.
    OrderAccess::fence();
    if (_epoch.has_holders()) {
      return;
    }

    // Check if pruning is done.
    if (phase == PruningState::Idle && idle_head != nullptr && _head.load_acquire() == idle_head) {
      return;
    }
  }
}

template <typename EntryT>
ZConcurrentTree<EntryT>::ZConcurrentTree()
  : _head(new PayloadLayer(4)),
    _pruning_state(PruningState::Idle),
    _epoch(),
    _pending(nullptr),
    _retained_payload(nullptr),
    _obsolete(nullptr)
{}

template <typename EntryT>
template <typename FunctionT>
void ZConcurrentTree<EntryT>::update(KeyT key, FunctionT function) {
  for (;;) {
    ZEpochHoldScope hold(this);
    Layer* snapshot = _head.load_acquire();
    idx_t node;
    EntryT entry = EntryT::empty();
    const bool exists = find_node(snapshot, key, &node, &entry);
    ValueT previous;
    if (exists) {
      previous = entry;
    }
    ValueT proposal;
    ValueT* proposal_addr = &proposal;
    ValueT* previous_addr = exists ? &previous : nullptr;
    function(previous_addr, &proposal_addr);

    if (proposal_addr == previous_addr) {
      // No publication needed; linearize at the captured view.
      return;
    }

    Layer* candidate = new PatchLayer(snapshot);
    if (proposal_addr == nullptr) {
      remove_at(candidate, node);
    } else if (!exists) {
      const EntryT proposed = EntryT::create(key, *proposal_addr);
      insert_at(candidate, proposed, node);
    } else {
      // Preserve all metadata: copy-back still writes the complete record.
      // No ancestor walk, occupancy change, or rebuilding is necessary.
      Record record = record_at(snapshot, node);
      record._entry = EntryT::create(key, *proposal_addr);
      assert(!EntryT::is_empty(record._entry) && EntryT::cmp(record._entry, key) == 0,
             "replacement must preserve the key and occupancy");
      static_cast<PatchLayer*>(candidate)->_patches.append(record);
    }
    if (_head.compare_set(snapshot, candidate)) {
      return;
    }
    delete candidate;
  }
}

template <typename EntryT>
bool ZConcurrentTree<EntryT>::find(KeyT key, ValueT* result) {
  bool found = false;
  update(key, [&](ValueT* previous, ValueT** proposal) {
    found = previous != nullptr;
    if (found) {
      *result = *previous;
    }
    *proposal = previous;
  });
  return found;
}

template <typename EntryT>
bool ZConcurrentTree<EntryT>::try_insert(KeyT key, ValueT value) {
  bool inserted = false;
  update(key, [&](ValueT* previous, ValueT** proposal) {
    // Reset on every retry: another writer may have inserted the key.
    inserted = previous == nullptr;
    if (inserted) {
      **proposal = value;
    } else {
      *proposal = previous;
    }
  });
  return inserted;
}

template <typename EntryT>
bool ZConcurrentTree<EntryT>::try_remove(KeyT key) {
  bool removed = false;
  update(key, [&](ValueT* previous, ValueT** proposal) {
    removed = previous != nullptr;
    *proposal = nullptr;
  });
  return removed;
}

template <typename EntryT>
ZConcurrentTree<EntryT>::~ZConcurrentTree() {
  const PruningState phase = _pruning_state.load_relaxed();
  assert(phase == PruningState::Idle, "maintenance still active");
  purge(_head.load_acquire(), nullptr);
}


#endif // SHARE_GC_Z_ZCONCURRENTTREE_INLINE_HPP
