/*
 * Copyright (c) 2016, 2018, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/z/zPhysicalMemory.inline.hpp"
#include "unittest.hpp"

TEST(ZPhysicalMemoryTest, copy) {
  const ZPhysicalMemorySegment seg0(0,   100);
  const ZPhysicalMemorySegment seg1(200, 100);

  ZPhysicalMemory pmem0;
  pmem0.add_segment(seg0);
  EXPECT_EQ(pmem0.nsegments(), 1u) << "wrong number of segments";
  EXPECT_EQ(pmem0.segment(0).size(), 100u) << "wrong segment size";

  ZPhysicalMemory pmem1;
  pmem1.add_segment(seg0);
  pmem1.add_segment(seg1);
  EXPECT_EQ(pmem1.nsegments(), 2u) << "wrong number of segments";
  EXPECT_EQ(pmem1.segment(0).size(), 100u) << "wrong segment size";
  EXPECT_EQ(pmem1.segment(1).size(), 100u) << "wrong segment size";

  ZPhysicalMemory pmem2(pmem0);
  EXPECT_EQ(pmem2.nsegments(), 1u) << "wrong number of segments";
  EXPECT_EQ(pmem2.segment(0).size(), 100u) << "wrong segment size";

  pmem2 = pmem1;
  EXPECT_EQ(pmem2.nsegments(), 2u) << "wrong number of segments";
  EXPECT_EQ(pmem2.segment(0).size(), 100u) << "wrong segment size";
  EXPECT_EQ(pmem2.segment(1).size(), 100u) << "wrong segment size";
}

TEST(ZPhysicalMemoryTest, segments) {
  const ZPhysicalMemorySegment seg0(0, 1);
  const ZPhysicalMemorySegment seg1(1, 1);
  const ZPhysicalMemorySegment seg2(2, 1);
  const ZPhysicalMemorySegment seg3(3, 1);
  const ZPhysicalMemorySegment seg4(4, 1);
  const ZPhysicalMemorySegment seg5(5, 1);
  const ZPhysicalMemorySegment seg6(6, 1);

  ZPhysicalMemory pmem0;
  EXPECT_EQ(pmem0.nsegments(), 0u) << "wrong number of segments";
  EXPECT_EQ(pmem0.is_null(), true) << "should be null";

  ZPhysicalMemory pmem1;
  pmem1.add_segment(seg0);
  pmem1.add_segment(seg1);
  pmem1.add_segment(seg2);
  pmem1.add_segment(seg3);
  pmem1.add_segment(seg4);
  pmem1.add_segment(seg5);
  pmem1.add_segment(seg6);
  EXPECT_EQ(pmem1.nsegments(), 1u) << "wrong number of segments";
  EXPECT_EQ(pmem1.segment(0).size(), 7u) << "wrong segment size";
  EXPECT_EQ(pmem1.is_null(), false) << "should not be null";

  ZPhysicalMemory pmem2;
  pmem2.add_segment(seg0);
  pmem2.add_segment(seg1);
  pmem2.add_segment(seg2);
  pmem2.add_segment(seg4);
  pmem2.add_segment(seg5);
  pmem2.add_segment(seg6);
  EXPECT_EQ(pmem2.nsegments(), 2u) << "wrong number of segments";
  EXPECT_EQ(pmem2.segment(0).size(), 3u) << "wrong segment size";
  EXPECT_EQ(pmem2.segment(1).size(), 3u) << "wrong segment size";
  EXPECT_EQ(pmem2.is_null(), false) << "should not be null";

  ZPhysicalMemory pmem3;
  pmem3.add_segment(seg0);
  pmem3.add_segment(seg2);
  pmem3.add_segment(seg3);
  pmem3.add_segment(seg4);
  pmem3.add_segment(seg6);
  EXPECT_EQ(pmem3.nsegments(), 3u) << "wrong number of segments";
  EXPECT_EQ(pmem3.segment(0).size(), 1u) << "wrong segment size";
  EXPECT_EQ(pmem3.segment(1).size(), 3u) << "wrong segment size";
  EXPECT_EQ(pmem3.segment(2).size(), 1u) << "wrong segment size";
  EXPECT_EQ(pmem3.is_null(), false) << "should not be null";

  ZPhysicalMemory pmem4;
  pmem4.add_segment(seg0);
  pmem4.add_segment(seg2);
  pmem4.add_segment(seg4);
  pmem4.add_segment(seg6);
  EXPECT_EQ(pmem4.nsegments(), 4u) << "wrong number of segments";
  EXPECT_EQ(pmem4.segment(0).size(), 1u) << "wrong segment size";
  EXPECT_EQ(pmem4.segment(1).size(), 1u) << "wrong segment size";
  EXPECT_EQ(pmem4.segment(2).size(), 1u) << "wrong segment size";
  EXPECT_EQ(pmem4.segment(3).size(), 1u) << "wrong segment size";
  EXPECT_EQ(pmem4.is_null(), false) << "should not be null";
}
