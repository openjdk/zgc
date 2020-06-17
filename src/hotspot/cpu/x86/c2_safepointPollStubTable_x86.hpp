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

#ifndef CPU_X86_C2_SAFEPOINTPOLLSTUBTABLE_X86_HPP
#define CPU_X86_C2_SAFEPOINTPOLLSTUBTABLE_X86_HPP

#include "asm/macroAssembler.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/macros.hpp"

#if defined(_LP64)
class C2SafepointPollStubTable {
private:
  struct C2SafepointPollStub: public ResourceObj {
    InternalAddress _safepoint_addr;
    Label _stub_label;
    Label _trampoline_label;
    C2SafepointPollStub(InternalAddress safepoint_addr) :
      _safepoint_addr(safepoint_addr),
      _stub_label(),
      _trampoline_label() {}
  };
  GrowableArray<C2SafepointPollStub*> _safepoints;

  void emit_stub(MacroAssembler& _masm, C2SafepointPollStub* entry) const;

public:
  Label& add_safepoint(InternalAddress safepoint_addr);

  int estimate_stub_size() const;
  void emit(CodeBuffer& cb);
};
#else
class C2SafepointPollStubTable {
public:
  int estimate_stub_size() { return 0; }
  void emit(CodeBuffer &cb) {}
};
#endif

#endif /* CPU_X86_C2_SAFEPOINTPOLLSTUBTABLE_X86_HPP */
