/*
 * Copyright (c) 2024, 2025, Oracle and/or its affiliates. All rights reserved.
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

#ifdef _WINDOWS

#include "gc/z/zAddress.inline.hpp"
#include "gc/z/zGlobals.hpp"
#include "gc/z/zInitialize.hpp"
#include "gc/z/zList.inline.hpp"
#include "gc/z/zMapper_windows.hpp"
#include "gc/z/zMemory.inline.hpp"
#include "gc/z/zNUMA.inline.hpp"
#include "gc/z/zSyscall_windows.hpp"
#include "gc/z/zValue.inline.hpp"
#include "gc/z/zVirtualMemoryManager.hpp"
#include "runtime/os.hpp"
#include "zunittest.hpp"

using namespace testing;

#define EXPECT_REMOVAL_OK(range) EXPECT_FALSE(range.is_null())

class ZMapperTest : public ZTest {
public:
  virtual void SetUp() {
    // Only run test on supported Windows versions
    if (!ZSyscall::is_supported()) {
      GTEST_SKIP() << "Requires Windows version 1803 or later";
      return;
    }
  }

  virtual void TearDown() {
    if (!ZSyscall::is_supported()) {
      // Test skipped, nothing to cleanup
      return;
    }
  }

  void test_unreserve() {
    const zaddress_unsafe base = zaddress_unsafe(ZAddressHeapBase * 2);
    const zaddress_unsafe reserved = ZMapper::reserve(base, 3 * ZGranuleSize);

    if (reserved == zaddress_unsafe::null) {
      GTEST_SKIP() << "Failed to reserve memory";
    }

    ZMapper::split_placeholder(reserved + 1 * ZGranuleSize, ZGranuleSize);

    ZMapper::unreserve(reserved + 0 * ZGranuleSize, ZGranuleSize);
    ZMapper::unreserve(reserved + 1 * ZGranuleSize, ZGranuleSize);
    ZMapper::unreserve(reserved + 2 * ZGranuleSize, ZGranuleSize);
  }
};

TEST_VM_F(ZMapperTest, test_unreserve) {
  test_unreserve();
}

#endif // _WINDOWS
