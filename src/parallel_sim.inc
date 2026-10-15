// Included by simulation.cpp (single translation unit: reader_internal.hpp includes core.cpp).
//
// Parallel packet-model scheduler (SDD v2.3 §1.4 / §7.4 / §8.4 / §11).
//   * Worker threads are auto-detected from the virtual processor count
//     (std::thread::hardware_concurrency(); HSN_SIM_THREADS overrides it for tests).
//   * Every node is assigned to a worker round-robin by its label: owner(n) = n % T, so
//     adjacent node ids land on different workers whenever T > 1.
//   * A worker owns the links whose *source* node it owns, a private route cache and its own
//     counters; it never touches another worker's state. The main thread stays the only
//     committer of the SimClock and of the output files.
//   * Causal rule: a packet is only forwarded after its Arrival event has been committed, and
//     an Arrival is scheduled at now + propagation delay (> now), so a worker can never send a
//     packet it has not received and the simulated clock strictly increases along a path.
//   * Same-timestamp events are partitioned across workers; workers own disjoint links/nodes,
//     so their relative order cannot change any committed value -> deterministic per thread count.
#include "hsn/events.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
namespace unified {
    using namespace sp;
    namespace pptr = hsn::trace;

    // Auto-detect the virtual processor count; HSN_SIM_THREADS pins it for the 1/4/32-thread checks.
    inline unsigned sim_thread_count(){
        const char*env=std::getenv("HSN_SIM_THREADS");
        if(env&&*env){
            char*d=nullptr; long v=std::strtol(env,&d,10);
            if(d!=env&&v>0)return v>1024?1024u:unsigned(v);
        }
        unsigned hc=std::thread::hardware_concurrency();
        return hc?hc:1u;
    }

    class ParallelPacketEngine {
        using Node=pptr::Node;
        struct Worker {
            unsigned id=0;
            std::priority_queue<Event,std::vector<Event>,Compare> q;
            std::mutex qm;
            std::map<U,LinkState> links;
            std::map<U,std::vector<Node>> route_cache;
            U generated=0,delivered=0,dropped=0,queue_drop=0,link_drop=0,no_route_drop=0,hop_drop=0;
            U packet_seq=0;
            std::atomic<U> local_seq{0};
        };
        enum class Cmd { Idle, Graph, Time, Stop };
        const unsigned T;
        const U src,dst,offer,payload,capacity,maxh,budget;
        const I begin,end,flow_start;
        std::vector<std::unique_ptr<Worker>> workers;
        std::vector<std::thread> pool;
        std::shared_ptr<const pptr::Graph> graph;
        std::mutex m; std::condition_variable cv_work,cv_main;
        Cmd cmd=Cmd::Idle; U gen=0,acked=0; std::exception_ptr err;
        I cur_now;
        std::atomic<long long> queued{0};
        std::atomic<long long> inflight{0};
        U epoch=1;

        static U service(U scaled,U rate){ return scaled/rate+(scaled%rate!=0); }
        static I add_checked(I t,U d){ need(d<=U(INT64_MAX-t),7,"time","overflow"); return t+I(d); }
        unsigned owner(U n)const{ return unsigned(n%T); }

        void push(unsigned target,Event e){
            need(e.time>=cur_now,5,"event","scheduled in past");
            need(mul(U(queued.load())+1,512)<budget/4,2,"budget","event queue budget exhausted");
            Worker&w=*workers[target];
            e.seq=(U(epoch)<<48)|(U(target)<<40)|(w.local_seq.fetch_add(1)&((U(1)<<40)-1));
            std::lock_guard<std::mutex>lk(w.qm);
            queued.fetch_add(1);
            w.q.push(std::move(e));
        }

        void start_link(U key){
            Worker&w=*workers[owner(key>>32)];
            auto it=w.links.find(key);
            if(it==w.links.end())return;
            LinkState&l=it->second;
            if(!l.up||l.busy||l.queue.empty())return;
            l.active=l.queue.front(); l.queue.pop_front(); l.busy=true;
            l.remaining_scaled=mul(mul(payload+28,8),1000000000ULL);
            l.last=cur_now; ++l.generation;
            Event e; e.kind=EventKind::ServiceComplete; e.phase=EventPhase::Completion;
            e.key=key; e.generation=l.generation;
            e.time=add_checked(cur_now,service(l.remaining_scaled,l.edge.capacity_bps));
            push(owner(key>>32),std::move(e));
        }

