/*
 * Copyright (c) 2015, Oracle and/or its affiliates. All rights reserved.
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

#include "precompiled.hpp"

#include "gc/g1/bufferingOopClosure.hpp"
#include "gc/g1/g1CodeBlobClosure.hpp"
#include "gc/g1/g1CollectedHeap.hpp"
#include "gc/g1/g1OopClosures.inline.hpp"
#include "gc/g1/g1RootClosures.hpp"

class G1ParScanThreadState;

// Simple holder object for a complete set of closures used by the G1 evacuation code.
template <G1Mark Mark>
class G1SharedClosures VALUE_OBJ_CLASS_SPEC {
public:
  G1ParCopyClosure<G1BarrierNone,  Mark> _oops;
  G1ParCopyClosure<G1BarrierKlass, Mark> _oop_in_klass;
  G1KlassScanClosure                     _klass_in_cld_closure;
  CLDToKlassAndOopClosure                _clds;
  G1CodeBlobClosure                      _codeblobs;
  BufferingOopClosure                    _buffered_oops;

  G1SharedClosures(G1CollectedHeap* g1h, G1ParScanThreadState* pss, bool process_only_dirty_klasses, bool must_claim_cld) :
    _oops(g1h, pss),
    _oop_in_klass(g1h, pss),
    _klass_in_cld_closure(&_oop_in_klass, process_only_dirty_klasses),
    _clds(&_klass_in_cld_closure, &_oops, must_claim_cld),
    _codeblobs(&_oops),
    _buffered_oops(&_oops) {}
};

class G1EvacuationClosures : public G1EvacuationRootClosures {
  G1SharedClosures<G1MarkNone> _closures;

public:
  G1EvacuationClosures(G1CollectedHeap* g1h,
                       G1ParScanThreadState* pss,
                       bool gcs_are_young) :
      _closures(g1h, pss, gcs_are_young, /* must_claim_cld */ false) {}

  OopClosure* weak_oops()   { return &_closures._buffered_oops; }
  OopClosure* strong_oops() { return &_closures._buffered_oops; }

  CLDClosure* weak_clds()             { return &_closures._clds; }
  CLDClosure* strong_clds()           { return &_closures._clds; }
  CLDClosure* thread_root_clds()      { return NULL; }
  CLDClosure* second_pass_weak_clds() { return NULL; }

  CodeBlobClosure* strong_codeblobs()      { return &_closures._codeblobs; }
  CodeBlobClosure* weak_codeblobs()        { return &_closures._codeblobs; }

  void flush()                 { _closures._buffered_oops.done(); }
  double closure_app_seconds() { return _closures._buffered_oops.closure_app_seconds(); }

  OopClosure* raw_strong_oops() { return &_closures._oops; }

  bool trace_metadata()         { return false; }
};

// Closures used during initial mark.
// The treatment of "weak" roots is selectable through the template parameter,
// this is usually used to control unloading of classes and interned strings.
template <G1Mark MarkWeak>
class G1InitalMarkClosures : public G1EvacuationRootClosures {
  G1SharedClosures<G1MarkFromRoot> _strong;
  G1SharedClosures<MarkWeak>       _weak;

  // Filter method to help with returning the appropriate closures
  // depending on the class template parameter.
  template <G1Mark Mark, typename T>
  T* null_if(T* t) {
    if (Mark == MarkWeak) {
      return NULL;
    }
    return t;
  }

public:
  G1InitalMarkClosures(G1CollectedHeap* g1h,
                       G1ParScanThreadState* pss) :
      _strong(g1h, pss, /* process_only_dirty_klasses */ false, /* must_claim_cld */ true),
      _weak(g1h, pss,   /* process_only_dirty_klasses */ false, /* must_claim_cld */ true) {}

  OopClosure* weak_oops()   { return &_weak._buffered_oops; }
  OopClosure* strong_oops() { return &_strong._buffered_oops; }

  // If MarkWeak is G1MarkPromotedFromRoot then the weak CLDs must be processed in a second pass.
  CLDClosure* weak_clds()             { return null_if<G1MarkPromotedFromRoot>(&_weak._clds); }
  CLDClosure* strong_clds()           { return &_strong._clds; }

  // If MarkWeak is G1MarkFromRoot then all CLDs are processed by the weak and strong variants
  // return a NULL closure for the following specialized versions in that case.
  CLDClosure* thread_root_clds()      { return null_if<G1MarkFromRoot>(&_strong._clds); }
  CLDClosure* second_pass_weak_clds() { return null_if<G1MarkFromRoot>(&_weak._clds); }

  CodeBlobClosure* strong_codeblobs()      { return &_strong._codeblobs; }
  CodeBlobClosure* weak_codeblobs()        { return &_weak._codeblobs; }

  void flush() {
    _strong._buffered_oops.done();
    _weak._buffered_oops.done();
  }

  double closure_app_seconds() {
    return _strong._buffered_oops.closure_app_seconds() +
           _weak._buffered_oops.closure_app_seconds();
  }

  OopClosure* raw_strong_oops() { return &_strong._oops; }

  // If we are not marking all weak roots then we are tracing
  // which metadata is alive.
  bool trace_metadata()         { return MarkWeak == G1MarkPromotedFromRoot; }
};

G1EvacuationRootClosures* G1EvacuationRootClosures::create_root_closures(G1ParScanThreadState* pss, G1CollectedHeap* g1h) {
  if (g1h->collector_state()->during_initial_mark_pause()) {
    if (ClassUnloadingWithConcurrentMark) {
      return new G1InitalMarkClosures<G1MarkPromotedFromRoot>(g1h, pss);
    } else {
      return new G1InitalMarkClosures<G1MarkFromRoot>(g1h, pss);
    }
  } else {
    return new G1EvacuationClosures(g1h, pss, g1h->collector_state()->gcs_are_young());
  }
}
