/*
 * Copyright (c) 2015, 2025, Oracle and/or its affiliates. All rights reserved.
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

#include "gc/shared/gc_globals.hpp"
#include "gc/z/zAddress.hpp"
#include "gc/z/zAddress.inline.hpp"
#include "gc/z/zFreeList.inline.hpp"
#include "gc/z/zGeneration.hpp"
#include "gc/z/zGeneration.inline.hpp"
#include "gc/z/zGlobals.hpp"
#include "gc/z/zPage.inline.hpp"
#include "gc/z/zPageAge.inline.hpp"
#include "gc/z/zPageType.hpp"
#include "gc/z/zReferenceCounting.hpp"
#include "gc/z/zRelocate.hpp"
#include "gc/z/zRememberedSet.inline.hpp"
#include "gc/z/zUtils.hpp"
#include "gc/z/zUtils.inline.hpp"
#include "runtime/atomicAccess.hpp"
#include "utilities/align.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"

ZPage::ZPage(ZPageType type, ZPageAge age, const ZVirtualMemory& vmem, ZMultiPartitionTracker* multi_partition_tracker, uint32_t partition_id, ZRememberedSet* remset)
  : _type(type),
    _generation_id(/* set in reset */),
    _age(/* set in reset */),
    _seqnum(/* set in reset */),
    _seqnum_other(/* set in reset */),
    _single_partition_id(partition_id),
    _virtual(vmem),
    _top(to_zoffset_end(start())),
    _livemap(object_max_count()),
    _remembered_set(),
    _multi_partition_tracker(multi_partition_tracker),
    _relocate_promoted(),
    _flip_aged(),
    _remset_flip_retained(),
    _free_list_unused() {
  assert(!_virtual.is_null(), "Should not be null");
  assert((_type == ZPageType::small && size() == ZPageSizeSmall) ||
         (_type == ZPageType::medium && ZPageSizeMediumMin <= size() && size() <= ZPageSizeMediumMax) ||
         (_type == ZPageType::large && is_aligned(size(), ZGranuleSize)),
         "Page type/size mismatch");
  reset(age);

  // TODO: Debug info for now
  const uintptr_t start_value = static_cast<uintptr_t>(start());
  assert(start_value <= ZAddressOffsetMax, "Offset out of bounds (" PTR_FORMAT " <= " PTR_FORMAT ")", start_value, ZAddressOffsetMax);

  if (is_old()) {
    if (remset != nullptr) {
      remset_init(remset);
    } else {
      remset_alloc();
    }
    if (ZOldRefCount) {
      if (_type == ZPageType::small) {
        _free_list_small = new ZFreeList<ZPageType::small>(*this);
      } else {
        _free_list_medium = new ZFreeList<ZPageType::medium>(*this);
      }
    }
  }
}

ZPage::ZPage(ZPageType type, ZPageAge age, const ZVirtualMemory& vmem, uint32_t partition_id)
  : ZPage(type, age, vmem, nullptr /* multi_partition_tracker */, partition_id, nullptr) {}

ZPage::ZPage(ZPageType type, ZPageAge age, const ZVirtualMemory& vmem, ZMultiPartitionTracker* multi_partition_tracker)
  : ZPage(type, age, vmem, multi_partition_tracker, -1u /* partition_id */, nullptr /* remset */) {}

ZPage* ZPage::clone(ZPageAge age, ZRememberedSet* remset) {
  precond(age >= this->age());

  // Only copy type and memory layouts, and also update _top. Let the rest be
  // lazily reconstructed when needed.
  ZPage* const page = new ZPage(_type, age, _virtual, _multi_partition_tracker, _single_partition_id, remset);
  page->_top = _top;

  // Now that ownership of the remset has moved on to the new page, make sue we
  // don't free it when the current page is freed.
  _remset_flip_retained = true;

  return page;
}

