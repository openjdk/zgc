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

static inline bool is_above_watermark(JavaThread* jt, uintptr_t sp, uintptr_t watermark) {
  if (watermark == 0) {
    return false;
  }
  return sp > watermark;
}

static inline bool is_above_watermark(JavaThread* jt, frame last_frame, uintptr_t watermark) {
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

inline bool StackWatermark::needs_processing(frame fr) {
  uint32_t state = Atomic::load_acquire(&_state);
  if (StackWatermarkState::is_done(state)) {
    return false;
  }
  return is_above_watermark(_jt, fr, watermark());
}

inline void StackWatermark::ensure_processed(frame fr) {
  assert(!should_start_iteration(),
         "Iteration should already have started");
  if (!needs_processing(fr)) {
    return;
  }
  process_one();
  assert(is_frame_safe(fr), "frame should be safe after processing");
}

inline void StackWatermark::on_unwind() {
  ensure_processed(_jt->last_frame());
}

inline void StackWatermark::on_iteration(frame fr) {
  if (process_on_iteration()) {
    ensure_processed(fr);
  }
}

#endif // SHARE_RUNTIME_STACKWATERMARK_INLINE_HPP
