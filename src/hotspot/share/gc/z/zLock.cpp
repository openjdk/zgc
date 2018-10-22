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
#include "gc/z/zLock.inline.hpp"

void ZLock::lock() {
  pthread_mutex_lock(&_lock);
}

bool ZLock::try_lock() {
  return pthread_mutex_trylock(&_lock) == 0;
}

void ZLock::unlock() {
  pthread_mutex_unlock(&_lock);
}


void ZReentrantLock::lock() {
  ZLock::lock();
  _owner = Thread::current();
}

bool ZReentrantLock::try_lock() {
  if (!ZLock::try_lock()) {
    return false;
  }

  _owner = Thread::current();
  return true;
}

void ZReentrantLock::unlock() {
  _owner = NULL;
  ZLock::unlock();
}
