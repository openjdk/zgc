/*
 * Copyright (c) 2019, 2020, Oracle and/or its affiliates. All rights reserved.
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
#include "classfile/classLoaderData.hpp"
#include "gc/z/zAddress.hpp"
#include "gc/z/zHeap.inline.hpp"
#include "gc/z/zOop.hpp"
#include "gc/z/zPageAllocator.hpp"
#include "gc/z/zResurrection.hpp"
#include "gc/z/zRootsIterator.hpp"
#include "gc/z/zStackWatermark.hpp"
#include "gc/z/zStat.hpp"
#include "gc/z/zVerify.hpp"
#include "memory/iterator.inline.hpp"
#include "oops/oop.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/stackWatermark.inline.hpp"
#include "runtime/stackWatermarkSet.inline.hpp"

#define BAD_OOP_ARG(o, p)   "Bad oop " PTR_FORMAT " found at " PTR_FORMAT, p2i(o), p2i(p)

static void z_verify_oop(oop* p) {
  const oop o = RawAccess<>::oop_load(p);
  if (o != NULL) {
    const uintptr_t addr = ZOop::to_address(o);
    guarantee(ZAddress::is_good(addr), BAD_OOP_ARG(o, p));
    guarantee(oopDesc::is_oop(ZOop::from_address(addr)), BAD_OOP_ARG(o, p));
  }
}

static void z_verify_possibly_weak_oop(oop* p) {
  const oop o = RawAccess<>::oop_load(p);
  if (o != NULL) {
    const uintptr_t addr = ZOop::to_address(o);
    guarantee(ZAddress::is_good(addr) || ZAddress::is_finalizable_good(addr), BAD_OOP_ARG(o, p));
    guarantee(oopDesc::is_oop(ZOop::from_address(ZAddress::good(addr))), BAD_OOP_ARG(o, p));
  }
}

class ZVerifyRootClosure : public ZRootsIteratorClosure {
private:
  bool _verify_all;
  bool _expect_bad;

  class ZVerifyCodeBlobClosure : public CodeBlobToOopClosure {
  private:
    ZVerifyRootClosure* _cl;

  public:
    ZVerifyCodeBlobClosure(ZVerifyRootClosure* cl) :
      CodeBlobToOopClosure(cl, false /* fix_relocations */),
      _cl(cl) {
    }
    virtual void do_code_blob(CodeBlob* cb) {
      bool expect_bad = _cl->_expect_bad;
      // We can never expect oops in a code blob are bad, because they are not only
      // members of this stack.
      _cl->_expect_bad = false;
      CodeBlobToOopClosure::do_code_blob(cb);
      _cl->_expect_bad = expect_bad;
    }
  };

  class ZVerifyStack {
  private:
    ZVerifyRootClosure* _cl;
    JavaThread*         _jt;
    bool                _verify_all;
    uint64_t            _last_good;

  public:
    ZVerifyStack(ZVerifyRootClosure* cl, JavaThread* jt) :
        _cl(cl),
        _jt(jt),
        _verify_all(cl->_verify_all),
        _last_good(0) {
      _cl->_verify_all = true;
      ZStackWatermark* stack_watermark = jt->stack_watermark_set()->get<ZStackWatermark>(StackWatermarkSet::gc);
      if (stack_watermark->should_start_iteration()) {
        _cl->_verify_all = false;
        _cl->_expect_bad = true;
      } else {
        _last_good = stack_watermark->last_processed();
      }
    }

    ~ZVerifyStack() {
      _cl->_verify_all = _verify_all;
      _cl->_expect_bad = false;
    }

    void prepare_next_frame(frame& frame) {
      uintptr_t sp = reinterpret_cast<uintptr_t>(frame.sp());
      if (!_cl->_expect_bad && sp == _last_good) {
        _cl->_verify_all = false;
        _cl->_expect_bad = true;
      }
    }

    void verify_frames() {
      ZVerifyCodeBlobClosure cb_cl(_cl);
      for (StackFrameStream frames(_jt, true /* update */, false /* process_frames */);
           !frames.is_done();
           frames.next()) {
        frame& frame = *frames.current();
        frame.oops_do(_cl, &cb_cl, frames.register_map());
        prepare_next_frame(frame);
      }
    }
  };

public:
  ZVerifyRootClosure(bool verify_all) :
      _verify_all(verify_all),
      _expect_bad(false)
  { }

  virtual void do_oop(oop* p) {
    if (!_verify_all) {
      oop obj = *p;
      if (_expect_bad) {
        guarantee(!ZAddress::is_good(ZOop::to_address(obj)), BAD_OOP_ARG(obj, p));
      }
      obj = NativeAccess<AS_NO_KEEPALIVE>::oop_load(&obj);
      z_verify_oop(&obj);
    } else {
      z_verify_oop(p);
    }
  }

  virtual void do_oop(narrowOop*) {
    ShouldNotReachHere();
  }

  virtual void do_thread(Thread* thread) {
    thread->oops_do(this, NULL, false /* process_frames */);

    if (!thread->is_Java_thread()) {
      return;
    }

    JavaThread* jt = static_cast<JavaThread*>(thread);
    if (!jt->has_last_Java_frame()) {
      return;
    }

    ZVerifyStack verify_stack(this, jt);
    verify_stack.verify_frames();
  }
};

