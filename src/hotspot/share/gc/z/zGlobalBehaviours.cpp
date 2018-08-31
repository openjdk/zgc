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
#include "gc/z/zNMethodTable.hpp"
#include "runtime/interfaceSupport.inline.hpp"

bool ZIsUnloadingBehaviour::is_unloading(CompiledMethod* cm) const {
  assert(cm->is_nmethod(), "JVMCI not yet supported");
  nmethod* nm = (nmethod*)cm;
  bool locked = ZNMethodTable::lock((nmethod*)cm);

  bool result = ClosureIsUnloadingBehaviour::is_unloading(cm);
  if (locked) {
    ZNMethodTable::unlock((nmethod*)cm);
  }

  return result;
}

bool ZICProtectionBehaviour::lock(CompiledMethod* method) {
  assert(method->is_nmethod(), "no support for JVMCI yet");

  Thread* thread = Thread::current();
  if (thread->is_Java_thread()) {
    ThreadBlockInVM b(static_cast<JavaThread*>(thread));
    return ZNMethodTable::lock((nmethod*)method);
  } else {
    return ZNMethodTable::lock((nmethod*)method);
  }
}

void ZICProtectionBehaviour::unlock(CompiledMethod* method) {
  assert(method->is_nmethod(), "no support for JVMCI yet");
  ZNMethodTable::unlock((nmethod*)method);
}

bool ZICProtectionBehaviour::is_safe(CompiledMethod* method) {
  assert(method->is_nmethod(), "no support for JVMCI yet");
  return SafepointSynchronize::is_at_safepoint() || ZNMethodTable::is_locked((nmethod*)method);
}

ZGlobalBehaviours::ZGlobalBehaviours() {
  Universe::gc_behaviours()->register_behaviour<IsUnloadingBehaviour>(_is_unloading_behaviour);
  Universe::gc_behaviours()->register_behaviour<CompiledICProtectionBehaviour>(_ic_protection_behaviour);
}
