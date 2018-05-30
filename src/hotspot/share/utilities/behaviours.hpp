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

#ifndef SHARE_UTILITIES_BEHAVIOURS_HPP
#define SHARE_UTILITIES_BEHAVIOURS_HPP

#include "memory/allocation.hpp"
#include "runtime/atomic.hpp"
#include "runtime/thread.hpp"
#include "utilities/debug.hpp"
#include "utilities/resourceHash.hpp"

// The BehaviourRegistry maps types to unique integers.
class BehaviourRegistry: AllStatic {
  template<typename T>
  struct BehaviourIdentifier {
    static int _behaviour_id;
  };

  template<typename T>
  friend struct BehaviourIdentifier;
  static volatile int _behaviour_count;

public:
  template<typename T>
  static int get_behaviour_id() {
    return BehaviourIdentifier<T>::_behaviour_id;
  }

  static int get_behaviour_count() { return _behaviour_count; }
};

template<typename T>
int BehaviourRegistry::BehaviourIdentifier<T>::_behaviour_id =
  Atomic::add(1, &BehaviourRegistry::_behaviour_count) - 1;

// A BehaviourProvider knows how to get the appropriate provider for a given
// behaviour type.
class BehaviourProvider {
  friend class Behaviours;
  BehaviourProvider* _parent;

public:
  BehaviourProvider(BehaviourProvider* parent) : _parent(parent) { }

  virtual BehaviourProvider* provider(Thread* current, int behaviour_id);
  virtual void* behaviour(int behaviour_id) = 0;
  BehaviourProvider* parent() const { return _parent; }
  virtual void set_parent(BehaviourProvider* parent) { _parent = parent; }
};

// A hash cache provider makes a hash table containing quick lookup of
// providers for a given behaviour type. For each lookup, it caches
// the found provider for a given behaviour type so that the next lookup
// will be quick.
class BehaviourProviderHashCache: public BehaviourProvider {
  typedef ResourceHashtable<int, BehaviourProvider*, primitive_hash,
                            primitive_equals, 8,
                            ResourceObj::C_HEAP, mtInternal> ProviderTable;
  ProviderTable* _cache;
  Thread*        _owner;

public:
  BehaviourProviderHashCache(BehaviourProvider* parent, Thread* attached_thread);
  ~BehaviourProviderHashCache();

  virtual BehaviourProvider* provider(Thread* current, int behaviour_id);
  virtual void* behaviour(int behaviour_id);
};

// A singleton behaviour provider provides a single behaviour and delegates
// all other requests to the parent provider. Chaining singleton providers
// is equivalent to creating a chain of responsibility, which allows layering
// behaviours in a structured way.
class SingletonBehaviourProvider: public BehaviourProvider {
  void* _behaviour;
  int _behaviour_id;

  bool provides_behaviour(int behaviour_id) {
    return _behaviour_id == behaviour_id;
  }

public:
  SingletonBehaviourProvider(BehaviourProvider* parent, void* behaviour, int behaviour_id)
    : BehaviourProvider(parent),
      _behaviour(behaviour),
      _behaviour_id(behaviour_id)
  { }

  SingletonBehaviourProvider(void* behaviour, int behaviour_id)
    : BehaviourProvider(NULL),
      _behaviour(behaviour),
      _behaviour_id(behaviour_id)
  { }

  virtual BehaviourProvider* provider(Thread* current, int behaviour_id) {
    if (provides_behaviour(behaviour_id)) {
      return this;
    } else {
      return BehaviourProvider::provider(current, behaviour_id);
    }
  }

  virtual void* behaviour(int behaviour_id) {
    return _behaviour;
  }
};

// A forwarding behaviour provider provides no behaviours, but delegates
// requests to the parent provider. This allows sending requests elsewhere.
class ForwardingBehaviourProvider: public BehaviourProvider {
public:
  ForwardingBehaviourProvider(BehaviourProvider* parent)
    : BehaviourProvider(parent)
  { }

  virtual void* behaviour(int behaviour_id) { ShouldNotReachHere(); return NULL; }
};

// A behaviour collection allows registering multiple behaviour providers
// in a single collection of behaviours. This is useful when layering behaviours
// so that one layer overrides the haviour of a parent layer of behaviours. Then
// a user may simply add behaviours to the layer (comprised by a collection).
class BehaviourProviderCollection: public BehaviourProvider, public CHeapObj<mtInternal> {
  class BehaviourProviderNode: public CHeapObj<mtInternal> {
    BehaviourProvider& _provider;

  public:
    BehaviourProviderNode(BehaviourProvider& provider)
      : _provider(provider)
    { }

