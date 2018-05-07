/*
 * Copyright (c) 2015, 2017, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/z/zVirtualMemory.hpp"
#include "logging/log.hpp"

#include <sys/mman.h>
#include <sys/types.h>

#ifndef MC_RESERVE_AS
#define MC_RESERVE_AS 12
#endif

bool ZVirtualMemoryManager::reserve(uintptr_t start, size_t size) {
  // Reserve address space
  const int res = memcntl((char*)start, size, MC_RESERVE_AS, NULL, 0, 0);
  if (res == -1) {
    log_error(gc)("Failed to reserve address space for Java heap");
    return false;
  }

  return true;
}
