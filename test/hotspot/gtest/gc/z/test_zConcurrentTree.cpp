/*
 * Copyright (c) 2026, Oracle and/or its affiliates. All rights reserved.
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

#include "gc/z/zArray.inline.hpp"
#include "gc/z/zConcurrentTree.inline.hpp"
#include "unittest.hpp"

class ZConcurrentTreeTestEntry {
  uint64_t _key;
  uint64_t _value;

public:
  using KeyT = uint64_t;
  using ValueT = uint64_t;

  ZConcurrentTreeTestEntry()
    : _key(0),
      _value(0) {}

  ZConcurrentTreeTestEntry(uint64_t key, uint64_t value)
    : _key(key),
      _value(value) {}

  operator ValueT() const {
    return _value;
  }

  static ZConcurrentTreeTestEntry empty() {
    return ZConcurrentTreeTestEntry();
  }

  static bool is_empty(ZConcurrentTreeTestEntry entry) {
    return entry._key == 0;
  }

  bool is_empty() const {
    return is_empty(*this);
  }

  static ZConcurrentTreeTestEntry create(KeyT key, ValueT value) {
    return ZConcurrentTreeTestEntry(key, value);
  }

  static int cmp(ZConcurrentTreeTestEntry entry, KeyT key) {
    return entry._key < key ? -1 : entry._key > key ? 1 : 0;
  }

  static int cmp(ZConcurrentTreeTestEntry left, ZConcurrentTreeTestEntry right) {
    return cmp(left, right._key);
  }
};

static constexpr size_t fibonacci_count = 80;

static void make_fibonacci(uint64_t* values) {
  // Start at F(2): F(0) is the empty-entry sentinel and F(1)/F(2) duplicate.
  values[0] = 1;
  values[1] = 2;
  for (size_t i = 2; i < fibonacci_count; ++i) {
    values[i] = values[i - 1] + values[i - 2];
  }
}

static void assert_present(ZConcurrentTree<ZConcurrentTreeTestEntry>* tree, uint64_t key) {
  uint64_t value = 0;
  ASSERT_TRUE(tree->find(key, &value));
  ASSERT_EQ(value, key);
}

static void assert_absent(ZConcurrentTree<ZConcurrentTreeTestEntry>* tree, uint64_t key) {
  uint64_t value = 0;
  ASSERT_FALSE(tree->find(key, &value));
  ASSERT_EQ(value, 0u);
}

static void shuffle(size_t* indexes, size_t count) {
  // A fixed local PRNG keeps the randomized insertion order reproducible.
  uint32_t state = 0x6d2b79f5;
  for (size_t i = count; i > 1; --i) {
    state = state * 1664525u + 1013904223u;
    const size_t other = state % i;
    const size_t last = i - 1;
    const size_t saved = indexes[last];
    indexes[last] = indexes[other];
    indexes[other] = saved;
  }
}

TEST(ZConcurrentTree, single_threaded) {
  ZConcurrentTree<ZConcurrentTreeTestEntry> tree;
  uint64_t value = UINT64_MAX;

  // find() reports an absent key without changing its result argument.
  ASSERT_FALSE(tree.find(10, &value));
  ASSERT_EQ(value, UINT64_MAX);

  // try_insert() accepts new keys and rejects duplicates.
  ASSERT_TRUE(tree.try_insert(10, 100));
  ASSERT_TRUE(tree.try_insert(5, 50));
  ASSERT_TRUE(tree.try_insert(15, 150));
  ASSERT_TRUE(tree.try_insert(3, 30));
  ASSERT_TRUE(tree.try_insert(7, 70));
  ASSERT_FALSE(tree.try_insert(10, 101));

  ASSERT_TRUE(tree.find(3, &value));
  ASSERT_EQ(value, 30u);
  ASSERT_TRUE(tree.find(5, &value));
  ASSERT_EQ(value, 50u);
  ASSERT_TRUE(tree.find(7, &value));
  ASSERT_EQ(value, 70u);
  ASSERT_TRUE(tree.find(10, &value));
  ASSERT_EQ(value, 100u);
  ASSERT_TRUE(tree.find(15, &value));
  ASSERT_EQ(value, 150u);

  // try_remove() removes both leaves and nodes with children, and rejects
  // attempts to remove an already absent key.
  ASSERT_FALSE(tree.try_remove(9));
  ASSERT_TRUE(tree.try_remove(3));
  ASSERT_FALSE(tree.find(3, &value));
  ASSERT_TRUE(tree.try_remove(10));
  ASSERT_FALSE(tree.find(10, &value));
  ASSERT_FALSE(tree.try_remove(10));

  ASSERT_TRUE(tree.find(5, &value));
  ASSERT_EQ(value, 50u);
  ASSERT_TRUE(tree.find(7, &value));
  ASSERT_EQ(value, 70u);
  ASSERT_TRUE(tree.find(15, &value));
  ASSERT_EQ(value, 150u);
}

TEST(ZConcurrentTree, fibonacci_random_insert_and_remove) {
  uint64_t fibonacci[fibonacci_count];
  size_t order[fibonacci_count];
  make_fibonacci(fibonacci);

  for (size_t i = 0; i < fibonacci_count; ++i) {
    order[i] = i;
  }
  shuffle(order, fibonacci_count);

  ZConcurrentTree<ZConcurrentTreeTestEntry> tree;
  for (size_t i = 0; i < fibonacci_count; ++i) {
    const uint64_t key = fibonacci[order[i]];
    ASSERT_TRUE(tree.try_insert(key, key));
    ASSERT_FALSE(tree.try_insert(key, key + 1));
  }

  for (size_t i = 0; i < fibonacci_count; ++i) {
    assert_present(&tree, fibonacci[i]);
  }

  // Remove a randomized two-thirds and verify both the removed keys and
  // the surviving sparse tree after every structural rebuild opportunity.
  const size_t removed_count = fibonacci_count * 2 / 3;
  for (size_t i = 0; i < removed_count; ++i) {
    ASSERT_TRUE(tree.try_remove(fibonacci[order[i]]));
    ASSERT_FALSE(tree.try_remove(fibonacci[order[i]]));
  }

  for (size_t i = 0; i < fibonacci_count; ++i) {
    if (i < removed_count) {
      assert_absent(&tree, fibonacci[order[i]]);
    } else {
      assert_present(&tree, fibonacci[order[i]]);
    }
  }
}

TEST(ZConcurrentTree, fibonacci_ascending_insert) {
  uint64_t fibonacci[fibonacci_count];
  make_fibonacci(fibonacci);

  ZConcurrentTree<ZConcurrentTreeTestEntry> tree;
  for (size_t i = 0; i < fibonacci_count; ++i) {
    ASSERT_TRUE(tree.try_insert(fibonacci[i], fibonacci[i]));
  }

  for (size_t i = 0; i < fibonacci_count; ++i) {
    assert_present(&tree, fibonacci[i]);
  }

  for (size_t i = 0; i < fibonacci_count; i += 2) {
    ASSERT_TRUE(tree.try_remove(fibonacci[i]));
  }
  for (size_t i = 0; i < fibonacci_count; ++i) {
    if (i % 2 == 0) {
      assert_absent(&tree, fibonacci[i]);
    } else {
      assert_present(&tree, fibonacci[i]);
    }
  }
}

TEST(ZConcurrentTree, fibonacci_descending_insert) {
  uint64_t fibonacci[fibonacci_count];
  make_fibonacci(fibonacci);

  ZConcurrentTree<ZConcurrentTreeTestEntry> tree;
  for (size_t i = fibonacci_count; i > 0; --i) {
    const uint64_t key = fibonacci[i - 1];
    ASSERT_TRUE(tree.try_insert(key, key));
  }

  for (size_t i = 0; i < fibonacci_count; ++i) {
    assert_present(&tree, fibonacci[i]);
  }

  for (size_t i = 1; i < fibonacci_count; i += 2) {
    ASSERT_TRUE(tree.try_remove(fibonacci[i]));
  }
  for (size_t i = 0; i < fibonacci_count; ++i) {
    if (i % 2 == 0) {
      assert_present(&tree, fibonacci[i]);
    } else {
      assert_absent(&tree, fibonacci[i]);
    }
  }
}

