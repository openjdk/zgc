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
import java.lang.invoke.VarHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodHandles.Lookup;
import java.util.concurrent.ThreadLocalRandom;

/**
 * @test
 * @summary Test ZGC barrier elision by allocation and domination. The tests use
 *          volatile memory accesses and blackholes to prevent C2 from simply
 *          optimizing them away.
 * @library /test/lib /
 * @requires vm.gc.Z
 * @run driver compiler.gcbarriers.TestZGCBarrierElision
 */

class Inner {}

class Outer {
    volatile Inner field1;
    volatile Inner field2;
    public Outer() {}
}

public class TestZGCBarrierElision {

    static Inner inner = new Inner();
    static Outer outer = new Outer();
    static Outer outer2 = new Outer();
    static Outer[] outerArray = new Outer[42];

    static final VarHandle field1VarHandle;
    static final VarHandle field2VarHandle;
    static final VarHandle outerArrayVarHandle;
    static {
        try {
            MethodHandles.Lookup l = MethodHandles.lookup();
            field1VarHandle = l.findVarHandle(Outer.class, "field1", Inner.class);
            field2VarHandle = l.findVarHandle(Outer.class, "field2", Inner.class);
            outerArrayVarHandle = MethodHandles.arrayElementVarHandle(Outer[].class);
        } catch (ReflectiveOperationException e) {
            throw new Error(e);
        }
    }

    static final String REMAINING = "strong";
    static final String ELIDED = "elided";

    static void blackhole(Object o) {}

    static void nonInlinedMethod() {}

    public static void main(String[] args) {
        String className = TestZGCBarrierElision.class.getName();
        TestFramework.runWithFlags("-XX:+UseZGC", "-XX:+UnlockExperimentalVMOptions", "-XX:CompileCommand=quiet",
                                   "-XX:CompileCommand=blackhole,"  + className + "::blackhole",
                                   "-XX:CompileCommand=dontinline," + className + "::nonInlinedMethod",
                                   "-XX:LoopMaxUnroll=0");
    }

    @Test
    @IR(counts = { IRNode.ZLOADP_WITH_BARRIER_FLAG, ELIDED, "1" }, phase = CompilePhase.FINAL_CODE)
    private static void testAllocateThenLoad() {
        Outer o1 = new Outer();
        blackhole(o1);
        // This load is directly optimized away by C2.
        blackhole(o1.field1);
        blackhole(o1.field1);
    }

