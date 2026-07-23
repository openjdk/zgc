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
#include "gc/z/zLock.hpp"
#include "gc/z/zPageType.hpp"
#include "utilities/resizableHashTable.hpp"

class outputStream;
class ZForwarding;
class ZPage;
class ZPageAllocator;
class ZPageTable;

// ZGC employes a deferred lazy reference counting scheme for old-to-old
// edges in the object graph, allowing the young generation collections to
// reclaim acycling garbage from the old generation.
class ZReferenceCounting {
public:
  // TODO: Remove, just added for ease of sorting.
  struct FreeListAllocation {
    ZPage* _page = nullptr;
    zaddress _address = zaddress::null;
  };
private:
  // TODO: Remove, just added for ease of sorting.
  struct AllocPair {
    ZPage* _page = nullptr;
    size_t _free = 0u;
  };
  ZArray<AllocPair> _allocating[2];
  Atomic<int> _next_page_index[2];

  void increment(zaddress addr);
  void decrement(zaddress addr);

public:
  void on_remember(volatile zpointer* p, zaddress addr);
  void on_failed_remember(zaddress addr);
  void on_forget(volatile zpointer* p, zaddress addr);

  void on_promotion(zaddress addr);
  void on_old_to_space_alloc(ZPage* to_page, zaddress to_addr, bool mutator);
  void on_old_to_old(zaddress addr, bool was_mutator);
  void on_mutator_old_to_old(ZForwarding* forwarding, zaddress from_addr, zaddress to_addr);

  void on_root(zaddress addr);

  void process_death_row(ZPageTable* page_table, ZPageAllocator* page_allocator);

  FreeListAllocation free_list_alloc_object(size_t size, ZPageType type);
  void print_free_lists_on(outputStream* st) const;
};


#endif // SHARE_GC_Z_ZREFERENCECOUNTING_HPP
