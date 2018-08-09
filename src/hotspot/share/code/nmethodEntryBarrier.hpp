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

#ifndef SHARE_CODE_NMETHOD_BARRIER_HPP
#define SHARE_CODE_NMETHOD_BARRIER_HPP

#include "memory/allocation.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/sizes.hpp"

class nmethod;

class NMethodEntryBarrier: public CHeapObj<mtGC> {
  bool supports_entry_barrier(nmethod* nm);

protected:
  class StubEntry;
  virtual int disarmed_value() const = 0;
  virtual bool nmethod_entry_barrier(nmethod* nm) = 0;

public:
  // Entry from compiled stub
  static int nmethod_stub_entry_barrier(address* return_address_ptr);
  bool nmethod_osr_entry_barrier(nmethod* nm);
  bool is_armed(nmethod* nm);
  void disarm_barrier(nmethod* nm);

  virtual ByteSize thread_disarmed_offset() const = 0;
};

class NMethodEntryBarrier::StubEntry: StackObj {
  NMethodEntryBarrier* _barrier;
  bool                 _is_deoptimized;
  nmethod*             _nm;
  address*             _return_address_ptr;

public:
  StubEntry(nmethod* nm);
  StubEntry(nmethod* nm, address* return_address_ptr);

  bool is_deoptimized() const { return _is_deoptimized; }
  void deoptimize();
  void disarm_barrier();
};


#endif // SHARE_CODE_NMETHOD_BARRIER_HPP