    @Test
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, ELIDED, "1" }, phase = CompilePhase.FINAL_CODE)
    private static void testAllocateThenStore(Inner i) {
        Outer o1 = new Outer();
        blackhole(o1);
        o1.field1 = i;
    }

    @Test
    @IR(counts = { IRNode.ZLOADP_WITH_BARRIER_FLAG, REMAINING, "1" }, phase = CompilePhase.FINAL_CODE)
    @IR(counts = { IRNode.ZLOADP_WITH_BARRIER_FLAG, ELIDED, "1" }, phase = CompilePhase.FINAL_CODE)
    private static void testLoadThenLoad(Outer o) {
        blackhole(o.field1);
        blackhole(o.field1);
    }

    @Test
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, REMAINING, "1" }, phase = CompilePhase.FINAL_CODE)
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, ELIDED, "1" }, phase = CompilePhase.FINAL_CODE)
    private static void testStoreThenStore(Outer o, Inner i) {
        o.field1 = i;
        o.field1 = i;
    }

    @Test
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, REMAINING, "1" }, phase = CompilePhase.FINAL_CODE)
    @IR(counts = { IRNode.ZLOADP_WITH_BARRIER_FLAG, ELIDED, "1" },  phase = CompilePhase.FINAL_CODE)
    private static void testStoreThenLoad(Outer o, Inner i) {
        o.field1 = i;
        blackhole(o.field1);
    }

    @Test
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, REMAINING, "1" }, phase = CompilePhase.FINAL_CODE)
    @IR(counts = { IRNode.ZLOADP_WITH_BARRIER_FLAG, REMAINING, "1" },  phase = CompilePhase.FINAL_CODE)
    private static void testLoadThenStore(Outer o, Inner i) {
        blackhole(o.field1);
        o.field1 = i;
    }

    @Test
    @IR(counts = { IRNode.ZLOADP_WITH_BARRIER_FLAG, REMAINING, "2" }, phase = CompilePhase.FINAL_CODE)
    private static void testLoadThenLoadAnotherField(Outer o) {
        blackhole(o.field1);
        blackhole(o.field2);
    }

    @Test
    @IR(counts = { IRNode.ZLOADP_WITH_BARRIER_FLAG, REMAINING, "2" }, phase = CompilePhase.FINAL_CODE)
    private static void testLoadThenLoadFromAnotherObject(Outer o1, Outer o2) {
        blackhole(o1.field1);
        blackhole(o2.field1);
    }

    @Run(test = {"testAllocateThenLoad",
                 "testAllocateThenStore",
                 "testLoadThenLoad",
                 "testStoreThenStore",
                 "testStoreThenLoad",
                 "testLoadThenStore",
                 "testLoadThenLoadAnotherField",
                 "testLoadThenLoadFromAnotherObject"})
    private void runBasicTests() {
        testAllocateThenLoad();
        testAllocateThenStore(inner);
        testLoadThenLoad(outer);
        testStoreThenStore(outer, inner);
        testStoreThenLoad(outer, inner);
        testLoadThenStore(outer, inner);
        testLoadThenLoadAnotherField(outer);
        testLoadThenLoadFromAnotherObject(outer, outer2);
    }

    @Test
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, ELIDED, "1" }, phase = CompilePhase.FINAL_CODE)
    private static void testAllocateArrayThenStoreAtKnownIndex(Outer o) {
        Outer[] a = new Outer[42];
        blackhole(a);
        outerArrayVarHandle.setVolatile(a, 0, o);
    }

    @Test
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, ELIDED, "1" }, phase = CompilePhase.FINAL_CODE)
    private static void testAllocateArrayThenStoreAtUnknownIndex(Outer o, int index) {
        Outer[] a = new Outer[42];
        blackhole(a);
        outerArrayVarHandle.setVolatile(a, index, o);
    }

    @Test
    @IR(counts = { IRNode.ZLOADP_WITH_BARRIER_FLAG, REMAINING, "1" }, phase = CompilePhase.FINAL_CODE)
    @IR(counts = { IRNode.ZLOADP_WITH_BARRIER_FLAG, ELIDED, "1" }, phase = CompilePhase.FINAL_CODE)
    private static void testArrayLoadThenLoad(Outer[] a) {
        blackhole(outerArrayVarHandle.getVolatile(a, 0));
        blackhole(outerArrayVarHandle.getVolatile(a, 0));
    }

    @Test
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, REMAINING, "1" }, phase = CompilePhase.FINAL_CODE)
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, ELIDED, "1" }, phase = CompilePhase.FINAL_CODE)
    private static void testArrayStoreThenStore(Outer[] a, Outer o) {
        outerArrayVarHandle.setVolatile(a, 0, o);
        outerArrayVarHandle.setVolatile(a, 0, o);
    }

    @Test
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, REMAINING, "1" }, phase = CompilePhase.FINAL_CODE)
    @IR(counts = { IRNode.ZLOADP_WITH_BARRIER_FLAG, ELIDED, "1" }, phase = CompilePhase.FINAL_CODE)
    private static void testArrayStoreThenLoad(Outer[] a, Outer o) {
        outerArrayVarHandle.setVolatile(a, 0, o);
        blackhole(outerArrayVarHandle.getVolatile(a, 0));
    }

    @Test
    @IR(counts = { IRNode.ZLOADP_WITH_BARRIER_FLAG, REMAINING, "1" }, phase = CompilePhase.FINAL_CODE)
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, REMAINING, "1" }, phase = CompilePhase.FINAL_CODE)
    private static void testArrayLoadThenStore(Outer[] a, Outer o) {
        blackhole(outerArrayVarHandle.getVolatile(a, 0));
        outerArrayVarHandle.setVolatile(a, 0, o);
    }

    @Test
    @IR(counts = { IRNode.ZLOADP_WITH_BARRIER_FLAG, REMAINING, "2" }, phase = CompilePhase.FINAL_CODE)
    private static void testArrayLoadThenLoadAnotherElement(Outer[] a) {
        blackhole(outerArrayVarHandle.getVolatile(a, 0));
        blackhole(outerArrayVarHandle.getVolatile(a, 10));
    }

    @Run(test = {"testAllocateArrayThenStoreAtKnownIndex",
                 "testAllocateArrayThenStoreAtUnknownIndex",
                 "testArrayLoadThenLoad",
                 "testArrayStoreThenStore",
                 "testArrayStoreThenLoad",
                 "testArrayLoadThenStore",
                 "testArrayLoadThenLoadAnotherElement"})
    private void runArrayTests() {
        testAllocateArrayThenStoreAtKnownIndex(outer);
        testAllocateArrayThenStoreAtUnknownIndex(outer, 10);
        testArrayLoadThenLoad(outerArray);
        testArrayStoreThenStore(outerArray, outer);
        testArrayStoreThenLoad(outerArray, outer);
        testArrayLoadThenStore(outerArray, outer);
        testArrayLoadThenLoadAnotherElement(outerArray);
    }

    @Test
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, REMAINING, "1" }, phase = CompilePhase.FINAL_CODE)
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, ELIDED, "1" }, phase = CompilePhase.FINAL_CODE)
    private static void testStoreThenConditionalStore(Outer o, Inner i, int value) {
        o.field1 = i;
        if (value % 2 == 0) {
            o.field1 = i;
        }
    }

    @Test
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, REMAINING, "2" }, phase = CompilePhase.FINAL_CODE)
    private static void testConditionalStoreThenStore(Outer o, Inner i, int value) {
        if (value % 2 == 0) {
            o.field1 = i;
        }
        o.field1 = i;
    }

    @Test
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, REMAINING, "1" }, phase = CompilePhase.FINAL_CODE)
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, ELIDED, "1" }, phase = CompilePhase.FINAL_CODE)
    private static void testStoreThenStoreInLoop(Outer o, Inner i) {
        o.field1 = i;
        for (int j = 0; j < 100; j++) {
            o.field1 = i;
        }
    }

    @Test
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, REMAINING, "2" }, phase = CompilePhase.FINAL_CODE)
    private static void testStoreThenCallThenStore(Outer o, Inner i) {
        o.field1 = i;
        nonInlinedMethod();
        o.field1 = i;
    }

    @Run(test = {"testStoreThenConditionalStore",
                 "testConditionalStoreThenStore",
                 "testStoreThenStoreInLoop",
                 "testStoreThenCallThenStore"})
    private void runControlFlowTests() {
        testStoreThenConditionalStore(outer, inner, ThreadLocalRandom.current().nextInt(0, 100));
        testConditionalStoreThenStore(outer, inner, ThreadLocalRandom.current().nextInt(0, 100));
        testStoreThenStoreInLoop(outer, inner);
        testStoreThenCallThenStore(outer, inner);
    }

    @Test
    @IR(counts = { IRNode.ZXCHGP_WITH_BARRIER_FLAG, REMAINING, "1" }, phase = CompilePhase.FINAL_CODE)
    private static void testAllocateThenAtomic(Inner i) {
        Outer o = new Outer();
        blackhole(o);
        field1VarHandle.getAndSet​(o, i);
    }

    @Test
    @IR(counts = { IRNode.ZLOADP_WITH_BARRIER_FLAG, REMAINING, "1" }, phase = CompilePhase.FINAL_CODE)
    @IR(counts = { IRNode.ZXCHGP_WITH_BARRIER_FLAG, REMAINING, "1" }, phase = CompilePhase.FINAL_CODE)
    private static void testLoadThenAtomic(Outer o, Inner i) {
        blackhole(o.field1);
        field1VarHandle.getAndSet​(o, i);
    }

    @Test
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, REMAINING, "1" }, phase = CompilePhase.FINAL_CODE)
    // The atomic access barrier should be elided, but is not.
    private static void testStoreThenAtomic(Outer o, Inner i) {
        o.field1 = i;
        field1VarHandle.getAndSet​(o, i);
    }

    @Test
    @IR(counts = { IRNode.ZXCHGP_WITH_BARRIER_FLAG, REMAINING, "1" }, phase = CompilePhase.FINAL_CODE)
    // The load barrier should be elided, but is not.
    private static void testAtomicThenLoad(Outer o, Inner i) throws Exception {
        field1VarHandle.getAndSet​(o, i);
        blackhole(o.field1);
    }

    @Test
    @IR(counts = { IRNode.ZXCHGP_WITH_BARRIER_FLAG, REMAINING, "1" }, phase = CompilePhase.FINAL_CODE)
    // The store barrier should be elided, but is not.
    private static void testAtomicThenStore(Outer o, Inner i) {
        field1VarHandle.getAndSet​(o, i);
        o.field1 = i;
    }

    @Test
    // The second atomic access barrier should be elided, but is not.
    private static void testAtomicThenAtomic(Outer o, Inner i) {
        field1VarHandle.getAndSet​(o, i);
        field1VarHandle.getAndSet​(o, i);
    }

    @Test
    @IR(counts = { IRNode.ZXCHGP_WITH_BARRIER_FLAG, REMAINING, "2" }, phase = CompilePhase.FINAL_CODE)
    private static void testAtomicThenAtomicAnotherField(Outer o, Inner i) {
        field1VarHandle.getAndSet​(o, i);
        field2VarHandle.getAndSet​(o, i);
    }

    @Run(test = {"testAllocateThenAtomic",
                 "testLoadThenAtomic",
                 "testStoreThenAtomic",
                 "testAtomicThenLoad",
                 "testAtomicThenStore",
                 "testAtomicThenAtomic",
                 "testAtomicThenAtomicAnotherField"})
    private void runAtomicOperationTests() throws Exception {
            testAllocateThenAtomic(inner);
            testLoadThenAtomic(outer, inner);
            testStoreThenAtomic(outer, inner);
            testAtomicThenLoad(outer, inner);
            testAtomicThenStore(outer, inner);
            testAtomicThenAtomic(outer, inner);
            testAtomicThenAtomicAnotherField(outer, inner);
    }

}
