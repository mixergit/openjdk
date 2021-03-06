/*
 * Copyright (c) 2001, 2018, Oracle and/or its affiliates. All rights reserved.
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
 *
 */

#ifndef SHARE_VM_GC_SHARED_CARDTABLERS_HPP
#define SHARE_VM_GC_SHARED_CARDTABLERS_HPP

#include "gc/shared/cardTable.hpp"
#include "memory/memRegion.hpp"
#include "oops/oop.hpp"

class DirtyCardToOopClosure;
class Generation;
class Space;
class OopsInGenClosure;

// Helper to remember modified oops in all clds.
class CLDRemSet {
  bool _accumulate_modified_oops;
 public:
  CLDRemSet() : _accumulate_modified_oops(false) {}
  void set_accumulate_modified_oops(bool value) { _accumulate_modified_oops = value; }
  bool accumulate_modified_oops() { return _accumulate_modified_oops; }
  bool mod_union_is_clear();
  void clear_mod_union();
};

// This RemSet uses a card table both as shared data structure
// for a mod ref barrier set and for the rem set information.

class CardTableRS: public CardTable {
  friend class VMStructs;
  // Below are private classes used in impl.
  friend class VerifyCTSpaceClosure;
  friend class ClearNoncleanCardWrapper;

  CLDRemSet _cld_rem_set;

  void verify_space(Space* s, HeapWord* gen_start);

  enum ExtendedCardValue {
    youngergen_card   = CT_MR_BS_last_reserved + 1,
    // These are for parallel collection.
    // There are three P (parallel) youngergen card values.  In general, this
    // needs to be more than the number of generations (including the perm
    // gen) that might have younger_refs_do invoked on them separately.  So
    // if we add more gens, we have to add more values.
    youngergenP1_card  = CT_MR_BS_last_reserved + 2,
    youngergenP2_card  = CT_MR_BS_last_reserved + 3,
    youngergenP3_card  = CT_MR_BS_last_reserved + 4,
    cur_youngergen_and_prev_nonclean_card =
      CT_MR_BS_last_reserved + 5
  };

  // An array that contains, for each generation, the card table value last
  // used as the current value for a younger_refs_do iteration of that
  // portion of the table. The perm gen is index 0. The young gen is index 1,
  // but will always have the value "clean_card". The old gen is index 2.
  jbyte* _last_cur_val_in_gen;

  jbyte _cur_youngergen_card_val;

  // Number of generations, plus one for lingering PermGen issues in CardTableRS.
  static const int _regions_to_iterate = 3;

  jbyte cur_youngergen_card_val() {
    return _cur_youngergen_card_val;
  }
  void set_cur_youngergen_card_val(jbyte v) {
    _cur_youngergen_card_val = v;
  }
  bool is_prev_youngergen_card_val(jbyte v) {
    return
      youngergen_card <= v &&
      v < cur_youngergen_and_prev_nonclean_card &&
      v != _cur_youngergen_card_val;
  }
  // Return a youngergen_card_value that is not currently in use.
  jbyte find_unused_youngergenP_card_value();

public:
  CardTableRS(MemRegion whole_heap);
  ~CardTableRS();

  CLDRemSet* cld_rem_set() { return &_cld_rem_set; }

  void younger_refs_in_space_iterate(Space* sp, OopsInGenClosure* cl, uint n_threads);

  // Override.
  void prepare_for_younger_refs_iterate(bool parallel);

  // Card table entries are cleared before application; "blk" is
  // responsible for dirtying if the oop is still older-to-younger after
  // closure application.
  void younger_refs_iterate(Generation* g, OopsInGenClosure* blk, uint n_threads);

  void inline_write_ref_field_gc(void* field, oop new_val) {
    jbyte* byte = byte_for(field);
    *byte = youngergen_card;
  }
  void write_ref_field_gc_work(void* field, oop new_val) {
    inline_write_ref_field_gc(field, new_val);
  }

  // Override.  Might want to devirtualize this in the same fashion as
  // above.  Ensures that the value of the card for field says that it's
  // a younger card in the current collection.
  virtual void write_ref_field_gc_par(void* field, oop new_val);

  bool is_aligned(HeapWord* addr) {
    return is_card_aligned(addr);
  }

  void verify();
  void initialize();

  void clear_into_younger(Generation* old_gen);

  void invalidate_or_clear(Generation* old_gen);

  bool is_prev_nonclean_card_val(jbyte v) {
    return
      youngergen_card <= v &&
      v <= cur_youngergen_and_prev_nonclean_card &&
      v != _cur_youngergen_card_val;
  }

  static bool youngergen_may_have_been_dirty(jbyte cv) {
    return cv == CardTableRS::cur_youngergen_and_prev_nonclean_card;
  }

