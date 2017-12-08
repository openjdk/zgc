/*
 * Copyright (c) 2015, 2017, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_Z_ZROOTSITERATOR_HPP
#define SHARE_GC_Z_ZROOTSITERATOR_HPP

#include "memory/allocation.hpp"
#include "memory/iterator.hpp"
#include "utilities/globalDefinitions.hpp"

typedef void (*ZOopsDoFunction)(OopClosure*);
typedef void (*ZUnlinkOrOopsDoFunction)(BoolObjectClosure*, OopClosure*);

template <ZOopsDoFunction F>
class ZSerialOopsDo VALUE_OBJ_CLASS_SPEC {
private:
  volatile bool _claimed ATTRIBUTE_ALIGNED(DEFAULT_CACHE_LINE_SIZE);

public:
  ZSerialOopsDo();
  void oops_do(OopClosure* cl);
};

template <ZOopsDoFunction F>
class ZParallelOopsDo VALUE_OBJ_CLASS_SPEC {
private:
  volatile bool _completed ATTRIBUTE_ALIGNED(DEFAULT_CACHE_LINE_SIZE);

public:
  ZParallelOopsDo();
  void oops_do(OopClosure* cl);
};

template <ZUnlinkOrOopsDoFunction F>
class ZSerialUnlinkOrOopsDo VALUE_OBJ_CLASS_SPEC {
private:
  volatile bool _claimed ATTRIBUTE_ALIGNED(DEFAULT_CACHE_LINE_SIZE);

public:
  ZSerialUnlinkOrOopsDo();
  void unlink_or_oops_do(BoolObjectClosure* is_alive, OopClosure* cl);
};

template <ZUnlinkOrOopsDoFunction F>
class ZParallelUnlinkOrOopsDo VALUE_OBJ_CLASS_SPEC {
private:
  volatile bool _completed ATTRIBUTE_ALIGNED(DEFAULT_CACHE_LINE_SIZE);

public:
  ZParallelUnlinkOrOopsDo();
  void unlink_or_oops_do(BoolObjectClosure* is_alive, OopClosure* cl);
};

class ZRootsIterator VALUE_OBJ_CLASS_SPEC {
private:
  static void do_universe(OopClosure* cl);
  static void do_jni_handles(OopClosure* cl);
  static void do_jni_weak_handles(OopClosure* cl);
  static void do_object_synchronizer(OopClosure* cl);
  static void do_management(OopClosure* cl);
  static void do_jvmti_export(OopClosure* cl);
  static void do_jvmti_weak_export(OopClosure* cl);
  static void do_system_dictionary(OopClosure* cl);
  static void do_class_loader_data_graph(OopClosure* cl);
  static void do_threads(OopClosure* cl);
  static void do_code_cache(OopClosure* cl);
  static void do_string_table(OopClosure* cl);

  ZSerialOopsDo<do_universe>                  _universe;
  ZSerialOopsDo<do_jni_handles>               _jni_handles;
  ZSerialOopsDo<do_jni_weak_handles>          _jni_weak_handles;
  ZSerialOopsDo<do_object_synchronizer>       _object_synchronizer;
  ZSerialOopsDo<do_management>                _management;
  ZSerialOopsDo<do_jvmti_export>              _jvmti_export;
  ZSerialOopsDo<do_jvmti_weak_export>         _jvmti_weak_export;
  ZSerialOopsDo<do_system_dictionary>         _system_dictionary;
  ZParallelOopsDo<do_class_loader_data_graph> _class_loader_data_graph;
  ZParallelOopsDo<do_threads>                 _threads;
  ZParallelOopsDo<do_code_cache>              _code_cache;
  ZParallelOopsDo<do_string_table>            _string_table;

public:
  ZRootsIterator();
  ~ZRootsIterator();

  void oops_do(OopClosure* cl, bool visit_jvmti_weak_export = false);
};

class ZWeakRootsIterator VALUE_OBJ_CLASS_SPEC {
private:
  static void do_jni_weak_handles(BoolObjectClosure* is_alive, OopClosure* cl);
  static void do_symbol_table(BoolObjectClosure* is_alive, OopClosure* cl);
  static void do_string_table(BoolObjectClosure* is_alive, OopClosure* cl);

  ZSerialUnlinkOrOopsDo<do_jni_weak_handles> _jni_weak_handles;
  ZParallelUnlinkOrOopsDo<do_symbol_table>   _symbol_table;
  ZParallelUnlinkOrOopsDo<do_string_table>   _string_table;

public:
  ZWeakRootsIterator();
  ~ZWeakRootsIterator();

  void unlink_or_oops_do(BoolObjectClosure* is_alive, OopClosure* cl);
  void oops_do(OopClosure* cl);
};

class ZThreadRootsIterator VALUE_OBJ_CLASS_SPEC {
private:
  static void do_threads(OopClosure* cl);

  ZParallelOopsDo<do_threads> _threads;

public:
  ZThreadRootsIterator();
  ~ZThreadRootsIterator();

  void oops_do(OopClosure* cl);
};

#endif // SHARE_GC_Z_ZROOTSITERATOR_HPP
