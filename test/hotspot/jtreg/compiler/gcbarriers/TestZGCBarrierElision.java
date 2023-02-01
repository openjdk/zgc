/*
 * Copyright (c) 2022, Oracle and/or its affiliates. All rights reserved.
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

package compiler.gcbarriers;

import compiler.lib.ir_framework.*;
import compiler.lib.ir_framework.CompilePhase;

/**
 * @test
 * @summary Test elision of dominating barriers in ZGC.
 * @library /test/lib /
 * @requires vm.gc.Z
 * @run driver compiler.gcbarriers.TestZGCBarrierElision
 */

class Inner {}

class Outer {
    volatile Inner inner;
    public Outer(Inner inner) {
        this.inner = inner;
    }
}

public class TestZGCBarrierElision {

    static Inner inner = new Inner();
    static Outer outer = new Outer(inner);

    public static void main(String[] args) {
        TestFramework framework = new TestFramework();
        Scenario zgc = new Scenario(0, "-XX:+UseZGC", "-XX:+UnlockExperimentalVMOptions", "-XX:CompileCommand=quiet",
                                       "-XX:CompileCommand=blackhole,compiler.gcbarriers.TestZGCBarrierElision::blackhole");
        framework.addScenarios(zgc).start();
    }

    static void blackhole(Object o) {}

    @Test
    @IR(counts = { IRNode.ZLOADP_WITH_BARRIER_FLAG, "strong", "1" }, phase = CompilePhase.FINAL_CODE)
    @IR(counts = { IRNode.ZLOADP_WITH_BARRIER_FLAG, "elided", "1" }, phase = CompilePhase.FINAL_CODE)
    private static void testLoadThenLoad(Outer o, Inner i) {
        blackhole(o.inner);
        blackhole(o.inner);
    }

    @Test
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, "strong", "1" }, phase = CompilePhase.FINAL_CODE)
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, "elided", "1" }, phase = CompilePhase.FINAL_CODE)
    private static void testStoreThenStore(Outer o, Inner i) {
        o.inner = i;
        o.inner = i;
    }

    @Test
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, "strong", "1" }, phase = CompilePhase.FINAL_CODE)
    @IR(counts = { IRNode.ZLOADP_WITH_BARRIER_FLAG, "elided", "1" },  phase = CompilePhase.FINAL_CODE)
    private static void testStoreThenLoad(Outer o, Inner i) {
        o.inner = i;
        blackhole(o.inner);
    }

    @Test
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, "strong", "1" }, phase = CompilePhase.FINAL_CODE)
    @IR(counts = { IRNode.ZLOADP_WITH_BARRIER_FLAG, "strong", "1" },  phase = CompilePhase.FINAL_CODE)
    private static void testLoadThenStore(Outer o, Inner i) {
        blackhole(o.inner);
        o.inner = i;
    }

    @Run(test = {"testLoadThenLoad",
                 "testStoreThenStore",
                 "testStoreThenLoad",
                 "testLoadThenStore"})
    private void run() {
        testLoadThenLoad(outer, inner);
        testStoreThenStore(outer, inner);
        testStoreThenLoad(outer, inner);
        testLoadThenStore(outer, inner);
    }
}
