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

#include "precompiled.hpp"
#include "memory/allocation.inline.hpp"
#include "runtime/thread.hpp"
#include "utilities/behaviours.hpp"
#include "utilities/copy.hpp"
#include "utilities/debug.hpp"

volatile int BehaviourRegistry::_behaviour_count = 0;
BehaviourProviderCollection* Behaviours::_global_provider = new BehaviourProviderCollection();

BehaviourProviderMark::BehaviourProviderMark(BehaviourProvider& provider)
  : _attached_thread(Thread::current()),
    _cache(&provider, _attached_thread),
    _provider(provider),
    _parent(&Behaviours::get_provider())
{
  _attached_thread->set_behaviour_provider(&_cache);
}

BehaviourProviderMark::~BehaviourProviderMark() {
  _attached_thread->set_behaviour_provider(_provider.parent());
}

BehaviourProvider* BehaviourProvider::provider(Thread* current, int behaviour_id) {
  assert(_parent != NULL, "could not find behaviour provider");
  BehaviourProvider* result = _parent->provider(current, behaviour_id);
  assert(result != NULL, "no behaviour provider found");
  return result;
}

BehaviourProviderHashCache::BehaviourProviderHashCache(BehaviourProvider* parent, Thread* attached_thread)
  : BehaviourProvider(parent),
    _cache(NULL),
    _owner(attached_thread)
{ }

BehaviourProviderHashCache::~BehaviourProviderHashCache() {
  if (_cache != NULL) {
    delete _cache;
  }
}

BehaviourProvider* BehaviourProviderHashCache::provider(Thread* current, int behaviour_id) {
  if (_owner != current) {
    return BehaviourProvider::provider(current, behaviour_id);
  }
  if (_cache == NULL) {
    _cache = new (ResourceObj::C_HEAP, mtInternal) ProviderTable();
  }

  BehaviourProvider** result = _cache->get(behaviour_id);
  if (result != NULL) {
    return *result;
  }

  _cache->put(behaviour_id, BehaviourProvider::provider(current, behaviour_id));
  return *_cache->get(behaviour_id);
}

void* BehaviourProviderHashCache::behaviour(int behaviour_id) {
  ShouldNotReachHere();
  return NULL;
}

void BehaviourProviderCollection::prepend_node(BehaviourProviderNode* node) {
  assert(node->provider().parent() == NULL, "invariant");

  if (is_empty()) {
    node->provider().set_parent(parent());
    _head = node;
    _tail = node;
  } else {
    node->provider().set_parent(&_head->provider());
    _head = node;
  }
}

BehaviourProviderCollection::BehaviourProviderCollection()
  : BehaviourProvider(NULL),
    _head(NULL),
    _tail(NULL)
{ }

BehaviourProvider* BehaviourProviderCollection::provider(Thread* current, int behaviour_id) {
  if (is_empty()) {
    return BehaviourProvider::provider(current, behaviour_id);
  } else {
    return _head->provider().provider(current, behaviour_id);
  }
}

void* BehaviourProviderCollection::behaviour(int behaviour_id) {
  ShouldNotReachHere();
  return NULL;
}

void BehaviourProviderCollection::register_provider(BehaviourProvider& provider) {
  BehaviourProviderNode* node = new BehaviourProviderNode(provider);
  prepend_node(node);
}

void BehaviourProviderCollection::set_parent(BehaviourProvider* parent) {
  BehaviourProvider::set_parent(parent);
  if (!is_empty()) {
    _tail->provider().set_parent(parent);
  }
}
