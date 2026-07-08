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
#include "gc/z/zLock.hpp"
#include "utilities/resizableHashTable.hpp"

class ZPageTable;
class ZPageAllocator;

// ZGC employes a deferred lazy reference counting scheme for old-to-old
// edges in the object graph, allowing the young generation collections to
// reclaim acycling garbage from the old generation.
class ZReferenceCounting {
private:
  void increment(zaddress addr);
  void decrement(zaddress addr);

public:
  void on_young_mark_start();
  void on_old_mark_start();

  void on_remember(volatile zpointer* p, zaddress addr);
  void on_forget(volatile zpointer* p, zaddress addr);

  void on_promotion(zaddress addr);
  void on_promotion_remset_race(zaddress addr);

  void on_root(zaddress addr);

  void process_death_row(ZPageTable* page_table, ZPageAllocator* page_allocator);
};


#endif // SHARE_GC_Z_ZREFERENCECOUNTING_HPP
