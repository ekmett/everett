/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Checks real redundant COLA jobs against chronological input records.
 *
 * \license
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 * \endlicense
 */
#include <diet/redundant_runtime.h>
#include <iostream>
#include <map>
#include <set>
#include <string>

namespace {
  using namespace diet;
  void check(bool value, char const * message) { if (!value) throw std::runtime_error(message); }
  template <class E = std::logic_error, class F> void rejects(F && f) {
    bool caught = false; try { f(); } catch (E const &) { caught = true; } check(caught, "expected rejection");
  }
  std::string bits(bit_view v) { std::string out; for (std::uint64_t i=0;i<v.size();++i) out += v.at(i)?'1':'0'; return out; }
  bit_string key(unsigned n) { return bit_string::from_bytes(std::string{char(n>>8),char(n)}); }
  profile_record row(unsigned k,unsigned v) { return {key(k),key(v)}; }
  template <class P> std::vector<std::string> values(redundant_snapshot<P> const & snapshot,unsigned k) {
    auto encoded=key(k); auto cursor=snapshot.cursor(encoded.view()); std::vector<std::string> out;
    for(unsigned steps=0;!cursor.done();++steps) {
      check(steps<10000,"query stalled"); cursor.step(1);
      if(cursor.has_match()){auto value=cursor.take_match();out.push_back(bits(value.value.view()));}
    }
    return out;
  }
  template <class P> void topology(redundant_snapshot<P> const & snapshot) {
    auto const & frontier=snapshot.frontier();
    std::map<std::uint64_t,redundant_object<P> const *> slots;
    for(unsigned i=0;i<frontier.levels.size();++i) {
      unsigned active=0,carriers=0,staging=0;
      for(auto const & slot:frontier.levels[i].slots) {
        if(slot.object) check(slots.emplace(slot.object->identity,slot.object.get()).second,"object in multiple slots");
        active+=slot.state==redundant_slot_state::active;
        carriers+=slot.state==redundant_slot_state::carrier_ready||slot.state==redundant_slot_state::root_carrier;
        staging+=slot.state==redundant_slot_state::reserved||slot.state==redundant_slot_state::carrier_building;
        if(slot.state==redundant_slot_state::active) check(slot.object->mass()==(std::uint64_t{1}<<i),"active mass");
      }
      check(active<=2&&carriers<=1&&staging<=1,"slot population");
    }
    std::set<std::uint64_t> closure;
    auto walk=[&](auto&& self,auto const & object)->void {
      if(!object||!closure.insert(object->identity).second)return;
      check(slots.contains(object->identity),"live dependency escaped slot closure");
      check(object->native->size()<=object->mass()||!object->mass(),"native exceeds mass");
      check(!object->secondary()||(!object->next.main&&!object->next.secondary),"recursive secondary");
      if(object->next.main)check(object->next.main->level==object->level+1,"main skips level");
      if(object->next.secondary)check(object->next.secondary->level==object->level+1&&object->next.secondary->secondary(),"secondary route role");
      self(self,object->next.main);self(self,object->next.secondary);
    };
    walk(walk,frontier.root.main);walk(walk,frontier.root.secondary);
    for(auto const & level:frontier.levels) {
      for(auto const & slot:level.slots){walk(walk,slot.object);walk(walk,slot.route.main);walk(walk,slot.route.secondary);}
      if(level.job){auto const & j=*level.job;walk(walk,j.existing_main);walk(walk,j.output);walk(walk,j.destination_route.main);walk(walk,j.destination_route.secondary);}
    }
    check(closure.size()<=3*frontier.levels.size(),"current object bound");
    std::uint64_t next=0;
    for(auto const & run:snapshot.runs()){check(run->first==next&&run->last>next,"chronological coverage");next=run->last;}
    check(next==snapshot.admissions(),"omitted admissions");
  }
  template <class R> void drain(R & runtime,std::uint64_t budget) {
    unsigned steps=0;while(runtime.pending()){check(++steps<1000000,"service stalled");runtime.advance(budget);}
  }
  template <class P> void scenario(unsigned count) {
    redundant_runtime<P> runtime; std::map<unsigned,std::string> oracle;
    auto empty=runtime.snapshot();
    std::vector<std::pair<redundant_snapshot<P>,std::map<unsigned,std::string>>> old;
    for(unsigned n=0;n<count;++n) {
      auto r=row(n%37,n); auto before=runtime.work().charged;
      if(n<16) {
        while(!runtime.admission_ready())runtime.advance(1);
        auto quote=runtime.admission_cost(); check(bool(runtime.try_contribute(r)),"ready rejected");
        check(runtime.work().charged-before>=quote,"admission undercharged");
        if(runtime.pending()) {
          auto state=runtime.snapshot(); auto prior=runtime.work().charged;
          check(!runtime.try_contribute(row(999,0)),"under-serviced admission accepted");
          check(state.same_layout(runtime.snapshot())&&prior==runtime.work().charged,"refusal mutated state");
          check(state.same_layout(runtime.advance(0)),"zero service changed checkpoint");
          auto fork=redundant_runtime<P>::from_snapshot(state);drain(fork,3);topology(fork.snapshot());
        }
      } else runtime.contribute(r);
      oracle[n%37]=bits(r.value.view());
      auto snapshot=runtime.snapshot();topology(snapshot);
      for(unsigned k=0;k<37;++k) {
        auto found=values(snapshot,k);auto want=oracle.find(k);
        check(found.empty()==(want==oracle.end()),"query presence");
        if(!found.empty())check(found.front()==want->second,"query chronology");
      }
      if(n%127==0)old.emplace_back(snapshot,oracle);
      auto w=runtime.work();check(w.charged==w.native_work+w.index_work+w.carrier_work+w.metadata_work+w.root_work,"charge sum");
      check(w.granted>=w.charged,"unfunded work");
    }
    drain(runtime,257);topology(runtime.snapshot());
    check(runtime.snapshot().admissions()==count,"admission mass lost");
    check(runtime.work().merges&&runtime.work().carriers&&runtime.work().indexes,"no real jobs");
    for(auto const & [snapshot,expected]:old)for(auto const & [k,v]:expected){auto got=values(snapshot,k);check(!got.empty()&&got.front()==v,"old snapshot mutated");}
    check(values(empty,0).empty(),"empty snapshot mutated");
    std::cout<<P::group_size<<":"<<count<<" merges="<<runtime.work().merges<<" bound="<<runtime.work().max_job_charge_per_mass<<"\n";
  }
  template<class P>void restart_stages() {
    redundant_runtime<P> runtime;
    auto empty=runtime.snapshot();runtime.try_contribute(row(1,11));runtime.try_contribute(row(2,22));
    unsigned stages=0,calls=0;
    while(runtime.pending()) {
      check(++calls<10000,"stage walk stalled");
      auto snapshot=runtime.checkpoint();topology(snapshot);
      if(!runtime.pending())break;
      for(auto const&level:snapshot.frontier().levels)if(level.job)stages|=1u<<static_cast<unsigned>(level.job->stage);
      auto restored=redundant_snapshot<P>::restore(snapshot.frontier(),snapshot.query_root().head());
      auto fork=redundant_runtime<P>::from_snapshot(restored);
      check(fork.credit()==0&&fork.recovering()&&!fork.admission_ready(),"restart bypassed recovery");
      auto old=fork.snapshot();check(!fork.try_contribute(row(3,33)),"recovery admitted");
      check(old.same_layout(fork.snapshot()),"recovery refusal changed layout");
      drain(fork,3);check(fork.admission_ready(),"recovery did not clear");
      for(unsigned k:{1u,2u}){auto want=row(k,k*11);auto got=values(fork.snapshot(),k);check(!got.empty()&&got.front()==bits(want.value.view()),"stage restart query");}
      auto due=runtime.service_due(),cost=runtime.next_service_cost(),credit=runtime.credit();
      auto budget=cost>credit?cost-credit:1;
      runtime.advance(budget);
      if(runtime.pending())check(runtime.service_due()==due-std::min(due,budget),"service obligation changed without offer");
      else check(runtime.service_due()==0,"settled obligation retained");
    }
    check(stages==15,"restart fixture missed stage");
    auto snapshot=runtime.checkpoint();auto bad=snapshot.frontier();bad.next_identity=0;
    rejects<std::invalid_argument>([&]{(void)redundant_snapshot<P>::restore(bad,snapshot.query_root().head());});
    bad=snapshot.frontier();bad.service_due=std::numeric_limits<std::uint64_t>::max();
    rejects<std::invalid_argument>([&]{(void)redundant_snapshot<P>::restore(bad,snapshot.query_root().head());});
    rejects<std::invalid_argument>([&]{(void)redundant_snapshot<P>::restore(snapshot.frontier(),empty.query_root().head());});
  }
  template<class P>void budget_fuzz() {
    redundant_runtime<P> runtime;
    std::vector<profile_record> input;
    std::vector<redundant_snapshot<P>> retained;
    std::uint64_t random=0x715e1fd53b43a291ULL;
    auto draw=[&]{random^=random<<13;random^=random>>7;random^=random<<17;return random;};
    auto oracle=[&](redundant_snapshot<P> const & state){
      topology(state);
      for(unsigned k=0;k!=11;++k){
        std::vector<std::string> expected;
        auto runs=state.runs();
        for(auto run=runs.rbegin();run!=runs.rend();++run){
          for(auto n=(*run)->last;n!=(*run)->first;){--n;if(n%11==k){expected.push_back(bits(input[n].value.view()));break;}}
        }
        check(values(state,k)==expected,"ordered run oracle");
      }
      auto p=state.query_root().head();std::uint64_t depth=0;
      for(;p;p=p->main_target())++depth;
      check(depth==state.query_root().head()->depth(),"cached main depth");
    };
    for(unsigned n=0;n!=192;++n){
      unsigned calls=0;
      while(!runtime.admission_ready()){
        check(++calls<100000,"random service stalled");
        auto before=runtime.snapshot();auto charged=runtime.work().charged,credit=runtime.credit(),due=runtime.service_due();
        check(!runtime.try_contribute(row(999,999)),"random refusal accepted");
        check(before.same_layout(runtime.snapshot())&&charged==runtime.work().charged,"random refusal mutated");
        auto cost=runtime.next_service_cost();auto shortfall=cost>credit?cost-credit:0;
        std::array<std::uint64_t,6> choices{0,shortfall?shortfall-1:0,shortfall,shortfall+1,due/3,due};
        auto amount=choices[draw()%choices.size()];runtime.advance(amount);
        check(runtime.work().charged-charged<=credit+amount,"service overspent credit");
        check(runtime.service_due()==(runtime.pending()?due-std::min(due,amount):0),"random service obligation");
        if(calls%7==0){auto state=runtime.checkpoint();oracle(state);retained.push_back(state);}
      }
      input.push_back(row(n%11,n));check(bool(runtime.try_contribute(input.back())),"random ready rejected");
      oracle(runtime.snapshot());
    }
    drain(runtime,31);oracle(runtime.snapshot());
    for(auto const & state:retained)oracle(state);
    auto snapshot=runtime.checkpoint();auto bad=snapshot.frontier();
    bool injected=false;
    for(auto & level:bad.levels)for(auto & slot:level.slots)if(!injected&&slot.state==redundant_slot_state::empty){
      slot.state=redundant_slot_state::reserved;injected=true;
    }
    check(injected,"no orphan reservation fixture");
    rejects<std::invalid_argument>([&]{(void)redundant_snapshot<P>::restore(bad,snapshot.query_root().head());});
  }
  struct fail_compose {
    bit_string operator()(bit_view,bit_view,bit_view) const {throw std::runtime_error("composition failure");}
  };
  template<class P>void failures(){
    redundant_runtime<P,fail_compose> runtime;
    auto first=row(1,11);runtime.try_contribute(first);auto prior=runtime.snapshot();
    rejects<std::runtime_error>([&]{runtime.try_contribute(row(1,22),runtime.service_budget(2));});
    check(runtime.failed()&&runtime.snapshot().same_layout(prior),"failed admission published");
    check(values(runtime.snapshot(),1)==std::vector<std::string>{bits(first.value.view())},"failed admission changed values");
    rejects([&]{runtime.advance(1);});rejects([&]{runtime.checkpoint();});rejects([&]{runtime.try_contribute(first);});
    auto moved=std::move(runtime);rejects([&]{(void)runtime.snapshot();});check(moved.failed(),"failed move lost poison");
    auto fork=redundant_runtime<P>::from_snapshot(prior);auto replacement=row(1,33);fork.contribute(replacement);
    check(values(fork.snapshot(),1).front()==bits(replacement.value.view()),"failed-admission restart");
    redundant_runtime<P,fail_compose> later;
    later.try_contribute(first);later.try_contribute(row(1,22));auto admitted=later.snapshot();
    rejects<std::runtime_error>([&]{later.advance(later.service_budget(2));});
    check(later.failed()&&later.snapshot().same_layout(admitted),"failed service changed publication");
    check(values(admitted,1).size()==2,"failed service lost source owners");
  }
  struct append {
    bit_string operator()(bit_view,bit_view older,bit_view newer) const {return bit_string::from_bits(bits(older)+bits(newer));}
  };
  template<class P>void composed() {
    redundant_runtime<P,append> runtime;std::map<unsigned,std::string> oracle;
    for(unsigned n=0;n<128;++n){auto r=row(n%3,n);runtime.contribute(r);oracle[n%3]+=bits(r.value.view());
      for(auto const&[k,v]:oracle){auto got=values(runtime.snapshot(),k);std::string actual;for(auto p=got.rbegin();p!=got.rend();++p)actual+=*p;check(actual==v,"noncommutative composition");}}
    drain(runtime,1);
  }
}
int main(){
  using bit=diet::storage_policy<diet::tip<diet::encoded_sort<diet::bit_encoding<>>>,3,diet::golomb<3>,5>;
  using byte=diet::storage_policy<diet::tip<diet::encoded_sort<diet::byte_encoding<>>>,15,diet::exponential_golomb<0>,4>;
  scenario<bit>(512);scenario<byte>(512);composed<bit>();composed<byte>();restart_stages<bit>();restart_stages<byte>();
  budget_fuzz<bit>();budget_fuzz<byte>();failures<bit>();failures<byte>();
  static_assert(std::is_same_v<redundant_runtime_family<bit>::runtime_type<append>,redundant_runtime<bit,append>>);
  std::cout<<"redundant runtime tests passed\n";
}
