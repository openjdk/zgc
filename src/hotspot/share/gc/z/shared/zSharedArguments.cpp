/*
 * Copyright (c) 2017, 2021, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/shared/gcArguments.hpp"
#include "gc/z/original/zOriginalArguments.hpp"
#include "gc/z/shared/zSharedArguments.hpp"
#include "gc/z/zArguments.hpp"
// FIXME: Move to shared?
#include "gc/z/zGlobals.hpp"
#include "runtime/globals.hpp"
#include "runtime/globals_extension.hpp"
#include "runtime/java.hpp"

void ZSharedArguments::initialize_alignments() {
  SpaceAlignment = ZGranuleSize;
  HeapAlignment = SpaceAlignment;
}

void ZSharedArguments::initialize() {
  GCArguments::initialize();

  if (ZGenerational) {
    ZArguments::initialize();
  } else {
    ZOriginal:ZArguments::initialize();
  }
}

size_t ZSharedArguments::heap_virtual_to_physical_ratio() {
  if (ZGenerational) {
    return ZArguments::heap_virtual_to_physical_ratio();
  } else {
    return ZOriginal::ZArguments::heap_virtual_to_physical_ratio();
  }
}

size_t ZSharedArguments::conservative_max_heap_alignment() {
  return 0;
}

CollectedHeap* ZSharedArguments::create_heap() {
  if (ZGenerational) {
    return ZArguments::create_heap();
  } else {
    return ZOriginal::ZArguments::create_heap();
  }
}

bool ZSharedArguments::is_supported() const {
  if (ZGenerational) {
    return ZArguments::is_os_supported();
  } else {
    return ZOriginal::ZArguments::is_os_supported();
  }
}
