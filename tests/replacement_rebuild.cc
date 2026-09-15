/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks strong replacement cleanup against an independent live map.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <diet/replacement_rebuild.h>
#include <diet/sort_runtime.h>
#include <iostream>
#include <map>
#include <set>

namespace {
  using namespace diet;
  using strings = unsorted<std::optional<std::string>>;
  using engine = replacement_rebuild_engine<>;
  void check(bool value, char const * message) { if (!value) throw std::runtime_error(message); }
  template<class Cola>auto mass(Cola const & state){return state.runtime().admissions();}
  template<class Cola>bool same(Cola const & a,Cola const & b){return a.runtime().same_layout(b.runtime());}
  template<class F>void rejects(F && f){bool rejected=false;try{f();}catch(std::exception const&){rejected=true;}check(rejected,"expected rejection");}
  template<class Cola>void verify(Cola state,std::map<std::string,std::string> const & expected){
    check(state.metadata().live_count==expected.size(),"live count");
    std::uint64_t hash=0;
    for(auto const &[k,v]:expected){check(state.get(k)==v,"point query");hash+=sort_semantics<strings>::hash_key(k)*sort_semantics<strings>::hash_value(k,v);}
    check(state.metadata().signature==hash,"independent fingerprint");
    auto rows=scan(state);auto at=expected.begin();
    while(!rows.done()){
      rows.step(1);if(rows.has_row()){auto row=rows.take_row();check(at!=expected.end()&&row.key==at->first&&row.value==at->second,"resolved scan");++at;}
    }
    check(at==expected.end(),"scan omitted rows");
  }
  template<class E>void invariant(E const & e){
    auto status=e.status();check(mass(e.snapshot())==status.clean_base+status.mutations,"generation mass");
    if(status.rebuilding){
      check(status.admitted<status.horizon,"handoff deadline");
      auto limit=status.initial_bound+status.admitted*status.action_bound;
      check(status.committed<=limit,"reserved bound");
      auto funded=status.committed+status.credit, scheduled=status.admitted*status.quantum;
      auto remainder=limit>funded?limit-funded:0;
      check(remainder<=(status.initial_bound>scheduled?status.initial_bound-scheduled:0),"funded-debt inequality");
    }
    auto w=e.work();check(w.committed<=w.granted,"unfunded background work");
  }
  void sequences(){
    engine e;std::map<std::string,std::string> expected;
    std::vector<std::pair<engine::cola_type,std::map<std::string,std::string>>> saved;
    auto batch=engine::batch();
    for(unsigned n=0;n!=256;++n){auto k="key/"+std::to_string(n);batch.put(k,"initial");expected[k]="initial";}
    e.contribute(std::move(batch).finish());verify(e.snapshot(),expected);invariant(e);
    saved.emplace_back(e.snapshot(),expected);
    std::uint64_t rebuilding=0;
    for(unsigned n=0;n!=1024;++n){
      auto k="key/"+std::to_string(n%256),v="revision/"+std::to_string(n);
      e.contribute(engine::put(k,v));expected[k]=v;invariant(e);rebuilding+=e.status().rebuilding;
      if(n%127==0){verify(e.snapshot(),expected);saved.emplace_back(e.snapshot(),expected);}
      check(!e.advance(0),"zero advance publication");
    }
    check(rebuilding&&e.work().replayed,"no live FIFO replay");
    for(unsigned n=0;n!=256;++n){auto k="key/"+std::to_string(n);e.contribute(engine::erase(k));expected.erase(k);invariant(e);if(n%31==0)verify(e.snapshot(),expected);}
    verify(e.snapshot(),expected);check(mass(e.snapshot())==0,"empty cola retained tombstone mass");
    for(auto const &[state,want]:saved)verify(state,want);
    auto prior=e.snapshot();auto count=e.work().mutations;
    rejects([&]{e.contribute(engine::erase("absent"));});
    check(!e.failed()&&same(e.snapshot(),prior)&&e.work().mutations==count,"absent delete changed state");
    for(unsigned n=0;n!=1024;++n){e.contribute(engine::put("one","same"));check(mass(e.snapshot())==1,"one-key obsolete universe grew");invariant(e);}
    check(e.work().mutations==count+1024,"unchanged mutations disappeared");
    auto copy=engine::from_clean(e.snapshot());verify(copy.snapshot(),{{"one","same"}});
    copy.contribute(engine::put("two","different"));verify(e.snapshot(),{{"one","same"}});
    std::cout<<"generations="<<e.work().generations<<" replayed="<<e.work().replayed<<" max_handoff="<<e.work().maximum_handoff_mutations<<'\n';
  }
  struct explosive : sort_semantics<strings> {
    using encoding=bit_encoding<>;
    using key_codec=sort_codec<strings>::key_codec;
    using value_codec=sort_codec<strings>::value_codec;
    inline static bool reject_clean=false;
    static state_type clean(std::string const & key,state_type const & value){
      if(reject_clean&&key=="b")throw std::runtime_error("clean callback failure");
      return value;
    }
  };
  void atomic_failure(){
    using P=storage_policy<bin<tip<explosive>,sort_undefined>,3,exponential_golomb<0>,5>;
    using E=replacement_rebuild_engine<P>;
    E e("explosive/1");auto old=e.snapshot();auto batch=E::batch();batch.put("a","one").put("b","two");
    explosive::reject_clean=true;rejects([&]{e.contribute(std::move(batch).finish());});explosive::reject_clean=false;
    check(e.failed()&&same(e.snapshot(),old),"half batch published after callback failure");
    check(e.snapshot().metadata().live_count==0&&!old.get("a"),"failed batch changed logical state");
    rejects([&]{e.advance(1000000);});rejects([&]{e.contribute(E::put("c","three"));});
    auto moved=std::move(e);rejects([&]{(void)e.snapshot();});check(moved.failed(),"failed move lost poison");
    auto recovered=E::from_clean(old);recovered.contribute(E::put("c","three"));check(recovered.snapshot().get("c")=="three","old snapshot recovery");
    using raw=typed_engine<string_policy,wrapping_fingerprint_algebra,256,redundant_runtime_family<string_policy>>;
    raw dirty;dirty.contribute(raw::put("k","one"));dirty.contribute(raw::put("k","two"));
    rejects([&]{(void)engine::from_clean(dirty.snapshot());});
  }
  void copied_and_uncopied(){
    engine e;std::map<std::string,std::string> expected;
    for(unsigned n=0;n!=256;++n){auto k=std::string("k")+char(n/16+'a')+char(n%16+'a');e.contribute(engine::put(k,"old"));expected[k]="old";}
    while(e.status().rebuilding)e.advance(e.status().action_bound);
    while(!e.status().rebuilding){auto scanned=e.work().scan_records;e.contribute(engine::put("kaa","old"));if(e.status().rebuilding)check(e.work().scan_records==scanned,"freeze eagerly scanned source");}
    auto frozen=e.snapshot();auto status=e.status();auto scanned=e.work().scan_records;
    check(status.source_records>=status.frozen_live,"source physical count");
    while(!e.status().clean_rows)e.advance(status.action_bound);
    check(e.status().clean_rows<status.frozen_live,"copy boundary fixture finished early");
    e.contribute(engine::put("kaa","copied"));expected["kaa"]="copied";
    e.contribute(engine::put("kpp","uncopied"));expected["kpp"]="uncopied";
    auto replay_bound=e.status().horizon;
    while(e.pending())e.advance(status.action_bound+1);
    verify(e.snapshot(),expected);
    check(e.work().scan_records-scanned==status.source_records,"scan skipped obsolete physical occurrences");
    check(e.status().mutations==2&&e.status().clean_base==256&&replay_bound>=2,"replay debt reset");
    check(frozen.get("kaa")=="old"&&frozen.get("kpp")=="old","frozen table mutated");
    std::set<void const*> old_natives;
    for(auto const & run:frozen.runtime().runs())old_natives.insert(run->native.get());
    auto current=e.snapshot();
    for(auto const & run:current.runtime().runs())check(!old_natives.contains(run->native.get()),"clean graph retained source native");
  }
  void native_sort_transport(){
    using E=replacement_rebuild_engine<string_policy,wrapping_fingerprint_algebra,256,sort_runtime_family<string_policy>>;
    E e;std::map<std::string,std::string> expected;
    for(unsigned n=0;n!=160;++n){auto key=std::string("raw\0key/",8)+std::to_string(n);e.contribute(E::put(key,"initial"));expected[key]="initial";}
    for(unsigned n=0;n!=480;++n){auto key=std::string("raw\0key/",8)+std::to_string(n%160);
      if(n%7==0&&expected.contains(key)){e.contribute(E::erase(key));expected.erase(key);}
      else {auto value=std::string(n%13,char(n));e.contribute(E::put(key,value));expected[key]=value;}
      invariant(e);if(n%79==0)verify(e.snapshot(),expected);
    }
    while(e.pending())e.advance(1000000);verify(e.snapshot(),expected);
    check(e.work().generations&&e.work().replayed,"sort transport did not rebuild/replay");
  }
  void idle_and_stale(){
    engine e;std::map<std::string,std::string> expected;
    for(unsigned n=0;n!=128;++n){auto k=std::to_string(n);e.contribute(engine::put(k,"v"));expected[k]="v";}
    while(!e.status().rebuilding){e.contribute(engine::put("0","v"));}
    auto old=e.snapshot();auto status=e.status();
    check(status.frozen_live>=64,"large rebuild fixture");
    unsigned calls=0;while(e.pending()){check(++calls<100000,"idle work stalled");e.advance(status.action_bound/3+1);invariant(e);}
    verify(e.snapshot(),expected);verify(old,expected);
    auto stale=e.snapshot().put("0","stale");e.contribute(engine::put("0","fresh"));expected["0"]="fresh";
    auto before=e.snapshot();rejects([&]{e.contribute(std::move(stale));});
    check(!e.failed()&&same(before,e.snapshot()),"stale batch mutated");verify(e.snapshot(),expected);
  }
}
int main(){sequences();idle_and_stale();atomic_failure();copied_and_uncopied();native_sort_transport();std::cout<<"replacement rebuild tests passed\n";}
