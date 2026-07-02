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

#ifndef SHARE_GC_Z_ZTREE_INLINE_HPP
#define SHARE_GC_Z_ZTREE_INLINE_HPP

#include "gc/z/zTree.hpp"

#include "gc/z/zLock.inline.hpp"
#include "utilities/rbTree.inline.hpp"

template <typename KeyT, typename ValueT, typename CmpT>
bool ZTree<KeyT, ValueT, CmpT>::find(KeyT key, ValueT* value) {
  ZLocker<ZLock> locker(&_lock);
  ValueT* result = _rbtree.find(key);
  if (result == nullptr) {
    return false;
  }
  *value = *result;
  return true;
}

template <typename KeyT, typename ValueT, typename CmpT>
template <typename FunctionT>
void ZTree<KeyT, ValueT, CmpT>::update(KeyT key, FunctionT function) {
  ZLocker<ZLock> locker(&_lock);
  ValueT* prev = _rbtree.find(key);
  ValueT proposal;
  ValueT* proposal_addr = &proposal;
  function(prev, &proposal_addr);
  if (proposal_addr == nullptr) {
    // Propose to drop
    if (prev != nullptr) {
      _rbtree.remove(key);
    }
  } else {
    // Propose to update
    _rbtree.upsert(key, *proposal_addr);
  }
}

#endif // SHARE_GC_Z_ZTREE_INLINE_HPP
