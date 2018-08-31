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

#ifndef SHARE_GC_Z_ZGLOBALBEHAVIOURS_HPP
#define SHARE_GC_Z_ZGLOBALBEHAVIOURS_HPP

#include "gc/shared/gcBehaviours.hpp"
#include "gc/z/zOopClosures.hpp"
#include "runtime/vmBehaviours.hpp"
#include "utilities/behaviours.hpp"

class ZIsUnloadingBehaviour: public ClosureIsUnloadingBehaviour {
  ZPhantomIsAliveObjectClosure _is_alive;
public:
  ZIsUnloadingBehaviour()
    : ClosureIsUnloadingBehaviour(&_is_alive)
  { }
  virtual bool is_unloading(CompiledMethod* cm) const;
};

class ZICProtectionBehaviour: public CompiledICProtectionBehaviour {
private:
  bool _locked;

public:
  virtual bool lock(CompiledMethod* method);
  virtual void unlock(CompiledMethod* method);
  virtual bool is_safe(CompiledMethod* method);
};

class ZGlobalBehaviours {
private:
  ZIsUnloadingBehaviour  _is_unloading_behaviour;
  ZICProtectionBehaviour _ic_protection_behaviour;

public:
  ZGlobalBehaviours();
};

#endif // SHARE_GC_Z_ZGLOBALBEHAVIOURS_HPP
