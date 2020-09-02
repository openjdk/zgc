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

inline void StackWatermark::ensure_safe(frame f) {
  assert(!should_start_iteration(), "Iteration should already have started");

  uint32_t state = Atomic::load_acquire(&_state);
  if (StackWatermarkState::is_done(state)) {
    return;
  }

  assert(is_frame_processed(f), "frame should be safe before processing");

  if (is_above_watermark(reinterpret_cast<uintptr_t>(f.sp()), watermark())) {
    process_one();
  }

  assert(is_frame_safe(f), "frame should be safe after processing");
  assert(!is_above_watermark(reinterpret_cast<uintptr_t>(f.sp()), watermark()), "");
}

inline void StackWatermark::on_unwind() {
  const frame& f = _jt->last_frame();
  assert(is_frame_safe(f), "frame should be safe after processing");

  if (f.is_first_frame()) {
    return;
  }

  // on_unwind() potentially exposes a new frame. The new exposed frame is
  // always the caller of the top frame, but for two different reasons.
  //
  // 1) Return sites in nmethods unwind the frame *before* polling. In other
  //    words, the frame of the nmethod performing the poll, will not be
  //    on-stack when it gets to the runtime. However, it trampolines into the
  //    runtime with a safepoint blob, which will be the top frame. Therefore,
  //    the caller of the safepoint blob, will be the new exposed frame.
  //
  // 2) All other calls to on_unwind() perform the unwinding *after* polling.
  //    Therefore, the caller of the top frame will be the new exposed frame.

  RegisterMap map(_jt, false /* update_map */, false /* process_frames */);
  const frame& caller = f.sender(&map);

  ensure_safe(caller);
}

inline void StackWatermark::on_iteration(frame f) {
  if (process_on_iteration()) {
    ensure_safe(f);
  }
}

#endif // SHARE_RUNTIME_STACKWATERMARK_INLINE_HPP
