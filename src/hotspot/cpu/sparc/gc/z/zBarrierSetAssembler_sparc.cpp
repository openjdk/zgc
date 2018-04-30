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
#include "gc/z/zBarrierSetAssembler.hpp"
#include "gc/z/zThreadLocalData.hpp"
#include "runtime/sharedRuntime.hpp"

#define __ masm->

#ifdef PRODUCT
#define BLOCK_COMMENT(str) /* nothing */
#else
#define BLOCK_COMMENT(str) __ block_comment(str)
#endif

static Address address_bad_mask() {
  return Address(G2_thread, ZThreadLocalData::address_bad_mask_offset());
}

static address barrier_load_at_entry_point(DecoratorSet decorators) {
  if (decorators & ON_PHANTOM_OOP_REF) {
    return CAST_FROM_FN_PTR(address, SharedRuntime::z_load_barrier_on_phantom_oop_field_preloaded);
  } else if (decorators & ON_WEAK_OOP_REF) {
    return CAST_FROM_FN_PTR(address, SharedRuntime::z_load_barrier_on_weak_oop_field_preloaded);
  } else {
    return CAST_FROM_FN_PTR(address, SharedRuntime::z_load_barrier_on_oop_field_preloaded);
  }
}

void ZBarrierSetAssembler::load_at(MacroAssembler* masm,
                                   DecoratorSet decorators,
                                   BasicType type,
                                   Address src,
                                   Register dst,
                                   Register tmp) {
  if (type != T_OBJECT && type != T_ARRAY) {
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

  // Load value
  __ ld_ptr(Address(tmp, 0), dst);

  // Check bad mask
  __ btst(dst, G6);
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
  __ call_VM_leaf(L7_thread_cache, barrier_load_at_entry_point());
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

  // Restore bad mask
  __ ld_ptr(address_bad_mask(), G6);

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
      __ btst(src, G6);
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

static address barrier_arraycopy_prologue_entry_point() {
  return CAST_FROM_FN_PTR(address, static_cast<void (*)(volatile oop*, size_t)>(ZBarrier::load_barrier_on_oop_array));
}

void ZBarrierSetAssembler::arraycopy_prologue(MacroAssembler* masm,
                                              DecoratorSet decorators,
                                              BasicType type,
                                              Register src,
                                              Register dst,
                                              Register count) {
  if (type != T_OBJECT) {
    // Barrier not needed
    assert(type != T_ARRAY, "Should never happen");
    return;
  }

  BLOCK_COMMENT("ZBarrierSetAssembler::arraycopy_prologue {");

  // Save frame and setup arguments
  __ save_frame_and_mov(0, src, O0, count, O1);

  // Call barrier
  __ call_VM_leaf(L7_thread_cache, barrier_arraycopy_prologue_entry_point());

  // Restore frame
  __ restore();

  BLOCK_COMMENT("} ZBarrierSetAssembler::arraycopy_prologue");
}
