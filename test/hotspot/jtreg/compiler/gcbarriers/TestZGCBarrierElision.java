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

class Payload {
    Content c;
    public Payload(Content c) {
        this.c = c;
    }
}

class Content {
    int id;
    public Content(int id) {
        this.id = id;
    }
}

public class TestZGCBarrierElision {

    Payload p = new Payload(new Content(5));
    Content c1 = new Content(45);
    Content c2 = new Content(15);

    public static void main(String[] args) {
        TestFramework framework = new TestFramework();
        Scenario zgc = new Scenario(0, "-XX:+UseZGC", "-XX:+UnlockExperimentalVMOptions", "-XX:CompileCommand=quiet",
                                       "-XX:CompileCommand=blackhole,compiler.gcbarriers.TestZGCBarrierElision::blackhole");
        framework.addScenarios(zgc).start();
    }

    static void blackhole(Content t) {}

    static void blackhole(Payload p) {}

    @Test
    @IR(counts = { IRNode.ZLOADP_WITH_BARRIER_FLAG, "strong", "1" },
        phase = CompilePhase.FINAL_CODE)
    @IR(counts = { IRNode.ZLOADP_WITH_BARRIER_FLAG, "elided", "1" },
        phase = CompilePhase.FINAL_CODE)
    private static void testLoadFollowedByLoad(Payload p) {
        Content t1 = p.c;
        blackhole(t1);
        Content t2 = p.c;
        blackhole(t2);
    }

    @Test
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, "strong", "1" },
        phase = CompilePhase.FINAL_CODE)
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, "elided", "1" },
        phase = CompilePhase.FINAL_CODE)
    private static void testStoreFollowedByStore(Payload p, Content t1, Content t2) {
        p.c = t1;
        blackhole(p);
        p.c = t2;
    }

    @Test
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, "strong", "1" },
        phase = CompilePhase.FINAL_CODE)
    @IR(counts = { IRNode.ZLOADP_WITH_BARRIER_FLAG, "elided", "1" },
        phase = CompilePhase.FINAL_CODE)
    private static void testStoreFollowedByLoad(Payload p, Content t1) {
        p.c = t1;
        blackhole(p);
        Content t2 = p.c;
        blackhole(t2);
    }

    @Test
    @IR(counts = { IRNode.ZSTOREP_WITH_BARRIER_FLAG, "strong", "1" },
        phase = CompilePhase.FINAL_CODE)
    @IR(counts = { IRNode.ZLOADP_WITH_BARRIER_FLAG, "strong", "1" },
        phase = CompilePhase.FINAL_CODE)
    private static void testLoadFollowedByStore(Payload p, Content t1) {
        Content t2 = p.c;
        blackhole(t2);
        p.c = t1;
    }

    @Run(test = {"testLoadFollowedByLoad",
                 "testStoreFollowedByStore",
                 "testStoreFollowedByLoad",
                 "testLoadFollowedByStore"})
    private void run() {
        testLoadFollowedByLoad(p);
        testStoreFollowedByStore(p, c1, c2);
        testStoreFollowedByLoad(p, c1);
        testLoadFollowedByStore(p, c1);
    }
}
