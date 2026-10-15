#pragma once
#include "hsn/traceroute.hpp"
#include "../../satpred/src/core.cpp"
namespace fixedtrace {
using namespace sp;
struct Package{fs::path graph,orbit;U k,M;I t0,step,start,end;};
fs::path relative_path(const fs::path&base,const std::string&s){auto p=fs::u8path(s);need(!p.is_absolute(),7,"path","absolute product path");for(auto&part:p)need(part!="..",7,"path","path escape");auto root=fs::weakly_canonical(base),resolved=fs::weakly_canonical(base/p);auto rel=resolved.lexically_relative(root);need(!rel.empty()&&*rel.begin()!="..",7,"path","symlink escape");return resolved;}
class Input{
    std::vector<Package>packages;U budget;Graph current;size_t current_package=SIZE_MAX;
public:
    I begin=0,end=0,step=0;J nodes;
    // Upstream v3.5 optional annotation keys (case_name, rf_parameters). Pass-through only: never a link-model input.
    J upstream_case=J::obj();
    // Canonical provenance (SDD §11/13.2): upstream run_id reference, actual backend, input manifest SHA.
    std::string upstream_run_id,actual_backend,input_manifest_sha256;
    Input(const fs::path&input,U memory):budget(memory){
        fs::path manifest=fs::is_directory(input)?input/"manifest.json":input;J entries=J::arr();
        if(input.extension()==".ngb"){
            auto stem=input.stem();stem+=".sbin";fs::path orbit;
            for(auto p:{input.parent_path()/"orbits"/stem,input.parent_path()/"orbits/samples"/stem,input.parent_path().parent_path()/"orbits"/stem,input.parent_path().parent_path()/"orbits/samples"/stem})if(fs::exists(p)){orbit=p;break;}
            need(!orbit.empty(),7,"source","bound orbit not found");packages.push_back({input,orbit,0,0,0,0,0,0});
            input_manifest_sha256=hex(filesha(input));
        }else{
            auto m=json(readtext(manifest));need(m.at("format").str()=="SATNET-1.1",7,"manifest","format");entries=m.at("products");need(entries.type==J::Arr&&!entries.a.empty(),7,"manifest","empty products");
            input_manifest_sha256=hex(filesha(manifest));
            if(m.o.count("run")&&m.at("run").o.count("run_id"))upstream_run_id=m.at("run").at("run_id").str();
            for(auto&e:entries.a){auto g=relative_path(manifest.parent_path(),e.at("graph_file").str()),o=relative_path(manifest.parent_path(),e.at("source_file").str());need(g.extension()==".ngb"&&o.extension()==".sbin",7,"path","unexpected product extension");need(hex(filesha(g))==e.at("graph_sha256").str()&&hex(filesha(o))==e.at("source_sha256").str(),7,"manifest","SHA mismatch");packages.push_back({g,o,0,0,0,0,0,0});}
        }
        for(size_t i=0;i<packages.size();++i){auto&p=packages[i];Graph g=graphread(p.graph,budget/3,p.orbit);Orbit o=readorbit(p.orbit,budget/3);need(get(g.b,10,2)==1&&get(g.b,16,4)==0,7,"graph","strict bound minor1 required");
            p.k=g.k0;p.M=g.M;p.t0=g.t0;p.step=g.step;p.start=o.start;p.end=o.end;
            if(i==0){begin=o.start;end=o.end;step=o.step;nodes=g.meta.at("nodes");for(auto key:{"case_name","rf_parameters"})if(g.meta.o.count(key))upstream_case[key]=g.meta.at(key);
                if(upstream_run_id.empty()&&g.meta.o.count("run_id"))upstream_run_id=g.meta.at("run_id").str();
                actual_backend=g.meta.o.count("actual_backend")?g.meta.at("actual_backend").str():"cpu";
            }
            else for(auto key:{"case_name","rf_parameters"})need((g.meta.o.count(key)>0)==(upstream_case.o.count(key)>0)&&(!g.meta.o.count(key)||g.meta.at(key).dump()==upstream_case.at(key).dump()),7,"packages","upstream case annotation mismatch across packages");
            need(o.start==begin&&o.end==end&&o.step==step&&g.meta.at("nodes").dump()==nodes.dump(),7,"packages","nodes/time mismatch");
        }
        need(nodes.a.size()<=(1U<<24),2,"IP","synthetic-v1 address collision above 2^24 nodes");
        std::sort(packages.begin(),packages.end(),[](auto&a,auto&b){return a.k<b.k;});U k=packages.front().k;I first=packages.front().t0;
        for(auto&p:packages){need(p.k==k&&p.t0==first+I(k-packages.front().k)*step,7,"packages","gap/overlap");k+=p.M;}
        if(input.extension()==".ngb"){begin=first;end=std::min(end,first+I(packages.front().M)*step);}else need(packages.front().k==0&&k==U((end-begin+step-1)/step),7,"packages","incomplete run");
    }
    hsn::trace::Graph at(I t){need(t>=begin&&t<end,1,"time","out of input coverage");auto it=std::upper_bound(packages.begin(),packages.end(),t,[](I t,const Package&p){return t<p.t0;});need(it!=packages.begin(),7,"time","missing package");size_t ix=size_t(it-packages.begin()-1);auto&p=packages[ix];
        if(ix!=current_package){current=graphread(p.graph,budget/3);current_package=ix;}
        U j=U((t-p.t0)/step);need(j<p.M,7,"time","gap");hsn::trace::Graph g{static_cast<std::uint32_t>(nodes.a.size()),{}};
        for(auto&e:current.at(j)){g.edges.push_back({e.src,e.dst,mul(e.delay,1000),e.bw});}
        return g;
    }
    std::optional<I> next(I t){if(t<begin||t>=end)return {};I n=begin+((t-begin)/step+1)*step;return n<end?std::optional<I>(n):std::nullopt;}
    U node_id(const std::string&value,int kind){if(value=="default"){for(auto&n:nodes.a)if(n.at("kind").u()==U(kind))return n.at("id").u();fail(1,"node","default endpoint kind not found; specify --src/--dst");}U id=sp::uint(value);need(id<nodes.a.size(),1,"node","id outside graph");return id;}
};
J encode(const hsn::trace::Result&r){J j=J::obj();j["format"]="hsn-traceroute-text-v1";j["src"]=U(r.src);j["dst"]=U(r.dst);j["issued_ns"]=r.issued_ns;j["finished_ns"]=r.finished_ns;j["reached"]=r.reached;j["censored"]=r.censored;j["verification"]="SOURCE_VERIFIED";j["model"]="ttl-hop-icmp-directed-return-no-queue-v1";j["rows"]=J::arr();
    for(auto&row:r.rows){J hop=J::obj();hop["ttl"]=U(row.ttl);hop["probes"]=J::arr();for(auto&p:row.probes){J probe=J::obj();probe["index"]=U(p.index);probe["sent_ns"]=p.sent_ns;probe["finished_ns"]=p.finished_ns;probe["status"]=p.status==hsn::trace::Status::Reply?"reply":p.status==hsn::trace::Status::Timeout?"timeout":"censored";probe["responder"]=p.responder?J(U(*p.responder)):J();probe["rtt_ns"]=p.rtt_ns?J(*p.rtt_ns):J();probe["destination_reply"]=p.destination_reply;probe["detail"]=p.detail;probe["code"]=p.code.empty()?J():J(p.code);hop["probes"].a.push_back(probe);}j["rows"].a.push_back(hop);}return j;
}
}