        std::vector<Node> route(U at){
            Worker&w=*workers[owner(at)];
            auto it=w.route_cache.find(at);
            if(it!=w.route_cache.end())return it->second;
            auto p=pptr::shortest(*graph,std::uint32_t(at),std::uint32_t(dst));
            if(p.size()>maxh+1)p.clear();
            w.route_cache[at]=p;
            return p;
        }

        void forward(Worker&w,Packet p){
            if(p.node==dst){ ++w.delivered; return; }
            if(++p.hops>maxh){ ++w.dropped; ++w.hop_drop; return; }
            auto path=route(p.node);
            if(path.size()<2){ ++w.dropped; ++w.no_route_drop; return; }
            U key=(p.node<<32)|path[1];
            auto it=w.links.find(key);
            if(it==w.links.end()||!it->second.up){ ++w.dropped; ++w.link_drop; return; }
            LinkState&l=it->second;
            if(l.busy&&l.queue.size()>=capacity){ ++w.dropped; ++w.queue_drop; return; }
            l.queue.push_back(p);
            start_link(key);
        }

        void dispatch(Worker&w,const Event&e){
            if(e.kind==EventKind::Generate){
                Packet p{++w.packet_seq,src,0};
                ++w.generated;
                forward(w,p);
                U bits=payload*8;
                long double offset=std::ceil(static_cast<long double>(w.packet_seq)*bits*1e9L/offer);
                if(offset<=INT64_MAX-begin){
                    I t=begin+I(offset);
                    if(t<end){ Event n; n.time=t; n.kind=EventKind::Generate; n.phase=EventPhase::Traffic; push(w.id,std::move(n)); }
                }
            }
            else if(e.kind==EventKind::ServiceComplete){
                auto it=w.links.find(e.key);
                if(it==w.links.end())return;
                LinkState&l=it->second;
                if(!l.busy||l.generation!=e.generation)return;
                Packet p=l.active; p.node=l.edge.dst; l.busy=false;
                inflight.fetch_add(1);
                Event a; a.time=add_checked(cur_now,l.edge.delay_ns); a.phase=EventPhase::Completion;
                a.kind=EventKind::Arrival; a.packet=p;
                push(owner(p.node),std::move(a));
                start_link(e.key);
            }
            else if(e.kind==EventKind::Arrival){
                inflight.fetch_sub(1);
                forward(w,e.packet);
            }
        }

        void run_time(Worker&w){
            U micro=0;
            for(;;){
                Event e; bool have=false;
                { std::lock_guard<std::mutex>lk(w.qm);
                  if(!w.q.empty()&&w.q.top().time<=cur_now){ e=w.q.top(); w.q.pop(); have=true; queued.fetch_sub(1); } }
                if(!have)break;
                need(e.time==cur_now,5,"loop","event before current time");
                need(++micro<=1000000,5,"loop","zero-time livelock");
                dispatch(w,e);
            }
        }

        void set_graph(Worker&w){
            const pptr::Graph&g=*graph;
            std::map<U,pptr::Link>fresh;
            for(auto&e:g.edges)if(owner(e.src)==w.id)fresh[(U(e.src)<<32)|e.dst]=e;
            for(auto&kv:w.links){
                U key=kv.first; LinkState&l=kv.second;
                if(l.busy&&U(cur_now-l.last)>=service(l.remaining_scaled,l.edge.capacity_bps)){
                    Packet done_p=l.active; done_p.node=l.edge.dst; l.busy=false; ++l.generation;
                    inflight.fetch_add(1);
                    Event a; a.time=add_checked(cur_now,l.edge.delay_ns); a.phase=EventPhase::Completion;
                    a.kind=EventKind::Arrival; a.packet=done_p;
                    push(owner(done_p.node),std::move(a));
                }
                auto it=fresh.find(key);
                if(it==fresh.end()){
                    if(l.busy){ ++w.dropped; ++w.link_drop; l.busy=false; ++l.generation; }
                    w.dropped+=l.queue.size(); w.link_drop+=l.queue.size(); l.queue.clear(); l.up=false;
                    continue;
                }
                if(l.busy&&l.edge.capacity_bps!=it->second.capacity_bps){
                    U elapsed=U(cur_now-l.last);
                    U served=elapsed>l.remaining_scaled/l.edge.capacity_bps?l.remaining_scaled:elapsed*l.edge.capacity_bps;
                    l.remaining_scaled-=served; l.last=cur_now; ++l.generation; l.edge=it->second;
                    Event e; e.kind=EventKind::ServiceComplete; e.phase=EventPhase::Completion;
                    e.key=key; e.generation=l.generation;
                    e.time=add_checked(cur_now,service(l.remaining_scaled,l.edge.capacity_bps));
                    push(w.id,std::move(e));
                }
                l.edge=it->second; l.up=true; fresh.erase(it);
            }
            for(auto&kv:fresh){ LinkState l; l.edge=kv.second; l.up=true; w.links.emplace(kv.first,std::move(l)); }
            w.route_cache.clear();
            for(auto&kv:w.links)start_link(kv.first);
        }