ZPage::~ZPage() {
  if (_remset_flip_retained) {
    _remembered_set.uninitialize();
  }
  if (ZOldRefCount && is_old()) {
    if (_type == ZPageType::small) {
      delete _free_list_small;
    } else {
      delete _free_list_medium;
    }
  }
}

ZPage* ZPage::inplace_relocate_page() {
  const ZPageAge to_age = ZRelocate::compute_to_age(age());

  if (::is_old(to_age)) {
    return clone(to_age, nullptr);
  }

  reset(to_age);

  return this;
}

ZPage* ZPage::flip_age() {
  if (is_old()) {
    // Old to Old
    precond(ZRelocate::compute_to_age(age()) == ZPageAge::old);

    ZPage* const page = clone(ZPageAge::old, &_remembered_set);
    AtomicAccess::store(&page->_flip_aged, true);

    // TODO: Make sure the old copy is safe deleted.

    return page;
  }

  const ZPageAge to_age = ZRelocate::compute_to_age(age());

  if (is_young() && to_age == ZPageAge::promotion) {
    // Young to Old (Promotion)
    ZPage* const page = clone(to_age, nullptr);
    AtomicAccess::store(&page->_flip_aged, true);

    return page;
  }

  // Young to Young
  precond(::is_young(to_age));

  reset(to_age);

  AtomicAccess::store(&_flip_aged, true);

  return this;
}

bool ZPage::is_flip_aged() const {
  return AtomicAccess::load(&_flip_aged);
}

bool ZPage::is_flip_promoted() const {
  return is_promoted() && is_flip_aged();
}

bool ZPage::is_promoted() const {
  return age() == ZPageAge::promotion;
}

bool ZPage::is_flip_promoted_current_young_collection() const {
  return is_flip_promoted() && _seqnum_other == ZGeneration::young()->seqnum();
}

bool ZPage::allows_raw_null() const {
  return is_young() && !AtomicAccess::load(&_relocate_promoted);
}

void ZPage::set_is_relocate_promoted() {
  AtomicAccess::store(&_relocate_promoted, true);
}

ZGeneration* ZPage::generation() {
  return ZGeneration::generation(_generation_id);
}

const ZGeneration* ZPage::generation() const {
  return ZGeneration::generation(_generation_id);
}

ZGeneration* ZPage::generation_other() {
  return ZGeneration::generation(_generation_id == ZGenerationId::young ? ZGenerationId::old : ZGenerationId::young);
}

const ZGeneration* ZPage::generation_other() const {
  return ZGeneration::generation(_generation_id == ZGenerationId::young ? ZGenerationId::old : ZGenerationId::young);
}

void ZPage::reset(ZPageAge age) {
  _age = age;

  _generation_id = ::is_old(age) ? ZGenerationId::old : ZGenerationId::young;

  reset_seqnum();
}

void ZPage::reset_seqnum() {
  AtomicAccess::store(&_seqnum, generation()->seqnum());
  AtomicAccess::store(&_seqnum_other, generation_other()->seqnum());
}


void ZPage::remset_alloc() {
  // Remsets should only be allocated/initialized once and only for old pages.
  assert(!_remembered_set.is_initialized(), "Should not be initialized");
  assert(is_old(), "Only old pages need a remset");

  _remembered_set.initialize(size());
}

void ZPage::remset_init(ZRememberedSet* remset) {
  assert(!_remembered_set.is_initialized(), "Should not be initialized");
  assert(is_old(), "Only old pages need a remset");

  _remembered_set.initialize(remset);
}

void ZPage::remset_uninit() {
  _remembered_set.uninitialize();
}

void ZPage::clear_livemap_bits() {
  _livemap.clear_bits();
}

void ZPage::reset_livemap() {
  _livemap.reset();
}

void ZPage::reset_top_for_allocation() {
  _top = to_zoffset_end(start());
}

class ZFindBaseOopClosure : public ObjectClosure {
private:
  volatile zpointer* _p;
  oop _result;

public:
  ZFindBaseOopClosure(volatile zpointer* p)
    : _p(p),
      _result(nullptr) {}

