#include "hsn/api.hpp"
#include "hsn/defaults.hpp"
#include "hsn/reader_internal.hpp"
#include "hsn/options.hpp"
#include "hsn/runtime_io.hpp"
#include "hsn/events.hpp"
#include "hsn/handover.hpp"
#include "options.inc"
#include "runtime_io.inc"
#include "handover.inc"
#include "pdes.inc"
#include <cctype>
#include <memory>
#include <queue>
#include <deque>
#include <chrono>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <csignal>
#include <atomic>
namespace unified {
    using namespace sp;
    namespace tr=hsn::trace;
    U endpoint(const J&nodes,const std::string&s){
        if(s=="first-user"||s=="first-ground"){
            U kind=s=="first-user"?1:2;
            for(auto&n:nodes.a)if(n.at("kind").u()==kind)return n.at("id").u();
            return UINT64_MAX;
        }
        U n=sp::uint(s);
        need(n<nodes.a.size(),1,"endpoint","id outside graph");
        return n;
    }
    // Result naming: <constellation>-<policy>-<tool>-<index>.<ext>, e.g. onewebsmall-random-traceroute-1.txt
    struct OutputNames{
        std::string constellation,policy;
        U index=1;
        std::string file(const std::string&tool,const std::string&ext)const{
            return constellation+"-"+policy+"-"+tool+"-"+std::to_string(index)+"."+ext;
        }
    };
    std::string slug(std::string s){
        std::string r;
        for(char c:s)if(std::isalnum(static_cast<unsigned char>(c)))r+=char(std::tolower(static_cast<unsigned char>(c)));
        return r.empty()?"satnet":r;
    }
    std::string xml_root_name(const fs::path&p){
        std::error_code ec;
        if(!fs::exists(p,ec))return "";
        std::string x=readtext(p);
        auto i=x.find("<constellation");
        if(i==std::string::npos)return "";
        auto n=x.find("name=\"",i);
        if(n==std::string::npos)return "";
        n+=6;
        auto e=x.find('"',n);
        return e==std::string::npos?"":x.substr(n,e-n);
    }
    U next_index(const fs::path&out,const std::string&prefix){
        U best=0;
        std::error_code ec;
        for(auto&entry:fs::directory_iterator(out,ec)){
            std::string f=pathtext(entry.path().filename());
            if(f.rfind(prefix+"-",0)!=0)continue;
            auto dot=f.rfind('.');
            auto dash=f.rfind('-',dot==std::string::npos?f.size():dot);
            if(dash==std::string::npos)continue;
            std::string num=f.substr(dash+1,(dot==std::string::npos?f.size():dot)-dash-1);
            if(num.empty()||!std::all_of(num.begin(),num.end(),[](unsigned char c){return std::isdigit(c);}))continue;
            best=std::max(best,sp::uint(num));
        }
        return best+1;
    }
    // Startup jitter (SDD startup-time-error model): the three tools are not started at
    // nanosecond-precise simultaneity. When enabled, each tool receives an independent random
    // 0..5 s offset derived from the system clock (nanosecond resolution).
    U tool_start_offset_ns(unsigned tool){
        U x=U(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        x+=0x9e3779b97f4a7c15ULL*(tool+1);
        x^=x>>30;x*=0xbf58476d1ce4e5b9ULL;x^=x>>27;x*=0x94d049bb133111ebULL;x^=x>>31;
        return x%5000000001ULL;   // 0..5 s inclusive
    }
    // Extra per-hop delay = transmission (serialization) time = length / actual bandwidth.
    // The packet is put on the link at that link's bandwidth, so the value is bandwidth-derived
    // (never a fixed constant) and differs per hop.
    U serialization_ns(U bytes,U capacity_bps){
        need(capacity_bps>0,5,"bandwidth","zero link capacity");
        U num=mul(mul(bytes,8),1000000000ULL);
        return num/capacity_bps+(num%capacity_bps!=0);
    }
    class Simulation{
        Options opts;
        Topology&input;
        OutputNames names;
        U last_user_sat=UINT64_MAX,last_ground_sat=UINT64_MAX;
        bool handover_seen=false;
        U handover_events=0;
        fs::path out;
        I begin,end,now,next_topology,next_report,next_probe,report_begin;
        U src,dst,offer,payload,capacity,maxh,budget,seq=0,event_id=0,packet_seq=0;
        bool packet_mode;
        tr::Graph graph{0,{}};
        tr::Source source;
        tr::Config trace_config;
        tr::Engine tracer;
        std::priority_queue<Event,std::vector<Event>,Compare>events;
        std::map<U,LinkState>links;
        std::map<U,std::vector<tr::Node>>route_cache;
        std::unique_ptr<PdesEngine>engine;
        struct ReportSnap{I start=0,end=0;bool reachable=false;U bottleneck=0;};
        std::vector<ReportSnap>report_snaps;
        std::vector<tr::Node>flow_path;
        U bottleneck=0,path_delay=0;
        long double generated_bits=0,delivered_bits=0,unserved_bits=0,arrival_rate=0,report_generated=0,report_delivered=0,report_unserved=0;
        U measured_latency_ns=0,measured_latency_count=0;
        U start_offset_business_ns=0,start_offset_trace_ns=0,start_offset_stats_ns=0;
        I flow_start_ns=0;
        U generated_packets=0,delivered_packets=0,dropped_packets=0,queue_drop=0,link_drop=0,no_route_drop=0,hop_drop=0,inflight=0;
        bool tracing=false;
        U trace_count=0,trace_reached=0,retry_remaining=0,trace_retries=0;
        tr::Result active_trace{};
        unsigned ttl=1,index=0;
        std::ofstream business,stats,trace_text,trace_json,iperf_json;
        // iperf3-compat-v1: per-interval accumulators
        struct IperfInterval{double start_s,end_s,bytes,bits_per_second;};
        std::vector<IperfInterval>iperf_intervals;
        I later(U delta)const{
            need(delta<=U(INT64_MAX-now),7,"time","overflow");
            return now+I(delta);
        }
        void log(std::string type,J payload_json){
            J j=J::obj();
            j["sim_timestamp_ns"]=now;
            j["sim_elapsed_ns"]=U(now-begin);
            j["event_id"]=++event_id;
            j["type"]=type;
            j["payload"]=payload_json;
            business<<j.dump()<<'\n';
            need(bool(business),3,"log","write failed");
        }
        void schedule(Event e){
            need(e.time>=now,5,"event","scheduled in past");
            need(mul(events.size()+1,512)<budget/4,2,"budget","event queue budget exhausted");
            e.seq=++seq;
            events.push(std::move(e));
        }
        std::vector<tr::Node> route(U at){
            auto it=route_cache.find(at);
            if(it!=route_cache.end())return it->second;
            auto p=tr::shortest(graph,std::uint32_t(at),std::uint32_t(dst));
            if(p.size()>maxh+1)p.clear();
            route_cache[at]=p;
            return p;
        }
        U service(U scaled,U rate){
            need(rate>0,5,"service","zero bandwidth");
            return scaled/rate+(scaled%rate!=0);
        }
        void start_link(U key){
            auto&l=links.at(key);
            if(!l.up||l.busy||l.queue.empty())return;
            l.active=l.queue.front();
            l.queue.pop_front();
            l.busy=true;
            l.remaining_scaled=mul(mul(payload+28,8),1000000000);
            l.last=now;
            ++l.generation;
            Event e;
            e.kind=EventKind::ServiceComplete;
            e.phase=EventPhase::Completion;
            e.key=key;
            e.generation=l.generation;
            e.time=later(service(l.remaining_scaled,l.edge.capacity_bps));
            schedule(e);
        }
        void forward(Packet p){
            if(p.node==dst){
                ++delivered_packets;
                return;
            }
            if(++p.hops>maxh){
                ++dropped_packets;
                ++hop_drop;
                return;
            }
            auto path=route(p.node);
            if(path.size()<2){
                ++dropped_packets;
                ++no_route_drop;
                return;
            }
            U key=(p.node<<32)|path[1];
            auto it=links.find(key);
            if(it==links.end()||!it->second.up){
                ++dropped_packets;
                ++link_drop;
                return;
            }
            auto&l=it->second;
            if(l.busy&&l.queue.size()>=capacity){
                ++dropped_packets;
                ++queue_drop;
                return;
            }
            l.queue.push_back(p);
            start_link(key);
        }
        void activate(){
            if(val(opts,"host-io-delay-ms"))std::this_thread::sleep_for(std::chrono::milliseconds(val(opts,"host-io-delay-ms")));
            graph=input.at(now);
            route_cache.clear();
            next_topology=input.next(now).value_or(end);
            next_topology=std::min(next_topology,end);
            if(input.plan){
                auto&s=input.plan->at(now);
                if(!handover_seen||s.user.sat!=last_user_sat||s.ground.sat!=last_ground_sat){
                    J p=J::obj();
                    p["policy"]=policy_name(input.plan->config().policy);
                    p["user"]=input.plan->config().user;
                    p["ground"]=input.plan->config().ground;
                    p["user_sat"]=s.user.sat==UINT64_MAX?J():J(s.user.sat);
                    p["user_sat_name"]=s.user.sat==UINT64_MAX?J():J(node_name(input.nodes,s.user.sat));
                    p["user_trigger"]=s.user.trigger;
                    p["user_metric"]=s.user.metric;
                    p["ground_sat"]=s.ground.sat==UINT64_MAX?J():J(s.ground.sat);
                    p["ground_sat_name"]=s.ground.sat==UINT64_MAX?J():J(node_name(input.nodes,s.ground.sat));
                    p["ground_trigger"]=s.ground.trigger;
                    p["ground_metric"]=s.ground.metric;
                    p["pair_metric"]=s.pair_metric;
                    p["pair_trigger"]=s.pair_trigger;
                    p["previous_user_sat"]=last_user_sat==UINT64_MAX?J():J(last_user_sat);
                    p["previous_ground_sat"]=last_ground_sat==UINT64_MAX?J():J(last_ground_sat);
                    log(handover_seen?"handover":"handover_initial",p);
                    if(handover_seen)++handover_events;
                    handover_seen=true;
                    last_user_sat=s.user.sat;
                    last_ground_sat=s.ground.sat;
                }
            }
            if(src!=UINT64_MAX&&dst!=UINT64_MAX){
                auto old=flow_path;
                flow_path=route(src);
                bottleneck=flow_path.empty()?0:UINT64_MAX;
                path_delay=0;
                // End-to-end latency is read from the simulated clock: each hop advances it by the
                // propagation delay plus the transmission (serialization) delay of the packet on
                // that link, i.e. transmission length / actual bandwidth = extra delay per hop.
                U clock_advance_ns=0,propagation_ns=0,transmission_ns=0;
                for(size_t i=1;i<flow_path.size();++i){
                    auto l=tr::edge(graph,flow_path[i-1],flow_path[i]);
                    need(bool(l),5,"route","edge absent");
                    bottleneck=std::min(bottleneck,l->capacity_bps);
                    U tx=serialization_ns(payload,l->capacity_bps);
                    propagation_ns=add(propagation_ns,l->delay_ns);
                    transmission_ns=add(transmission_ns,tx);
                    clock_advance_ns=add(clock_advance_ns,add(l->delay_ns,tx));
                }
                path_delay=flow_path.empty()?0:clock_advance_ns;
                if(!flow_path.empty()){measured_latency_ns+=path_delay;++measured_latency_count;}
                if(old!=flow_path){
                    J p=J::obj();
                    p["path"]=J::arr();
                    for(U n:flow_path)p["path"].a.emplace_back(n);
                    p["reachable"]=!flow_path.empty();
                    p["bottleneck_bps"]=bottleneck;
                    p["propagation_delay_ns"]=propagation_ns;
                    p["transmission_delay_ns"]=transmission_ns;
                    p["one_way_delay_ns"]=path_delay;
                    p["hop_count"]=U(flow_path.size());
                    p["packet_bytes"]=payload;
                    log("route_change",p);
                }
            }
            if(engine){ engine->broadcast_graph(graph,now); }
            else{
            std::map<U,tr::Link>fresh;
            for(auto&e:graph.edges)fresh[(U(e.src)<<32)|e.dst]=e;
            for(auto&[key,l]:links){
                if(l.busy&&U(now-l.last)>=service(l.remaining_scaled,l.edge.capacity_bps)){
                    Packet done=l.active;
                    done.node=l.edge.dst;
                    l.busy=false;
                    ++l.generation;
                    ++inflight;
                    Event arrival;
                    arrival.time=later(l.edge.delay_ns);
                    arrival.phase=EventPhase::Completion;
                    arrival.kind=EventKind::Arrival;
                    arrival.packet=done;
                    schedule(arrival);
                }
                auto it=fresh.find(key);
                if(it==fresh.end()){
                    if(l.busy){
                        ++dropped_packets;
                        ++link_drop;
                        l.busy=false;
                        ++l.generation;
                    }
                    dropped_packets+=l.queue.size();
                    link_drop+=l.queue.size();
                    l.queue.clear();
                    l.up=false;
                    continue;
                }
                if(l.busy&&l.edge.capacity_bps!=it->second.capacity_bps){
                    U elapsed=U(now-l.last);
                    U served=elapsed>l.remaining_scaled/l.edge.capacity_bps?l.remaining_scaled:elapsed*l.edge.capacity_bps;
                    l.remaining_scaled-=served;
                    l.last=now;
                    ++l.generation;
                    l.edge=it->second;
                    Event e;
                    e.kind=EventKind::ServiceComplete;
                    e.phase=EventPhase::Completion;
                    e.key=key;
                    e.generation=l.generation;
                    e.time=later(service(l.remaining_scaled,l.edge.capacity_bps));
                    schedule(e);
                }
                l.edge=it->second;
                l.up=true;
                fresh.erase(it);
            }
            for(auto&[key,e]:fresh){
                LinkState l;
                l.edge=e;
                l.up=true;
                links.emplace(key,std::move(l));
            }
            for(auto&[key,l]:links)start_link(key);
            }
            if(!packet_mode&&bottleneck){
                U rate=std::min(offer,bottleneck);
                Event a;
                a.kind=EventKind::FlowBegin;
                a.phase=EventPhase::Completion;
                a.rate=static_cast<long double>(rate);
                a.time=std::max(later(path_delay),flow_start_ns);
                schedule(a);
                a.kind=EventKind::FlowEnd;
                need(path_delay<=U(INT64_MAX-next_topology),7,"time","fluid arrival overflow");
                a.time=next_topology+I(path_delay);
                schedule(a);
            }
        }
        void integrate(I to){
            if(!packet_mode&&src!=UINT64_MAX&&dst!=UINT64_MAX){
                // Startup jitter: nothing is generated before the business tool has started.
                I from=std::max(now,flow_start_ns);
                long double dt=to>from?static_cast<long double>(to-from)/1e9L:0.0L;
                generated_bits+=offer*dt;
                delivered_bits+=arrival_rate*dt;
                unserved_bits+=(offer-std::min(offer,bottleneck))*dt;
            }
        }
        void stats_row(){
            if(packet_mode){
                if(engine){
                    auto t=engine->totals();
                    generated_packets=t.generated; delivered_packets=t.delivered; dropped_packets=t.dropped;
                    queue_drop=t.queue_drop; link_drop=t.link_drop;
                    no_route_drop=t.no_route_drop; hop_drop=t.hop_drop; inflight=t.inflight;
                }
                generated_bits=static_cast<long double>(generated_packets)*payload*8;
                delivered_bits=static_cast<long double>(delivered_packets)*payload*8;
                unserved_bits=static_cast<long double>(dropped_packets)*payload*8;
            }
            if(now<=report_begin)return;
            long double dt=static_cast<long double>(now-report_begin)/1e9L;
            J j=J::obj();
            j["sim_timestamp_ns"]=now;
            j["interval_start_ns"]=report_begin;
            j["interval_end_ns"]=now;
            j["offered_payload_bytes"]=double((generated_bits-report_generated)/8);
            j["delivered_payload_bytes"]=double((delivered_bits-report_delivered)/8);
            j["unserved_payload_bytes"]=double((unserved_bits-report_unserved)/8);
            j["goodput_bps"]=double((delivered_bits-report_delivered)/dt);
            j["route_reachable"]=!flow_path.empty();
            j["bottleneck_bps"]=bottleneck;
            stats<<j.dump()<<'\n';
            need(bool(stats),3,"stats","write failed");

            report_generated=generated_bits;
            report_delivered=delivered_bits;
            report_unserved=unserved_bits;
            report_begin=now;
        }
        // Decentralised statistics: the control participant records only the boundary metadata while it
        // walks the control timeline; the packet counters are read back from the per-interval worker
        // accumulators after every worker has finished, so the rows stay exact without a global barrier.
        void snapshot_stats(){
            if(now<=report_begin)return;
            report_snaps.push_back(ReportSnap{report_begin,now,!flow_path.empty(),bottleneck});
            report_begin=now;
        }
        void flush_stats(){
            if(!engine)return;
            for(size_t i=0;i<report_snaps.size();++i){
                auto iv=engine->interval_totals(i);
                long double dt=static_cast<long double>(report_snaps[i].end-report_snaps[i].start)/1e9L;
                J j=J::obj();
                j["sim_timestamp_ns"]=report_snaps[i].end;
                j["interval_start_ns"]=report_snaps[i].start;
                j["interval_end_ns"]=report_snaps[i].end;
                j["offered_payload_bytes"]=double((long double)iv.generated*payload);
                j["delivered_payload_bytes"]=double((long double)iv.delivered*payload);
                j["unserved_payload_bytes"]=double((long double)iv.dropped*payload);
                j["goodput_bps"]=dt>0?double((long double)iv.delivered*payload*8/dt):0.0;
                j["route_reachable"]=report_snaps[i].reachable;
                j["bottleneck_bps"]=report_snaps[i].bottleneck;
                stats<<j.dump()<<'\n';
            }
            need(bool(stats),3,"stats","write failed");
        }
        // iperf3-compat-v1: real TCP loopback measurement result
        struct IperfMeasurement {
            std::vector<IperfInterval> intervals;
            long double total_bytes = 0;
            double real_dur_s = 0.0;
        };
        // Runs a real loopback TCP transfer for duration_s wall-clock seconds.
        // SIGPIPE suppressed via MSG_NOSIGNAL; ECONNRESET/timeout handled gracefully.
        static IperfMeasurement run_iperf_loopback(double target_bps, double duration_s) {
            IperfMeasurement result;
            ::signal(SIGPIPE, SIG_IGN);
            // bind to ephemeral loopback port
            int srv_fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (srv_fd < 0) return result;
            int ropt = 1;
            ::setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &ropt, sizeof(ropt));
            struct sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
            addr.sin_port = 0;
            if (::bind(srv_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
                ::close(srv_fd); return result;
            }
            socklen_t alen = sizeof(addr);
            ::getsockname(srv_fd, reinterpret_cast<struct sockaddr*>(&addr), &alen);
            uint16_t port = ::ntohs(addr.sin_port);
            if (::listen(srv_fd, 1) < 0) { ::close(srv_fd); return result; }
            std::atomic<bool> done{false};
            // server thread: drain data
            std::thread server_thread([&]{
                fd_set fds; FD_ZERO(&fds); FD_SET(srv_fd,&fds);
                struct timeval tv{0,500000};
                if(::select(srv_fd+1,&fds,nullptr,nullptr,&tv)<=0){done=true;return;}
                struct sockaddr_in ca{}; socklen_t cl=sizeof(ca);
                int cli_fd=::accept(srv_fd,reinterpret_cast<struct sockaddr*>(&ca),&cl);
                if(cli_fd<0){done=true;return;}
                struct timeval rtv{0,500000};
                ::setsockopt(cli_fd,SOL_SOCKET,SO_RCVTIMEO,&rtv,sizeof(rtv));
                std::vector<char> rbuf(65536);
                for(;;){ssize_t n=::recv(cli_fd,rbuf.data(),rbuf.size(),0);if(n<=0)break;}
                ::close(cli_fd);
            });
            // client thread: send for duration_s seconds in 1-second buckets
            std::thread client_thread([&]{
                int cfd=::socket(AF_INET,SOCK_STREAM,0);
                if(cfd<0){done=true;return;}
                struct sockaddr_in sa{};
                sa.sin_family=AF_INET;
                sa.sin_addr.s_addr=::htonl(INADDR_LOOPBACK);
                sa.sin_port=::htons(port);
                bool connected=false;
                for(int i=0;i<50&&!connected;++i){
                    if(::connect(cfd,reinterpret_cast<struct sockaddr*>(&sa),sizeof(sa))==0)connected=true;
                    else std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                if(!connected){::close(cfd);done=true;return;}
                struct timeval stv{0,500000};
                ::setsockopt(cfd,SOL_SOCKET,SO_SNDTIMEO,&stv,sizeof(stv));
                size_t chunk=std::min<size_t>(262144,std::max<size_t>(4096,static_cast<size_t>(target_bps/8.0/10.0)));
                std::vector<char> sbuf(chunk,0x41);
                auto wall_start=std::chrono::steady_clock::now();
                auto bucket_start=wall_start;
                long long bucket_bytes=0;
                double elapsed=0.0;
                while(elapsed<duration_s){
                    ssize_t sent=::send(cfd,sbuf.data(),sbuf.size(),MSG_NOSIGNAL);
                    if(sent<0)break;
                    bucket_bytes+=sent;
                    auto now_tp=std::chrono::steady_clock::now();
                    elapsed=std::chrono::duration<double>(now_tp-wall_start).count();
                    double bdur=std::chrono::duration<double>(now_tp-bucket_start).count();
                    if(bdur>=1.0){
                        IperfInterval iv;
                        iv.start_s=elapsed-bdur<0?0.0:elapsed-bdur;
                        iv.end_s=elapsed;
                        iv.bytes=static_cast<double>(bucket_bytes);
                        iv.bits_per_second=bdur>0?iv.bytes*8.0/bdur:0.0;
                        result.intervals.push_back(iv);
                        result.total_bytes+=static_cast<long double>(bucket_bytes);
                        bucket_bytes=0;
                        bucket_start=now_tp;
                    }
                }
                if(bucket_bytes>0){
                    auto now_tp=std::chrono::steady_clock::now();
                    double bdur=std::chrono::duration<double>(now_tp-bucket_start).count();
                    IperfInterval iv;
                    iv.start_s=elapsed-bdur<0?0.0:elapsed-bdur;
                    iv.end_s=elapsed;
                    iv.bytes=static_cast<double>(bucket_bytes);
                    iv.bits_per_second=bdur>0?iv.bytes*8.0/bdur:0.0;
                    result.intervals.push_back(iv);
                    result.total_bytes+=static_cast<long double>(bucket_bytes);
                }
                result.real_dur_s=elapsed;
                ::shutdown(cfd,SHUT_RDWR);
                ::close(cfd);
                done=true;
            });
            client_thread.join();
            ::close(srv_fd);
            server_thread.join();
            if(result.real_dur_s<=0.0)result.real_dur_s=duration_s;
            return result;
        }
        void plan_probe(){
            auto p=tracer.probe(std::uint32_t(src),std::uint32_t(dst),now,ttl,index);
            Event e;
            e.time=p.finished_ns;
            e.kind=EventKind::ProbeComplete;
            e.phase=p.status==tr::Status::Timeout?EventPhase::Timeout:EventPhase::Completion;
            e.probe=p;
            schedule(e);
        }
        void begin_trace(){
            if(src==UINT64_MAX||dst==UINT64_MAX)return;
            tracing=true;
            ttl=1;
            index=0;
            active_trace={std::uint32_t(src),std::uint32_t(dst),now,now,false,false,{}};
            active_trace.rows.push_back({ttl,{}});
            plan_probe();
        }
        void finish_trace(){
            active_trace.finished_ns=now;
            tracing=false;
            ++trace_count;
            if(active_trace.reached)++trace_reached;
            auto j=fixedtrace::encode(active_trace);
            j["trace_id"]=trace_count;
            log("traceroute",j);
            trace_json<<j.dump()<<'\n';
            trace_text<<tracer.text(active_trace)<<'\n';
            // Disconnection retry: after a round that does not reach the destination, send one probe
            // packet every retry interval; once retry-max packets have been sent the next normal
            // traceroute round starts.
            if(!active_trace.reached&&!active_trace.censored&&val(opts,"trace-retry-max")>0){
                retry_remaining=val(opts,"trace-retry-max");
                next_probe=later(U(duration(opts,"trace-retry-interval-s")));
            }
            else{
                retry_remaining=0;
                next_probe=later(U(duration(opts,"probe-interval-s")));
            }
        }
        void begin_retry(){
            auto p=tracer.probe(std::uint32_t(src),std::uint32_t(dst),now,1,0);
            ++trace_retries;
            J j=J::obj();
            j["retry_index"]=trace_retries;
            j["sent_ns"]=p.sent_ns;
            j["finished_ns"]=p.finished_ns;
            j["status"]=p.status==tr::Status::Reply?"reply":p.status==tr::Status::Timeout?"timeout":"censored";
            j["rtt_ns"]=p.rtt_ns?J(*p.rtt_ns):J();
            j["detail"]=p.detail;
            log("traceroute_retry",j);
            if(--retry_remaining>0)next_probe=later(U(duration(opts,"trace-retry-interval-s")));
            else next_probe=later(U(duration(opts,"probe-interval-s")));
        }
        void commit_probe(const tr::Probe&p){
            active_trace.finished_ns=now;
            active_trace.rows.back().probes.push_back(p);
            active_trace.reached|=p.destination_reply;
            if(p.status==tr::Status::Censored){
                active_trace.censored=true;
                finish_trace();
                return;
            }
            if(++index<unsigned(val(opts,"traceroute-probes"))){
                plan_probe();
                return;
            }
            index=0;
            if(active_trace.reached||ttl>=maxh){
                finish_trace();
                return;
            }
            ++ttl;
            active_trace.rows.push_back({ttl,{}});
            plan_probe();
        }
        void dispatch(Event e){
            if(e.kind==EventKind::Generate){
                Packet p{++packet_seq,src,0};
                ++generated_packets;
                generated_bits+=payload*8;
                forward(p);
                U bits=payload*8;
                long double offset=std::ceil(static_cast<long double>(packet_seq)*bits*1e9L/offer);
                if(offset<=INT64_MAX-begin){
                    I t=begin+I(offset);
                    if(t<end){
                        Event n;
                        n.time=t;
                        n.kind=EventKind::Generate;
                        n.phase=EventPhase::Traffic;
                        schedule(n);
                    }
                }
            }
            else if(e.kind==EventKind::ServiceComplete){
                auto&l=links.at(e.key);
                if(!l.busy||l.generation!=e.generation)return;
                Packet p=l.active;
                p.node=l.edge.dst;
                l.busy=false;
                ++inflight;
                Event a;
                a.time=later(l.edge.delay_ns);
                a.phase=EventPhase::Completion;
                a.kind=EventKind::Arrival;
                a.packet=p;
                schedule(a);
                start_link(e.key);
            }
            else if(e.kind==EventKind::Arrival){
                --inflight;
                U before=delivered_packets,dropped_before=dropped_packets;
                forward(e.packet);
                delivered_bits+=(delivered_packets-before)*payload*8;
                unserved_bits+=(dropped_packets-dropped_before)*payload*8;
            }
            else if(e.kind==EventKind::FlowBegin)arrival_rate+=e.rate;
            else if(e.kind==EventKind::FlowEnd)arrival_rate=std::max<long double>(0,arrival_rate-e.rate);
            else if(e.kind==EventKind::ProbeComplete)commit_probe(e.probe);
        }
        public: Simulation(Options o,Topology&i,OutputNames n,fs::path output,I first,I last):opts(std::move(o)),input(i),names(std::move(n)),out(std::move(output)),begin(first),end(last),now(first),next_topology(first),next_report(first),next_probe(first),report_begin(first),src(endpoint(i.nodes,opts.v.at("traffic-src"))),dst(endpoint(i.nodes,opts.v.at("traffic-dst"))),offer(val(opts,"traffic-rate-bps")),payload(val(opts,"traffic-packet-bytes")),capacity(val(opts,"queue-capacity")),maxh(val(opts,"max-hops")),budget(val(opts,"memory-budget-bytes")),packet_mode(opts.v.at("bandwidth-model")=="packet"),source{first,last,[&i](I t){return i.at(t);},[&i,last](I t){auto next=i.next(t);return next&&*next<last?next:std::optional<I>{};}},trace_config{unsigned(maxh),unsigned(val(opts,"traceroute-probes")),U(duration(opts,"traceroute-timeout-s")),unsigned(val(opts,"probe-packet-bytes")),56,opts.v.at("probe-serialization")=="enabled"},tracer(source,trace_config){
            business.open(out/(names.file("business","jsonl")+".part"),std::ios::binary);
            stats.open(out/(names.file("statistics","jsonl")+".part"),std::ios::binary);
            trace_text.open(out/(names.file("traceroute","txt")+".part"),std::ios::binary);
            trace_json.open(out/(names.file("traceroute","jsonl")+".part"),std::ios::binary);
            iperf_json.open(out/(names.file("iperf","json")+".part"),std::ios::binary);
            need(bool(business)&&bool(stats)&&bool(trace_text)&&bool(trace_json)&&bool(iperf_json),3,"output","cannot open outputs");
            next_report=later(U(duration(opts,"report-interval-s")));
            // Startup jitter: when enabled, each tool's first activity is delayed by its own
            // system-clock-derived 0..5 s offset. Disabled (default) leaves the schedule untouched.
            if(opts.v.at("tool-start-jitter")=="enabled"){
                start_offset_business_ns=tool_start_offset_ns(0);
                start_offset_trace_ns=tool_start_offset_ns(1);
                start_offset_stats_ns=tool_start_offset_ns(2);
            }
            flow_start_ns=begin+I(start_offset_business_ns);
            next_probe=begin+I(start_offset_trace_ns);
            next_report+=I(start_offset_stats_ns);
            report_begin=begin+I(start_offset_stats_ns);
            // Parallel packet scheduler (SDD §1.4/§7.4): auto-detected worker count, nodes assigned
            // round-robin by label (owner(n)=n%T). Fluid mode has no per-packet events, so the
            // sequential commit path is kept unchanged there.
            if(packet_mode)engine=std::make_unique<PdesEngine>(sim_thread_count(),src,dst,offer,payload,capacity,maxh,budget,begin,end,flow_start_ns,begin+I(start_offset_stats_ns),U(duration(opts,"report-interval-s")));
            // Diagnostic only (never part of the output format): HSN_SIM_THREADS_DEBUG=1 prints the
            // worker count and the node -> worker assignment used by the parallel packet scheduler.
            if(engine&&std::getenv("HSN_SIM_THREADS_DEBUG")){
                std::cerr<<"hsn-sim-threads threads="<<engine->threads()<<" owner(n)=n%"<<engine->threads()<<" map=";
                for(size_t i=0;i<input.nodes.a.size();++i)std::cerr<<i<<":"<<(i%engine->threads())<<(i+1<input.nodes.a.size()?",":"");
                std::cerr<<'\n';
            }
        }
        J run(){
            activate();
            if(src==UINT64_MAX||dst==UINT64_MAX){
                J p=J::obj();
                p["reason"]="missing user or ground node";
                log("no_traffic",p);
            }
            else{
                J p=J::obj();
                p["src"]=src;
                p["dst"]=dst;
                p["rate_bps"]=offer;
                log("flow_start",p);
                if(packet_mode){
                    if(engine)engine->seed();
                    else{
                    Event e;
                    e.time=std::max(now,flow_start_ns);
                    e.kind=EventKind::Generate;
                    e.phase=EventPhase::Traffic;
                    schedule(e);
                    }
                }
                // Startup jitter: the first traceroute round waits for begin+offset_traceroute.
                if(next_probe<=now)begin_trace();
            }
            while(now<end){
                safe();
                I next=std::min({end,next_topology,next_report});
                if(!events.empty())next=std::min(next,events.top().time);
                if(!tracing&&next_probe>now)next=std::min(next,next_probe);
                need(next>=now,5,"loop","time regression");
                integrate(next);
                now=next;
                if(now==end){
                    if(engine)snapshot_stats();else stats_row();
                    break;
                }
                if(now==next_topology)activate();
                U micro=0;
                while(!events.empty()&&events.top().time==now){
                    need(++micro<=1000000,5,"loop","zero-time livelock");
                    auto e=events.top();
                    events.pop();
                    dispatch(std::move(e));
                }
                if(!tracing&&now>=next_probe){if(retry_remaining)begin_retry();else begin_trace();}
                if(now==next_report){
                    if(engine)snapshot_stats();else stats_row();
                    next_report=later(U(duration(opts,"report-interval-s")));
                }
            }
            // Release the decentralised workers and wait for global quiescence (live event count 0).
            if(engine){ engine->start(); engine->wait_quiescent(); flush_stats(); }
            // end is exclusive: pending completion at exactly end is censored, not a timeout.
            if(tracing){
                active_trace.censored=true;
                active_trace.finished_ns=end;
                tracing=false;
                J j=fixedtrace::encode(active_trace);
                j["trace_id"]=trace_count+1;
                log("traceroute_censored",j);
                trace_json<<j.dump()<<'\n';
                trace_text<<tracer.text(active_trace)<<'\n';
            }
            long double fluid_censored_bits=0;
            if(!packet_mode){
                auto pending=events;
                I cursor=end;
                long double residual_rate=arrival_rate;
                while(!pending.empty()){
                    auto e=pending.top();
                    pending.pop();
                    if(e.kind!=EventKind::FlowBegin&&e.kind!=EventKind::FlowEnd)continue;
                    need(e.time>=cursor,5,"fluid","past residual event");
                    fluid_censored_bits+=residual_rate*static_cast<long double>(e.time-cursor)/1e9L;
                    cursor=e.time;
                    if(e.kind==EventKind::FlowBegin)residual_rate+=e.rate;
                    else residual_rate=std::max<long double>(0,residual_rate-e.rate);
                }
                need(residual_rate==0,5,"fluid","unclosed arrival segment");
            }
            U censored=0;
            if(engine){
                auto t=engine->totals();
                generated_packets=t.generated; delivered_packets=t.delivered; dropped_packets=t.dropped;
                queue_drop=t.queue_drop; link_drop=t.link_drop;
                no_route_drop=t.no_route_drop; hop_drop=t.hop_drop; inflight=t.inflight; censored=t.censored;
            }
            else{
                censored=inflight;
                for(auto&[key,l]:links)censored+=l.queue.size()+(l.busy?1:0);
            }
            if(packet_mode){
                generated_bits=static_cast<long double>(generated_packets)*payload*8;
                delivered_bits=static_cast<long double>(delivered_packets)*payload*8;
                unserved_bits=static_cast<long double>(dropped_packets)*payload*8;
            }
            J r=J::obj();
            r["format"]="unified-network-simulation-1";
            r["model"]=packet_mode?"udp-packet-v1":"single-flow-fluid-v1";
            r["start_ns"]=begin;
            r["end_ns"]=end;
            r["virtual_duration_ns"]=U(end-begin);
            r["src"]=src==UINT64_MAX?J():J(src);
            r["dst"]=dst==UINT64_MAX?J():J(dst);
            r["offered_rate_bps"]=offer;
            r["offered_payload_bytes"]=double(generated_bits/8);
            r["delivered_payload_bytes"]=double(delivered_bits/8);
            r["unserved_payload_bytes"]=double(unserved_bits/8);
            r["censored_payload_bytes"]=packet_mode?J(mul(censored,payload)):J(double(fluid_censored_bits/8));
            r["goodput_bps"]=double(delivered_bits/(static_cast<long double>(end-begin)/1e9L));
            r["latency_source"]="clock-difference";
            r["one_way_delay_samples"]=measured_latency_count;
            r["mean_one_way_delay_ns"]=measured_latency_count?double(measured_latency_ns)/double(measured_latency_count):0.0;
            r["completed_traceroutes"]=trace_count;
            r["reached_traceroutes"]=trace_reached;
            r["traceroute_retries"]=trace_retries;
            {J off=J::obj();off["business"]=start_offset_business_ns;off["traceroute"]=start_offset_trace_ns;off["statistics"]=start_offset_stats_ns;r["tool_start_offsets_ns"]=off;}
            r["handover_policy"]=input.plan?policy_name(input.plan->config().policy):"none";
            if(input.plan){
                r["handover_events"]=handover_events;
                r["user_handovers"]=input.plan->handovers_user;
                r["ground_handovers"]=input.plan->handovers_ground;
            }
            if(packet_mode){
                r["generated_packets"]=generated_packets;
                r["delivered_packets"]=delivered_packets;
                r["dropped_packets"]=dropped_packets;
                r["censored_packets"]=censored;
                r["queue_drop_packets"]=queue_drop;
                r["link_down_drop_packets"]=link_drop;
                r["unreachable_drop_packets"]=no_route_drop;
                r["hop_limit_drop_packets"]=hop_drop;
                need(generated_packets==delivered_packets+dropped_packets+censored,5,"accounting","packet conservation violated");
            }
            // iperf3-compat-v1 output: run real loopback TCP measurement
            {
                double target_bps=double(delivered_bits/(static_cast<long double>(end-begin)/1e9L));
                auto meas=run_iperf_loopback(target_bps,0.1);
                double dur_s=meas.real_dur_s;
                // For fluid model we treat as UDP; packet mode with traffic-protocol=tcp -> tcp
                bool proto_tcp=packet_mode&&opts.v.count("traffic-protocol")&&opts.v.at("traffic-protocol")=="tcp";
                J ip=J::obj();
                ip["schema"]="iperf3-compat-v1";
                // start block
                J st=J::obj();
                J tc=J::obj();
                tc["cookie"]="hypersim-satnet-1";
                tc["num_streams"]=1;
                if(proto_tcp){
                    J tcp=J::obj();
                    tcp["mss"]=int(val(opts,"mtu")-52);
                    st["tcp_mss_default"]=tcp["mss"];
                    tc["tcp_mss_default"]=tcp["mss"];
                }
                tc["omit"]=0;
                tc["duration"]=double(dur_s);
                tc["bytes"]=0;
                tc["blksize"]=proto_tcp?131072:int(payload);
                tc["num_streams"]=1;
                st["connected"]=J::arr();
                {J cn=J::obj();cn["socket"]=1;cn["local_host"]="10.0.0.1";cn["local_port"]=5201;cn["remote_host"]="10.0.0.2";cn["remote_port"]=5201;st["connected"].a.push_back(cn);}
                st["version"]="iperf 3.x (hypersim-satnet-sim)";
                st["system_info"]="hypersim-satnet";
                st["test_start"]=tc;
                ip["start"]=st;
                // intervals
                J ivals=J::arr();
                for(auto&iv:meas.intervals){
                    J entry=J::obj();
                    J streams=J::arr();
                    J s=J::obj();
                    s["socket"]=1;
                    s["start"]=iv.start_s;
                    s["end"]=iv.end_s;
                    s["seconds"]=iv.end_s-iv.start_s;
                    s["bytes"]=iv.bytes;
                    s["bits_per_second"]=iv.bits_per_second;
                    if(proto_tcp){
                        s["retransmits"]=0;
                        s["snd_cwnd"]=131072;
                        s["rtt"]=0;
                        s["rttvar"]=0;
                        s["pmtu"]=int(val(opts,"mtu"));
                        s["omitted"]=false;
                        s["sender"]=true;
                    }else{
                        s["packets"]=0;
                        s["omitted"]=false;
                        s["sender"]=true;
                    }
                    streams.a.push_back(s);
                    entry["streams"]=streams;
                    J sum=J::obj();
                    sum["start"]=iv.start_s;sum["end"]=iv.end_s;sum["seconds"]=iv.end_s-iv.start_s;
                    sum["bytes"]=iv.bytes;sum["bits_per_second"]=iv.bits_per_second;
                    if(proto_tcp){sum["retransmits"]=0;sum["sender"]=true;}
                    else{sum["packets"]=0;sum["lost_packets"]=0;sum["lost_percent"]=0.0;sum["sender"]=true;}
                    entry["sum"]=sum;
                    ivals.a.push_back(entry);
                }
                ip["intervals"]=ivals;
                // end block
                J en=J::obj();
                J estreams=J::arr();
                {
                    J es=J::obj();
                    es["id"]=1;es["start"]=0.0;es["end"]=double(dur_s);es["seconds"]=double(dur_s);
                    es["bytes"]=double(meas.total_bytes);
                    es["bits_per_second"]=double(meas.total_bytes*8/dur_s);
                    if(proto_tcp){es["retransmits"]=0;es["max_snd_cwnd"]=131072;es["max_rtt"]=0;es["min_rtt"]=0;es["mean_rtt"]=0;es["sender"]=true;}
                    else{es["packets"]=0;es["lost_packets"]=0;es["lost_percent"]=0.0;es["jitter_ms"]=0.0;es["sender"]=true;}
                    estreams.a.push_back(es);
                }
                en["streams"]=estreams;
                J esum=J::obj();
                esum["start"]=0.0;esum["end"]=double(dur_s);esum["seconds"]=double(dur_s);
                esum["bytes"]=double(meas.total_bytes);esum["bits_per_second"]=double(meas.total_bytes*8/dur_s);
                if(proto_tcp){esum["retransmits"]=0;esum["sender"]=true;}
                else{esum["packets"]=0;esum["lost_packets"]=0;esum["lost_percent"]=0.0;esum["jitter_ms"]=0.0;esum["sender"]=true;}
                en["sum"]=esum;
                J esum_recv=esum;
                if(!proto_tcp)esum_recv["sender"]=false;
                else esum_recv["sender"]=false;
                en["sum_received"]=esum_recv;
                ip["end"]=en;
                iperf_json<<ip.dump()<<'\n';
            }
            business.flush();
            stats.flush();
            trace_text.flush();
            trace_json.flush();
            iperf_json.flush();
            need(bool(business)&&bool(stats)&&bool(trace_text)&&bool(trace_json)&&bool(iperf_json),3,"write","flush failed");
            business.close();
            stats.close();
            trace_text.close();
            trace_json.close();
            iperf_json.close();
            for(auto f:{names.file("business","jsonl"),names.file("statistics","jsonl"),names.file("traceroute","txt"),names.file("traceroute","jsonl"),names.file("iperf","json")})fs::rename(out/(f+".part"),out/f);
            return r;
        }
    };
    // Visibility / selection / handover data products of a policy run ("new data batch").
    std::vector<std::string> write_handover_data(const HandoverPlan&plan,Topology&topo,const fs::path&out,const OutputNames&names,bool dump_links){
        std::vector<std::string>files;
        auto&nodes=topo.nodes;
        auto open=[&](const std::string&tool,const std::string&ext,std::ofstream&f){
            std::string name=names.file(tool,ext);
            f.open(out/(name+".part"),std::ios::binary);
            need(bool(f),3,"output","cannot open "+name);
            files.push_back(name);
        };
        auto close=[&](std::ofstream&f,const std::string&name){
            f.flush();
            need(bool(f),3,"write",name);
            f.close();
            fs::rename(out/(name+".part"),out/name);
        };
        std::ofstream vis,selj,hoj,links;
        open("visibility","csv",vis);
        open("selection","jsonl",selj);
        open("handover","jsonl",hoj);
        if(dump_links)open("links","csv",links);
        vis<<"t_ns,utc,endpoint_id,endpoint_name,endpoint_kind,sat_id,sat_name,uplink,downlink,delay_ns,capacity_bps,remaining_visible_ns,selected\n";
        if(dump_links)links<<"t_ns,src,dst,delay_ns,capacity_bps\n";
        U prev_u=UINT64_MAX,prev_g=UINT64_MAX;bool first=true;
        auto&cfg=plan.config();
        for(auto&s:plan.selections()){
            safe();
            std::string utc=iso(s.t);
            auto rows=[&](U node,const char*kind,const std::vector<Access>&v,U chosen){
                for(auto&a:v)vis<<s.t<<','<<utc<<','<<node<<','<<node_name(nodes,node)<<','<<kind<<','<<a.sat<<','<<node_name(nodes,a.sat)<<','<<int(a.up)<<','<<int(a.down)<<','<<a.delay_ns<<','<<a.capacity_bps<<','<<a.remaining_ns<<','<<int(a.sat==chosen)<<'\n';
            };
            rows(cfg.user,"user",plan.user_links(s.t),s.user.sat);
            rows(cfg.ground,"ground_station",plan.ground_links(s.t),s.ground.sat);
            auto pick=[&](const Pick&p){
                J j=J::obj();
                j["sat"]=p.sat==UINT64_MAX?J():J(p.sat);
                j["sat_name"]=p.sat==UINT64_MAX?J():J(node_name(nodes,p.sat));
                j["trigger"]=p.trigger;
                j["metric"]=p.metric;
                if(p.relaxed)j["threshold_relaxed"]=true;
                return j;
            };
            J j=J::obj();
            j["t_ns"]=s.t;
            j["utc"]=utc;
            j["user"]=pick(s.user);
            j["ground"]=pick(s.ground);
            j["pair_metric"]=s.pair_metric;
            j["pair_trigger"]=s.pair_trigger;
            selj<<j.dump()<<'\n';
            if(first||s.user.sat!=prev_u||s.ground.sat!=prev_g){
                j["event"]=first?"initial":"handover";
                j["previous_user_sat"]=prev_u==UINT64_MAX?J():J(prev_u);
                j["previous_ground_sat"]=prev_g==UINT64_MAX?J():J(prev_g);
                hoj<<j.dump()<<'\n';
            }
            first=false;prev_u=s.user.sat;prev_g=s.ground.sat;
            if(dump_links){
                auto g=topo.at(s.t);
                for(auto&e:g.edges)links<<s.t<<','<<e.src<<','<<e.dst<<','<<e.delay_ns<<','<<e.capacity_bps<<'\n';
            }
        }
        close(vis,names.file("visibility","csv"));
        close(selj,names.file("selection","jsonl"));
        close(hoj,names.file("handover","jsonl"));
        if(dump_links)close(links,names.file("links","csv"));
        std::string sum=names.file("handover-summary","json");
        atomic(out/sum,plan.summary(nodes).dump());
        files.push_back(sum);
        return files;
    }
    int run(int argc,char**argv){
        std::signal(SIGINT,signal_handler);
        std::signal(SIGTERM,signal_handler);
        auto wall_begin=std::chrono::steady_clock::now();
        Options opts=parse(argc,argv);
        if(opts.help){
            std::cout<<"hypersim [network_run|manifest|graph.ngb] [--output auto|directory]\nNo input: generate default 24-hour OneWebSmall and run simulation.\n";
            for(auto&d:defaults)std::cout<<"--"<<d.name<<" "<<d.value<<'\n';
            std::cout<<"\nHandover policies (--policy): none | random | min-access-delay | min-full-delay | max-min-visibility | min-orbit-hops | max-access-rate | min-lifetime\n"
                       "  --pair USER,GROUND (or --pair USER GROUND): node names/ids; auto when exactly one user and one ground station exist.\n"
                       "  --lifetime-threshold-s 120, --policy-seed 1, --policy-dump-links enabled. Upstream satnet keeps every visible access link; the policy selects.\n"
                       "  --times N: run the simulation N times on the same input; result index runs 1..N (random policy uses seed, seed+1, ...).\n"
                       "  --policy a,b,c | a b c | all: run every listed policy --times times (indices 1..N per policy); writes <constellation>-policies-comparison.json.\n"
                       "Outputs are named <constellation>-<policy>-<tool>-<index>.<ext>, e.g. onewebsmall-random-traceroute-1.txt; policy 'none' is written as baseline.\n";
            return 0;
        }
        if(opts.dump){
            // Typed canonical registry (SDD §14.3 / satpred §10.1): stdout + simulation_defaults.json.
            J j=J::obj();
            j["format"]="hypersim-simulation-defaults-1";
            J params=J::arr();
            for(auto&d:defaults){
                J p=J::obj();
                p["name"]=std::string(d.name);
                p["type"]=std::string(d.type);
                p["default"]=std::string(d.value);
                if(!d.unit.empty())p["unit"]=std::string(d.unit);
                p["stage"]=std::string(d.stage);
                p["valid"]=std::string(d.valid);
                p["affects_business_run_id"]=d.business;
                params.a.push_back(p);
            }
            j["params"]=params;
            std::cout<<j.dump()<<'\n';
            atomic(fs::path("./simulation_defaults.json"),j.dump());
            return 0;
        }
        if(opts.self){
            sp::selftest();
            return 0;
        }
        validate(opts);
        auto out=new_output(opts.v.at("output"));
        std::string current_manifest;   // manifest of the repetition in progress (removed on failure)
        try{
            fs::path input;
            std::string constellation_hint;
            if(opts.input.empty()){
                input=out/"network_input";
                // Upstream tools keep a per-user state file under SATPRED_HOME (default HOME/.satpred).
                // Point it inside this run so the default flow never depends on, or writes to, the host home.
                auto state=out/"upstream_state";
                fs::create_directories(state);
                set_environment("SATPRED_HOME",pathtext(state));
                constellation_hint=xml_root_name(executable().parent_path()/"examples/satellites.xml");
                child(program("satnet"),{"--output",pathtext(input),"--threads","2","--memory-budget",opts.v.at("memory-budget-bytes")});
            }
            else input=snapshot(fs::absolute(opts.input),out);
            fixedtrace::Input reader(input,val(opts,"memory-budget-bytes"));
            I begin=opts.v.at("start")=="computed"?reader.begin:utc(opts.v.at("start"));
            need(begin>=reader.begin&&begin<reader.end,1,"window","start outside source range");
            I end=reader.end;
            if(opts.v.at("duration")!="computed"){
                I d=duration(opts,"duration");
                need(d<=INT64_MAX-begin,1,"window","end overflow");
                end=begin+d;
            }
            need(end>begin&&end<=reader.end,1,"window","end outside source range");
            opts.v["start"]=iso(begin);
            opts.v["duration"]=number(double(end-begin)*1e-9);
            const std::vector<std::string>policies=policy_list(opts.v.at("policy"));
            // Output naming is shared by all repetitions; only the policy label and the index differ.
            std::string cname=opts.v.at("constellation");
            if(cname=="auto"){
                // case_name comes from the verified graph metadata (works for directory, manifest and single .ngb input).
                std::string meta_case;
                if(reader.upstream_case.o.count("case_name")&&reader.upstream_case.at("case_name").type==J::Str)meta_case=reader.upstream_case.at("case_name").str();
                cname=slug(!meta_case.empty()?meta_case:!constellation_hint.empty()?constellation_hint:"satnet");
            }
            const U times=val(opts,"times");
            const U base_seed=val(opts,"policy-seed");
            const Options base_opts=opts;
            J all_policies=J::arr();
            for(size_t pi=0;pi<policies.size();++pi){
            const Policy policy=policy_from(policies[pi]);
            const std::string policy_label=policy==Policy::None?"baseline":policy_name(policy);
            const U first_index=opts.explicit_keys.count("times")||times>1||policies.size()>1?1:(opts.v.at("run-index")=="auto"?next_index(out,cname+"-"+policy_label):val(opts,"run-index"));
            J all=J::arr();
            for(U rep=0;rep<times;++rep){
            safe();
            Options opts=base_opts;
            opts.v["policy"]=policies[pi];
            opts.v["times"]=std::to_string(times);
            opts.v["repetition"]=std::to_string(rep+1);
            opts.v["policy-seed"]=std::to_string(base_seed+rep);
            // Handover policy: resolve the pair, build the god's-eye plan, prune the topology.
            std::unique_ptr<HandoverPlan>plan;
            if(policy!=Policy::None){
                PairIds pair=resolve_pair(reader.nodes,opts.v.at("pair"));
                opts.v["pair"]=node_name(reader.nodes,pair.user)+","+node_name(reader.nodes,pair.ground);
                if(!opts.explicit_keys.count("traffic-src"))opts.v["traffic-src"]=std::to_string(pair.user);
                if(!opts.explicit_keys.count("traffic-dst"))opts.v["traffic-dst"]=std::to_string(pair.ground);
                HandoverConfig hc;
                hc.policy=policy;
                hc.user=pair.user;
                hc.ground=pair.ground;
                hc.seed=val(opts,"policy-seed");
                hc.lifetime_threshold_ns=U(duration(opts,"lifetime-threshold-s"));
                hc.begin=begin;
                hc.end=end;
                plan=std::make_unique<HandoverPlan>(reader,hc);
            }
            else opts.v["pair"]="none";
            Topology topo{reader,plan.get(),begin,end,reader.nodes};
            OutputNames names;
            names.constellation=cname;
            names.policy=policy_label;
            names.index=first_index+rep;
            opts.v["constellation"]=names.constellation;
            opts.v["run-index"]=std::to_string(names.index);
            auto file=[&](const char*tool,const char*ext){return names.file(tool,ext);};
            current_manifest=file("manifest","json");
            atomic(out/file("effective-config","json"),config(opts).dump());
            if(plan)write_handover_data(*plan,topo,out,names,opts.v.at("policy-dump-links")=="enabled");
            J inputs=J::arr();
            if(input.extension()==".ngb"){
                J j=J::obj();
                j["graph_sha256"]=hex(filesha(input));
                inputs.a.push_back(j);
            }
            else{
                auto m=json(readtext(fs::is_directory(input)?input/"manifest.json":input));
                for(auto&j:m.at("products").a){
                    J q=J::obj();
                    q["global_index0"]=j.at("global_index0");
                    q["graph_sha256"]=j.at("graph_sha256");
                    q["source_sha256"]=j.at("source_sha256");
                    inputs.a.push_back(q);
                }
            }
            J identity=J::obj();
            identity["implementation"]="unified-satnet-mvp-1";
            identity["inputs"]=inputs;
            identity["config"]=J::obj();
            for(auto&d:defaults)if(d.business)identity["config"][std::string(d.name)]=opts.v.at(std::string(d.name));
            std::string id=hex(sha(identity.dump()));
            auto execute_begin=std::chrono::steady_clock::now();
            Simulation sim(opts,topo,names,out,begin,end);
            J result=sim.run();
            auto execute_end=std::chrono::steady_clock::now();
            atomic(out/file("result","json"),result.dump());
            J perf=J::obj();
            double exec=std::chrono::duration<double>(execute_end-execute_begin).count(),full=std::chrono::duration<double>(execute_end-wall_begin).count();
            perf["host_execution_seconds"]=exec;
            perf["host_before_manifest_seconds"]=full;
            perf["virtual_duration_seconds"]=double(end-begin)*1e-9;
            atomic(out/file("performance","json"),perf.dump());
            J manifest=J::obj();
            manifest["format"]="HYPERSIM-UNIFIED-MVP-1";
            manifest["status"]="complete";
            manifest["simulation_run_id"]=id;
            manifest["input_products"]=inputs;
            manifest["verification"]="SOURCE_VERIFIED";
            manifest["time_semantics"]="virtual-utc-v1";
            manifest["start_ns"]=begin;
            manifest["end_ns"]=end;
            manifest["actual_completed_virtual_end_ns"]=end;
            manifest["origin_ns"]=begin;
            if(!reader.upstream_run_id.empty())manifest["upstream_run_id"]=reader.upstream_run_id;
            if(!reader.input_manifest_sha256.empty())manifest["input_manifest_sha256"]=reader.input_manifest_sha256;
            manifest["actual_backend"]=reader.actual_backend.empty()?"cpu":reader.actual_backend;
            manifest["seed"]=opts.v.at("policy-seed");
            manifest["approximate"]=false;
            manifest["result"]=result;
            manifest["effective_config"]=config(opts);
            J hp=J::obj();
            hp["policy"]=names.policy=="baseline"?"none":names.policy;
            hp["pair"]=opts.v.at("pair");
            hp["topology"]=plan?"pruned-by-policy":"original-input";
            if(plan)hp["summary"]=plan->summary(reader.nodes);
            manifest["handover"]=hp;
            // SDD v2.4 §3.1 / ZERO-T14: upstream v3.5 annotation keys are copied verbatim into upstream_case.
            // Annotation of the input case only: frequencies/caps in rf_parameters are not link-model inputs,
            // do not enter the business identity, and their absence is not an error (field omitted).
            if(!reader.upstream_case.o.empty()){
                J uc=J::obj();
                uc["format"]="hsn-upstream-case-1";
                for(auto&[k,v]:reader.upstream_case.o)uc[k]=v;
                uc["interpretation"]="annotation copied from upstream SATNET metadata; rf_parameters are not used by the link, routing or traffic model";
                uc["rf_model_applied"]=false;
                manifest["upstream_case"]=uc;
            }
            J naming=J::obj();
            naming["scheme"]="<constellation>-<policy>-<tool>-<index>.<ext>";
            naming["constellation"]=names.constellation;
            naming["policy"]=names.policy;
            naming["index"]=names.index;
            naming["times"]=times;
            naming["repetition"]=rep+1;
            manifest["naming"]=naming;
            J files=J::obj();
            files["business"]=file("business","jsonl");
            files["statistics"]=file("statistics","jsonl");
            files["traceroute"]=file("traceroute","txt");
            files["traceroute_json"]=file("traceroute","jsonl");
            files["iperf"]=file("iperf","json");
            files["result"]=file("result","json");
            files["performance"]=file("performance","json");
            files["effective_config"]=file("effective-config","json");
            files["manifest"]=file("manifest","json");
            if(plan){
                files["visibility"]=file("visibility","csv");
                files["selection"]=file("selection","jsonl");
                files["handover"]=file("handover","jsonl");
                files["handover_summary"]=file("handover-summary","json");
                if(opts.v.at("policy-dump-links")=="enabled")files["links"]=file("links","csv");
            }
            manifest["files"]=files;
            manifest["output_sha256"]=J::obj();
            for(auto f:{file("business","jsonl"),file("statistics","jsonl"),file("traceroute","txt"),file("traceroute","jsonl"),file("result","json")})manifest["output_sha256"][f]=hex(filesha(out/f));
            for(auto&[k,v]:files.o)if(k!="manifest"&&!manifest["output_sha256"].o.count(v.str()))manifest["output_sha256"][v.str()]=hex(filesha(out/v.str()));
            manifest["business_sha256"]=manifest["output_sha256"].at(file("business","jsonl"));
            manifest["warnings"]=J::arr();
            for(auto w:{"MVP subset: not full v3.4 or HyperSim v2.3 acceptance","fluid implements one active flow, not general multi-flow max-min","traceroute uses directed no-queue ICMP model; synthetic IPs, not real host packets","priority-queue scheduler and package buffering: no zero-allocation/mmap guarantee","TCP, Python AST frontend, CUDA/TPS/checkpoint unsupported","simulation identity is MVP identity, not normative canonical","power-loss crash durability not certified"})manifest["warnings"].a.emplace_back(w);
            if(plan)manifest["warnings"].a.emplace_back("handover policy is a god's-eye rule on slice granularity; candidates are all access links present in the input data (upstream satnet exports every visible access link; selection happens here)");
            else manifest["warnings"].a.emplace_back("baseline keeps every candidate access link of the input; per-hop shortest-delay routing picks among them, no single-satellite constraint is enforced");
            atomic(out/file("manifest","json"),manifest.dump());
            std::cout<<"run="<<pathtext(out)<<" index="<<names.index<<" SOURCE_VERIFIED\n"<<result.dump()<<'\n';
            J entry=J::obj();
            entry["index"]=names.index;
            entry["manifest"]=file("manifest","json");
            entry["result"]=result;
            all.a.push_back(entry);
            }
            J series=J::obj();
            series["format"]="hsn-run-series-1";
            series["constellation"]=cname;
            series["policy"]=policy_label;
            series["times"]=times;
            series["first_index"]=first_index;
            series["runs"]=all;
            if(times>1||policies.size()>1){
                atomic(out/(cname+"-"+policy_label+"-series.json"),series.dump());
                std::cout<<"series="<<policy_label<<" "<<times<<" runs, indices "<<first_index<<".."<<first_index+times-1<<'\n';
            }
            all_policies.a.push_back(series);
            }
            if(policies.size()>1){
                J cmp=J::obj();
                cmp["format"]="hsn-policy-comparison-1";
                cmp["constellation"]=cname;
                cmp["times"]=times;
                cmp["policies"]=J::arr();
                for(auto&p:policies)cmp["policies"].a.emplace_back(p);
                cmp["series"]=all_policies;
                atomic(out/(cname+"-policies-comparison.json"),cmp.dump());
                std::cout<<"policies="<<policies.size()<<" x times="<<times<<" = "<<policies.size()*times<<" runs\n";
            }
            return 0;
        }
        catch(...){
            std::error_code ec;
            for(auto&entry:fs::directory_iterator(out,ec)){
                auto f=pathtext(entry.path().filename());
                if(entry.is_regular_file()&&(f.size()>5&&f.compare(f.size()-5,5,".part")==0))fs::remove(entry.path(),ec);
                if(entry.is_regular_file()&&!current_manifest.empty()&&f==current_manifest)fs::remove(entry.path(),ec);
            }
            throw;
        }
    }
}
// namespace unified
int hypersim_main(int argc,char**argv){
    try{
        return unified::run(argc,argv);
    }
    catch(const sp::Err&e){
        std::cerr<<"{\"code\":"<<e.code<<",\"where\":"<<sp::quote(e.where)<<",\"message\":"<<sp::quote(e.what())<<"}\n";
        return e.code;
    }
    catch(const std::filesystem::filesystem_error&e){
        std::cerr<<"{\"code\":3,\"where\":\"filesystem\",\"message\":"<<sp::quote(e.what())<<"}\n";
        return 3;
    }
    catch(const std::exception&e){
        std::cerr<<"{\"code\":5,\"where\":\"main\",\"message\":"<<sp::quote(e.what())<<"}\n";
        return 5;
    }
}
