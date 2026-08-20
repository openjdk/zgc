/*
 * Copyright (c) 2020, 2023, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_Z_ZPAGEALLOCATOR_INLINE_HPP
#define SHARE_GC_Z_ZPAGEALLOCATOR_INLINE_HPP

#include "gc/z/zPageAllocator.hpp"

inline ZPageAllocatorStats::ZPageAllocatorStats(size_t heuristic_max_capacity,
                                                size_t capacity,
                                                size_t used,
                                                size_t used_high,
                                                size_t used_low,
                                                size_t used_generation,
                                                size_t freelist_available_at_start,
                                                size_t freelist_promoted,
                                                size_t mutator_freelist_promoted,
                                                size_t freelist_compacted,
                                                size_t mutator_freelist_compacted,
                                                size_t freed,
                                                size_t promoted,
                                                size_t mutator_promoted,
                                                size_t flip_promoted,
                                                size_t compacted,
                                                size_t mutator_compacted,
                                                size_t allocation_stalls)
  : _heuristic_max_capacity(heuristic_max_capacity),
    _capacity(capacity),
    _used(used),
    _used_high(used_high),
    _used_low(used_low),
    _used_generation(used_generation),
    _freelist_available_at_start(freelist_available_at_start),
    _freelist_promoted(freelist_promoted),
    _mutator_freelist_promoted(mutator_freelist_promoted),
    _freelist_compacted(freelist_compacted),
    _mutator_freelist_compacted(mutator_freelist_compacted),
    _freed(freed),
    _promoted(promoted),
    _mutator_promoted(mutator_promoted),
    _flip_promoted(flip_promoted),
    _compacted(compacted),
    _mutator_compacted(mutator_compacted),
    _allocation_stalls(allocation_stalls) {}

inline size_t ZPageAllocatorStats::heuristic_max_capacity() const {
  return _heuristic_max_capacity;
}

inline size_t ZPageAllocatorStats::capacity() const {
  return _capacity;
}

inline size_t ZPageAllocatorStats::used() const {
  return _used;
}

inline size_t ZPageAllocatorStats::used_high() const {
  return _used_high;
}

inline size_t ZPageAllocatorStats::used_low() const {
  return _used_low;
}

inline size_t ZPageAllocatorStats::used_generation() const {
  return _used_generation;
}

inline size_t ZPageAllocatorStats::freelist_available_at_start() const {
  return _freelist_available_at_start;
}

inline size_t ZPageAllocatorStats::freelist_promoted() const {
  return _freelist_promoted;
}

inline size_t ZPageAllocatorStats::mutator_freelist_promoted() const {
  return _mutator_freelist_promoted;
}

inline size_t ZPageAllocatorStats::freelist_compacted() const {
  return _freelist_compacted;
}

inline size_t ZPageAllocatorStats::mutator_freelist_compacted() const {
  return _mutator_freelist_compacted;
}

inline size_t ZPageAllocatorStats::freed() const {
  return _freed;
}

inline size_t ZPageAllocatorStats::promoted() const {
  return _promoted;
}

inline size_t ZPageAllocatorStats::mutator_promoted() const {
  return _mutator_promoted;
}

inline size_t ZPageAllocatorStats::flip_promoted() const {
  return _flip_promoted;
}

inline size_t ZPageAllocatorStats::compacted() const {
  return _compacted;
}

inline size_t ZPageAllocatorStats::mutator_compacted() const {
  return _mutator_compacted;
}

inline size_t ZPageAllocatorStats::allocation_stalls() const {
  return _allocation_stalls;
}

#endif // SHARE_GC_Z_ZPAGEALLOCATOR_INLINE_HPP
