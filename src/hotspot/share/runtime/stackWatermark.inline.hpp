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

#ifndef SHARE_RUNTIME_STACKWATERMARK_INLINE_HPP
#define SHARE_RUNTIME_STACKWATERMARK_INLINE_HPP

#include "runtime/stackWatermark.hpp"
#include "runtime/thread.hpp"

static inline bool is_above_watermark(uintptr_t sp, uintptr_t watermark) {
  if (watermark == 0) {
    return false;
  }
  return sp > watermark;
}

inline bool StackWatermark::has_barrier(frame& f) {
  if (f.is_interpreted_frame()) {
    return true;
  }
  if (f.is_compiled_frame()) {
    nmethod* nm = f.cb()->as_nmethod();
    if (nm->is_compiled_by_c1() || nm->is_compiled_by_c2()) {
      return true;
    }
    if (nm->is_native_method()) {
      return true;
    }
  }
  return false;
}

inline bool StackWatermark::iteration_started(uint32_t state) const {
  return StackWatermarkState::epoch(state) == epoch_id();
}

inline bool StackWatermark::iteration_completed(uint32_t state) const {
  assert(iteration_started(state), "Check is only valid if iteration has been started");
  return StackWatermarkState::is_done(state);
}

inline void StackWatermark::ensure_safe(frame f) {
  assert(iteration_started(), "Iteration should already have started");

  if (iteration_completed_acquire()) {
    return;
  }

  // Get caller sp to get the callee fp.
  RegisterMap map(_jt, false /* update_map */, false /* process_frames */);
  frame f_caller = f.sender(&map);
  uintptr_t f_fp = reinterpret_cast<uintptr_t>(f_caller.sp());

  if (is_above_watermark(f_fp, watermark())) {
    process_one();
  }

  assert(is_frame_safe(f), "frame should be safe after processing");
}

inline void StackWatermark::before_unwind() {
  frame f = _jt->last_frame();

  // Skip any stub frames etc up until the frame that triggered before_unwind().
  RegisterMap map(_jt, false /* update_map */, false /* process_frames */);
  if (f.is_safepoint_blob_frame() || f.is_runtime_frame()) {
    f = f.sender(&map);
  }

  assert(is_frame_safe(f), "frame should be safe before processing");
  assert(!f.is_runtime_frame(), "should have skipped all runtime stubs");

  // before_unwind() potentially exposes a new frame. The new exposed frame is
  // always the caller of the top frame.
  if (!f.is_first_frame()) {
    f = f.sender(&map);
    ensure_safe(f);
  }
}

inline void StackWatermark::after_unwind() {
  frame f = _jt->last_frame();

  if (f.is_safepoint_blob_frame() || f.is_runtime_frame()) {
    // Skip safepoint blob.
    RegisterMap map(_jt, false /* update_map */, false /* process_frames */);
    f = f.sender(&map);
  }

  assert(!f.is_runtime_frame(), "should have skipped all runtime stubs");

  // after_unwind() potentially exposes the top frame.
  ensure_safe(f);
}

inline void StackWatermark::on_iteration(frame f) {
  if (process_on_iteration()) {
    ensure_safe(f);
  }
}

#endif // SHARE_RUNTIME_STACKWATERMARK_INLINE_HPP
