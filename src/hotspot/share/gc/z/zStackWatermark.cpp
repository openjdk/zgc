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
#include "gc/z/zBarrier.inline.hpp"
#include "gc/z/zOopClosures.inline.hpp"
#include "gc/z/zStackWatermark.hpp"
#include "gc/z/zThreadLocalAllocBuffer.hpp"
#include "gc/z/zThreadLocalData.hpp"
#include "gc/z/zAddress.hpp"
#include "memory/resourceArea.inline.hpp"
#include "runtime/frame.inline.hpp"
#include "utilities/preserveException.hpp"

void ZMarkConcurrentStackRootsClosure::do_oop(oop* p) {
  ZBarrier::mark_barrier_on_oop_field(p, false /* finalizable */);
}

void ZMarkConcurrentStackRootsClosure::do_oop(narrowOop* p) {
  ShouldNotReachHere();
}

ZOnStackCodeBlobClosure::ZOnStackCodeBlobClosure() :
  _bs_nm(BarrierSet::barrier_set()->barrier_set_nmethod()) {
}

void ZOnStackCodeBlobClosure::do_code_blob(CodeBlob* cb) {
  nmethod* nm = cb->as_nmethod_or_null();
  if (nm != NULL) {
    bool result = _bs_nm->nmethod_entry_barrier(nm);
    assert(result, "nmethod on-stack must be alive");
  }
}

uint32_t ZStackWatermark::epoch_id() const {
  const uintptr_t mask_addr = reinterpret_cast<uintptr_t>(&ZAddressGoodMask);
  const uintptr_t epoch_addr = mask_addr + ZNMethodDisarmedOffset;
  return *reinterpret_cast<uint32_t*>(epoch_addr);
}

ZStackWatermark::ZStackWatermark(JavaThread* jt) :
  StackWatermark(jt, StackWatermarkSet::gc),
  _jt_cl(),
  _cb_cl() {
}

class ZVerifyBadOopClosure : public OopClosure {
public:
  virtual void do_oop(oop* p) {
    oop o = *p;
    vmassert(o == NULL || ZAddress::is_bad(ZOop::to_address(o)),
             "this oop is too good to be true");
  }

  virtual void do_oop(narrowOop* p) {
    ShouldNotReachHere();
  }
};

void ZStackWatermark::start_iteration(void* context) {
  OopClosure* gc_cl = reinterpret_cast<OopClosure*>(context);
#ifdef ASSERT
  {
    ZVerifyBadOopClosure verify_cl;
    _jt->oops_do(&verify_cl, NULL, false /* do_frames */);
  }
#endif

  if (Thread::current()->is_ConcurrentGC_thread()) {
    _jt->oops_do(gc_cl, &_cb_cl, false /* do_frames */);
  } else {
    _jt->oops_do(&_jt_cl, &_cb_cl, false /* do_frames */);
  }

#ifdef ASSERT
  {
    Thread* thread = Thread::current();
    ResetNoHandleMark rnhm;
    HandleMark hm(thread);
    PreserveExceptionMark pem(thread);
    ResourceMark rm(thread);
    ZVerifyBadOopClosure verify_cl;

    if (_jt->has_last_Java_frame()) {
      // Traverse the execution stack
      for (StackFrameStream fst(_jt, true /* update */, false /* process_frames */); !fst.is_done(); fst.next()) {
        fst.current()->oops_do(&verify_cl, NULL /* code_cl */, fst.register_map());
      }
    }
  }
#endif

  StackWatermark::start_iteration(gc_cl);

  // Update thread local address bad mask
  ZThreadLocalData::set_address_bad_mask(_jt, ZAddressBadMask);

  // Mark invisible root
  ZThreadLocalData::do_invisible_root(_jt, ZBarrier::load_barrier_on_invisible_root_oop_field);

  // Retire TLAB
  if (UseTLAB) {
    if(ZGlobalPhase == ZPhaseMark) {
      _stats.reset();
      ZThreadLocalAllocBuffer::retire(_jt, &_stats);
    } else {
      ZThreadLocalAllocBuffer::remap(_jt);
    }
  }
}

void ZStackWatermark::process(frame frame, RegisterMap& register_map, void* context) {
  OopClosure* cl = context == NULL ? &_jt_cl : reinterpret_cast<OopClosure*>(context);
#ifdef ASSERT
  ZVerifyBadOopClosure verify_cl;
  frame.oops_do(&verify_cl, NULL, &register_map);
#endif
  frame.oops_do(cl, &_cb_cl, &register_map);
}
