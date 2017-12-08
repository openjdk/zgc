/*
 * Copyright (c) 2017, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/z/zAddress.hpp"
#include "runtime/thread.hpp"
#include "runtime/threadSMR.hpp"
#include "utilities/debug.hpp"

void ZAddressMasks::platform_set_bad_mask(uintptr_t old_mask, uintptr_t new_mask) {
  if (!UseR15TestInLoadBarrier) {
    // R15-based test not used in load barriers
    return;
  }

  // Update bad mask in all Java threads
  for (JavaThreadIteratorWithHandle jtiwh; JavaThread* thread = jtiwh.next();) {
    assert(thread->zaddress_bad_mask() == old_mask, "Previous bad mask is invalid");
    thread->set_zaddress_bad_mask(new_mask);
  }
}
