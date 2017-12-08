/*
 * Copyright (c) 2015, 2017, Oracle and/or its affiliates. All rights reserved.
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

#ifndef OS_CPU_SOLARIS_SPARC_ZGLOBALS_SOLARIS_SPARC_HPP
#define OS_CPU_SOLARIS_SPARC_ZGLOBALS_SOLARIS_SPARC_HPP

//
// Page Allocation Tiers
// ---------------------
//
//  Page Type     Page Size     Object Size Limit     Object Alignment
//  ------------------------------------------------------------------
//  Small         4M            <= 512K               <MinObjAlignmentInBytes>
//  Medium        64M           <= 8M                 8K
//  Large         X*M           > 8M                  4M
//  ------------------------------------------------------------------
//
//
// Address Space & Pointer Layout
// ------------------------------
//
//  +--------------------------------+ 0xFFFFFFFFFFFFFFFF (16EB)
//  .                                .
//  .                                .
//  .                                .
//  +--------------------------------+ 0x0000080000000000 (8TB)
//  |              Heap              |
//  +--------------------------------+ 0x0000040000000000 (4TB)
//  .                                .
//  +--------------------------------+ 0x0000000000000000
//
//
//  * 63-60 ADI bits (4-bits)
//  |
//  |    * 59-56 VA Masking Bits (4-bits)
//  |    |
//  |    |
//  |6  6|5  5 5            4 4 4                                             0
//  |3  0|9  6 5            3 2 1                                             0
//  +----+----+--------------+-+-----------------------------------------------+
//  |0000|1111|00000000 00000|1|11 11111111 11111111 11111111 11111111 11111111|
//  +----+----+--------------+-+-----------------------------------------------+
//  |    |    |              | |
//  |    |    |              | * 41-0 Object Offset (42-bits, 4TB address space)
//  |    |    |              |
//  |    |    |              * 42-42 Address Base (1-bit)
//  |    |    |
//  |    |    * 55-43 Unused (13-bits, always zero)
//  |    |
//  |    * 59-56 Metadata Bits (4-bits)  0001 = Marked0
//  |                                    0010 = Marked1
//  |                                    0100 = Remapped
//  |                                    1000 = Finalizable
//  * 63-60 Fixed (4-bits, always zero)
//

const size_t    ZPlatformPageSizeSmallShift   = 22; // 4M

const size_t    ZPlatformAddressOffsetBits    = 42; // 4TB

const uintptr_t ZPlatformAddressMetadataShift = BitsPerWord - BitsPerByte; // 1 byte allocated for VA masking

const uintptr_t ZPlatformAddressSpaceStart    = (uintptr_t)1 << ZPlatformAddressOffsetBits;
const uintptr_t ZPlatformAddressSpaceSize     = (uintptr_t)1 << ZPlatformAddressOffsetBits;

const int       ZPlatformADIBits              = 4; // Always assume 4 ADI bits
const int       ZPlatformVAMaskBits           = 4;

#endif // OS_CPU_SOLARIS_SPARC_ZGLOBALS_SOLARIS_SPARC_HPP
