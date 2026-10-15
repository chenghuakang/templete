#include "hsn/traceroute.hpp"
#include <iostream>
using namespace hsn::trace;
unsigned passed=0;
void require(bool ok,const std::string&name){if(!ok)throw Error("FAIL "+name);++passed;}
Source fixed(Graph g,Ns end=100000000000LL){return{0,end,[g](Ns){return g;},[](Ns){return std::optional<Ns>{};}};}
int main(){try{
 Config c;c.serialization=false;
 auto source=fixed({3,{{0,1,1000000,40000000},{1,0,4000000,20000000},{1,2,2000000,10000000},{2,0,7000000,10000000}}});
 Engine e(source,c);auto r=e.run(0,2,0);
 require(r.reached,"destination reply reached");require(r.rows.size()==2,"stops after destination TTL");require(r.rows[0].probes.size()==3&&r.rows[1].probes.size()==3,"three probes per TTL");
 require(*r.rows[0].probes[0].rtt_ns==5000000,"asymmetric hop1 RTT");require(*r.rows[1].probes[0].rtt_ns==10000000,"distinct destination reverse route");
 std::string golden="traceroute to 10.0.0.2 (10.0.0.2), 30 hops max, 60 byte packets\n 1  10.0.0.1 (10.0.0.1)  5.000 ms  5.000 ms  5.000 ms\n 2  10.0.0.2 (10.0.0.2)  10.000 ms  10.000 ms  10.000 ms\n";
 require(e.text(r)==golden,"text-v1 exact golden");
 auto asymmetric=fixed({3,{{0,1,1000000,40000000},{1,0,1000000,20000000},{1,2,2000000,10000000}}});c.max_hops=3;Engine ea(asymmetric,c);auto a=ea.run(0,2,0);
 require(!a.reached,"destination forward arrival without reverse reply is not reached");require(ea.text(a).find(" 2  * * *\n")!=std::string::npos,"missing return route prints three stars");require(a.rows.size()==3,"continues after timeout hop");
 auto deadline=fixed({2,{{0,1,2500000000ULL,1000000},{1,0,2500000000ULL,1000000}}});Engine ed(deadline,c);auto p=ed.probe(0,1,0,1,0);require(p.status==Status::Reply&&*p.rtt_ns==5000000000ULL,"reply exactly timeout is accepted");
 auto late=fixed({2,{{0,1,2500000001ULL,1000000},{1,0,2500000000ULL,1000000}}});Engine el(late,c);p=el.probe(0,1,0,1,0);require(p.status==Status::Timeout&&!p.rtt_ns&&!p.responder,"late reply no invented RTT/IP");
 auto cropped=fixed({2,{{0,1,2000000000ULL,1000000},{1,0,2000000000ULL,1000000}}},3000000000LL);Engine ec(cropped,c);auto cr=ec.run(0,1,0);require(cr.censored&&!cr.reached,"run_end censored");require(ec.text(cr).find("*")==std::string::npos,"censored row not fake timeout");
 auto empty=fixed({2,{}});Engine ez(empty,c);auto z=ez.run(0,1,0);require(!z.reached&&z.rows.size()==3,"no forward path TTL exhaustion");require(ez.text(z).find(" 1  * * *\n")!=std::string::npos,"empty graph standard stars");
 Source varying{0,100000000000LL,[](Ns t){Graph g{3,{{0,1,1000000,10000000},{1,2,1000000,10000000},{2,0,1000000,10000000}}};if(t>=2000000)g.edges.push_back({1,0,1000000,10000000});return g;},[](Ns t)->std::optional<Ns>{if(t<2000000)return 2000000;return {};}};
 // Remove the indirect return for the first hop, while retain it for the destination test separately.
 Source mixed{0,100000000000LL,[](Ns t){Graph g{3,{{0,1,1000000,10000000},{1,2,1000000,10000000}}};if(t>=2000000){g.edges.push_back({1,0,1000000,10000000});g.edges.push_back({2,0,1000000,10000000});}return g;},[](Ns t)->std::optional<Ns>{if(t<2000000)return 2000000;return {};}};
 Engine em(mixed,c);auto mr=em.run(0,2,0);require(mr.reached,"later probes reach after topology change");require(em.text(mr).find(" 1  * 10.0.0.1 (10.0.0.1)  2.000 ms  2.000 ms\n")!=std::string::npos,"mixed timeout and reply slots");
 // Edge removed after a send completes: propagation still delivers according to edge-down-policy-v1.
 Source moving{0,100000000000LL,[](Ns t){return t<1000000?Graph{2,{{0,1,2000000,1000000},{1,0,1000000,1000000}}}:Graph{2,{{1,0,1000000,1000000}}};},[](Ns t)->std::optional<Ns>{if(t<1000000)return 1000000;return {};}};
 Engine ep(moving,c);p=ep.probe(0,1,0,1,0);require(p.status==Status::Reply&&*p.rtt_ns==3000000,"latched propagation survives removal");
 // Enable serialization and cut the edge before send completion.
 c.serialization=true;Source cut{0,100000000000LL,[](Ns t){return t<1000000?Graph{2,{{0,1,1000,1000},{1,0,1000,1000}}}:Graph{2,{{1,0,1000,1000}}};},[](Ns t)->std::optional<Ns>{if(t<1000000)return 1000000;return {};}};
 Engine es(cut,c);p=es.probe(0,1,0,1,0);require(p.status==Status::Timeout&&p.detail=="edge_down_during_send","unfinished send down -> timeout");
 auto stable=fixed({2,{{0,1,1000000,1000000},{1,0,2000000,1000000}}});Engine eser(stable,c);p=eser.probe(0,1,0,1,0);require(*p.rtt_ns==3928000,"forward 60B + return 56B serialization");
 // ICMP unreachable annotation matches traceroute(8): "addr (addr)  rtt ms !H" (also !N/!P/!S/!F/!X/!V/!C/!<num>).
 {Config c1=c;c1.probes=1;c1.max_hops=30;Engine ecode(source,c1);Result hr{};hr.src=0;hr.dst=1;hr.rows.push_back({1,{}});Probe hp;hp.status=Status::Reply;hp.responder=1;hp.rtt_ns=1234000;hp.code="!H";hr.rows[0].probes.push_back(hp);require(ecode.text(hr)=="traceroute to 10.0.0.1 (10.0.0.1), 30 hops max, 60 byte packets\n 1  10.0.0.1 (10.0.0.1)  1.234 ms !H\n","icmp unreachable code rendering");}
 require(milliseconds(999499)=="0.999"&&milliseconds(999500)=="1.000","integer ns-to-ms display rounding");
 require(ip(50)=="10.0.0.50","synthetic-v1 IP");
 auto self=eser.run(1,1,0);require(self.reached&&self.rows.size()==1&&*self.rows[0].probes[0].rtt_ns==0,"self destination zero RTT");
 // Equal-cost routing uses node-ID lexicographic order.
 require(shortest({4,{{0,2,1,1},{0,1,1,1},{1,3,1,1},{2,3,1,1}}},0,3)==std::vector<Node>{0,1,3},"deterministic path tie");
 // Source clock is independent of host waiting; identical repeated run.
 auto rerun=e.run(0,2,0);require(e.text(rerun)==golden&&rerun.finished_ns==r.finished_ns,"deterministic repeated trace");
 std::cout<<"TRACEROUTE_TESTS_PASS "<<passed<<"\n"<<golden;return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