class ZVerifyOopClosure : public ClaimMetadataVisitingOopIterateClosure, public ZRootsIteratorClosure  {
private:
  const bool _verify_weaks;

public:
  ZVerifyOopClosure(bool verify_weaks) :
      ClaimMetadataVisitingOopIterateClosure(ClassLoaderData::_claim_other),
      _verify_weaks(verify_weaks) {}

  virtual void do_oop(oop* p) {
    if (_verify_weaks) {
      z_verify_possibly_weak_oop(p);
    } else {
      // We should never encounter finalizable oops through strong
      // paths. This assumes we have only visited strong roots.
      z_verify_oop(p);
    }
  }

  virtual void do_oop(narrowOop* p) {
    ShouldNotReachHere();
  }

  virtual ReferenceIterationMode reference_iteration_mode() {
    return _verify_weaks ? DO_FIELDS : DO_FIELDS_EXCEPT_REFERENT;
  }

#ifdef ASSERT
  // Verification handled by the closure itself
  virtual bool should_verify_oops() {
    return false;
  }
#endif
};

template <typename RootsIterator>
void ZVerify::roots(bool verify_all) {
  assert(SafepointSynchronize::is_at_safepoint(), "Must be at a safepoint");
  assert(!ZResurrection::is_blocked(), "Invalid phase");

  if (ZVerifyRoots) {
    ZVerifyRootClosure cl(verify_all);
    RootsIterator iter;
    iter.oops_do(&cl);
  }
}

void ZVerify::roots_strong() {
  roots<ZRootsIterator>(true /* verify_all */);
}

void ZVerify::roots_weak() {
  roots<ZWeakRootsIterator>(true /* verify_all */);
}

void ZVerify::roots_concurrent_strong(bool verify_all) {
  roots<ZConcurrentRootsIteratorClaimNone>(verify_all);
}

void ZVerify::roots_concurrent_weak() {
  roots<ZConcurrentWeakRootsIterator>(true /* verify_all */);
}

void ZVerify::roots(bool verify_all_strong, bool verify_weaks) {
  roots_strong();
  roots_concurrent_strong(verify_all_strong);
  if (verify_weaks) {
    roots_weak();
    roots_concurrent_weak();
  }
}

void ZVerify::objects(bool verify_weaks) {
  assert(SafepointSynchronize::is_at_safepoint(), "Must be at a safepoint");
  assert(ZGlobalPhase == ZPhaseMarkCompleted, "Invalid phase");
  assert(!ZResurrection::is_blocked(), "Invalid phase");

  if (ZVerifyObjects) {
    ZVerifyOopClosure cl(verify_weaks);
    ObjectToOopClosure object_cl(&cl);
    ZHeap::heap()->object_iterate(&object_cl, verify_weaks);
  }
}

void ZVerify::roots_and_objects(bool verify_weaks) {
  roots(true /* verify_all_strong */, verify_weaks);
  objects(verify_weaks);
}

void ZVerify::before_zoperation() {
  // Verify strong roots
  ZStatTimerDisable disable;
  roots(false /* verify_all_strong */, false /* verify_weaks */);
}

void ZVerify::after_mark() {
  // Verify all strong roots and strong references
  ZStatTimerDisable disable;
  roots_and_objects(false /* verify_weaks */);
}

void ZVerify::after_weak_processing() {
  // Verify all roots and all references
  ZStatTimerDisable disable;
  roots_and_objects(true /* verify_weaks */);
}

template <bool Map>
class ZPageDebugMapOrUnmapClosure : public ZPageClosure {
private:
  const ZPageAllocator* const _allocator;

public:
  ZPageDebugMapOrUnmapClosure(const ZPageAllocator* allocator) :
      _allocator(allocator) {}

  void do_page(const ZPage* page) {
    if (Map) {
      _allocator->debug_map_page(page);
    } else {
      _allocator->debug_unmap_page(page);
    }
  }
};

ZVerifyViewsFlip::ZVerifyViewsFlip(const ZPageAllocator* allocator) :
    _allocator(allocator) {
  if (ZVerifyViews) {
    // Unmap all pages
    ZPageDebugMapOrUnmapClosure<false /* Map */> cl(_allocator);
    ZHeap::heap()->pages_do(&cl);
  }
}

ZVerifyViewsFlip::~ZVerifyViewsFlip() {
  if (ZVerifyViews) {
    // Map all pages
    ZPageDebugMapOrUnmapClosure<true /* Map */> cl(_allocator);
    ZHeap::heap()->pages_do(&cl);
  }
}
