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
#include <everett/replacement_rebuild.h>
#include <everett/sort_runtime.h>
#include <iostream>
#include <map>
#include <set>

namespace {
  using namespace everett;
  using strings = unsorted<std::optional<std::string>>;
  using engine = replacement_rebuild_engine<>;
  void check(bool value, char const * message) { if (!value) throw std::runtime_error(message); }
  template<class World>auto mass(World const & state){return state.runtime().admissions();}
  template<class World>bool same(World const & a,World const & b){return a.runtime().same_layout(b.runtime());}
  template<class F>void rejects(F && f){bool rejected=false;try{f();}catch(std::exception const&){rejected=true;}check(rejected,"expected rejection");}
  template<class World>void verify(World state,std::map<std::string,std::string> const & expected){
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
  template<class E>void quoted(E & e,typename E::contribution_type input){
    auto quote=E::reservation(input);auto before=e.work();
    e.contribute(std::move(input));auto after=e.work();
    check(after.granted-before.granted+after.foreground_charged-before.foreground_charged<=quote.work,"admission exceeded static allowance");
  }
  template <class E> void clean_insertions() {
    E active;
    std::map<std::string, std::string> expected;
    for (unsigned i = 0; i != 129; ++i) {
      auto key = "new/" + std::to_string(i);
      expected[key] = "value";
      quoted(active, E::put(key, "value"));
      auto state = active.snapshot();
      check(state.metadata().clean_base == i + 1 && !state.metadata().mutations &&
        mass(state) == i + 1, "new key did not extend clean base");
      check(!active.work().generations && !active.work().scan_records &&
        active.work().mutations == i + 1, "clean insertion rebuilt or lost work accounting");
      invariant(active);
    }
    auto clean = active.snapshot();
    verify(clean, expected);
    quoted(active, E::put("new/0", "changed")); expected["new/0"] = "changed";
    quoted(active, E::put("later", "new")); expected["later"] = "new";
    check(active.status().clean_base == 129 && active.status().mutations == 2,
      "new key erased existing mutation debt");
    verify(active.snapshot(), expected);
    check(clean.get("new/0") == "value" && !clean.get("later"), "clean insertion mutated snapshot");

    E small;
    quoted(small, E::put("a", "one")); quoted(small, E::put("b", "two"));
    quoted(small, E::put("a", "changed"));
    auto generations = small.work().generations;
    check(generations == 1 && !small.status().mutations, "small overwrite skipped cleanup");
    quoted(small, E::put("c", "three"));
    check(small.work().generations == generations && small.status().clean_base == 3,
      "post-cleanup insert rebuilt again");
    quoted(small, E::erase("b"));
    check(small.work().generations == generations + 1 && mass(small.snapshot()) == 2,
      "small delete retained history");
    quoted(small, E::put("b", "returned"));
    verify(small.snapshot(), {{"a", "changed"}, {"b", "returned"}, {"c", "three"}});
    invariant(small);
  }
  void recovery_metadata(){
    engine e;
    for(unsigned n=0;n!=128;++n)quoted(e,engine::put(std::to_string(n),"v"));
    while(e.pending())e.advance(1000000);
    quoted(e,engine::put("0","dirty"));
    auto dirty=e.snapshot();check(dirty.metadata().mutations&& !dirty.metadata().rebuilding,"inactive dirty fixture");
    auto restored=engine::world_type::restore(dirty.runtime(),engine::metadata_type::decode(dirty.metadata().encode()),dirty.metadata().schema_id);
    auto resumed=engine::from_snapshot(restored);
    check(resumed.status().clean_base==e.status().clean_base&&resumed.status().mutations==e.status().mutations,"restore reset dirty generation");
    while(!e.status().rebuilding)quoted(e,engine::put("0","changed"));
    auto active=e.snapshot();auto bytes=active.metadata().encode();
    check(bytes.size()>56&&bytes[0]==std::byte{'E'}&&bytes[7]==std::byte{0}&&bytes[8]==std::byte{1},"metadata header");
    for(std::size_t i=0;i<=56;++i)rejects([&]{(void)engine::metadata_type::decode(std::span(bytes).first(i));});
    for(auto at:{0u,7u,8u,15u,32u,39u}){auto bad=bytes;bad[at]^=std::byte{2};rejects([&]{(void)engine::metadata_type::decode(bad);});}
    auto bad=active.metadata();bad.mutations++;rejects([&]{(void)engine::world_type::restore(active.runtime(),bad,bad.schema_id);});
    bad=active.metadata();bad.rebuilding=false;rejects([&]{(void)engine::world_type::restore(active.runtime(),bad,bad.schema_id);});
    bad=active.metadata();bad.clean_base=0;bad.mutations=mass(active);rejects([&]{(void)engine::world_type::restore(active.runtime(),bad,bad.schema_id);});
    rejects([&]{(void)engine::metadata_type::decode(static_cast<engine::typed_world_type const&>(active).metadata().encode());});
    auto recovering=engine::from_snapshot(active);
    check(recovering.pending()&&!recovering.admission_ready()&&recovering.work().scan_records==0,"recovery not lazy/gated");
    rejects([&]{recovering.contribute(engine::put("forbidden","before cleanup"));});
    check(!recovering.failed()&&recovering.snapshot().metadata()==active.metadata(),"blocked admission changed recovery");
    unsigned steps=0;while(!recovering.status().clean_rows){recovering.advance(1000000);check(++steps<10000,"recovery stalled before copy");}
    auto partial=recovering.snapshot();check(partial.metadata()==active.metadata(),"partial recovery changed generation");
    auto again=engine::from_snapshot(partial);
    check(!again.admission_ready()&&again.work().scan_records==0,"interrupted recovery resumed unsupported cursor");
    steps=0;while(again.pending()){again.advance(1000000);check(++steps<10000,"recovery stalled");}
    auto clean=again.snapshot();check(!clean.metadata().rebuilding&&clean.metadata().mutations==0&&clean.metadata().clean_base==128,"recovery failed clean handoff");
    check(clean.signature()==active.signature()&&clean.get("0")=="changed"&&mass(clean)==128,"recovery changed logical state");
    quoted(again,engine::template change<strings>("0","after"));check(again.snapshot().get("0")=="after","templated change");
    auto quote=engine::reservation(engine::template put<strings>("quote","v"));
    check(quote.work<128000000&&quote.bytes>0,"default admission exceeds connection allowance");
    std::cout<<"default rebuild reservation="<<quote.work<<'\n';
  }
  void sequences(){
    engine e;std::map<std::string,std::string> expected;
    std::vector<std::pair<engine::world_type,std::map<std::string,std::string>>> saved;
    auto batch=engine::batch();
    for(unsigned n=0;n!=256;++n){auto k="key/"+std::to_string(n);batch.put(k,"initial");expected[k]="initial";}
    e.contribute(std::move(batch).finish());verify(e.snapshot(),expected);invariant(e);
    saved.emplace_back(e.snapshot(),expected);
    std::uint64_t rebuilding=0;
    for(unsigned n=0;n!=1024;++n){
      auto k="key/"+std::to_string(n%256),v="revision/"+std::to_string(n);
      quoted(e,engine::put(k,v));expected[k]=v;invariant(e);rebuilding+=e.status().rebuilding;
      if(n%127==0){verify(e.snapshot(),expected);saved.emplace_back(e.snapshot(),expected);}
      check(!e.advance(0),"zero advance publication");
    }
    check(rebuilding&&e.work().replayed,"no live FIFO replay");
    for(unsigned n=0;n!=256;++n){auto k="key/"+std::to_string(n);quoted(e,engine::erase(k));expected.erase(k);invariant(e);if(n%31==0)verify(e.snapshot(),expected);}
    verify(e.snapshot(),expected);check(mass(e.snapshot())==0,"empty world retained tombstone mass");
    for(auto const &[state,want]:saved)verify(state,want);
    auto prior=e.snapshot();auto count=e.work().mutations;
    rejects([&]{e.contribute(engine::erase("absent"));});
    check(!e.failed()&&same(e.snapshot(),prior)&&e.work().mutations==count,"absent delete changed state");
    for(unsigned n=0;n!=1024;++n){quoted(e,engine::put("one","same"));check(mass(e.snapshot())==1,"one-key obsolete universe grew");invariant(e);}
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
    using raw=engine::engine_type;
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
int main(){
  clean_insertions<engine>();
  clean_insertions<replacement_rebuild_engine<string_policy, wrapping_fingerprint_algebra, 256, sort_runtime_family<string_policy>>>();
  recovery_metadata();sequences();idle_and_stale();atomic_failure();copied_and_uncopied();native_sort_transport();
  std::cout<<"replacement rebuild tests passed\n";
}
