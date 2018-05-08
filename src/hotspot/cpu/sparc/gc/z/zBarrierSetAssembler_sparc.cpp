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
 */

#include "precompiled.hpp"
#include "asm/macroAssembler.inline.hpp"
#include "gc/z/zBarrier.inline.hpp"
#include "gc/z/zBarrierSet.hpp"
#include "gc/z/zBarrierSetAssembler.hpp"
#include "gc/z/zBarrierSetRuntime.hpp"
#ifdef COMPILER1
#include "gc/z/c1/zBarrierSetC1.hpp"
#endif // COMPILER1

#undef __
#define __ masm->

#ifdef PRODUCT
#define BLOCK_COMMENT(str) /* nothing */
#else
#define BLOCK_COMMENT(str) __ block_comment(str)
#endif

#define G6_badmask G6

void ZBarrierSetAssembler::load_at(MacroAssembler* masm,
                                   DecoratorSet decorators,
                                   BasicType type,
                                   Address src,
                                   Register dst,
                                   Register tmp) {
  if (!ZBarrierSet::barrier_needed(decorators, type)) {
    // Barrier not needed
    BarrierSetAssembler::load_at(masm, decorators, type, src, dst, tmp);
    return;
  }

  assert_different_registers(src.base(), src.index(), tmp);
  assert_different_registers(dst, tmp);

  Label done;

  BLOCK_COMMENT("ZBarrierSetAssembler::load_at {");

  //
  // Fast path
  //

  // Load address
  if (Assembler::is_simm13(src.disp())) {
    if (src.index()->is_valid()) {
      __ add(src.base(), src.disp(), tmp);
      __ add(src.index(), tmp, tmp);
    } else {
      __ add(src.base(), src.disp(), tmp);
    }
  } else {
    __ set(src.disp(), tmp);
    if (src.index()->is_valid()) {
      __ add(src.index(), tmp, tmp);
    }
    __ add(src.base(), tmp, tmp);
  }

  // Load oop at address
  __ ld_ptr(Address(tmp, 0), dst);

  // Test address bad mask
  __ btst(dst, G6_badmask);
  __ brx(Assembler::zero, false, Assembler::pt, done);
  __ delayed()->nop();

  //
  // Slow path
  //

  // Call the slow path
  __ save_frame_and_mov(0, dst, O0, tmp, O1);
  __ mov(G1, L1);
  __ mov(G2, L2);
  __ mov(G3, L3);
  __ mov(G4, L4);
  __ mov(G5, L5);
  __ mov(G7, L6);
  __ call_VM_leaf(L7_thread_cache, ZBarrierSetRuntime::load_barrier_on_oop_field_preloaded_addr(decorators));
  __ mov(L1, G1);
  __ mov(L2, G2);
  __ mov(L3, G3);
  __ mov(L4, G4);
  __ mov(L5, G5);
  __ mov(L6, G7);

  // Save result
  __ mov(O0, G6);

  __ restore();

  // Restore result
  __ mov(G6, dst);

  // Restore address bad mask
  __ ld_ptr(address_bad_mask_from_thread(G2_thread), G6_badmask);

  __ bind(done);

  // Verify result
  __ verify_oop(dst);

  BLOCK_COMMENT("} ZBarrierSetAssembler::load_at");
}

#ifdef ASSERT
void ZBarrierSetAssembler::store_at(MacroAssembler* masm,
                                    DecoratorSet decorators,
                                    BasicType type,
                                    Register src,
                                    Address dst,
                                    Register tmp) {
  BLOCK_COMMENT("ZBarrierSetAssembler::store_at {");

  // Verify value
  if (type == T_OBJECT || type == T_ARRAY) {
    // Note that src could be noreg, which means we
    // are storing null and can skip verification.
    if (src != noreg) {
      Label done;
      __ btst(src, G6_badmask);
      __ brx(Assembler::zero, false, Assembler::pt, done);
      __ stop("Verify oop store failed");
      __ should_not_reach_here();
      __ bind(done);
    }
  }

  // Store value
  BarrierSetAssembler::store_at(masm, decorators, type, src, dst, tmp);

  BLOCK_COMMENT("} ZBarrierSetAssembler::store_at");
}
#endif // ASSERT

