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

#ifndef SHARE_RUNTIME_STACKWATERMARKSET_HPP
#define SHARE_RUNTIME_STACKWATERMARKSET_HPP

#include "memory/allocation.hpp"
#include "runtime/frame.hpp"

class JavaThread;
class StackWatermark;

class StackWatermarkSetInstance {
  friend class StackWatermarkSet;
private:
  StackWatermark* _head;

public:
  StackWatermarkSetInstance();
  ~StackWatermarkSetInstance();
};

class StackWatermarkSet : public AllStatic {
public:
  enum Kind {
    gc
  };

private:
  static StackWatermark* head(JavaThread* jt);
  static void set_head(JavaThread* jt, StackWatermark* watermark);

public:
  static void add_watermark(JavaThread* jt, StackWatermark* watermark);

  static StackWatermark* get(JavaThread* jt, Kind kind);

  template <typename T>
  static T* get(JavaThread* jt, Kind kind);

  static bool has_watermark(JavaThread* jt, Kind kind);

  // Called when a thread is about to unwind a frame
  static void before_unwind(JavaThread* jt);

  // Called when a thread just unwinded a frame
  static void after_unwind(JavaThread* jt);

  // Called by stack walkers when walking into a frame
  static void on_iteration(JavaThread* jt, frame fr);

  // Called to ensure iterations are initialized
  static void start_iteration(JavaThread* jt, Kind kind);

  // Called to finish an iteration
  static void finish_iteration(JavaThread* jt, void* context, Kind kind);


  static uintptr_t lowest_watermark(JavaThread* jt);
};

#endif // SHARE_RUNTIME_STACKWATERMARKSET_HPP
