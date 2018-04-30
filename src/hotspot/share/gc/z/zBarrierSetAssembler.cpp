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
#include "gc/z/zBarrierSetAssembler.hpp"
#include "gc/z/zThreadLocalData.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/thread.hpp"

bool ZBarrierSetAssemblerBase::barrier_needed(DecoratorSet decorators, BasicType type) const {
  assert((decorators & AS_RAW) == 0, "Unexpected decorator");
  assert((decorators & AS_NO_KEEPALIVE) == 0, "Unexpected decorator");
  assert((decorators & IN_ARCHIVE_ROOT) == 0, "Unexpected decorator");
  assert((decorators & ON_UNKNOWN_OOP_REF) == 0, "Unexpected decorator");

  if (type == T_OBJECT || type == T_ARRAY) {
    if (((decorators & IN_HEAP) != 0) ||
        ((decorators & IN_CONCURRENT_ROOT) != 0) ||
        ((decorators & ON_PHANTOM_OOP_REF) != 0)) {
      // Barrier needed
      return true;
    }
  }

  // Barrier not neeed
  return false;
}

Address ZBarrierSetAssemblerBase::address_bad_mask_from_thread(Register thread) const {
  return Address(thread, ZThreadLocalData::address_bad_mask_offset());
}

Address ZBarrierSetAssemblerBase::address_bad_mask_from_jni_env(Register env) const {
  return Address(env, ZThreadLocalData::address_bad_mask_offset() - JavaThread::jni_environment_offset());
}

address ZBarrierSetAssemblerBase::barrier_load_at_entry_point(DecoratorSet decorators) const {
  if (decorators & ON_PHANTOM_OOP_REF) {
    return reinterpret_cast<address>(SharedRuntime::z_load_barrier_on_phantom_oop_field_preloaded);
  } else if (decorators & ON_WEAK_OOP_REF) {
    return reinterpret_cast<address>(SharedRuntime::z_load_barrier_on_weak_oop_field_preloaded);
  } else {
    return reinterpret_cast<address>(SharedRuntime::z_load_barrier_on_oop_field_preloaded);
  }
}

address ZBarrierSetAssemblerBase::barrier_arraycopy_prologue_entry_point() const {
  return reinterpret_cast<address>(SharedRuntime::z_load_barrier_on_oop_array);
}
