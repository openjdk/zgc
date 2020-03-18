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
class StackWatermark;

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

class StackWatermarkIterator : public CHeapObj<mtGC> {
  JavaThread* _jt;
  uintptr_t _caller;
  uintptr_t _callee;
  StackFrameStream _frame_stream;
  StackWatermark& _owner;
  bool _is_done;

public:
  StackWatermarkIterator(StackWatermark& owner);
  uintptr_t caller() const { return _caller; }
  uintptr_t callee() const { return _callee; }
  void process_one(void* context);
  void process_all(void* context);
  void set_watermark(uintptr_t sp);
  RegisterMap& register_map();
  frame& current();
  bool has_next() const;
  void next();
};

class StackWatermark : public CHeapObj<mtGC> {
  friend class StackWatermarkSet;
protected:
  volatile uint32_t _state;
  volatile uintptr_t _watermark;
  StackWatermark* _next;
  JavaThread* _jt;
  StackWatermarkIterator* _iterator;
  Mutex _lock;
  StackWatermarkSet::StackWatermarkKind _kind;

  void process_one(JavaThread* jt);

public:
  bool should_start_iteration() const;
  virtual void start_iteration(void* context);
  void init_epoch();

  StackWatermark(JavaThread* jt, StackWatermarkSet::StackWatermarkKind kind);
  virtual ~StackWatermark();

  static bool has_barrier(frame& f);

  // API for consumers of the stack watermark barrier.
  // The rule for consumers is: do not perform thread transitions
  // or take locks of rank >= special. This is all very special code.
  virtual uint32_t epoch_id() const = 0;
  virtual void process(frame frame, RegisterMap& register_map, void* context) = 0;
  virtual bool process_for_iterator() { return true; }

  void update_watermark();
  Mutex* lock() { return &_lock; }
  JavaThread* thread() const { return _jt; }
  StackWatermarkIterator* iterator() const { return _iterator; }
  uintptr_t watermark();
  StackWatermarkSet::StackWatermarkKind kind() const { return _kind; }
  StackWatermark* next() const { return _next; }
  uintptr_t last_processed();
};

#endif // SHARE_RUNTIME_STACKWATERMARK_HPP
