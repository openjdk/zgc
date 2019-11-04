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

#ifndef SHARE_RUNTIME_STACKWATERMARKSET_INLINE_HPP
#define SHARE_RUNTIME_STACKWATERMARKSET_INLINE_HPP

#include "runtime/stackWatermark.hpp"
#include "runtime/stackWatermarkSet.hpp"

template <typename T>
T* StackWatermarkSet::get(StackWatermarkKind kind) {
  for (StackWatermark* stack_watermark = _head; stack_watermark != NULL; stack_watermark = stack_watermark->next()) {
    if (stack_watermark->kind() == kind) {
      return static_cast<T*>(stack_watermark);
    }
  }
  return NULL;
}

inline bool StackWatermarkSet::has_watermark(StackWatermarkKind kind) {
  return get<StackWatermark>(kind) != NULL;
}

inline static bool state_is_done(uint32_t state) {
  return state & 1;
}

inline static uint32_t state_epoch(uint32_t state) {
  return state >> 1;
}

inline static uint32_t create_state(uint32_t epoch, bool is_done) {
  return (epoch << 1) | (is_done ? 1u : 0u);
}

#endif // SHARE_RUNTIME_STACKWATERMARKSET_INLINE_HPP
