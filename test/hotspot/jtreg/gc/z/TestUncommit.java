/*
 * Copyright (c) 2019, Oracle and/or its affiliates. All rights reserved.
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

package gc.z;

/*
 * @test TestUncommit
 * @requires vm.gc.Z
 * @summary Verify that ZGC uncommits memory
 * @run main/othervm -XX:+UnlockExperimentalVMOptions -XX:+UseZGC -Xms128M -Xmx256M -XX:ZUncommitDelay=10 gc.z.TestUncommit
 */

import java.util.ArrayList;

public class TestUncommit {
    private static final long uncommitDelay = 10;
    private static final Runtime runtime = Runtime.getRuntime();
    private static final long minCapacity = runtime.totalMemory();
    private static final long maxCapacity = runtime.maxMemory();
    private static final long allocCapacity = (minCapacity + maxCapacity) / 2;
    private static volatile ArrayList<Object> keepAlive;

    private static void testUncommit() throws Exception {
        // Allocate memory
        keepAlive = new ArrayList<>();
        while (runtime.totalMemory() < allocCapacity) {
            keepAlive.add(new byte[4096]);
        }

        final var afterAllocCapacity = runtime.totalMemory();

        // Reclaim memory
        keepAlive = null;
        System.gc();

        // Wait shorter than the uncommit delay
        Thread.sleep(uncommitDelay * 1000 / 2);
        final var afterReclaimCapacity = runtime.totalMemory();

        // Wait longer than the uncommit delay
        Thread.sleep(uncommitDelay * 1000);
        final var afterUncommitCapacity= runtime.totalMemory();

        // Check capacity
        System.out.println("           Min Capacity: " + minCapacity);
        System.out.println("           Max Capacity: " + maxCapacity);
        System.out.println("         Alloc Capacity: " + allocCapacity);
        System.out.println("   After Alloc Capacity: " + afterAllocCapacity);
        System.out.println(" After Reclaim Capacity: " + afterReclaimCapacity);
        System.out.println("After Uncommit Capacity: " + afterUncommitCapacity);

        if (afterAllocCapacity > afterReclaimCapacity) {
            throw new Exception("Uncommitted too fast");
        }

        if (afterUncommitCapacity < minCapacity) {
            throw new Exception("Uncommitted too much");
        }

        if (afterUncommitCapacity > minCapacity) {
            throw new Exception("Uncommitted too little");
        }
    }

    public static void main(String[] args) throws Exception {
        for (int i = 0; i < 3; i++) {
            System.out.println("Iteration " + i);
            testUncommit();
        }
    }
}
