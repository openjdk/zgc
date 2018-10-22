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
#include "gc/z/zGlobalBehaviours.hpp"
#include "gc/z/zLock.inline.hpp"
#include "gc/z/zNMethodTable.hpp"
#include "runtime/interfaceSupport.inline.hpp"

bool ZIsUnloadingBehaviour::is_unloading(CompiledMethod* cm) const {
  nmethod* nm = (nmethod*)cm;
  ZReentrantLock* lock = ZNMethodTable::lock_for_nmethod(nm);
  bool locked = !SafepointSynchronize::is_at_safepoint() && lock->reentrant_lock();
  bool result = ClosureIsUnloadingBehaviour::is_unloading(cm);
  if (locked) {
    lock->unlock();
  }
  return result;
}

bool ZICProtectionBehaviour::lock(CompiledMethod* cm) {
  nmethod* nm = (nmethod*)cm;
  ZReentrantLock* lock = ZNMethodTable::lock_for_nmethod(nm);
  if (lock == NULL) {
    assert(cm->is_unloaded(), "nmethods must have a lock unless unloaded");
    return false;
  }
  if (Thread::current()->is_Java_thread()) {
    CompiledIC_lock->lock();
    lock->lock();
    lock->unlock();
    return true;
  } else {
    if (SafepointSynchronize::is_at_safepoint() || !lock->reentrant_lock()) {
      return false;
    }
    if (CompiledIC_lock->is_locked()) {
      lock->unlock();
      CompiledIC_lock->lock_without_safepoint_check();
      lock->lock();
      lock->unlock();
    }
    return true;
  }
}

void ZICProtectionBehaviour::unlock(CompiledMethod* cm) {
  assert(cm->is_nmethod(), "no support for JVMCI yet");
  nmethod* nm = (nmethod*)cm;
  ZReentrantLock* lock = ZNMethodTable::lock_for_nmethod(nm);
  if (Thread::current()->is_Java_thread()) {
    lock->lock();
    CompiledIC_lock->unlock();
    lock->unlock();
  } else {
    if (CompiledIC_lock->owned_by_self()) {
      CompiledIC_lock->unlock();
    } else {
      lock->unlock();
    }
  }
}

bool ZICProtectionBehaviour::is_safe(CompiledMethod* cm) {
  if (SafepointSynchronize::is_at_safepoint() ||
      CompiledIC_lock->owned_by_self() ||
      cm->is_unloaded()) {
    return true;
  }
  assert(cm->is_nmethod(), "no support for JVMCI yet");
  nmethod* nm = (nmethod*)cm;
  ZReentrantLock* lock = ZNMethodTable::lock_for_nmethod(nm);
  return lock->is_owned();
}

ZGlobalBehaviours::ZGlobalBehaviours() {
  IsUnloadingBehaviour::set_current(&_is_unloading_behaviour);
  CompiledICProtectionBehaviour::set_current(&_ic_protection_behaviour);
}
