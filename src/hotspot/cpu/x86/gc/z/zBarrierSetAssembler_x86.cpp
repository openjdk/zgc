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
  return Address(r15_thread, ZThreadLocalData::address_bad_mask_offset());
}

static void call_vm(MacroAssembler* masm, address entry_point, Register arg0, Register arg1) {
  // Setup arguments
  if (arg1 == c_rarg0) {
    if (arg0 == c_rarg1) {
      __ xchgptr(c_rarg1, c_rarg0);
    } else {
      __ movptr(c_rarg1, arg1);
      __ movptr(c_rarg0, arg0);
    }
  } else {
    if (arg0 != c_rarg0) {
      __ movptr(c_rarg0, arg0);
    }
    if (arg1 != c_rarg1) {
      __ movptr(c_rarg1, arg1);
    }
  }

  // Call VM
  __ MacroAssembler::call_VM_leaf_base(entry_point, 2);
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

static bool barrier_needed(DecoratorSet decorators, BasicType type) {
  assert((decorators & AS_RAW) == 0, "Unexpected decorator");
  assert((decorators & AS_NO_KEEPALIVE) == 0, "Unexpected decorator");
  assert((decorators & IN_ARCHIVE_ROOT) == 0, "Unexpected decorator");
  assert((decorators & ON_UNKNOWN_OOP_REF) == 0, "Unexpected decorator");

  if (type == T_OBJECT || type == T_ARRAY) {
    if (((decorators & IN_HEAP) != 0) ||
        ((decorators & IN_CONCURRENT_ROOT) != 0) ||
        ((decorators & ON_PHANTOM_OOP_REF) != 0)) {
      // Barrier needed
      return true;
    }
  }

  // Barrier not neeed
  return false;
}

void ZBarrierSetAssembler::load_at(MacroAssembler* masm,
                                   DecoratorSet decorators,
                                   BasicType type,
                                   Register dst,
                                   Address src,
                                   Register tmp1,
                                   Register tmp_thread) {
  if (!barrier_needed(decorators, type)) {
    // Barrier not needed
    BarrierSetAssembler::load_at(masm, decorators, type, dst, src, tmp1, tmp_thread);
    return;
  }

  BLOCK_COMMENT("ZBarrierSetAssembler::load_at {");

  // Allocate scratch register
  Register scratch = tmp1;
  if (tmp1 == noreg) {
    scratch = r12;
    __ push(scratch);
  }

  assert_different_registers(dst, scratch);

  Label done;

  //
  // Fast Path
  //

  // Load address
  __ lea(scratch, src);

  // Load oop at address
  __ movptr(dst, Address(scratch, 0));

  // Check address bad mask
  __ testptr(dst, address_bad_mask());
  __ jcc(Assembler::zero, done);

  //
  // Slow path
  //

  // Save registers
  __ push(rax);
  __ push(rcx);
  __ push(rdx);
  __ push(rdi);
  __ push(rsi);
  __ push(r8);
  __ push(r9);
  __ push(r10);
  __ push(r11);

  // We may end up here from generate_native_wrapper, then the method may have
  // floats as arguments, and we must spill them before calling the VM runtime
  // leaf. From the interpreter all floats are passed on the stack.
  assert(Argument::n_float_register_parameters_j == 8, "Assumption");
  const int xmm_size = wordSize * 2;
  const int xmm_spill_size = xmm_size * Argument::n_float_register_parameters_j;
  __ subptr(rsp, xmm_spill_size);
  __ movdqu(Address(rsp, xmm_size * 7), xmm7);
  __ movdqu(Address(rsp, xmm_size * 6), xmm6);
  __ movdqu(Address(rsp, xmm_size * 5), xmm5);
  __ movdqu(Address(rsp, xmm_size * 4), xmm4);
  __ movdqu(Address(rsp, xmm_size * 3), xmm3);
  __ movdqu(Address(rsp, xmm_size * 2), xmm2);
  __ movdqu(Address(rsp, xmm_size * 1), xmm1);
  __ movdqu(Address(rsp, xmm_size * 0), xmm0);

  // Call VM
  call_vm(masm, barrier_load_at_entry_point(decorators), dst, scratch);

  // Restore registers
  __ movdqu(xmm0, Address(rsp, xmm_size * 0));
  __ movdqu(xmm1, Address(rsp, xmm_size * 1));
  __ movdqu(xmm2, Address(rsp, xmm_size * 2));
  __ movdqu(xmm3, Address(rsp, xmm_size * 3));
  __ movdqu(xmm4, Address(rsp, xmm_size * 4));
  __ movdqu(xmm5, Address(rsp, xmm_size * 5));
  __ movdqu(xmm6, Address(rsp, xmm_size * 6));
  __ movdqu(xmm7, Address(rsp, xmm_size * 7));
  __ addptr(rsp, xmm_spill_size);

  __ pop(r11);
  __ pop(r10);
  __ pop(r9);
  __ pop(r8);
  __ pop(rsi);
  __ pop(rdi);
  __ pop(rdx);
  __ pop(rcx);

  if (dst == rax) {
    __ addptr(rsp, wordSize);
  } else {
    __ movptr(dst, rax);
    __ pop(rax);
  }

  __ bind(done);

  // Restore scratch register
  if (tmp1 == noreg) {
    __ pop(scratch);
  }

  BLOCK_COMMENT("} ZBarrierSetAssembler::load_at");
}

#ifdef ASSERT
void ZBarrierSetAssembler::store_at(MacroAssembler* masm,
                                    DecoratorSet decorators,
                                    BasicType type,
                                    Address dst,
                                    Register src,
                                    Register tmp1,
                                    Register tmp2) {
  BLOCK_COMMENT("ZBarrierSetAssembler::store_at {");

  // Verify oop store
  if (type == T_OBJECT || type == T_ARRAY) {
    // Note that src could be noreg, which means we
    // are storing null and can skip verification.
    if (src != noreg) {
      Label done;
      __ testptr(src, address_bad_mask());
      __ jcc(Assembler::zero, done);
      __ stop("Verify oop store failed");
      __ should_not_reach_here();
      __ bind(done);
    }
  }

  // Store value
  BarrierSetAssembler::store_at(masm, decorators, type, dst, src, tmp1, tmp2);

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
  if (!barrier_needed(decorators, type)) {
    // Barrier not needed
    return;
  }

  BLOCK_COMMENT("ZBarrierSetAssembler::arraycopy_prologue {");

  // Save registers
  __ pusha();

  // Call VM
  call_vm(masm, barrier_arraycopy_prologue_entry_point(), src, count);

  // Restore registers
  __ popa();

  BLOCK_COMMENT("} ZBarrierSetAssembler::arraycopy_prologue");
}

static Address address_bad_mask_from_jni_env(Register env) {
  return Address(env, ZThreadLocalData::address_bad_mask_offset() - JavaThread::jni_environment_offset());
}

void ZBarrierSetAssembler::try_resolve_jobject_in_native(MacroAssembler* masm,
                                                         Register obj,
                                                         Register tmp,
                                                         Label& slowpath) {
  // NOTE! The code generated here is executed in native context, and therefore
  // we don't have the thread pointer in r15_thread. However, we do have the
  // JNIEnv* in c_rarg0 from the call to JNI_FastGetField and so we use that to
  // get the address bad mask.

  BLOCK_COMMENT("ZBarrierSetAssembler::try_resolve_jobject_in_native {");

  // Resolve jobject
  BarrierSetAssembler::try_resolve_jobject_in_native(masm, obj, tmp, slowpath);

  // Check address bad mask
  __ testptr(obj, address_bad_mask_from_jni_env(c_rarg0));
  __ jcc(Assembler::notZero, slowpath);

  BLOCK_COMMENT("} ZBarrierSetAssembler::try_resolve_jobject_in_native");
}
