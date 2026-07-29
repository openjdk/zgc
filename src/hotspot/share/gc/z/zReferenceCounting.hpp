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
#include "gc/z/zGlobals.hpp"
#include "gc/z/zLock.hpp"
#include "gc/z/zPageType.hpp"
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
private:
  template <ZPageType PageType>
  struct CPUAllocPages {
    // static constexpr ZPageType PageType = ZPageType::small;
    static_assert(PageType != ZPageType::large, "Unsupported ZPageType");
    static constexpr bool IsSmallPage = PageType == ZPageType::small;
    // Inclusive
    static constexpr int MinAllocSizeShift = IsSmallPage ? ZMinMinObjectSizeSmallShift : ZMinMinObjectSizeMediumShift;
    static constexpr size_t MinAllocSize = size_t(1) << MinAllocSizeShift;

    // Exclusive, a fully free page is returned to the PageAllocator
    static constexpr int MaxFreeBlockSizeShift = IsSmallPage ? ZPageSizeSmallShift : ZPageSizeMediumMaxMaxShift;
    static constexpr int MaxAllocSizeShift = MaxFreeBlockSizeShift - 3;
    static constexpr size_t MaxAllocSize = size_t(1) << MaxAllocSizeShift;

    static constexpr int SizeClasses = MaxAllocSizeShift - MinAllocSizeShift + 1;

    ZArray<ZPage*> _alloc_pages;
    const uint32_t _cpu_id;
    const uint32_t _numa_id;
    Atomic<int> _next_page_index[SizeClasses];

    CPUAllocPages(uint32_t cpu_id);

    void reset();
    void reserve(int capacity);
    void push(ZPage* page);
    bool free_list_alloc_object(size_t size, FreeListAllocation* allocation);
  };

  ZPerCPU<CPUAllocPages<ZPageType::small>> _small_allocation_pages;
  ZPerCPU<CPUAllocPages<ZPageType::medium>> _medium_allocation_pages;

  void increment(zaddress addr);
  void decrement(zaddress addr);

public:
  ZReferenceCounting();

  void on_remember(volatile zpointer* p, zaddress addr, bool remembered);
  void on_failed_remember(zaddress addr);
  void on_forget(volatile zpointer* p, zaddress addr);

  void on_promotion(zaddress addr);
  void on_old_to_space_alloc(ZPage* to_page, zaddress to_addr, bool mutator);
  void on_old_to_old(zaddress addr, bool was_mutator);
  void on_mutator_old_to_old(ZForwarding* forwarding, zaddress from_addr, zaddress to_addr);

  void on_root(zaddress addr);

  void process_death_row(ZPageTable* page_table, ZPageAllocator* page_allocator);

  FreeListAllocation free_list_alloc_object(size_t size, ZPageType type);
};


#endif // SHARE_GC_Z_ZREFERENCECOUNTING_HPP
