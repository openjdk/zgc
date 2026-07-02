/*
 * Copyright (c) 2021, 2024, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_Z_ZSTOREBARRIERBUFFER_HPP
#define SHARE_GC_Z_ZSTOREBARRIERBUFFER_HPP

#include "gc/z/zAddress.hpp"
#include "gc/z/zGenerationId.hpp"
#include "gc/z/zLock.hpp"
#include "memory/allocation.hpp"
#include "runtime/atomic.hpp"
#include "utilities/sizes.hpp"

struct ZStoreBarrierEntry {
  volatile zpointer* _p;
  zpointer           _prev;

  static ByteSize p_offset();
  static ByteSize prev_offset();
};

class ZStoreBarrierBuffer : public CHeapObj<mtGC> {
  friend class ZVerify;

private:
  static const size_t BufferLength    = 32;
  static const size_t BufferSizeBytes = BufferLength * sizeof(ZStoreBarrierEntry);

  ZStoreBarrierEntry _buffer[BufferLength];

  // sizeof(ZStoreBarrierEntry) scaled index growing downwards
  size_t             _current;

  Atomic<uint64_t>   _flush_counter;

  void on_new_phase_relocate(size_t i);
  void on_new_phase_remember(size_t i);
  void on_new_phase_mark(size_t i);

  void clear();

  bool is_empty() const;
  size_t current() const;

  void install_base_pointers_inner();

  void on_error(outputStream* st);
  class OnError;

public:
  ZStoreBarrierBuffer();

  static ByteSize buffer_offset();
  static ByteSize current_offset();

  static ZStoreBarrierBuffer* buffer_for_store(bool heal);

  void flush();
  void flush_for_safepoint(uint64_t safepoint_counter);
  void add(volatile zpointer* p, zpointer prev);

  // Check if p is contained in any store barrier buffer entry in the system
  static bool is_in(volatile zpointer* p);
};

#endif // SHARE_GC_Z_ZSTOREBARRIERBUFFER_HPP
