/*
 * Copyright (c) 2018, Oracle and/or its affiliates. All rights reserved.
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

#include "precompiled.hpp"
#include "code/codeCache.hpp"
#include "code/nmethod.hpp"
#include "gc/shared/barrierSetNMethod.hpp"
#include "gc/z/zBarrierSetNMethod.hpp"
#include "gc/z/zGlobals.hpp"
#include "gc/z/zLock.inline.hpp"
#include "gc/z/zResurrection.inline.hpp"
#include "gc/z/zOopClosures.hpp"
#include "gc/z/zNMethodTable.hpp"
#include "gc/z/zThreadLocalData.hpp"
#include "logging/log.hpp"
#include "memory/resourceArea.hpp"
#include "runtime/interfaceSupport.inline.hpp"

bool ZBarrierSetNMethod::nmethod_entry_barrier(nmethod* nm) {
  ZLocker<ZReentrantLock> l(ZNMethodTable::lock_for_nmethod(nm));
  log_trace(nmethod, barrier)("entered critical zone for %p", nm);

  if (!is_armed(nm)) {
    return true;
  }

  if (nm->is_unloading()) {
    return false;
  }

  heal(nm);

  return true;
}

void ZBarrierSetNMethod::heal(nmethod* nm) {
  ZNMethodOopClosure cl;
  nm->oops_do(&cl);
  nm->fix_oop_relocations();
  OrderAccess::release();
  disarm(nm);
}

int ZBarrierSetNMethod::disarmed_value() const {
  // We override the default BarrierSetNMethod::disarmed_value() since
  // this can be called by GC threads, which doesn't keep an up to date
  // address_bad_mask.
  const uintptr_t disarmed_addr = ((uintptr_t)&ZAddressBadMask) + ZNMethodDisarmedOffset;
  return *((int*)disarmed_addr);
}

ByteSize ZBarrierSetNMethod::thread_disarmed_offset() const {
  return ZThreadLocalData::nmethod_disarmed_offset();
}
