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

#include "precompiled.hpp"
#include "logging/log.hpp"
#include "runtime/atomic.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/stackWatermark.inline.hpp"
#include "runtime/thread.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/preserveException.hpp"

static const uintptr_t _frame_padding = 8;
static const uintptr_t _frames_per_poll_gc = 5;

void StackWatermarkIterator::set_watermark(uintptr_t sp) {
  if (!has_next()) {
    return;
  } else if (_callee == 0) {
    _callee = sp;
  } else if (_caller == 0) {
    _caller = sp;
  } else {
    _callee = _caller;
    _caller = sp;
  }
}

void StackWatermarkIterator::process_one(void* context) {
  uintptr_t sp = 0;
  Thread* thread = Thread::current();
  ResetNoHandleMark rnhm;
  HandleMark hm(thread);
  PreserveExceptionMark pem(thread);
  ResourceMark rm(thread);
  while (has_next()) {
    frame f = current();
    sp = reinterpret_cast<uintptr_t>(f.sp());
    bool frame_has_barrier = StackWatermark::has_barrier(f);
    _owner.process(f, register_map(), context);
    next();
    if (frame_has_barrier) {
      break;
    }
  }
  set_watermark(sp);
}

void StackWatermarkIterator::process_all(void* context) {
  ResourceMark rm;
  log_info(stackbarrier)("Sampling whole stack for tid %d",
                         _jt->osthread()->thread_id());
  uint i = 0;
  while (has_next()) {
    frame f = current();
    assert(reinterpret_cast<uintptr_t>(f.sp()) >= _caller, "invariant");
    uintptr_t sp = reinterpret_cast<uintptr_t>(f.sp());
    bool frame_has_barrier = StackWatermark::has_barrier(f);
    _owner.process(f, register_map(), context);
    next();
    if (frame_has_barrier) {
      set_watermark(sp);
      if (++i == _frames_per_poll_gc) {
        // Yield every N frames so mutator can progress faster.
        i = 0;
        _owner.update_watermark();
        MutexUnlocker mul(_owner.lock(), Mutex::_no_safepoint_check_flag);
      }
    }
  }
}

StackWatermarkIterator::StackWatermarkIterator(StackWatermark& owner) :
    _jt(owner.thread()),
    _caller(0),
    _callee(0),
    _frame_stream(owner.thread(), true /* update_registers */, false /* process_frames */),
    _owner(owner),
    _is_done(_frame_stream.is_done()) {
}

frame& StackWatermarkIterator::current() {
  return *_frame_stream.current();
}

RegisterMap& StackWatermarkIterator::register_map() {
  return *_frame_stream.register_map();
}

bool StackWatermarkIterator::has_next() const {
  return !_is_done;
}

void StackWatermarkIterator::next() {
  _frame_stream.next();
  _is_done = _frame_stream.is_done();
}

StackWatermark::StackWatermark(JavaThread* jt, StackWatermarkSet::StackWatermarkKind kind) :
    _state(0),
    _watermark(0),
    _next(NULL),
    _jt(jt),
    _iterator(NULL),
    _lock(Mutex::tty - 1, "stack_watermark_lock", true, Mutex::_safepoint_check_never),
    _kind(kind) {
}

StackWatermark::~StackWatermark() {
  delete _iterator;
}

bool StackWatermark::should_start_iteration() const {
  return StackWatermarkState::epoch(_state) != epoch_id();
}

void StackWatermark::start_iteration(void* context) {
  log_info(stackbarrier)("Starting stack scanning iteration for tid %d",
                         _jt->osthread()->thread_id());
  delete _iterator;
  if (_jt->has_last_Java_frame()) {
    _iterator = new StackWatermarkIterator(*this);
    _iterator->process_one(context); // process callee
    _iterator->process_one(context); // process caller
  } else {
    _iterator = NULL;
  }
  update_watermark();
  uint32_t state = StackWatermarkState::create(epoch_id(), false /* is_done */);
  Atomic::release_store(&_state, state);
}

void StackWatermark::init_epoch() {
  // Disarm when thread is created
  _state = StackWatermarkState::create(epoch_id(), true /* is_done */);
}

void StackWatermark::update_watermark() {
  assert(lock()->owned_by_self(), "invariant");
  if (_iterator != NULL && _iterator->has_next()) {
    assert(_iterator->callee() != 0, "sanity");
    Atomic::release_store(&_watermark, _iterator->callee());
  } else {
    Atomic::release_store(&_watermark, uintptr_t(0)); // Release stack data modifications w.r.t. watermark
    Atomic::release_store(&_state, StackWatermarkState::create(epoch_id(), true /* is_done */)); // release watermark w.r.t. epoch
    log_info(stackbarrier)("Finished stack scanning iteration for tid %d",
                           _jt->osthread()->thread_id());
  }
}

void StackWatermark::process_one(JavaThread* jt) {
  MutexLocker ml(lock(), Mutex::_no_safepoint_check_flag);
  if (should_start_iteration()) {
    start_iteration(NULL /*context */);
    if (iterator() == NULL) {
      update_watermark();
      return;
    }
  }
  StackWatermarkIterator* it = iterator();
  if (it == NULL) {
    return;
  }
  it->process_one(NULL /* context */);
  update_watermark();
}

uintptr_t StackWatermark::watermark() {
  return Atomic::load_acquire(&_watermark);
}

uintptr_t StackWatermark::last_processed() {
  if (watermark() == 0) {
    return 0;
  }
  MutexLocker ml(lock(), Mutex::_no_safepoint_check_flag);
  if (should_start_iteration()) {
    return 0;
  }
  StackWatermarkIterator* it = iterator();
  if (it == NULL) {
    return 0;
  }
  return it->caller();
}
