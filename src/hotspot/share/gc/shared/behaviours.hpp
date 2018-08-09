/*
 * Copyright (c) 2018, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_SHARED_BEHAVIOURS_HPP
#define SHARE_GC_SHARED_BEHAVIOURS_HPP

#include "memory/iterator.hpp"
#include "oops/oopsHierarchy.hpp"

// This is the behaviour for checking if an oop is phantomly alive
class PhantomIsAliveBehaviour {
  BoolObjectClosure *_is_alive;

public:
  PhantomIsAliveBehaviour(BoolObjectClosure *is_alive)
    : _is_alive(is_alive) { }

  bool is_alive(oop obj)  {
    return _is_alive->do_object_b(obj);
  }

  bool is_alive_or_null(oop obj)  {
    return obj == NULL || _is_alive->do_object_b(obj);
  }
};

#endif // SHARE_GC_SHARED_BEHAVIOURS_HPP