  virtual void do_object(oop obj) {
    const uintptr_t p_int = reinterpret_cast<uintptr_t>(_p);
    const uintptr_t base_int = cast_from_oop<uintptr_t>(obj);
    const uintptr_t end_int = base_int + wordSize * obj->size();
    if (p_int >= base_int && p_int < end_int) {
      _result = obj;
    }
  }

  oop result() const { return _result; }
};

bool ZPage::is_remset_cleared_current() const {
  return _remembered_set.is_cleared_current();
}

bool ZPage::is_remset_cleared_previous() const {
  return _remembered_set.is_cleared_previous();
}

void ZPage::verify_remset_cleared_current() const {
  if (ZVerifyRemembered && !is_remset_cleared_current()) {
    fatal_msg(" current remset bits should be cleared");
  }
}

void ZPage::verify_remset_cleared_previous() const {
  if (ZVerifyRemembered && !is_remset_cleared_previous()) {
    fatal_msg(" previous remset bits should be cleared");
  }
}

void ZPage::verify_remset_cleared_or_store_good_previous() {
  if (!ZVerifyRemembered) {
    return;
  }

  _remembered_set.iterate_previous([&](uintptr_t local_offset) {
    const zaddress field_addr = ZOffset::address(start() + local_offset);
    volatile zpointer* p = (volatile zpointer*)field_addr;
    zpointer o = AtomicAccess::load(p);

    if (!ZPointer::is_store_good_or_null(o)) {
      fatal_msg(" previous remset bits should be cleared");
    }

    return true;
  });
}

void ZPage::clear_remset_previous() {
  _remembered_set.clear_previous();
}

void ZPage::prune_dead_remset_entries() {
  const uint32_t young_marks = ZGeneration::old()->young_marks_since_old_mark_end();

  assert(young_marks <= 1, "why is this invoked after the first YC after old mark end?");

  // When pruning dead remembered set entries, we must keep in mind that the
  // maintenance of these entries stop at old mark end. Then, the young collector
  // starts filtering dead entries, acting as if they don't exist. Therefore, the
  // location of the dead remembered set entries may be either in the current or
  // previous bits, dependong on whether an even or odd number of young generation
  // collections have started since old mark end.

  if (young_marks == 0) {
    _remembered_set.iterate_current([&](uintptr_t local_offset) {
      const zaddress field_addr = ZOffset::address(start() + local_offset);
      const zaddress base = safe(find_base((volatile zpointer*)field_addr));

      if (is_null(base) || field_addr - base >= ZUtils::object_size(base)) {
        // In dead object
        _remembered_set.unset_current(local_offset);
      }

      return true;
    });
  } else {
    assert(young_marks == 1, "must be");

    _remembered_set.iterate_previous([&](uintptr_t local_offset) {
      const zaddress field_addr = ZOffset::address(start() + local_offset);
      const zaddress base = safe(find_base((volatile zpointer*)field_addr));

      if (is_null(base) || field_addr - base >= ZUtils::object_size(base)) {
        // In dead object
        _remembered_set.unset_previous(local_offset);
      }

      return true;
    });
  }
}

void ZPage::swap_remset_bitmaps() {
  _remembered_set.swap_remset_bitmaps();
}

void* ZPage::remset_current() {
  return _remembered_set.current();
}

size_t ZPage::coalesce_free_list() {
  precond(!ZGeneration::young()->is_phase_relocate());
  precond(is_allocating());

  if (_type == ZPageType::small) {
    return _free_list_small->coalesce_free_list();
  } else {
    return _free_list_medium->coalesce_free_list();
  }
}

