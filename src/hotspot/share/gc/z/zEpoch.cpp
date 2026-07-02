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

#include "gc/z/zEpoch.hpp"
#include "utilities/debug.hpp"

ZEpoch::ZEpoch()
  : _slots(),
    _current_slot(0),
    _current_epoch(0) {
  _slots[0].store_relaxed(ZEpochEntry(true, 0));
}

ZEpoch::ZEpochId ZEpoch::advance() {
  assert(is_quiescent(), "Previous epoch has not quiesced");

  // Alternate between odd and even slots.
  const ZHold old_slot = _current_slot.load_acquire();
  const ZHold new_slot = static_cast<ZHold>(old_slot ^ 1u);

  const ZEpochEntry old_entry = _slots[new_slot].load_acquire();
  assert(!old_entry.open() && old_entry.readers() == 0, "Slot is not reusable");

  // Open the alternate slot before publishing it as current. Readers can then
  // always join either the old slot (while it remains open) or the new slot.
  _slots[new_slot].release_store(ZEpochEntry(true, 0));
  _current_slot.release_store(new_slot);

  // Close the old epoch with a CAS
  for (;;) {
    ZEpochEntry state = _slots[old_slot].load_acquire();
    assert(state.open(), "Old slot is not open");
    const ZEpochEntry closed(false, state.readers());
    if (_slots[old_slot].compare_set(state, closed)) {
      break;
    }
  }

  return ++_current_epoch;
}

bool ZEpoch::is_quiescent() {
  const ZHold old_slot = static_cast<ZHold>(_current_slot.load_acquire() ^ 1u);
  const ZEpochEntry entry = _slots[old_slot].load_acquire();
  if (entry.readers() != 0) {
    return false;
  }

  assert(!entry.open(), "Draining slot is open");
  return true;
}

ZEpoch::ZHold ZEpoch::hold() {
  for (;;) {
    const ZHold slot = _current_slot.load_acquire();
    ZEpochEntry entry = _slots[slot].load_acquire();

    if (!entry.open()) {
      continue;
    }

    const uint64_t readers = entry.readers();

    const ZEpochEntry held(true, readers + 1);
    if (_slots[slot].compare_set(entry, held)) {
      return slot;
    }
  }
}

void ZEpoch::release(ZHold hold) {
  assert(hold < 2, "Invalid hold");

  for (;;) {
    ZEpochEntry entry = _slots[hold].load_acquire();
    const uint64_t readers = entry.readers();
    assert(readers != 0, "Where is my stake?");

    const ZEpochEntry released(entry.open(), readers - 1);
    if (_slots[hold].compare_set(entry, released)) {
      return;
    }
  }
}

bool ZEpoch::has_holders() const {
  return _slots[0].load_acquire().readers() != 0 ||
         _slots[1].load_acquire().readers() != 0;
}