        void loop(unsigned idx){
            Worker&w=*workers[idx];
            U seen=0;
            for(;;){
                Cmd c;
                { std::unique_lock<std::mutex>lk(m); cv_work.wait(lk,[&]{return gen!=seen;}); seen=gen; c=cmd; }
                if(c==Cmd::Stop)return;
                try{
                    if(c==Cmd::Graph)set_graph(w);
                    else run_time(w);
                }
                catch(...){ std::lock_guard<std::mutex>lk(m); if(!err)err=std::current_exception(); }
                { std::lock_guard<std::mutex>lk(m); if(++acked==T)cv_main.notify_all(); }
            }
        }

        void issue(Cmd c){
            { std::lock_guard<std::mutex>lk(m); cmd=c; ++gen; acked=0; }
            cv_work.notify_all();
            std::unique_lock<std::mutex>lk(m);
            cv_main.wait(lk,[&]{return acked==T;});
            if(err){ auto e=err; err=nullptr; std::rethrow_exception(e); }
        }

    public:
        ParallelPacketEngine(unsigned threads,U s,U d,U off,U pay,U cap,U hops,U bud,I b,I e,I fs)
            :T(threads?threads:1u),src(s),dst(d),offer(off),payload(pay),capacity(cap),maxh(hops),budget(bud),
             begin(b),end(e),flow_start(fs),cur_now(b){
            for(unsigned i=0;i<T;++i){ auto w=std::make_unique<Worker>(); w->id=i; workers.push_back(std::move(w)); }
            for(unsigned i=0;i<T;++i)pool.emplace_back([this,i]{ loop(i); });
        }
        ~ParallelPacketEngine(){ stop(); }
        void stop(){
            if(pool.empty())return;
            { std::lock_guard<std::mutex>lk(m); cmd=Cmd::Stop; ++gen; }
            cv_work.notify_all();
            for(auto&t:pool)if(t.joinable())t.join();
            pool.clear();
        }
        unsigned threads()const{ return T; }
        // Seed the self-sustaining generation loop at max(begin, business start offset).
        void seed(){
            Event e; e.time=std::max(begin,flow_start); e.kind=EventKind::Generate; e.phase=EventPhase::Traffic;
            push(owner(src),std::move(e));
        }
        void on_topology_change(const pptr::Graph&g,I now){ graph=std::make_shared<const pptr::Graph>(g); cur_now=now; ++epoch; issue(Cmd::Graph); }
        I earliest(){
            I best=end;
            for(auto&w:workers){ std::lock_guard<std::mutex>lk(w->qm); if(!w->q.empty()&&w->q.top().time<best)best=w->q.top().time; }
            return best;
        }
        void run_timestamp(I now){ cur_now=now; issue(Cmd::Time); }
        struct Totals { U generated=0,delivered=0,dropped=0,queue_drop=0,link_drop=0,no_route_drop=0,hop_drop=0,inflight=0,censored=0; };
        Totals totals(){
            Totals t;
            for(auto&w:workers){
                t.generated+=w->generated; t.delivered+=w->delivered; t.dropped+=w->dropped;
                t.queue_drop+=w->queue_drop; t.link_drop+=w->link_drop;
                t.no_route_drop+=w->no_route_drop; t.hop_drop+=w->hop_drop;
                std::lock_guard<std::mutex>lk(w->qm);
                for(auto&kv:w->links)t.censored+=kv.second.queue.size()+(kv.second.busy?1:0);
            }
            t.inflight=U(inflight.load());
            t.censored+=t.inflight;
            return t;
        }
    };
}
