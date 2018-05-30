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
#include "unittest.hpp"
#include "utilities/behaviours.hpp"

class ReturnNumberBehaviour {
public:
  virtual int number() = 0;
};

class ReturnFiveBehaviour: public ReturnNumberBehaviour {
public:
  virtual int number() {
    return 5;
  }
};

class ReturnSixBehaviour: public ReturnNumberBehaviour {
public:
  virtual int number() {
    return 6;
  }
};

class ReturnOneBehaviour: public ReturnNumberBehaviour {
public:
  virtual int number() {
    return 1;
  }
};

TEST(Behaviours, local) {
  DefaultBehaviourMark<ReturnNumberBehaviour, ReturnFiveBehaviour> bm;
  ASSERT_EQ(Behaviours::get_behaviour<ReturnNumberBehaviour>().number(), 5) << "Should be 5";
}

TEST(Behaviours, local_stacked) {
  DefaultBehaviourMark<ReturnNumberBehaviour, ReturnFiveBehaviour> bm;
  DefaultBehaviourMark<ReturnNumberBehaviour, ReturnSixBehaviour> bm2;
  ASSERT_EQ(Behaviours::get_behaviour<ReturnNumberBehaviour>().number(), 6) << "Should be 6";
}

TEST(Behaviours, global) {
  BehaviourProviderCollection providers;
  Behaviours::register_global_provider(providers);

  ReturnFiveBehaviour b1;
  ReturnSixBehaviour b2;

  providers.register_behaviour<ReturnNumberBehaviour>(b1);
  providers.register_behaviour<ReturnNumberBehaviour>(b2);

  ASSERT_EQ(Behaviours::get_behaviour<ReturnNumberBehaviour>().number(), 6) << "Should be 6";

  {
    DefaultBehaviourMark<ReturnNumberBehaviour, ReturnOneBehaviour> bm;
    ASSERT_EQ(Behaviours::get_behaviour<ReturnNumberBehaviour>().number(), 1) << "Should be 1";
    ASSERT_EQ(Behaviours::get_behaviour<ReturnNumberBehaviour>().number(), 1) << "Should be 1";

    {
      DefaultBehaviourMark<ReturnNumberBehaviour, ReturnFiveBehaviour> bm;
      ASSERT_EQ(Behaviours::get_behaviour<ReturnNumberBehaviour>().number(), 5) << "Should be 5";
      ASSERT_EQ(Behaviours::get_behaviour<ReturnNumberBehaviour>().number(), 5) << "Should be 5";
    }

    ASSERT_EQ(Behaviours::get_behaviour<ReturnNumberBehaviour>().number(), 1) << "Should be 1";
    ASSERT_EQ(Behaviours::get_behaviour<ReturnNumberBehaviour>().number(), 1) << "Should be 1";
  }

  ASSERT_EQ(Behaviours::get_behaviour<ReturnNumberBehaviour>().number(), 6) << "Should be 6";
}
