/*
 * Copyright (c) 2020, Oracle and/or its affiliates. All rights reserved.
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
 *
 */

#ifndef SHARE_RUNTIME_STACKWATERMARK_HPP
#define SHARE_RUNTIME_STACKWATERMARK_HPP

#include "memory/allocation.hpp"
#include "runtime/frame.hpp"
#include "runtime/stackWatermarkSet.hpp"

class JavaThread;
class StackWatermarkIterator;

class StackWatermarkState : public AllStatic {
public:
  inline static bool is_done(uint32_t state) {
    return state & 1;
  }

  inline static uint32_t epoch(uint32_t state) {
    return state >> 1;
  }

  inline static uint32_t create(uint32_t epoch, bool is_done) {
    return (epoch << 1) | (is_done ? 1u : 0u);
  }
};

class StackWatermark : public CHeapObj<mtInternal> {
  friend class StackWatermarkIterator;
protected:
  volatile uint32_t _state;
  volatile uintptr_t _watermark;
  StackWatermark* _next;
  JavaThread* _jt;
  StackWatermarkIterator* _iterator;
  Mutex _lock;
  StackWatermarkSet::StackWatermarkKind _kind;

  void process_one();

  void update_watermark();
  void yield_processing();
  static bool has_barrier(frame& f);
  void ensure_safe(frame f);
  bool is_frame_processed(frame f);
  bool is_frame_safe(frame f);

  // API for consumers of the stack watermark barrier.
  // The rule for consumers is: do not perform thread transitions
  // or take locks of rank >= special. This is all very special code.
  virtual uint32_t epoch_id() const = 0;
  virtual void process(frame frame, RegisterMap& register_map, void* context) = 0;
  virtual void start_iteration_impl(void* context);

  // Set process_on_iteration to false if you don't want to move the
  // watermark when new frames are discovered from stack walkers, as
  // opposed to due to frames being unwinded by the owning thread.
  virtual bool process_on_iteration() { return true; }

public:
  StackWatermark(JavaThread* jt, StackWatermarkSet::StackWatermarkKind kind, uint32_t epoch);
  virtual ~StackWatermark();

  uintptr_t watermark();

  StackWatermarkSet::StackWatermarkKind kind() const { return _kind; }
  StackWatermark* next() const { return _next; }
  void set_next(StackWatermark* n) { _next = n; }

  bool should_start_iteration() const;
  bool should_start_iteration_acquire() const;

  uintptr_t last_processed();

  void on_unwind();
  void on_iteration(frame fr);
  void start_iteration();
  void finish_iteration(void* context);
};

#endif // SHARE_RUNTIME_STACKWATERMARK_HPP
