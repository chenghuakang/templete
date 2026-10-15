#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
namespace hsn::trace {
using Ns=std::int64_t;using U=std::uint64_t;using Node=std::uint32_t;
struct Error:std::runtime_error{using std::runtime_error::runtime_error;};
inline U add(U a,U b){if(b>UINT64_MAX-a)throw Error("integer overflow");return a+b;}
inline Ns after(Ns t,U d){if(t<0||d>U(INT64_MAX-t))throw Error("time overflow");return t+Ns(d);}
struct Link{Node src,dst;U delay_ns,capacity_bps;};
struct Graph{Node node_count;std::vector<Link>edges;};
struct Source{
    Ns begin,end;
    std::function<Graph(Ns)> at;
    std::function<std::optional<Ns>(Ns)> next_transition;
};
struct Config{unsigned max_hops=30,probes=3;U timeout_ns=5000000000ULL;unsigned packet_bytes=60,icmp_bytes=56;bool serialization=true;};
enum class Status{Reply,Timeout,Censored};
struct Probe{Status status=Status::Timeout;unsigned ttl=0,index=0;Ns sent_ns=0,finished_ns=0;std::optional<Node>responder;std::optional<U>rtt_ns;bool destination_reply=false;std::string detail,code;};
struct Row{unsigned ttl;std::vector<Probe>probes;};
struct Result{Node src,dst;Ns issued_ns,finished_ns;bool reached=false,censored=false;std::vector<Row>rows;};
inline std::string ip(Node id){return "10."+std::to_string((id>>16)&255)+"."+std::to_string((id>>8)&255)+"."+std::to_string(id&255);}
inline std::string milliseconds(U ns){U rounded=ns/1000;U rem=ns%1000;if(rem>=500)++rounded;std::ostringstream s;s<<rounded/1000<<'.'<<std::setfill('0')<<std::setw(3)<<rounded%1000;return s.str();}
inline std::optional<Link> edge(const Graph&g,Node a,Node b){for(auto&e:g.edges)if(e.src==a&&e.dst==b)return e;return {};}
// Positive integer-delay weights. Lexicographic full node path resolves equal costs.
inline std::vector<Node> shortest(const Graph&g,Node src,Node dst){
    if(src>=g.node_count||dst>=g.node_count)throw Error("node outside graph");
    std::vector<U>d(g.node_count,UINT64_MAX);std::vector<std::vector<Node>>p(g.node_count);std::vector<bool>done(g.node_count,false);d[src]=0;p[src]={src};
    for(Node k=0;k<g.node_count;++k){Node v=g.node_count;for(Node i=0;i<g.node_count;++i)if(!done[i]&&d[i]!=UINT64_MAX&&(v==g.node_count||d[i]<d[v]||(d[i]==d[v]&&p[i]<p[v])))v=i;
        if(v==g.node_count||v==dst){break;}
        done[v]=true;
        for(auto&e:g.edges)if(e.src==v){if(e.delay_ns==0||e.capacity_bps==0||e.dst>=g.node_count)throw Error("invalid link");U nd=add(d[v],e.delay_ns);auto path=p[v];path.push_back(e.dst);if(nd<d[e.dst]||(nd==d[e.dst]&&(p[e.dst].empty()||path<p[e.dst]))){d[e.dst]=nd;p[e.dst]=std::move(path);}}
    }return p[dst];
}
class Engine{
    const Source& source;Config cfg;
    struct Transit{bool arrived=false,censored=false;Ns time=0;std::string detail;};
    // A completed send latches its propagation delay. Down during propagation does not destroy it.
    Transit transit(Node a,Node b,Ns t,unsigned bytes,Ns deadline)const{
        U remaining=U(bytes)*8*1000000000ULL;
        auto current=edge(source.at(t),a,b);if(!current)return{false,false,t,"edge_down_before_send"};
        if(cfg.serialization){
            while(remaining){
                U rate=current->capacity_bps;U dt=remaining/rate+(remaining%rate!=0);Ns complete=after(t,dt);
                auto change=source.next_transition(t);Ns boundary=std::min(source.end,deadline);
                if(change&&*change<complete&&*change<=boundary){
                    if(*change<=t)throw Error("non-increasing topology transition");
                    U elapsed=U(*change-t);
                    // elapsed*rate < remaining because this boundary precedes ceil completion.
                    if(elapsed>remaining/rate)throw Error("service arithmetic invariant");
                    remaining-=elapsed*rate;t=*change;
                    current=edge(source.at(t),a,b);if(!current)return{false,false,t,"edge_down_during_send"};continue;
                }
                if(complete>deadline)return{false,false,deadline,"serialization_timeout"};
                if(complete>=source.end)return{false,true,source.end,"run_end_during_send"};
                t=complete;remaining=0;
            }
        }
        Ns arrival=after(t,current->delay_ns);
        if(arrival>deadline)return{false,false,deadline,"propagation_timeout"};
        if(arrival>=source.end)return{false,true,source.end,"run_end_during_propagation"};
        return{true,false,arrival,""};
    }
    Probe fail(Probe p,const std::string&why,Ns deadline)const{
        p.detail=why;
        if(deadline>=source.end){p.status=Status::Censored;p.finished_ns=source.end;}
        else{p.status=Status::Timeout;p.finished_ns=deadline;}
        return p;
    }
public:
    Engine(const Source&s,Config c={}):source(s),cfg(c){
        if(cfg.max_hops<1||cfg.max_hops>32||cfg.probes<1||cfg.probes>3||cfg.timeout_ns==0||cfg.packet_bytes<28||cfg.packet_bytes>9000||cfg.icmp_bytes<28||cfg.icmp_bytes>9000)throw Error("invalid traceroute configuration");
    }
    Probe probe(Node src,Node dst,Ns sent,unsigned ttl,unsigned index)const{
        if(sent<source.begin||sent>=source.end)throw Error("probe outside source coverage");
        if(ttl<1||ttl>cfg.max_hops||index>=cfg.probes)throw Error("invalid probe TTL/index");
        Probe p;p.sent_ns=sent;p.finished_ns=sent;p.ttl=ttl;p.index=index;Ns deadline=after(sent,cfg.timeout_ns),t=sent;Node at=src;unsigned remaining=ttl;Node responder=src;bool destination=false;
        if(src==dst){p.status=Status::Reply;p.responder=src;p.rtt_ns=0;p.destination_reply=true;return p;}
        // Each router uses the topology valid at its forwarding instant. TTL counts traversed links.
        for(;;){
            auto path=shortest(source.at(t),at,dst);
            if(path.size()<2)return fail(p,"forward_unreachable",deadline);
            auto tr=transit(at,path[1],t,cfg.packet_bytes,deadline);
            if(!tr.arrived)return fail(p,tr.detail,deadline);
            at=path[1];t=tr.time;--remaining;
            if(at==dst){responder=at;destination=true;break;}
            if(remaining==0){responder=at;break;}
        }
        // Synthetic ICMP Time Exceeded (intermediate) or Port Unreachable (destination).
        // The response has its own directed route and may follow a different path.
        at=responder;unsigned reply_hops=0;
        while(at!=src){
            if(++reply_hops>cfg.max_hops)return fail(p,"return_max_hops",deadline);
            auto path=shortest(source.at(t),at,src);
            if(path.size()<2)return fail(p,"return_unreachable",deadline);
            auto tr=transit(at,path[1],t,cfg.icmp_bytes,deadline);
            if(!tr.arrived)return fail(p,tr.detail,deadline);
            at=path[1];t=tr.time;
        }
        // Equality at the deadline is a reply, unless it is exactly the exclusive run end.
        p.status=Status::Reply;p.finished_ns=t;p.responder=responder;p.rtt_ns=U(t-sent);p.destination_reply=destination;p.detail=destination?"icmp_port_unreachable":"icmp_time_exceeded";return p;
    }
    Result run(Node src,Node dst,Ns start)const{
        Result r{src,dst,start,start,false,false,{}};Ns t=start;
        for(unsigned ttl=1;ttl<=cfg.max_hops;++ttl){Row row{ttl,{}};bool destination_seen=false;
            for(unsigned i=0;i<cfg.probes;++i){if(t>=source.end){r.censored=true;break;}auto p=probe(src,dst,t,ttl,i);t=p.finished_ns;destination_seen|=p.destination_reply;row.probes.push_back(p);if(p.status==Status::Censored){r.censored=true;break;}}
            r.rows.push_back(std::move(row));r.finished_ns=t;
            if(destination_seen){r.reached=true;break;}if(r.censored)break;
        }return r;
    }
    std::string text(const Result&r)const{
        std::ostringstream out;out<<"traceroute to "<<ip(r.dst)<<" ("<<ip(r.dst)<<"), "<<cfg.max_hops<<" hops max, "<<cfg.packet_bytes<<" byte packets\n";
        for(auto&row:r.rows){
            // A censored/incomplete TTL is not printed as fake timeout stars.
            if(row.probes.size()!=cfg.probes||std::any_of(row.probes.begin(),row.probes.end(),[](auto&p){return p.status==Status::Censored;}))break;
            // Layout is byte-for-byte that of traceroute(8): "%2d  " then one probe field per
            // probe joined by a single space. A timeout is "*"; a reply is "addr  rtt ms" and the
            // address is repeated only when the responder changes (otherwise one extra space);
            // an ICMP unreachable code is appended as " !H" / " !N" / " !X" / " !<num>".
            out<<std::setw(2)<<row.ttl<<"  ";
            std::optional<Node>last;
            for(size_t i=0;i<row.probes.size();++i){
                const auto&p=row.probes[i];
                if(i)out<<' ';
                if(p.status==Status::Timeout){out<<'*';continue;}
                if(!p.responder||!p.rtt_ns)throw Error("reply lacks address/RTT");
                if(last!=p.responder){out<<ip(*p.responder)<<" ("<<ip(*p.responder)<<")  ";last=p.responder;}
                else out<<' ';
                out<<milliseconds(*p.rtt_ns)<<" ms";
                if(!p.code.empty())out<<' '<<p.code;
            }
            out<<'\n';
        }return out.str();
    }
};
} // namespace hsn::trace
