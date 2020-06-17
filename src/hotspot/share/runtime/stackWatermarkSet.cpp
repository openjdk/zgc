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
#include "runtime/safepointMechanism.inline.hpp"
#include "runtime/stackWatermark.hpp"
#include "runtime/stackWatermarkSet.hpp"
#include "runtime/thread.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/preserveException.hpp"
#include "utilities/vmError.hpp"

StackWatermarkSet::StackWatermarkSet() :
    _head(NULL) {
}

StackWatermarkSet::~StackWatermarkSet() {
  StackWatermark* current = _head;
  while (current != NULL) {
    StackWatermark* next = current->next();
    delete current;
    current = next;
  }
}

static bool is_above_watermark(JavaThread* jt, uintptr_t sp, uintptr_t watermark) {
  if (watermark == 0) {
    return false;
  }
  bool result = sp > watermark;
  return result;
}

static bool is_above_watermark(JavaThread* jt, frame last_frame, uintptr_t watermark) {
  RegisterMap map(jt, false /* update_map */, false /* process_frames */);
  if (last_frame.is_safepoint_blob_frame()) {
    // The return barrier of compiled methods might get to the runtime through a
    // safepoint blob. Skip it to the frame that triggered the barrier.
    last_frame = last_frame.sender(&map);
  }
  if (!last_frame.is_first_frame()) {
    last_frame = last_frame.sender(&map);
  }
  return is_above_watermark(jt, reinterpret_cast<uintptr_t>(last_frame.sp()), watermark);
}

static bool is_above_watermark(JavaThread* jt, uintptr_t watermark) {
  return is_above_watermark(jt, jt->last_frame_raw(), watermark);
}

static void verify_poll_context() {
#ifdef ASSERT
  Thread* thread = Thread::current();
  if (thread->is_Java_thread()) {
    JavaThread* jt = static_cast<JavaThread*>(thread);
    JavaThreadState state = jt->thread_state();
    assert(state != _thread_in_native && state != _thread_blocked, "unsafe thread state");
  } else if (thread->is_VM_thread()) {
  } else {
    assert(SafepointSynchronize::is_at_safepoint() || Threads_lock->owned_by_self(),
           "non-java threads must block out safepoints with Threads_lock");
  }
#endif
}

void StackWatermarkSet::on_unwind(JavaThread* jt) {
  verify_poll_context();
  StackWatermark* current = jt->stack_watermark_set()->_head;
  if (current == NULL) {
    return;
  }
  for (; current != NULL; current = current->next()) {
    uint32_t state = Atomic::load_acquire(&current->_state);
    assert(StackWatermarkState::epoch(state) == current->epoch_id(),
           "Starting new stack snapshots is done explicitly or when waking up from safepoints");
    if (StackWatermarkState::is_done(state)) {
      continue;
    } else if (jt->has_last_Java_frame() && !is_above_watermark(jt, current->watermark())) {
      continue;
    }

    current->process_one(jt);
  }

  SafepointMechanism::update_poll_values(jt);
}

void StackWatermarkSet::on_vm_operation(JavaThread* jt) {
  verify_poll_context();
  for (StackWatermark* current = jt->stack_watermark_set()->_head; current != NULL; current = current->next()) {
    uint32_t state = Atomic::load_acquire(&current->_state);
    current->process_one(jt);
  }
}

void StackWatermarkSet::on_iteration(JavaThread* jt, frame fr) {
  if (VMError::is_error_reported()) {
    // Don't perform barrier when error reporting walks the stack.
    return;
  }
  verify_poll_context();
  for (StackWatermark* current = jt->stack_watermark_set()->_head; current != NULL; current = current->next()) {
    if (!current->process_for_iterator()) {
      continue;
    }
    uint32_t state = Atomic::load_acquire(&current->_state);
    if (StackWatermarkState::epoch(state) == current->epoch_id()) {
      if (StackWatermarkState::is_done(state)) {
        continue;
      } else if (!is_above_watermark(jt, fr, current->watermark())) {
        continue;
      }
    }
    current->process_one(jt);
  }
}

void StackWatermarkSet::start_iteration(JavaThread* jt, StackWatermarkKind kind) {
  verify_poll_context();
  StackWatermark* current = jt->stack_watermark_set()->_head;
  if (current == NULL) {
    return;
  }
  for (; current != NULL; current = current->next()) {
    if (current->kind() != kind) {
      continue;
    }
    uint32_t state = Atomic::load_acquire(&current->_state);
    if (StackWatermarkState::epoch(state) == current->epoch_id()) {
      continue;
    }
    current->process_one(jt);
  }
}

void StackWatermarkSet::finish_iteration(JavaThread* jt, void* context, StackWatermarkKind kind) {
  for (StackWatermark* current = jt->stack_watermark_set()->_head; current != NULL; current = current->next()) {
    if (current->kind() != kind) {
      continue;
    }
    MutexLocker ml(current->lock(), Mutex::_no_safepoint_check_flag);
    if (current->should_start_iteration()) {
      current->start_iteration(context);
    }
    StackWatermarkIterator* iterator = current->iterator();
    if (iterator != NULL) {
      iterator->process_all(context);
    }
    current->update_watermark();
  }
}

uintptr_t StackWatermarkSet::lowest_watermark() {
  uintptr_t max_watermark = uintptr_t(0) - 1;
  uintptr_t watermark = max_watermark;
  StackWatermark* current = _head;
  while (current != NULL) {
    watermark = MIN2(watermark, current->watermark());
    current = current->_next;
  }
  if (watermark == max_watermark) {
    return 0;
  } else {
    return watermark;
  }
}

void StackWatermarkSet::add_watermark(StackWatermark* watermark) {
  StackWatermark* prev = _head;
  watermark->_next = prev;
  _head = watermark;
}
