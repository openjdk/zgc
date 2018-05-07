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
#include "gc/z/zAddress.inline.hpp"
#include "gc/z/zErrno.hpp"
#include "gc/z/zGlobals.hpp"
#include "gc/z/zLargePages.inline.hpp"
#include "gc/z/zPhysicalMemory.inline.hpp"
#include "gc/z/zPhysicalMemoryBacking_solaris_sparc.hpp"
#include "logging/log.hpp"
#include "runtime/os.hpp"
#include "utilities/align.hpp"
#include "utilities/debug.hpp"

#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>

// Support for building on Solaris systems older than 11.3
#ifndef VA_MASK_OVERLAP
#define VA_MASK_OVERLAP      1
#endif

#ifndef _SC_OSM_PAGESIZE_MIN
#define _SC_OSM_PAGESIZE_MIN 519
#endif

#ifndef MC_LOCK_GRANULE
#define MC_LOCK_GRANULE      8
#endif

#ifndef MC_UNLOCK_GRANULE
#define MC_UNLOCK_GRANULE    9
#endif

typedef int (*shmget_osm_func_t)(key_t, size_t, int, size_t);
typedef int (*va_mask_alloc_func_t)(int, int, int*);

static shmget_osm_func_t shmget_osm_func = NULL;
static va_mask_alloc_func_t va_mask_alloc_func = NULL;

static bool initialize_symbols() {
  va_mask_alloc_func = (va_mask_alloc_func_t)dlsym(RTLD_DEFAULT, "va_mask_alloc");
  if (va_mask_alloc_func == NULL) {
    log_error(gc, init)("System does not support VA masking");
    return false;
  }

  if (UseOSMHeap) {
    shmget_osm_func = (shmget_osm_func_t)dlsym(RTLD_DEFAULT, "shmget_osm");
    if (shmget_osm_func == NULL) {
      log_error(gc, init)("System does not support OSM");
      return false;
    }
  }

  return true;
}

ZPhysicalMemoryBacking::ZPhysicalMemoryBacking(size_t max_capacity, size_t granule_size) :
    _granule_size(granule_size),
    _initialized(false) {

  if (!initialize_symbols()) {
    return;
  }

  if (!initialize_vamask()) {
    return;
  }

  if (UseOSMHeap) {
    _initialized = initialize_osm();
  } else {
    _initialized = initialize_anonymous();
  }
}

bool ZPhysicalMemoryBacking::initialize_vamask() const {
  const int total_vamask_bits = align_up_(ZPlatformADIBits + ZPlatformVAMaskBits, BitsPerByte);
  const int alloc_vamask_bits = total_vamask_bits - ZPlatformADIBits;
  int lsb;

  if (va_mask_alloc_func(alloc_vamask_bits, VA_MASK_OVERLAP, &lsb) == -1) {
    log_error(gc, init)("Failed to allocate VA mask");
    return false;
  }

  if (lsb != ZAddressMetadataShift) {
    log_error(gc, init)("Failed to allocate expected VA mask");
    return false;
  }

  return true;
}

bool ZPhysicalMemoryBacking::initialize_osm() const {
  const uintptr_t start = ZAddressSpaceStart;
  const size_t size = ZAddressSpaceSize;
  const size_t min_page_size = sysconf(_SC_OSM_PAGESIZE_MIN);

  if (!is_aligned(_granule_size, min_page_size)) {
    log_error(gc, init)("OSM page size not supported");
    return false;
  }

  const int osm = shmget_osm_func(IPC_PRIVATE, size, IPC_CREAT | 0600, _granule_size);
  if (osm == -1) {
    log_error(gc, init)("Failed to create OSM for Java heap");
    return false;
  }

  const uintptr_t actual_start = (uintptr_t)shmat(osm, (void*)start, 0);

  if (shmctl(osm, IPC_RMID, NULL) == -1) {
    log_error(gc, init)("Failed to destroy OSM for Java heap");
    return false;
  }

  if (actual_start == (uintptr_t)-1) {
    log_error(gc, init)("Failed to attach OSM for Java heap");
    return false;
  }

  if (actual_start != start) {
    log_error(gc, init)("Failed to reserve address space for Java heap");
    return false;
  }

  if (madvise((char*)start, size, MADV_ACCESS_DEFAULT) == -1) {
    log_error(gc, init)("Failed to set NUMA policy for Java heap");
    return false;
  }

  return true;
}

