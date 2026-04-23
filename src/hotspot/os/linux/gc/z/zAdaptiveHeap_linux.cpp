/*
 * Copyright (c) 2026, Oracle and/or its affiliates. All rights reserved.
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

#include "gc/shared/gcLogPrecious.hpp"
#include "gc/z/zAdaptiveHeap.hpp"
#include "gc/z/zNUMA.inline.hpp"
#include "jvm_io.h"
#include "logging/log.hpp"
#include "os_linux.hpp"
#include "runtime/os.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"

void ZAdaptiveHeap::pd_machine_memory_info(ZMachineMemoryInfo& info) {
  info._physical_memory = 0;
  info._available_memory = 0;
  info._is_valid = false;

  if (!ZNUMA::is_bound()) {
    info._physical_memory = os::Machine::physical_memory();
    info._is_valid = os::Machine::available_memory(info._available_memory);
    return;
  }

  for (uint32_t numa_id = 0; numa_id < ZNUMA::count(); numa_id++) {
    const int node = ZNUMA::numa_id_to_node(numa_id);
    char path[128];
    jio_snprintf(path, sizeof(path), "/sys/devices/system/node/node%d/meminfo", node);

    FILE* f = os::fopen(path, "r");
    if (!f) {
      continue;
    }

    char line[256];
    physical_memory_size_type node_physical_memory = 0;
    physical_memory_size_type node_available_memory = 0;
    bool found_mem_total = false;
    bool found_mem_available = false;
    bool found_mem_free = false;
    bool found_active_file = false;
    bool found_inactive_file = false;
    bool found_sreclaimable = false;
    while (fgets(line, sizeof(line), f) != nullptr) {
      int n = -1;
      physical_memory_size_type read_value = 0;
      if (sscanf(line, "Node %d MemTotal: " PHYS_MEM_TYPE_FORMAT " kB", &n, &read_value) == 2) {
        node_physical_memory = read_value * K;
        found_mem_total = true;
      } else if (sscanf(line, "Node %d MemAvailable: " PHYS_MEM_TYPE_FORMAT " kB", &n, &read_value) == 2) {
        // If the Kernel has an approximation of MemAvailable, use it
        node_available_memory = read_value * K;
        found_mem_available = true;
      } else if (!found_mem_available && sscanf(line, "Node %d MemFree: " PHYS_MEM_TYPE_FORMAT " kB", &n, &read_value) == 2) {
        node_available_memory += read_value * K;
        found_mem_free = true;
      } else if (!found_mem_available && sscanf(line, "Node %d Active(file): " PHYS_MEM_TYPE_FORMAT " kB", &n, &read_value) == 2) {
        node_available_memory += read_value * K;
        found_active_file = true;
      } else if (!found_mem_available && sscanf(line, "Node %d Inactive(file): " PHYS_MEM_TYPE_FORMAT " kB", &n, &read_value) == 2) {
        node_available_memory += read_value * K;
        found_inactive_file = true;
      } else if (!found_mem_available && sscanf(line, "Node %d SReclaimable: " PHYS_MEM_TYPE_FORMAT " kB", &n, &read_value) == 2) {
        node_available_memory += read_value * K;
        found_sreclaimable = true;
      }

      if (found_mem_total && (found_mem_available || (found_mem_free && found_active_file && found_inactive_file && found_sreclaimable))) {
        break;
      }
    }

    fclose(f);

    if (!found_mem_total) {
      // Some Kernels might not have "MemTotal" available in the node-specific file.
      // Fall back to numa_node_size64 if that's the case.
      long long res = os::Linux::numa_node_size64(node, nullptr);
      if (res != -1) {
        node_physical_memory = (physical_memory_size_type)res;
        found_mem_total = true;
      }
    }

    if (!(found_mem_total && (found_mem_available || (found_mem_free && found_active_file && found_inactive_file && found_sreclaimable)))) {
      static bool n = [&]() {
        log_warning_p(gc, heap)("Failed to read one of the NUMA-node specific values: "
                                "MemTotal: %d, MemAvailable: %d, MemFree: %d, Active(file): %d, Inactive(file): %d, SReclaimable: %d",
                                found_mem_total, found_mem_available, found_mem_free, found_active_file, found_inactive_file, found_sreclaimable);
        return true;
      }();
      assert(false, "This should not happen");

      info._physical_memory = os::Machine::physical_memory();
      info._is_valid = os::Machine::available_memory(info._available_memory);
      return;
    }

    info._physical_memory += node_physical_memory;
    info._available_memory += node_available_memory;
  }

  info._is_valid = true;
}

bool ZAdaptiveHeap::pd_machine_compressed_memory(physical_memory_size_type& value) {
  return false;
}
