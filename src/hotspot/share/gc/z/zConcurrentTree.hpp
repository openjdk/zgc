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

#ifndef SHARE_GC_Z_ZCONCURRENTTREE_HPP
#define SHARE_GC_Z_ZCONCURRENTTREE_HPP

#include "gc/z/zArray.hpp"
#include "gc/z/zEpoch.hpp"
#include "memory/allocation.hpp"
#include "runtime/atomic.hpp"
#include "utilities/globalDefinitions.hpp"

// The ZConcurrentTree class is a balanced search tree flattened out in an Eytzinger
// style array backing. The tree has amortized O(log N) operations and maintains an
// optimal height at all times. By doing so, this tree can be packed very tightly.
// The root element has index 1, the rest have their search path encoded as their
// element index; zero means left and one means right.
//
// The amortized O(log N) time complexity on mutations comes from amortizing the cost
// of rebuilding increasingly large subtrees with credits for internal nodes above
// a particular "terminal height". Below that height, it's a best effort.
//
// To manage concurrency, the array backing is layered in a lock-free fashion. A
// base PayloadLayer contains the main array. When growing or shrinking the array,
// a new PayloadLayer is created. Between growing and shrinking, an overlay layer
// is created in the shape of a PatchLayer, describing a diff in the underlying layer.
// Mutators accessing an element at a particular index will stop searching at the first
// overlay layer that contains that element index.
//
// The layers create a lock-free linked list which is aggressively pruned using epoch
// based reclamation. A first grace period is used to wait until a particular overlay
// layer (or newer) is guaranteed to be observed by all operations, hence guaranteeing
// the elements they overlay are no longer accessed. Then, the payload can be copied back
// into the PayloadLayer. A subsequent grace period is used to safely unlink and destroy
// the overlay layers. Each quiescence step is claimed by a single mutator, but it can
// guarantee progress such that when concurrent mutation stops, all links are gone. The
// links are only transiently there to provide a linearization point during concurrent
// mutation.

template <typename EntryT>
class ZConcurrentTree : public CHeapObj<mtGC> {
  using idx_t = size_t;
  using height_t = uint32_t;
  using KeyT = typename EntryT::KeyT;
  using ValueT = typename EntryT::ValueT;

  static constexpr size_t growth_reserve_denominator = 32;
  static constexpr size_t growth_numerator = 31;
  static constexpr size_t shrink_denominator = 32;
  static constexpr size_t shrink_numerator = 15;
  static constexpr height_t terminal_height = 8;
  static constexpr size_t credit_denominator = 256;

  enum class LayerKind { Payload, Patch };

  struct Layer : public CHeapObj<mtGC> {
    const LayerKind _kind;
    Atomic<Layer*> _prev;
    size_t _capacity;
    size_t _size;

    Layer(LayerKind kind, Layer* prev, size_t capacity, size_t size);
    virtual ~Layer();
  };

  static size_t population_slots(size_t capacity);
  static size_t credit_slots(size_t capacity);

  struct PayloadLayer : public Layer {
    EntryT* _entries;
    Atomic<uint64_t>* _occupied;
    size_t* _population;
    uint64_t* _credits;

    static size_t occupancy_words(size_t capacity);
    PayloadLayer(size_t capacity);
    ~PayloadLayer();

    void put_entry(idx_t index, EntryT entry);
    bool is_full(idx_t index) const;
    uint64_t credits(idx_t index) const;
  };

  // A record always masks all fields at its index. Copying an older patch
  // into the base must not race even with a reader of an unchanged field.
  struct Record {
    idx_t _index;
    EntryT _entry;
    size_t _population; // Only meaningful above terminal roots.
    uint64_t _credits;
    bool _full; // Logical fullness at terminal roots and below.
  };

  struct PatchLayer : public Layer {
    ZArray<Record> _patches; // Sorted; mutable only before successful head CAS.

    explicit PatchLayer(Layer* prev);

    int lower_bound(idx_t index) const;
  };

  // Published layers are immutable except for maintenance's atomic prev
  // bypass and grace-protected base copy. The first payload terminates the
  // logical view; its prev retains obsolete history for reclamation only.
  // Resizes publish a complete payload directly, retaining the old head.
  // Maintenance captures the head and folds patches into the first payload.
  // The captured chain shields every base field written by that batch.
  // A writer cannot reintroduce a detached head: its publication CAS fails.
  Atomic<Layer*> _head;
  enum class PruningState {
    Idle,
    BeforeSequester,
    BeforePurge,
    Claimed
  };
  Atomic<PruningState> _pruning_state; // Phase when unclaimed; Claimed while owned.
  ZEpoch _epoch;
  Layer* _pending;
  PayloadLayer* _retained_payload; // Bottom layer after maintenance
  Layer* _obsolete; // Unlinking patches during maintenance