bool ZPhysicalMemoryBacking::initialize_anonymous() const {
  if (ZLargePages::is_enabled()) {
    // Check if granule size is a supported large page size (we always ignore LargePageSizeInBytes)
    if (os::page_size_for_region_aligned(_granule_size, 1) != _granule_size) {
      log_error(gc, init)("Page size " SIZE_FORMAT " not supported", _granule_size);
      return false;
    }
  }

  return true;
}

bool ZPhysicalMemoryBacking::is_initialized() const {
  return _initialized;
}

bool ZPhysicalMemoryBacking::expand(size_t from, size_t to) {
  assert(is_aligned(to - from, _granule_size), "Invalid size");
  return true;
}

ZPhysicalMemory ZPhysicalMemoryBacking::alloc(size_t size) {
  assert(is_aligned(size, _granule_size), "Invalid size");
  return ZPhysicalMemory(size);
}

void ZPhysicalMemoryBacking::free(ZPhysicalMemory pmem) {
  assert(pmem.nsegments() == 1, "Invalid number of segments");
}

void ZPhysicalMemoryBacking::map_osm(ZPhysicalMemory pmem, uintptr_t offset) const {
  const uintptr_t addr = ZAddress::address(offset);
  if (memcntl((char*)addr, pmem.size(), MC_LOCK_GRANULE, 0, 0, 0) == -1) {
    fatal("Failed to lock OSM granule");
  }
  // No need to pre-touch OSM mappings
}

void ZPhysicalMemoryBacking::unmap_osm(ZPhysicalMemory pmem, uintptr_t offset) const {
  const uintptr_t addr = ZAddress::address(offset);
  if (memcntl((char*)addr, pmem.size(), MC_UNLOCK_GRANULE, 0, 0, 0) == -1) {
    fatal("Failed to unlock OSM granule");
  }
}

void ZPhysicalMemoryBacking::advise_anonymous(uintptr_t addr, size_t size) const {
  memcntl_mha mha;
  mha.mha_cmd = MHA_MAPSIZE_VA;
  mha.mha_pagesize = _granule_size;
  mha.mha_flags = 0;

  if (memcntl((char*)addr, size, MC_HAT_ADVISE, (caddr_t)&mha, 0, 0) == -1) {
    ZErrno err;
    log_error(gc)("Failed to advise use of large pages (%s)", err.to_string());
  }
}

void ZPhysicalMemoryBacking::map_anonymous(ZPhysicalMemory pmem, uintptr_t offset) const {
  const uintptr_t addr = ZAddress::address(offset);
  const size_t size = pmem.size();
  void* const res = mmap((void*)addr, size, PROT_READ|PROT_WRITE, MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if (res == MAP_FAILED) {
    ZErrno err;
    fatal("Failed to map memory (%s)", err.to_string());
  }

  if (ZLargePages::is_transparent()) {
    advise_anonymous(addr, size);
  }

  if (AlwaysPreTouch) {
    os::pretouch_memory((void*)addr, (void*)(addr + size));
  }
}

void ZPhysicalMemoryBacking::unmap_anonymous(ZPhysicalMemory pmem, uintptr_t offset) const {
  const uintptr_t addr = ZAddress::address(offset);
  if (munmap((char*)addr, pmem.size()) == -1) {
    ZErrno err;
    fatal("Failed to unmap memory (%s)", err.to_string());
  }
}

uintptr_t ZPhysicalMemoryBacking::nmt_address(uintptr_t offset) const {
  // We only have one heap mapping, so just convert the offset to a heap address
  return ZAddress::address(offset);
}

void ZPhysicalMemoryBacking::map(ZPhysicalMemory pmem, uintptr_t offset) const {
  if (UseOSMHeap) {
    map_osm(pmem, offset);
  } else {
    map_anonymous(pmem, offset);
  }
}

void ZPhysicalMemoryBacking::unmap(ZPhysicalMemory pmem, uintptr_t offset) const {
  if (UseOSMHeap) {
    unmap_osm(pmem, offset);
  } else {
    unmap_anonymous(pmem, offset);
  }
}

void ZPhysicalMemoryBacking::flip(ZPhysicalMemory pmem, uintptr_t offset) const {
  // Does nothing when using VA-masking
}
