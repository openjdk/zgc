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
#include "code/nmethodEntryBarrier.hpp"
#include "gc/z/zGlobals.hpp"
#include "gc/z/zResurrection.inline.hpp"
#include "gc/z/zOopClosures.hpp"
#include "gc/z/zNMethodEntryBarrier.hpp"
#include "gc/z/zNMethodTable.hpp"
#include "gc/z/zThreadLocalData.hpp"
#include "logging/log.hpp"
#include "memory/resourceArea.hpp"
#include "runtime/interfaceSupport.inline.hpp"

bool ZNMethodBarrier::enter(nmethod* nm) {
  if (!ZNMethodTable::enter_entry_barrier(nm)) {
    log_trace(nmethod_barrier)("skipping critical zone for %p", nm);
    return false;
  }

  // this is a critical zone, all threads that have entered the zone must be done
  // before any of the threads are allowed to exit
  log_trace(nmethod_barrier)("entered critical zone for %p", nm);
  return true;
}

void ZNMethodBarrier::leave(nmethod* nm) {
  ZNMethodTable::leave_entry_barrier(nm);
  log_trace(nmethod_barrier)("left critical zone for %p", nm);
}

bool ZNMethodBarrier::nmethod_entry_barrier(nmethod* nm) {
  assert(!nm->is_zombie(), "no zombies allowed");
#ifdef ASSERT
  bool verify = Thread::current()->is_Java_thread();
#endif
  debug_only(NoSafepointVerifier no_safepoints(verify, verify);) // Safepointing here could be fatal

  log_trace(nmethod_barrier)("nmethod entry barrier: " PTR_FORMAT, p2i(nm));

  bool entered = enter(nm);

  if (nm->is_unloading()) {
    if (nm->is_in_use() && !nm->method()->is_method_handle_intrinsic()) {
      nm->make_not_entrant();
    }
    if (entered) {
      leave(nm);
    }
    return false;
  }

  if (!entered) {
    return true;
  }

  if (ZResurrection::is_blocked()) {
    ZPhantomKeepAliveOopClosure keep_alive;
    nm->oops_do(&keep_alive);
    nm->fix_oop_relocations();
    // Hope for the best, but plan for the worst. We do not yet know if
    // any classes got unloaded. So we assume that could happen and clean
    // more aggressively from mutators.
    nm->unload_nmethod_caches(/*unloading_occurred*/ true);
  } else {
    load_barrier(nm);
    nm->fix_oop_relocations();
  }

  disarm_barrier(nm);
  leave(nm);

  return true;
}

void ZNMethodBarrier::load_barrier(nmethod* nm) {
  ZLoadBarrierOopClosure closure;
  nm->oops_do(&closure, false);
}

int ZNMethodBarrier::disarmed_value() const {
  return (int)(ZAddressBadMask >> 32);
}

ByteSize ZNMethodBarrier::thread_disarmed_offset() const {
  return ZThreadLocalData::address_nmethod_barrier_offset();
}
