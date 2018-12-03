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
#include "classfile/classLoaderDataGraph.hpp"
#include "classfile/systemDictionary.hpp"
#include "code/codeBehaviours.hpp"
#include "code/codeCache.hpp"
#include "code/dependencyContext.hpp"
#include "gc/shared/gcBehaviours.hpp"
#include "gc/shared/suspendibleThreadSet.hpp"
#include "gc/z/zLock.inline.hpp"
#include "gc/z/zNMethodTable.hpp"
#include "gc/z/zOopClosures.hpp"
#include "gc/z/zStat.hpp"
#include "gc/z/zUnload.hpp"
#include "gc/z/zWorkers.hpp"
#include "runtime/interfaceSupport.inline.hpp"

class ZIsUnloadingBehaviour : public ClosureIsUnloadingBehaviour {
private:
  ZPhantomIsAliveObjectClosure _is_alive;

public:
  ZIsUnloadingBehaviour() :
      ClosureIsUnloadingBehaviour(&_is_alive) {}

  virtual bool is_unloading(CompiledMethod* method) const {
    nmethod* const nm = (nmethod*)method;
    ZReentrantLock* const lock = ZNMethodTable::lock_for_nmethod(nm);
    if (SafepointSynchronize::is_at_safepoint() || lock == NULL) {
      return ClosureIsUnloadingBehaviour::is_unloading(method);
    } else {
      ZLocker<ZReentrantLock> locker(lock);
      return ClosureIsUnloadingBehaviour::is_unloading(method);
    }
  }
};

class ZICProtectionBehaviour : public CompiledICProtectionBehaviour {
public:
  virtual bool lock(CompiledMethod* method) {
    nmethod* const nm = (nmethod*)method;
    ZReentrantLock* const lock = ZNMethodTable::lock_for_nmethod(nm);
    if (SafepointSynchronize::is_at_safepoint() || lock == NULL || lock->is_owned()) {
      return false;
    }
    lock->lock();
    return true;
  }

  virtual void unlock(CompiledMethod* method) {
    nmethod* const nm = (nmethod*)method;
    ZReentrantLock* const lock = ZNMethodTable::lock_for_nmethod(nm);
    if (lock == NULL) {
      return;
    }
    lock->unlock();
  }

  virtual bool is_safe(CompiledMethod* method) {
    nmethod* const nm = (nmethod*)method;
    ZReentrantLock* const lock = ZNMethodTable::lock_for_nmethod(nm);
    return SafepointSynchronize::is_at_safepoint() || lock == NULL || lock->is_owned();
  }
};

ZUnload::ZUnload(ZWorkers* workers) :
    _workers(workers) {

  if (!ClassUnloading) {
    return;
  }

  static ZIsUnloadingBehaviour is_unloading_behaviour;
  IsUnloadingBehaviour::set_current(&is_unloading_behaviour);

  static ZICProtectionBehaviour ic_protection_behaviour;
  CompiledICProtectionBehaviour::set_current(&ic_protection_behaviour);
}

void ZUnload::prepare() {
  if (!ClassUnloading) {
    return;
  }

  CodeCache::increment_unloading_cycle();
  DependencyContext::gc_prologue();
}

class ZUnloadRendezvousClosure : public ThreadClosure {
public:
  void do_thread(Thread* thread) {}
};

void ZUnload::unload() {
  if (!ClassUnloading) {
    return;
  }

  //
  // Phase 1: Unlink - Remove references to stale metadata and nmethods
  //
  bool unloading_occurred;

  // Unlink klasses
  {
    SuspendibleThreadSetJoiner sts;
    MutexLockerEx ml(ClassLoaderDataGraph_lock);
    unloading_occurred = SystemDictionary::do_unloading(ZStatPhase::timer());
  }

  // Unload nmethods
  ZNMethodTable::clean_caches(_workers, unloading_occurred);

  // Unlink klasses from subklass/sibling/implementor lists
  /* if (unloading_occurred) */ {
    SuspendibleThreadSetJoiner sts;
    Klass::clean_weak_klass_links(unloading_occurred);
  }

  DependencyContext::gc_epilogue();

  // Make sure the old links are no longer observable before purging
  {
    ZUnloadRendezvousClosure cl;
    Handshake::execute(&cl);
  }

  //
  // Phase 2: Purge - Delete the stale metadata that was unlinked
  //

  // Purge metaspace
  ZNMethodTable::unload(_workers);
  ClassLoaderDataGraph::purge();
  CodeCache::purge_exception_caches();
}

void ZUnload::finish() {
  // Resize and verify metaspace
  MetaspaceGC::compute_new_size();
  MetaspaceUtils::verify_metrics();
}
