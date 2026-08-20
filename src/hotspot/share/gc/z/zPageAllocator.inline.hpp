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
                                                const size_t freelist_available_at_start[ZPageTypeCount],
                                                const size_t freelist_promoted[ZPageTypeCount],
                                                const size_t mutator_freelist_promoted[ZPageTypeCount],
                                                const size_t freelist_compacted[ZPageTypeCount],
                                                const size_t mutator_freelist_compacted[ZPageTypeCount],
                                                size_t freed,
                                                const size_t promoted[ZPageTypeCount],
                                                const size_t mutator_promoted[ZPageTypeCount],
                                                const size_t flip_promoted[ZPageTypeCount],
                                                const size_t compacted[ZPageTypeCount],
                                                const size_t mutator_compacted[ZPageTypeCount],
                                                size_t allocation_stalls)
  : _heuristic_max_capacity(heuristic_max_capacity),
    _capacity(capacity),
    _used(used),
    _used_high(used_high),
    _used_low(used_low),
    _used_generation(used_generation),
    _freed(freed),
    _allocation_stalls(allocation_stalls) {
  for (uint i = 0; i < ZPageTypeCount; i++) {
    _freelist_available_at_start[i] = freelist_available_at_start[i];
    _freelist_promoted[i] = freelist_promoted[i];
    _mutator_freelist_promoted[i] = mutator_freelist_promoted[i];
    _freelist_compacted[i] = freelist_compacted[i];
    _mutator_freelist_compacted[i] = mutator_freelist_compacted[i];
    _promoted[i] = promoted[i];
    _mutator_promoted[i] = mutator_promoted[i];
    _flip_promoted[i] = flip_promoted[i];
    _compacted[i] = compacted[i];
    _mutator_compacted[i] = mutator_compacted[i];
  }
}

static size_t sum_page_type_stats(const size_t values[ZPageTypeCount]) {
  size_t sum = 0;
  for (uint i = 0; i < ZPageTypeCount; i++) {
    sum += values[i];
  }
  return sum;
}

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

inline size_t ZPageAllocatorStats::freelist_available_at_start(ZPageType type) const {
  return _freelist_available_at_start[untype(type)];
}

inline size_t ZPageAllocatorStats::freelist_available_at_start() const {
  return sum_page_type_stats(_freelist_available_at_start);
}

inline size_t ZPageAllocatorStats::freelist_promoted(ZPageType type) const {
  return _freelist_promoted[untype(type)];
}

inline size_t ZPageAllocatorStats::freelist_promoted() const {
  return sum_page_type_stats(_freelist_promoted);
}

inline size_t ZPageAllocatorStats::mutator_freelist_promoted(ZPageType type) const {
  return _mutator_freelist_promoted[untype(type)];
}

inline size_t ZPageAllocatorStats::mutator_freelist_promoted() const {
  return sum_page_type_stats(_mutator_freelist_promoted);
}

inline size_t ZPageAllocatorStats::freelist_compacted(ZPageType type) const {
  return _freelist_compacted[untype(type)];
}

inline size_t ZPageAllocatorStats::freelist_compacted() const {
  return sum_page_type_stats(_freelist_compacted);
}

inline size_t ZPageAllocatorStats::mutator_freelist_compacted(ZPageType type) const {
  return _mutator_freelist_compacted[untype(type)];
}

inline size_t ZPageAllocatorStats::mutator_freelist_compacted() const {
  return sum_page_type_stats(_mutator_freelist_compacted);
}

inline size_t ZPageAllocatorStats::freed() const {
  return _freed;
}

inline size_t ZPageAllocatorStats::promoted(ZPageType type) const {
  return _promoted[untype(type)];
}

inline size_t ZPageAllocatorStats::promoted() const {
  return sum_page_type_stats(_promoted);
}

inline size_t ZPageAllocatorStats::mutator_promoted(ZPageType type) const {
  return _mutator_promoted[untype(type)];
}

inline size_t ZPageAllocatorStats::mutator_promoted() const {
  return sum_page_type_stats(_mutator_promoted);
}

inline size_t ZPageAllocatorStats::flip_promoted(ZPageType type) const {
  return _flip_promoted[untype(type)];
}

inline size_t ZPageAllocatorStats::flip_promoted() const {
  return sum_page_type_stats(_flip_promoted);
}

inline size_t ZPageAllocatorStats::compacted(ZPageType type) const {
  return _compacted[untype(type)];
}

inline size_t ZPageAllocatorStats::compacted() const {
  return sum_page_type_stats(_compacted);
}

inline size_t ZPageAllocatorStats::mutator_compacted(ZPageType type) const {
  return _mutator_compacted[untype(type)];
}

inline size_t ZPageAllocatorStats::mutator_compacted() const {
  return sum_page_type_stats(_mutator_compacted);
}

inline size_t ZPageAllocatorStats::allocation_stalls() const {
  return _allocation_stalls;
}

#endif // SHARE_GC_Z_ZPAGEALLOCATOR_INLINE_HPP