void ZBarrierSetAssembler::arraycopy_prologue(MacroAssembler* masm,
                                              DecoratorSet decorators,
                                              BasicType type,
                                              Register src,
                                              Register dst,
                                              Register count) {
  if (!ZBarrierSet::barrier_needed(decorators, type)) {
    // Barrier not needed
    return;
  }

  BLOCK_COMMENT("ZBarrierSetAssembler::arraycopy_prologue {");

  // Save frame and setup arguments
  __ save_frame_and_mov(0, src, O0, count, O1);

  // Call barrier
  __ call_VM_leaf(L7_thread_cache, ZBarrierSetRuntime::load_barrier_on_oop_array_addr());

  // Restore frame
  __ restore();

  BLOCK_COMMENT("} ZBarrierSetAssembler::arraycopy_prologue");
}

void ZBarrierSetAssembler::try_resolve_jobject_in_native(MacroAssembler* masm,
                                                         Register obj,
                                                         Register tmp,
                                                         Label& slowpath) {
  // NOTE! The code generated here is executed in native context, and therefore
  // we don't have the address bad mask in G6 and we don't have the thread pointer
  // in G2_thread. However, we do have the JNIEnv* in c_rarg0 from the call to
  // JNI_FastGetField and so we use that to get the address bad mask.

  BLOCK_COMMENT("ZBarrierSetAssembler::try_resolve_jobject_in_native {");

  // Resolve jobject
  BarrierSetAssembler::try_resolve_jobject_in_native(masm, obj, tmp, slowpath);

  // Load address bad mask
  __ ld_ptr(address_bad_mask_from_jni_env(O0), tmp);

  // Test address bad mask
  __ btst(obj, tmp);
  __ brx(Assembler::notZero, false, Assembler::pn, slowpath);
  __ delayed()->nop();

  BLOCK_COMMENT("} ZBarrierSetAssembler::try_resolve_jobject_in_native");
}

#ifdef COMPILER1

#undef __
#define __ ce->masm()->

void ZBarrierSetAssembler::generate_c1_load_barrier_test(LIR_Assembler* ce,
                                                         LIR_Opr ref) const {
  __ btst(ref->as_register(), G6_badmask);
}

void ZBarrierSetAssembler::generate_c1_load_barrier_stub(LIR_Assembler* ce,
                                                         ZLoadBarrierStubC1* stub) const {
  const LIR_Opr ref = stub->ref();
  const LIR_Opr ref_addr = stub->ref_addr();
  const LIR_Opr tmp = stub->tmp();

  assert(ref->is_register(), "Must be a register");
  assert(ref_addr->is_register() != tmp->is_register(), "Only one should be a register");

  // Stub entry
  __ bind(*stub->entry());

  Register ref_reg = ref->as_register();
  Register ref_addr_reg = noreg;

  if (ref_addr->is_register()) {
    // Address already in register
    ref_addr_reg = ref_addr->as_pointer_register();
  } else {
    assert(ref_addr->is_address(), "Must be an address");
    if (ref_addr->as_address_ptr()->index()->is_valid() ||
        ref_addr->as_address_ptr()->disp() != 0) {
      // Has index or displacement, need to load address into register
      ce->leal(ref_addr, tmp, stub->patch_code(), stub->patch_info());
      ref_addr_reg = tmp->as_pointer_register();
    } else {
      // No index or displacement, address available in base register
      ref_addr_reg = ref_addr->as_address_ptr()->base()->as_pointer_register();
    }
  }

  assert_different_registers(ref_reg, ref_addr_reg, noreg);

  // Setup arguments and call runtime stub
  __ mov(ref_addr_reg, G5);
  __ call(stub->runtime_stub());
  __ delayed()->mov(ref_reg, G4);

  // Verify result
  __ verify_oop(G4);

  // Return result and exit stub
  __ br(Assembler::always, false, Assembler::pt, *stub->continuation());
  __ delayed()->mov(G4, ref_reg);
}

#undef __
#define __ sasm->

void ZBarrierSetAssembler::generate_c1_load_barrier_runtime_stub(StubAssembler* sasm,
                                                                 DecoratorSet decorators) const {
  // Save frame
  __ save_frame(0);

  // Call VM
  __ call_VM_leaf(L7_thread_cache,
                  ZBarrierSetRuntime::load_barrier_on_oop_field_preloaded_addr(decorators),
                  G4, G5);

  // Move result to G4
  __ mov(O0, G4);

  // Restore frame and return
  __ ret();
  __ delayed()->restore();
}

#endif // COMPILER1