void ZPage::free_tail_to_free_list(zaddress_unsafe addr, size_t size) {
  assert(is_old(), "Reference-counting may only free objects on old pages");
  assert(is_allocating(), "Reference-counting may only free objects on allocating pages");

  if (_type == ZPageType::small) {
    _free_list_small->free_tail(addr, size);
  } else {
    _free_list_medium->free_tail(addr, size);
  }

  ZGeneration::young()->on_free_list_insert(this);

#ifdef ASSERT
  // TODO: Use the right zap function instead.
  const size_t header_size = 8;
  ZUtils::fill(reinterpret_cast<uintptr_t*>(untype(addr) + header_size), ZUtils::bytes_to_words(size - header_size), 0xdeafbabedeafbabe);
#endif
}

void ZPage::undo_alloc_object_from_free_list(zaddress_unsafe addr, size_t size) {
  assert(is_old(), "Reference-counting may only free objects on old pages");
  assert(is_allocating(), "Reference-counting may only free objects on allocating pages");

  if (_type == ZPageType::small) {
    _free_list_small->undo_allocate(addr, size);
  } else {
    _free_list_medium->undo_allocate(addr, size);
  }

  ZGeneration::young()->on_free_list_insert(this);
#ifdef ASSERT
  // TODO: Use the right zap function instead.
  const size_t header_size = 8;
  ZUtils::fill(reinterpret_cast<uintptr_t*>(untype(addr) + header_size), ZUtils::bytes_to_words(size - header_size), 0xdeafbabedeafbabe);
#endif
}

void ZPage::free_object_to_free_list(zaddress_unsafe addr, size_t size) {
  assert(is_old(), "Reference-counting may only free objects on old pages");
  assert(is_allocating(), "Reference-counting may only free objects on allocating pages");

  if (_type == ZPageType::small) {
    _free_list_small->free(addr, size);
  } else {
    _free_list_medium->free(addr, size);
  }

  ZGeneration::young()->on_free_list_insert(this);
#ifdef ASSERT
  // TODO: Use the right zap function instead.
  const size_t header_size = 8;
  ZUtils::fill(reinterpret_cast<uintptr_t*>(untype(addr) + header_size), ZUtils::bytes_to_words(size - header_size), 0xdeafbabedeafbabe);
#endif
}

void ZPage::free_object_to_free_list(zaddress addr) {
  size_t size = ZUtils::object_size(addr);
  free_object_to_free_list(unsafe(addr), size);
}

zaddress ZPage::alloc_object_from_free_list(size_t size) {
  if (_type == ZPageType::small) {
    return _free_list_small->allocate(size);
  } else {
    return _free_list_medium->allocate(size);
  }
}

void ZPage::print_free_list_on(outputStream* st) const {
  if (_type == ZPageType::small) {
    _free_list_small->print_on(st);
  } else {
    _free_list_medium->print_on(st);
  }
}

void ZPage::print_on_msg(outputStream* st, const char* msg) const {
  st->print_cr("%-6s  " PTR_FORMAT " " PTR_FORMAT " " PTR_FORMAT " %s/%-4u %s%s%s%s",
                type_to_string(), untype(start()), untype(top()), untype(end()),
                is_young() ? "Y" : "O",
                seqnum(),
                is_relocatable() ? " Relocatable" : "",
                is_allocating()  ? " Allocating"  : "",
                is_allocating() && msg != nullptr ? " " : "",
                msg != nullptr ? msg : "");
}

void ZPage::print_on(outputStream* st) const {
  print_on_msg(st, nullptr);
}

void ZPage::print() const {
  print_on(tty);
}

void ZPage::verify_live(uint32_t live_objects, size_t live_bytes, bool in_place) const {
  if (!in_place) {
    // In-place relocation has changed the page to allocating
    assert_zpage_mark_state();
  }
  guarantee(live_objects == _livemap.live_objects(), "Invalid number of live objects");
  guarantee(live_bytes == _livemap.live_bytes(), "Invalid number of live bytes");
}

void ZPage::fatal_msg(const char* msg) const {
  stringStream ss;
  print_on_msg(&ss, msg);
  fatal("%s", ss.base());
}
