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

#ifndef SHARE_GC_Z_ZREFERENCECOUNTING_HPP
#define SHARE_GC_Z_ZREFERENCECOUNTING_HPP

#include "gc/z/zAddress.hpp"
#include "gc/z/zArray.hpp"
#include "gc/z/zBitMap.hpp"
#include "gc/z/zGlobals.hpp"
#include "gc/z/zLock.hpp"
#include "gc/z/zPageAge.hpp"
#include "gc/z/zPageType.hpp"
#include "gc/z/zTree.hpp"
#include "gc/z/zValue.hpp"
#include "utilities/resizableHashTable.hpp"

class ZForwarding;
class ZPage;
class ZPageAllocator;
class ZPageTable;

// ZGC employes a deferred lazy reference counting scheme for old-to-old
// edges in the object graph, allowing the young generation collections to
// reclaim acycling garbage from the old generation.
class ZReferenceCounting {
public:
  // TODO: Cleanup type, name, class.
  struct FreeListAllocation {
    ZPage* _page = nullptr;
    zaddress _address = zaddress::null;
  };

  struct State;

private:
  struct FoundDeathRow {
    ZMovableBitMap _bitmaps[2];
    int            _current;

    FoundDeathRow();

    void flip();
    void register_page(ZPage* page);

    void verify_previous() const;

    template <typename Function>
    void par_iterate_death_row_pages(ZPageTable* page_table, Function function);

    BitMap& current_bitmap();
    const BitMap& current_bitmap() const;
    BitMap& previous_bitmap();
    const BitMap& previous_bitmap() const;
  } _found_death_row;

  State* _state;

  State* state();
  const State* state() const;

  // Returns the fetched value before the mutation
  int64_t increment(zaddress addr, ZPage* page);
  int64_t decrement(zaddress addr, ZPage* page);

  bool try_kill_root(ZPage* page, zaddress addr, size_t& pardoned);
  bool try_kill_followed(ZPage* page, zaddress addr, size_t& pardoned, int64_t observed_count);

  class ZProcessDeathRowTask;

public:
  ZReferenceCounting();

  void on_remember(volatile zpointer* p, zaddress addr, bool remembered);
  void on_failed_remember(zaddress addr);
  void on_forget(volatile zpointer* p, zaddress addr);

  void on_promotion(zaddress addr);
  void on_old_to_space_alloc(ZPage* to_page, zaddress to_addr, bool mutator);
  void on_old_to_old(zaddress from_addr, ZPage* from_page, zaddress to_addr, ZPage* to_page, bool was_mutator);
  void on_mutator_old_to_old(ZForwarding* forwarding, zaddress from_addr, zaddress to_addr);
  void on_undo(zaddress addr, ZPage* page);

  void on_root(zaddress addr);

  void process_death_row(ZPageTable* page_table, ZPageAllocator* page_allocator);

  template <typename Function>
  void par_iterate_death_row_pages(ZPageTable* page_table, Function function);

  void flip_found_death_row();

  FreeListAllocation free_list_alloc_object(size_t size, ZPageType type, ZPageAge to_age);

  void on_free_list_insert(const ZPage* page);

  void register_old_alloction_page(ZPage* page);
  void construct_old_allocator();
  void reset_old_allocator();
};


#endif // SHARE_GC_Z_ZREFERENCECOUNTING_HPP