  // *** Support for parallel card scanning.

  // dirty and precleaned are equivalent wrt younger_refs_iter.
  static bool card_is_dirty_wrt_gen_iter(jbyte cv) {
    return cv == dirty_card || cv == precleaned_card;
  }

  // Returns "true" iff the value "cv" will cause the card containing it
  // to be scanned in the current traversal.  May be overridden by
  // subtypes.
  bool card_will_be_scanned(jbyte cv);

  // Returns "true" iff the value "cv" may have represented a dirty card at
  // some point.
  bool card_may_have_been_dirty(jbyte cv);

  // Iterate over the portion of the card-table which covers the given
  // region mr in the given space and apply cl to any dirty sub-regions
  // of mr. Clears the dirty cards as they are processed.
  void non_clean_card_iterate_possibly_parallel(Space* sp, MemRegion mr,
                                                OopsInGenClosure* cl, CardTableRS* ct,
                                                uint n_threads);

  // Work method used to implement non_clean_card_iterate_possibly_parallel()
  // above in the parallel case.
  void non_clean_card_iterate_parallel_work(Space* sp, MemRegion mr,
                                            OopsInGenClosure* cl, CardTableRS* ct,
                                            uint n_threads);

  // This is an array, one element per covered region of the card table.
  // Each entry is itself an array, with one element per chunk in the
  // covered region.  Each entry of these arrays is the lowest non-clean
  // card of the corresponding chunk containing part of an object from the
  // previous chunk, or else NULL.
  typedef jbyte*  CardPtr;
  typedef CardPtr* CardArr;
  CardArr* _lowest_non_clean;
  size_t*  _lowest_non_clean_chunk_size;
  uintptr_t* _lowest_non_clean_base_chunk_index;
  volatile int* _last_LNC_resizing_collection;

  // Initializes "lowest_non_clean" to point to the array for the region
  // covering "sp", and "lowest_non_clean_base_chunk_index" to the chunk
  // index of the corresponding to the first element of that array.
  // Ensures that these arrays are of sufficient size, allocating if necessary.
  // May be called by several threads concurrently.
  void get_LNC_array_for_space(Space* sp,
                               jbyte**& lowest_non_clean,
                               uintptr_t& lowest_non_clean_base_chunk_index,
                               size_t& lowest_non_clean_chunk_size);

  // Returns the number of chunks necessary to cover "mr".
  size_t chunks_to_cover(MemRegion mr) {
    return (size_t)(addr_to_chunk_index(mr.last()) -
                    addr_to_chunk_index(mr.start()) + 1);
  }

  // Returns the index of the chunk in a stride which
  // covers the given address.
  uintptr_t addr_to_chunk_index(const void* addr) {
    uintptr_t card = (uintptr_t) byte_for(addr);
    return card / ParGCCardsPerStrideChunk;
  }

  // Apply cl, which must either itself apply dcto_cl or be dcto_cl,
  // to the cards in the stride (of n_strides) within the given space.
  void process_stride(Space* sp,
                      MemRegion used,
                      jint stride, int n_strides,
                      OopsInGenClosure* cl,
                      CardTableRS* ct,
                      jbyte** lowest_non_clean,
                      uintptr_t lowest_non_clean_base_chunk_index,
                      size_t lowest_non_clean_chunk_size);

  // Makes sure that chunk boundaries are handled appropriately, by
  // adjusting the min_done of dcto_cl, and by using a special card-table
  // value to indicate how min_done should be set.
  void process_chunk_boundaries(Space* sp,
                                DirtyCardToOopClosure* dcto_cl,
                                MemRegion chunk_mr,
                                MemRegion used,
                                jbyte** lowest_non_clean,
                                uintptr_t lowest_non_clean_base_chunk_index,
                                size_t    lowest_non_clean_chunk_size);

  virtual bool is_in_young(oop obj) const;

};

class ClearNoncleanCardWrapper: public MemRegionClosure {
  DirtyCardToOopClosure* _dirty_card_closure;
  CardTableRS* _ct;
  bool _is_par;
private:
  // Clears the given card, return true if the corresponding card should be
  // processed.
  inline bool clear_card(jbyte* entry);
  // Work methods called by the clear_card()
  inline bool clear_card_serial(jbyte* entry);
  inline bool clear_card_parallel(jbyte* entry);
  // check alignment of pointer
  bool is_word_aligned(jbyte* entry);

public:
  ClearNoncleanCardWrapper(DirtyCardToOopClosure* dirty_card_closure, CardTableRS* ct, bool is_par);
  void do_MemRegion(MemRegion mr);
};

#endif // SHARE_VM_GC_SHARED_CARDTABLERS_HPP
