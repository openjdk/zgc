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
#include "opto/compile.hpp"
#include "opto/node.hpp"
#include "opto/output.hpp"
#include "opto/safepointPollStubTable.hpp"
#include "runtime/sharedRuntime.hpp"

Label& C2SafepointPollStubTable::add_safepoint(InternalAddress safepoint_addr) {
  C2SafepointPollStub* entry = new (Compile::current()->comp_arena()) C2SafepointPollStub(safepoint_addr);
  int index = _safepoints.append(entry);
  return _safepoints.at(index)->_stub_label;
}

int C2SafepointPollStubTable::estimate_stub_size() const {
  Compile* const C = Compile::current();
  BufferBlob* const blob = C->output()->scratch_buffer_blob();
  int size = 0;

  for (int i = _safepoints.length() - 1; i >= 0; i--) {
    CodeBuffer cb(blob->content_begin(), (address)C->output()->scratch_locs_memory() - blob->content_begin());
    MacroAssembler masm(&cb);
    C2SafepointPollStub* entry = _safepoints.at(i);
    emit_stub(masm, entry);
    size += cb.insts_size();
  }

  return size;
}

#define __ _masm.
void C2SafepointPollStubTable::emit(CodeBuffer& cb) {
  MacroAssembler _masm(&cb);
  for (int i = _safepoints.length() - 1; i >= 0; i--) {
    // Make sure there is enough space in the code buffer
    if (cb.insts()->maybe_expand_to_ensure_remaining(PhaseOutput::MAX_inst_size) && cb.blob() == NULL) {
      ciEnv::current()->record_failure("CodeCache is full");
      return;
    }

    C2SafepointPollStub* entry = _safepoints.at(i);
    emit_stub(_masm, entry);
  }
}

void C2SafepointPollStubTable::emit_stub(MacroAssembler& _masm, C2SafepointPollStub* entry) const {
  address stub;

  assert(SharedRuntime::polling_page_return_handler_blob() != NULL,
         "polling page return stub not created yet");
  stub = SharedRuntime::polling_page_return_handler_blob()->entry_point();

  RuntimeAddress callback_addr(stub);

  __ bind(entry->_stub_label);
  __ lea(rscratch1, entry->_safepoint_addr);
  __ str(rscratch1, Address(rthread, JavaThread::saved_exception_pc_offset()));
  __ far_jump(callback_addr);
}
#undef __
