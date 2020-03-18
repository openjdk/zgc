/*
 * Copyright (c) 2017, 2019, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_RUNTIME_SAFEPOINTMECHANISM_HPP
#define SHARE_RUNTIME_SAFEPOINTMECHANISM_HPP

#include "runtime/globals.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"
#include "utilities/sizes.hpp"

class Thread;
class JavaThread;

// This is the abstracted interface for the safepoint implementation
class SafepointMechanism : public AllStatic {
  friend class StackWatermark;
  static uintptr_t _poll_page_armed_value;
  static uintptr_t _poll_page_disarmed_value;

  static uintptr_t _poll_word_armed_value;
  static uintptr_t _poll_word_disarmed_value;

  static uintptr_t poll_page_armed_value()    { return _poll_page_armed_value; }
  static uintptr_t poll_page_disarmed_value() { return _poll_page_disarmed_value; }

  static uintptr_t poll_word_armed_value()    { return _poll_word_armed_value; }
  static uintptr_t poll_word_disarmed_value() { return _poll_word_disarmed_value; }

  static inline bool local_poll_armed(JavaThread* thread);

  static inline void disarm_local_poll(JavaThread* thread);

  static inline bool local_poll(Thread* thread);
  static inline bool global_poll();

  static void process_operation(JavaThread *thread);

  static void default_initialize();

  static void pd_initialize() NOT_AIX({ default_initialize(); });

  static uintptr_t compute_poll_word(bool armed, uintptr_t stack_watermark);

  const static intptr_t _poll_bit = 1;
public:
  static intptr_t poll_bit() { return _poll_bit; }

  struct ThreadData {
    volatile uintptr_t _polling_word;
    volatile uintptr_t _polling_page;

    inline void set_polling_word(uintptr_t poll_value);
    inline uintptr_t get_polling_word();

    inline void set_polling_page(uintptr_t poll_value);
    inline uintptr_t get_polling_page();
  };

  static bool uses_global_page_poll() { return !uses_thread_local_poll(); }
  static bool uses_thread_local_poll() {
#ifdef THREAD_LOCAL_POLL
    return true;
#else
    return false;
#endif
  }

  // Call this method to see if this thread should block for a safepoint or process handshake.
  static inline bool should_process_operation(Thread* thread);

  // Blocks a thread until safepoint/handshake is completed.
  static inline void process_operation_if_requested(JavaThread* thread);
  // The slow path is triggered when we are certain a fast path has allowed it.
  static void process_operation_if_requested_slow(JavaThread *thread);
  // Compute what the poll values should be and install them.
  static void update_poll_values(JavaThread* thread);

  // Caller is responsible for using a memory barrier if needed.
  static inline void arm_local_poll(JavaThread* thread);
  // Release semantics
  static inline void arm_local_poll_release(JavaThread* thread);

  // Setup the selected safepoint mechanism
  static void initialize();
  static void initialize_header(JavaThread* thread);
};

#endif // SHARE_RUNTIME_SAFEPOINTMECHANISM_HPP