  // Utilities for the Eytzinger array layout
  static idx_t root_idx();
  static idx_t left_idx(idx_t node);
  static idx_t right_idx(idx_t node);
  static idx_t parent_idx(idx_t node);
  static idx_t sibling_idx(idx_t node);
  static size_t max_nodes(height_t height);
  static height_t height(idx_t node);
  static size_t capacity(idx_t node, size_t total_nodes);
  static size_t subtree_capacity(idx_t node, size_t tree_capacity);
  static size_t grow_limit(size_t capacity);
  static size_t shrink_limit(size_t capacity);

  // Low level accessors
  Record record_at(Layer* snapshot, idx_t index);
  Record& writable_record_at(Layer* snapshot, idx_t index);
  EntryT entry_at(Layer* snapshot, idx_t index);
  void put_entry_at(Layer* snapshot, idx_t index, EntryT entry);
  uint64_t credits_at(Layer* snapshot, idx_t index);
  void put_credits_at(Layer* snapshot, idx_t index, uint64_t value);
  size_t population_at(Layer* snapshot, idx_t index);
  bool is_full(Layer* snapshot, idx_t index);
  void put_population_at(Layer* snapshot, idx_t index, size_t value);

  // Like value / denominator but rounded up instead of down
  static size_t ceil_div(size_t value, size_t denominator);

  template <typename Function>
  void visit(Layer* snapshot, idx_t node, Function visitor);

  // Rebuilding support
  size_t inject(Layer* snapshot, ZArray<EntryT>* entries, idx_t start, idx_t end, idx_t cursor);
  void extract(Layer* snapshot, ZArray<EntryT>* entries, EntryT new_entry, idx_t cursor, bool clear);
  void extract(Layer* snapshot, ZArray<EntryT>* entries, idx_t cursor, bool clear);
  void rebuild_patch(Layer* snapshot, ZArray<EntryT>* entries, idx_t cursor);
  void rebuild_and_insert(Layer* source, Layer* target, idx_t cursor, EntryT new_entry);
  void rebuild(Layer* source, Layer* target, idx_t cursor);
  void add_population_to_root(Layer* snapshot, idx_t node, int64_t diff);
  idx_t select_rebuild_subtree(Layer* snapshot, idx_t cursor, size_t* credits_spent, idx_t* charged_bank);

  // Medium level accessors
  bool find_node(Layer* snapshot, KeyT key, idx_t* result, EntryT* found_entry);
  bool insert_at(Layer*& snapshot, EntryT new_entry, idx_t found);
  bool remove_at(Layer*& snapshot, idx_t removed);
  static void replace_private_patch(Layer*& snapshot, PayloadLayer* replacement);

  // Sequester patches back to a base payload layer. A resize supersedes everything below it.
  PayloadLayer* sequester_patches();

  // Purging of layers after a grace period
  static void purge(Layer* curr, Layer* stop);

  // Do some purining of the chain of layers for every operation to keep its size
  // down to maximum the number of contending mutator threads.
  void do_pruning();

  class ZEpochHoldScope {
  private:
    ZConcurrentTree<EntryT>* _tree;
    ZEpoch::ZHold _hold;

  public:
    ZEpochHoldScope(ZConcurrentTree<EntryT>* tree);
    ~ZEpochHoldScope();
  };

public:
  ZConcurrentTree();

  // The update function performs an atomic read-update-write operation on
  // the mapping relating to key. The passed in function gets the current
  // mappnig which is null if there is none. The proposed action is an out
  // argument. Mapping to null means remove if it was there before. Otherwise,
  // a new entry is proposed, which would be an insert if there was no mapping
  // there before, and otherwise turns into an update operation.
  //
  // Callbacks may run repeatedly after competing publications. They must reset
  // captured outputs on every invocation and must not accumulate side effects.
  // The find, try_insert and try_remove functions are just wrappers around update.
  template <typename FunctionT>
  void update(KeyT key, FunctionT function);
  bool find(KeyT key, ValueT* result);
  bool try_insert(KeyT key, ValueT value);
  bool try_remove(KeyT key);

  ~ZConcurrentTree();
};

#endif // SHARE_GC_Z_ZCONCURRENTTREE_HPP
