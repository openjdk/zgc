/*
 * Copyright (c) 2017, 2025, Oracle and/or its affiliates. All rights reserved.
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

#include "gc/z/zLargePages.hpp"
#include "hugepages.hpp"
#include "os_linux.hpp"
#include "runtime/globals.hpp"
#include "runtime/globals_extension.hpp"

#include <sys/mman.h>

static bool madv_collapse_available() {
  const size_t size = 2 * M;
  void* const res = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

  if (res == MAP_FAILED) {
    return false;
  }

  assert(size >= os::vm_page_size(), "Unexpected page size");
  os::pretouch_memory(res, (void*)(((char*)res) + os::vm_page_size()));

  bool result = os::Linux::madvise_collapse_transparent_huge_pages(res, size);

  munmap(res, size);

  return result;
}

void ZLargePages::pd_initialize() {
  bool can_collapse = ZMemoryHeating && madv_collapse_available();

  if (os::Linux::thp_requested()) {
    if (can_collapse) {
      _state = Collapse;
      return;
    }
    // Check if the OS config turned off transparent huge pages for shmem.
    _os_enforced_transparent_mode = HugePages::shmem_thp_info().is_disabled();
    _state = _os_enforced_transparent_mode ? Disabled : Transparent;
    return;
  }

  if (UseLargePages) {
    _state = Explicit;
    return;
  }

  if (FLAG_IS_DEFAULT(UseTransparentHugePages) && can_collapse) {
    _state = Collapse;
    return;
  }

  // Check if the OS config turned on transparent huge pages for shmem.
  _os_enforced_transparent_mode = HugePages::shmem_thp_info().is_forced();
  _state = _os_enforced_transparent_mode ? Transparent : Disabled;
}