    BehaviourProvider& provider() {
      return _provider;
    }
  };

  class SingletonBehaviourProviderNode: public BehaviourProviderNode {
    SingletonBehaviourProvider _provider;

  public:
    SingletonBehaviourProviderNode(void* behaviour, int behaviour_id)
      : BehaviourProviderNode(_provider),
        _provider(NULL, behaviour, behaviour_id)
    { }
  };

  BehaviourProviderNode* _head;
  BehaviourProviderNode* _tail;

  bool is_empty() const {
    return _head == NULL && _tail == NULL;
  }

  void prepend_node(BehaviourProviderNode* node);

public:
  BehaviourProviderCollection();

  virtual void set_parent(BehaviourProvider* parent);
  virtual BehaviourProvider* provider(Thread* current, int behaviour_id);
  virtual void* behaviour(int behaviour_id);
  void register_provider(BehaviourProvider& provider);

  template<typename T>
  void register_behaviour(T& behaviour) {
    BehaviourProviderNode* node = new SingletonBehaviourProviderNode(&behaviour,
                                                                     BehaviourRegistry::get_behaviour_id<T>());
    prepend_node(node);
  }
};

// This utility class is used to get the current provider for the current execution
// context, as well as getting the current behaviour for the current execution context.
class Behaviours: AllStatic {
  static BehaviourProviderCollection* _global_provider;

public:
  static BehaviourProvider& get_provider(Thread* current = Thread::current()) {
    BehaviourProvider* provider = current->behaviour_provider();
    if (provider == NULL) {
      provider = _global_provider;
    }
    return *provider;
  }

  template<typename T>
  static T& get_behaviour() {
    Thread* current = Thread::current();
    BehaviourProvider* provider = &get_provider();
    int behaviour_id = BehaviourRegistry::get_behaviour_id<T>();
    provider = provider->provider(current, behaviour_id);
    void* behaviour = provider->behaviour(behaviour_id);
    assert(behaviour != NULL, "did not find any provided behaviour");
    return *reinterpret_cast<T*>(behaviour);
  }

  template<typename T>
  static T& get_super_behaviour(T* child) {
    Thread* current = Thread::current();
    BehaviourProvider* provider = &get_provider();
    int behaviour_id = BehaviourRegistry::get_behaviour_id<T>();
    provider = provider->provider(current, behaviour_id);
    while (reinterpret_cast<T*>(provider->behaviour(behaviour_id)) != child) {
      provider = provider->parent()->provider(current, behaviour_id);
    }
    provider = provider->parent()->provider(current, behaviour_id);
    void* behaviour = provider->behaviour(behaviour_id);
    assert(behaviour != NULL, "did not find any provided behaviour");
    T* result = reinterpret_cast<T*>(behaviour);
    assert(result != child, "sanity");
    return *result;
  }

  static void register_global_provider(BehaviourProvider& provider) {
    _global_provider->register_provider(provider);
  }

  static BehaviourProvider& global_provider() {
    return *_global_provider;
  }
};

class BehaviourProviderMark: StackObj {
protected:
  Thread*                    _attached_thread;
  BehaviourProviderHashCache _cache;
  BehaviourProvider&         _provider;
  BehaviourProvider*         _parent;

public:
  BehaviourProviderMark(BehaviourProvider& provider);
  ~BehaviourProviderMark();
};

// The BehaviourMark is used to provide a behaviour in a local scope, layering it
// above the current execution context.
template<typename T>
class BehaviourMark: public BehaviourProviderMark {
  SingletonBehaviourProvider _provider;

public:
  BehaviourMark(T& behaviour)
    : BehaviourProviderMark(_provider),
      _provider(_parent,
                &behaviour,
                BehaviourRegistry::get_behaviour_id<T>())
  { }
};

// The ForwardingProviderMark is used to forward a provider into a local scope, layering it
// above the current execution context.
class ForwardingProviderMark: public BehaviourProviderMark {
  ForwardingBehaviourProvider _provider;

public:
  ForwardingProviderMark(BehaviourProvider& provider)
    : BehaviourProviderMark(_provider),
      _provider(&provider)
  { }
};

// The DefaultBehaviourMark is similar to the BehaviourMark, but also contains the
// actual behaviour as part of the sack allocated object, created with the default
// constructor for that type.
template<typename ProvidedT, typename ConcreteT>
class DefaultBehaviourMark: public BehaviourMark<ProvidedT> {
  ConcreteT _behaviour;

public:
  DefaultBehaviourMark()
    : BehaviourMark<ProvidedT>(_behaviour)
  { }
};

#endif // SHARE_UTILITIES_BEHAVIOURS_HPP
