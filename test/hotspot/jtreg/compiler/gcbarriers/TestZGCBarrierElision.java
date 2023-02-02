/*
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
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
import java.util.concurrent.ThreadLocalRandom;
import jdk.internal.misc.Unsafe;

/**
 * @test
 * @summary Test elision of dominating barriers in ZGC.
 * @modules java.base/jdk.internal.misc
 * @library /test/lib /
 * @requires vm.gc.Z
 * @run driver compiler.gcbarriers.TestZGCBarrierElision
 */

class Inner {}

class Outer {
    // Declared volatile to prevent C2 from optimizing memory accesses away.
    volatile Inner field1;
    volatile Inner field2;
    public Outer() {}
}

public class TestZGCBarrierElision {

    static Inner inner = new Inner();
    static Outer outer = new Outer();
    static Outer[] outerArray = new Outer[42];
    public static final Unsafe U = Unsafe.getUnsafe();

    public static void main(String[] args) {
        TestFramework framework = new TestFramework();
        Scenario zgc = new Scenario(0, "--add-exports=java.base/jdk.internal.misc=ALL-UNNAMED",
                                    "-XX:+UseZGC", "-XX:+UnlockExperimentalVMOptions", "-XX:CompileCommand=quiet",
                                    "-XX:CompileCommand=blackhole,compiler.gcbarriers.TestZGCBarrierElision::blackhole",
                                    "-XX:CompileCommand=dontinline,compiler.gcbarriers.TestZGCBarrierElision::nonInlinedMethod",
                                    "-XX:-UseCountedLoopSafepoints", "-XX:LoopMaxUnroll=0");
        framework.addScenarios(zgc).start();
    }

    static void blackhole(Object o) {}
    static void nonInlinedMethod() {}

    @Test
    @IR(counts = { IRNode.ZLOADP_WITH_BARRIER_FLAG, "elided", "1" }, phase = CompilePhase.FINAL_CODE)
    private static void testAllocateThenLoad(Outer o, Inner i) {
        Outer o1 = new Outer();
        // This blackhole is necessary to prevent C2 from optimizing away the entire body.
        blackhole(o1);
        // Two loads are required, the first one is directly optimized away.
        blackhole(o1.field1);
        blackhole(o1.field1);
    }

