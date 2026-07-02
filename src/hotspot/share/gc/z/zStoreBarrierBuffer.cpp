/*
 * Copyright (c) 2021, 2025, Oracle and/or its affiliates. All rights reserved.
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

#include "gc/shared/gc_globals.hpp"
#include "gc/z/zAddress.inline.hpp"
#include "gc/z/zBarrier.inline.hpp"
#include "gc/z/zGeneration.inline.hpp"
#include "gc/z/zStoreBarrierBuffer.inline.hpp"
#include "gc/z/zUncoloredRoot.inline.hpp"
#include "memory/resourceArea.hpp"
#include "runtime/threadSMR.hpp"
#include "utilities/ostream.hpp"
#include "utilities/vmError.hpp"

ByteSize ZStoreBarrierEntry::p_offset() {
  return byte_offset_of(ZStoreBarrierEntry, _p);
}

ByteSize ZStoreBarrierEntry::prev_offset() {
  return byte_offset_of(ZStoreBarrierEntry, _prev);
}

ByteSize ZStoreBarrierBuffer::buffer_offset() {
  return byte_offset_of(ZStoreBarrierBuffer, _buffer);
}

ByteSize ZStoreBarrierBuffer::current_offset() {
  return byte_offset_of(ZStoreBarrierBuffer, _current);
}

ZStoreBarrierBuffer::ZStoreBarrierBuffer()
  : _buffer(),
    _current(ZBufferStoreBarriers ? BufferSizeBytes : 0),
    _flush_counter(0) {}

void ZStoreBarrierBuffer::clear() {
  _current = BufferSizeBytes;
}

bool ZStoreBarrierBuffer::is_empty() const {
  return _current == BufferSizeBytes;
}

class ZStoreBarrierBuffer::OnError : public VMErrorCallback {
private:
  ZStoreBarrierBuffer* _buffer;

public:
  OnError(ZStoreBarrierBuffer* buffer)
    : _buffer(buffer) {}

  virtual void call(outputStream* st) {
    _buffer->on_error(st);
  }
};

void ZStoreBarrierBuffer::on_error(outputStream* st) {
  st->print_cr("ZStoreBarrierBuffer: error when flushing");

  for (size_t i = current(); i < BufferLength; ++i) {
    st->print_cr(" [%2zu]: p: " PTR_FORMAT " prev: " PTR_FORMAT,
        i,
        p2i(_buffer[i]._p),
        untype(_buffer[i]._prev));
  }
}

void ZStoreBarrierBuffer::flush() {
  if (!ZBufferStoreBarriers) {
    return;
  }

  OnError on_error(this);
  VMErrorCallbackMark mark(&on_error);

  for (size_t i = current(); i < BufferLength; ++i) {
    const ZStoreBarrierEntry& entry = _buffer[i];
    const zaddress addr = ZBarrier::make_load_good(entry._prev);
    ZBarrier::mark_and_remember(entry._p, addr, entry._prev);
  }

  clear();
}

void ZStoreBarrierBuffer::flush_for_safepoint(uint64_t safepoint_counter) {
  for (;;) {
    uint64_t old_counter = _flush_counter.load_relaxed();
    if (old_counter == safepoint_counter) {
      // Already claimed flushing
      return;
    }

    if (_flush_counter.compare_exchange(old_counter, safepoint_counter, memory_order_relaxed) == old_counter) {
      // We claimed it
      break;
    }
  }

  flush();
}

bool ZStoreBarrierBuffer::is_in(volatile zpointer* p) {
  if (!ZBufferStoreBarriers) {
    return false;
  }

  for (JavaThreadIteratorWithHandle jtiwh; JavaThread * const jt = jtiwh.next(); ) {
    ZStoreBarrierBuffer* const buffer = ZThreadLocalData::store_barrier_buffer(jt);

    for (size_t i = buffer->current(); i < BufferLength; ++i) {
      const ZStoreBarrierEntry& entry = buffer->_buffer[i];
      volatile zpointer* entry_p = entry._p;

      // Check if p matches
      if (entry_p == p) {
        return true;
      }
    }
  }

  return false;
}