    @Test
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, "elided", "1" }, phase = CompilePhase.FINAL_CODE)
    private static void testAllocateThenStore(Outer o, Inner i) {
        Outer o1 = new Outer();
        // This blackhole is necessary to prevent C2 from optimizing away the entire body.
        blackhole(o1);
        o1.field1 = i;
    }

    @Test
    @IR(counts = { IRNode.ZLOADP_WITH_BARRIER_FLAG, "strong", "1" }, phase = CompilePhase.FINAL_CODE)
    @IR(counts = { IRNode.ZLOADP_WITH_BARRIER_FLAG, "elided", "1" }, phase = CompilePhase.FINAL_CODE)
    private static void testLoadThenLoad(Outer o, Inner i) {
        blackhole(o.field1);
        blackhole(o.field1);
    }

    @Test
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, "strong", "1" }, phase = CompilePhase.FINAL_CODE)
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, "elided", "1" }, phase = CompilePhase.FINAL_CODE)
    private static void testStoreThenStore(Outer o, Inner i) {
        o.field1 = i;
        o.field1 = i;
    }

    @Test
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, "strong", "1" }, phase = CompilePhase.FINAL_CODE)
    @IR(counts = { IRNode.ZLOADP_WITH_BARRIER_FLAG, "elided", "1" },  phase = CompilePhase.FINAL_CODE)
    private static void testStoreThenLoad(Outer o, Inner i) {
        o.field1 = i;
        blackhole(o.field1);
    }

    @Test
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, "strong", "1" }, phase = CompilePhase.FINAL_CODE)
    @IR(counts = { IRNode.ZLOADP_WITH_BARRIER_FLAG, "strong", "1" },  phase = CompilePhase.FINAL_CODE)
    private static void testLoadThenStore(Outer o, Inner i) {
        blackhole(o.field1);
        o.field1 = i;
    }

    @Test
    @IR(counts = { IRNode.ZLOADP_WITH_BARRIER_FLAG, "strong", "2" }, phase = CompilePhase.FINAL_CODE)
    private static void testLoadThenLoadAnotherField(Outer o, Inner i) {
        blackhole(o.field1);
        blackhole(o.field2);
    }

    @Run(test = {"testAllocateThenLoad",
                 "testAllocateThenStore",
                 "testLoadThenLoad",
                 "testStoreThenStore",
                 "testStoreThenLoad",
                 "testLoadThenStore",
                 "testLoadThenLoadAnotherField"})
    private void runBasicTests() {
        testAllocateThenLoad(outer, inner);
        testAllocateThenStore(outer, inner);
        testLoadThenLoad(outer, inner);
        testStoreThenStore(outer, inner);
        testStoreThenLoad(outer, inner);
        testLoadThenStore(outer, inner);
        testLoadThenLoadAnotherField(outer, inner);
    }

    @Test
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, "elided", "1" }, phase = CompilePhase.FINAL_CODE)
    private static void testAllocateArrayThenStoreAtKnownIndex(Outer o, Inner i) {
        long offset = U.arrayBaseOffset(Outer[].class);
        Outer[] a = new Outer[42];
        U.putReferenceVolatile(a, offset, o);
    }

    @Test
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, "elided", "1" }, phase = CompilePhase.FINAL_CODE)
    private static void testAllocateArrayThenStoreAtUnknownIndex(Outer o, Inner i, int index) {
        long offset = U.arrayBaseOffset(Outer[].class);
        int scale = U.arrayIndexScale(Outer[].class);
        Outer[] a = new Outer[42];
        U.putReferenceVolatile(a, offset + index * scale, o);
    }

    @Test
    @IR(counts = { IRNode.ZLOADP_WITH_BARRIER_FLAG, "strong", "1" }, phase = CompilePhase.FINAL_CODE)
    @IR(counts = { IRNode.ZLOADP_WITH_BARRIER_FLAG, "elided", "1" }, phase = CompilePhase.FINAL_CODE)
    private static void testArrayLoadThenLoad(Outer[] o, Outer o1, Outer o2, Inner i) {
        long offset = U.arrayBaseOffset(Outer[].class);
        blackhole(U.getReferenceVolatile(o, offset));
        blackhole(U.getReferenceVolatile(o, offset));
    }

    @Test
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, "strong", "1" }, phase = CompilePhase.FINAL_CODE)
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, "elided", "1" }, phase = CompilePhase.FINAL_CODE)
    private static void testArrayStoreThenStore(Outer[] o, Outer o1, Outer o2, Inner i) {
        long offset = U.arrayBaseOffset(Outer[].class);
        U.putReferenceVolatile(o, offset, o1);
        U.putReferenceVolatile(o, offset, o2);
    }

    @Test
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, "strong", "1" }, phase = CompilePhase.FINAL_CODE)
    @IR(counts = { IRNode.ZLOADP_WITH_BARRIER_FLAG, "elided", "1" }, phase = CompilePhase.FINAL_CODE)
    private static void testArrayStoreThenLoad(Outer[] o, Outer o1, Outer o2, Inner i) {
        long offset = U.arrayBaseOffset(Outer[].class);
        U.putReferenceVolatile(o, offset, o1);
        blackhole(U.getReferenceVolatile(o, offset));
    }

    @Test
    @IR(counts = { IRNode.ZLOADP_WITH_BARRIER_FLAG, "strong", "1" }, phase = CompilePhase.FINAL_CODE)
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, "strong", "1" }, phase = CompilePhase.FINAL_CODE)
    private static void testArrayLoadThenStore(Outer[] o, Outer o1, Outer o2, Inner i) {
        long offset = U.arrayBaseOffset(Outer[].class);
        blackhole(U.getReferenceVolatile(o, offset));
        U.putReferenceVolatile(o, offset, o1);
    }

    @Test
    @IR(counts = { IRNode.ZLOADP_WITH_BARRIER_FLAG, "strong", "2" }, phase = CompilePhase.FINAL_CODE)
    private static void testArrayLoadThenLoadAnotherElement(Outer[] o, Outer o1, Outer o2, Inner i) {
        long offset = U.arrayBaseOffset(Outer[].class);
        int scale = U.arrayIndexScale(Outer[].class);
        blackhole(U.getReferenceVolatile(o, offset));
        blackhole(U.getReferenceVolatile(o, offset + 10 * scale));
    }

    @Run(test = {"testAllocateArrayThenStoreAtKnownIndex",
                 "testAllocateArrayThenStoreAtUnknownIndex",
                 "testArrayLoadThenLoad",
                 "testArrayStoreThenStore",
                 "testArrayStoreThenLoad",
                 "testArrayLoadThenStore",
                 "testArrayLoadThenLoadAnotherElement"})
    private void runArrayTests() {
        testAllocateArrayThenStoreAtKnownIndex(outer, inner);
        testAllocateArrayThenStoreAtUnknownIndex(outer, inner, 10);
        testArrayLoadThenLoad(outerArray, outer, outer, inner);
        testArrayStoreThenStore(outerArray, outer, outer, inner);
        testArrayStoreThenLoad(outerArray, outer, outer, inner);
        testArrayLoadThenStore(outerArray, outer, outer, inner);
        testArrayLoadThenLoadAnotherElement(outerArray, outer, outer, inner);
    }

    @Test
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, "strong", "1" }, phase = CompilePhase.FINAL_CODE)
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, "elided", "1" }, phase = CompilePhase.FINAL_CODE)
    private static void testStoreThenConditionalStore(Outer o, Inner i, int value) {
        o.field1 = i;
        if (value % 2 == 0) {
            o.field1 = i;
        }
    }

    @Test
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, "strong", "2" }, phase = CompilePhase.FINAL_CODE)
    private static void testConditionalStoreThenStore(Outer o, Inner i, int value) {
        if (value % 2 == 0) {
            o.field1 = i;
        }
        o.field1 = i;
    }

    @Test
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, "strong", "1" }, phase = CompilePhase.FINAL_CODE)
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, "elided", "1" }, phase = CompilePhase.FINAL_CODE)
    private static void testStoreThenLoopThenStore(Outer o, Inner i, int value) {
        o.field1 = i;
        for (int j = 0; j < 100; j++) {
            o.field1 = i;
        }
    }

    @Test
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, "strong", "2" }, phase = CompilePhase.FINAL_CODE)
    private static void testStoreThenCallThenStore(Outer o, Inner i) {
        o.field1 = i;
        nonInlinedMethod();
        o.field1 = i;
    }

    @Run(test = {"testStoreThenConditionalStore",
                 "testConditionalStoreThenStore",
                 "testStoreThenLoopThenStore",
                 "testStoreThenCallThenStore"})
    private void runControlFlowTests() {
        testStoreThenConditionalStore(outer, inner, ThreadLocalRandom.current().nextInt(0, 100));
        testConditionalStoreThenStore(outer, inner, ThreadLocalRandom.current().nextInt(0, 100));
        testStoreThenLoopThenStore(outer, inner, 10);
        testStoreThenCallThenStore(outer, inner);
    }

}
