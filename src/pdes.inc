// Included by simulation.cpp (single translation unit: reader_internal.hpp includes core.cpp).
//
// Fully decentralised, lock-free parallel discrete-event simulation (PDES) for the packet model.
//   * Worker threads are auto-detected from the virtual processor count (std::thread::hardware_concurrency();
//     HSN_SIM_THREADS pins it for the thread-count checks).
//   * Every node is owned by exactly one worker, round-robin by label: owner(n) = n % T.
//   * There is NO central scheduler, NO global time barrier and NO mutex on the event path. Once released,
//     every worker runs its own independent event loop over its private event heap, link table (a link is
//     owned by its source node), route cache and counters. The only coupling is lock-free message passing.
//   * The fabric is a lock-free MPSC stack per worker (atomic exchange/CAS).
//   * Conservative lookahead (Chandy-Misra-Bryant style, implemented with shared atomics instead of null
//     messages): a worker publishes the timestamp of its earliest pending event and only commits an event
//     whose timestamp is not later than the global minimum, so no worker ever commits out of order.
//   * Topology is time-indexed: the control participant materialises every topology epoch BEFORE the workers
//     are released and publishes an immutable (time, graph) table. A worker looks up the graph that is valid
//     at the timestamp of the event it is handling, so routing stays correct even if a partition runs ahead.
//   * Causal rule: a packet is only forwarded after its Arrival has been committed, and an Arrival is always
//     scheduled at send_time + propagation delay (> send_time), so a worker can never send what it has not
//     received and a packet's timestamp strictly increases along its path.
//   * Timestamps come from the simulated clock handed to the test applications; physical completion order is
//     irrelevant and the result of a run is not bit-reproducible in general (same input -> results inside a
//     bounded, documented range). Per-interval counters keep the statistics boundaries meaningful.
#include "hsn/events.hpp"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <tuple>
#include <vector>
namespace unified {
    using namespace sp;
    namespace pptr = hsn::trace;

    // Auto-detect the virtual processor count; HSN_SIM_THREADS pins it for the thread-count checks.
    inline unsigned sim_thread_count(){
        const char*env=std::getenv("HSN_SIM_THREADS");
        if(env&&*env){
            char*d=nullptr; long v=std::strtol(env,&d,10);
            if(d!=env&&v>0)return v>1024?1024u:unsigned(v);
        }
        unsigned hc=std::thread::hardware_concurrency();
        return hc?hc:1u;
    }

    class PdesEngine {
        using Node=pptr::Node;
        struct Epoch { I t=0; std::shared_ptr<const pptr::Graph> g; };
        struct Item { Event e; };
        struct ItemCmp {
            bool operator()(const Item&a,const Item&b)const{
                return std::tie(a.e.time,a.e.phase,a.e.seq)>std::tie(b.e.time,b.e.phase,b.e.seq);
            }
        };
        // Lock-free MPSC message: producers CAS it onto the target's inbox stack, the owner pops it.
        struct Msg { Event e; Msg* next=nullptr; };
        struct Worker {
            unsigned id=0;
            std::atomic<Msg*> inbox{nullptr};
            std::priority_queue<Item,std::vector<Item>,ItemCmp> ready;   // owner only
            std::map<U,LinkState> links;                                 // owner only
            std::map<U,std::vector<Node>> route_cache;                   // owner only
            size_t applied=0;                                            // owner only
            bool seeded=false;                                           // owner only
            U packet_seq=0;                                              // owner only
            std::atomic<U> generated{0},delivered{0},dropped{0},queue_drop{0},link_drop{0},no_route_drop{0},hop_drop{0};
            std::atomic<long long> queued{0},busy{0};
            std::atomic<I> clock{0};
            std::atomic<I> next{(std::numeric_limits<I>::max)()};
            std::atomic<bool> idle{false};
            std::vector<std::atomic<U>> iv_gen,iv_del,iv_drop;
        };
        const unsigned T;
        const U src,dst,offer,payload,capacity,maxh,budget;
        const I begin,end,flow_start,report_origin;
        const U interval_ns;
        const size_t intervals;
        std::vector<std::unique_ptr<Worker>> workers;
        std::vector<std::thread> pool;
        std::vector<Epoch> epochs;                 // written by control, read-only once started
        std::atomic<long long> live{0};
        std::atomic<long long> inflight{0};
        std::atomic<U> seq_gen{0};
        std::atomic<bool> started{false},control_done{false},failed{false};
        std::mutex err_m; std::exception_ptr err;

        static U service(U scaled,U rate){ return scaled/rate+(scaled%rate!=0); }
        static I add_checked(I t,U d){ need(d<=U(INT64_MAX-t),7,"time","overflow"); return t+I(d); }
        unsigned owner(U n)const{ return unsigned(n%T); }
        size_t slot(I t)const{
            if(t<=report_origin)return 0;
            U d=U(t-report_origin)/interval_ns;
            return d<intervals?size_t(d):intervals-1;
        }
        // Index of the topology epoch in force at simulated time t (epochs are sorted by time).
        size_t epoch_at(I t)const{
            size_t lo=0,hi=epochs.size();
            while(lo<hi){ size_t mid=(lo+hi)/2; if(epochs[mid].t<=t)lo=mid+1; else hi=mid; }
            return lo?lo-1:0;
        }

        // Lock-free MPSC push: count the message first so that "live==0" can never be observed while an
        // event is queued or being processed anywhere in the fabric.
        void push(unsigned target,Event e){
            need(e.time>=begin,5,"event","scheduled before begin");
            need(mul(U(live.load())+1,512)<budget/4,2,"budget","event queue budget exhausted");
            e.seq=seq_gen.fetch_add(1,std::memory_order_relaxed);
            live.fetch_add(1,std::memory_order_relaxed);
            Msg*m=new Msg{std::move(e),nullptr};
            std::atomic<Msg*>&head=workers[target]->inbox;
            Msg*cur=head.load(std::memory_order_relaxed);
            do{ m->next=cur; }while(!head.compare_exchange_weak(cur,m,std::memory_order_release,std::memory_order_relaxed));
        }
        void drain(Worker&w){
            Msg*list=w.inbox.exchange(nullptr,std::memory_order_acquire);
            while(list){
                Msg*n=list->next;
                w.ready.push(Item{list->e});
                delete list;
                list=n;
            }
        }

        // Route lookup for a given epoch; the cache key carries the epoch so a topology change invalidates it.
        std::vector<Node> route(Worker&w,U node,size_t e){
            U ck=(U(e)<<32)|node;
            auto it=w.route_cache.find(ck);
            if(it!=w.route_cache.end())return it->second;
            auto p=pptr::shortest(*epochs[e].g,std::uint32_t(node),std::uint32_t(dst));
            if(p.size()>maxh+1)p.clear();
            w.route_cache[ck]=p;
            return p;
        }
        void start_link(Worker&w,U key,I at){
            auto it=w.links.find(key);
            if(it==w.links.end())return;
            LinkState&l=it->second;
            if(!l.up||l.busy||l.queue.empty())return;
            l.active=l.queue.front(); l.queue.pop_front(); l.busy=true;
            w.queued.fetch_sub(1,std::memory_order_relaxed);
            w.busy.fetch_add(1,std::memory_order_relaxed);
            l.remaining_scaled=mul(mul(payload+28,8),1000000000ULL);
            l.last=at; ++l.generation;
            Event e; e.kind=EventKind::ServiceComplete; e.phase=EventPhase::Completion;
            e.key=key; e.generation=l.generation;
            e.time=add_checked(at,service(l.remaining_scaled,l.edge.capacity_bps));
            push(w.id,std::move(e));
        }
        void forward(Worker&w,Packet p,I at){
            if(p.node==dst){ w.delivered.fetch_add(1,std::memory_order_relaxed); w.iv_del[slot(at)].fetch_add(1,std::memory_order_relaxed); return; }
            if(++p.hops>maxh){ w.dropped.fetch_add(1,std::memory_order_relaxed); w.hop_drop.fetch_add(1,std::memory_order_relaxed); w.iv_drop[slot(at)].fetch_add(1,std::memory_order_relaxed); return; }
            size_t e=epoch_at(at); sync_epoch(w,e);
            auto path=route(w,p.node,e);
            if(path.size()<2){ w.dropped.fetch_add(1,std::memory_order_relaxed); w.no_route_drop.fetch_add(1,std::memory_order_relaxed); w.iv_drop[slot(at)].fetch_add(1,std::memory_order_relaxed); return; }
            U key=(p.node<<32)|path[1];
            auto it=w.links.find(key);
            if(it==w.links.end()||!it->second.up){ w.dropped.fetch_add(1,std::memory_order_relaxed); w.link_drop.fetch_add(1,std::memory_order_relaxed); w.iv_drop[slot(at)].fetch_add(1,std::memory_order_relaxed); return; }
            LinkState&l=it->second;
            if(l.busy&&l.queue.size()>=capacity){ w.dropped.fetch_add(1,std::memory_order_relaxed); w.queue_drop.fetch_add(1,std::memory_order_relaxed); w.iv_drop[slot(at)].fetch_add(1,std::memory_order_relaxed); return; }
            l.queue.push_back(p);
            w.queued.fetch_add(1,std::memory_order_relaxed);
            start_link(w,key,at);
        }

        void dispatch(Worker&w,const Event&e){
            I at=e.time;
            if(e.kind==EventKind::Generate){
                Packet p{++w.packet_seq,src,0};
                w.generated.fetch_add(1,std::memory_order_relaxed);
                w.iv_gen[slot(at)].fetch_add(1,std::memory_order_relaxed);
                forward(w,p,at);
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
                w.busy.fetch_sub(1,std::memory_order_relaxed);
                inflight.fetch_add(1,std::memory_order_relaxed);
                Event a; a.time=add_checked(at,l.edge.delay_ns); a.phase=EventPhase::Completion;
                a.kind=EventKind::Arrival; a.packet=p;
                push(owner(p.node),std::move(a));
                start_link(w,e.key,at);
            }
            else if(e.kind==EventKind::Arrival){
                inflight.fetch_sub(1,std::memory_order_relaxed);
                forward(w,e.packet,at);
            }
        }

        // Adopt a topology epoch for this partition only (links are owned by their source node). Called
        // lazily in time order when the partition first handles an event at/after the epoch's timestamp.
        void transition(Worker&w,const pptr::Graph&g,I at){
            std::map<U,pptr::Link>fresh;
            for(auto&e:g.edges)if(owner(e.src)==w.id)fresh[(U(e.src)<<32)|e.dst]=e;
            for(auto&kv:w.links){
                U key=kv.first; LinkState&l=kv.second;
                if(l.busy&&at>=l.last&&U(at-l.last)>=service(l.remaining_scaled,l.edge.capacity_bps)){
                    Packet dp=l.active; dp.node=l.edge.dst; l.busy=false; ++l.generation;
                    w.busy.fetch_sub(1,std::memory_order_relaxed);
                    inflight.fetch_add(1,std::memory_order_relaxed);
                    Event a; a.time=add_checked(at,l.edge.delay_ns); a.phase=EventPhase::Completion;
                    a.kind=EventKind::Arrival; a.packet=dp;
                    push(owner(dp.node),std::move(a));
                }
                auto it=fresh.find(key);
                if(it==fresh.end()){
                    size_t n=l.queue.size();
                    if(l.busy){ w.dropped.fetch_add(1,std::memory_order_relaxed); w.link_drop.fetch_add(1,std::memory_order_relaxed); l.busy=false; w.busy.fetch_sub(1,std::memory_order_relaxed); ++l.generation; }
                    w.dropped.fetch_add(U(n),std::memory_order_relaxed); w.link_drop.fetch_add(U(n),std::memory_order_relaxed);
                    for(size_t i=0;i<n;++i)w.iv_drop[slot(at)].fetch_add(1,std::memory_order_relaxed);
                    w.queued.fetch_sub((long long)n,std::memory_order_relaxed);
                    l.queue.clear(); l.up=false;
                    continue;
                }
                if(l.busy&&l.edge.capacity_bps!=it->second.capacity_bps){
                    U elapsed=(at>l.last)?U(at-l.last):0;
                    U served=elapsed>l.remaining_scaled/l.edge.capacity_bps?l.remaining_scaled:elapsed*l.edge.capacity_bps;
                    l.remaining_scaled-=served; l.last=at; ++l.generation; l.edge=it->second;
                    Event e; e.kind=EventKind::ServiceComplete; e.phase=EventPhase::Completion;
                    e.key=key; e.generation=l.generation;
                    e.time=add_checked(at,service(l.remaining_scaled,l.edge.capacity_bps));
                    push(w.id,std::move(e));
                }
                l.edge=it->second; l.up=true; fresh.erase(it);
            }
            for(auto&kv:fresh){ LinkState l; l.edge=kv.second; l.up=true; w.links.emplace(kv.first,std::move(l)); }
            for(auto&kv:w.links)start_link(w,kv.first,at);
        }
        void sync_epoch(Worker&w,size_t e){
            if(!w.seeded){ w.seeded=true; transition(w,*epochs[0].g,epochs[0].t); }
            while(w.applied<e){ ++w.applied; transition(w,*epochs[w.applied].g,epochs[w.applied].t); }
        }

        // A failed worker gives its undelivered events back to the global counter so that the other
        // workers (and the control participant) can observe termination instead of spinning forever.
        void abandon(Worker&w){
            Msg*list=w.inbox.exchange(nullptr,std::memory_order_acquire);
            long long n=0;
            while(list){ Msg*x=list->next; delete list; list=x; ++n; }
            n+=(long long)w.ready.size();
            while(!w.ready.empty())w.ready.pop();
            live.fetch_sub(n,std::memory_order_relaxed);
        }
        void run_worker(unsigned idx){
            Worker&w=*workers[idx];
            try{
                for(;;){
                    drain(w);
                    // Conservative lookahead: publish this partition's earliest pending timestamp, then only
                    // commit an event whose timestamp is not later than the global minimum. No worker can
                    // then still produce an earlier event, so commits never happen out of order.
                    w.next.store(w.ready.empty()?(std::numeric_limits<I>::max)():w.ready.top().e.time,std::memory_order_seq_cst);
                    I safe=(std::numeric_limits<I>::max)();
                    for(auto&o:workers){ I v=o->next.load(std::memory_order_seq_cst); if(v<safe)safe=v; }
                    if(!w.ready.empty()&&w.ready.top().e.time<=safe){
                        Event e=w.ready.top().e;
                        w.ready.pop();
                        if(!epochs.empty()&&e.time<end)dispatch(w,e);
                        if(e.time>w.clock.load(std::memory_order_relaxed))w.clock.store(e.time,std::memory_order_relaxed);
                        live.fetch_sub(1,std::memory_order_relaxed);
                        continue;
                    }
                    if(live.load(std::memory_order_relaxed)==0&&control_done.load(std::memory_order_relaxed))break;
                    w.idle.store(true,std::memory_order_relaxed);
                    std::this_thread::yield();
                    w.idle.store(false,std::memory_order_relaxed);
                }
            }
            catch(...){
                { std::lock_guard<std::mutex>lk(err_m); if(!err)err=std::current_exception(); }
                abandon(w);
                failed.store(true,std::memory_order_relaxed);
            }
        }

    public:
        struct Totals { U generated=0,delivered=0,dropped=0,queue_drop=0,link_drop=0,no_route_drop=0,hop_drop=0,inflight=0,censored=0; };
        struct Interval { U generated=0,delivered=0,dropped=0; };

        PdesEngine(unsigned threads,U s,U d,U off,U pay,U cap,U hops,U bud,I b,I e,I fs,I origin,U interval)
            :T(threads?threads:1u),src(s),dst(d),offer(off),payload(pay),capacity(cap),maxh(hops),budget(bud),
             begin(b),end(e),flow_start(fs),report_origin(origin),interval_ns(interval?interval:1),
             intervals(size_t((e>origin?U(e-origin):0)/interval_ns)+2){
            epochs.reserve(1024);
            for(unsigned i=0;i<T;++i){
                auto w=std::make_unique<Worker>(); w->id=i;
                w->iv_gen=std::vector<std::atomic<U>>(intervals);
                w->iv_del=std::vector<std::atomic<U>>(intervals);
                w->iv_drop=std::vector<std::atomic<U>>(intervals);
                for(size_t k=0;k<intervals;++k){ w->iv_gen[k].store(0); w->iv_del[k].store(0); w->iv_drop[k].store(0); }
                workers.push_back(std::move(w));
            }
        }
        ~PdesEngine(){ finish(); }
        unsigned threads()const{ return T; }
        size_t interval_count()const{ return intervals; }
        void start(){ if(started.exchange(true))return; for(unsigned i=0;i<T;++i)pool.emplace_back([this,i]{ run_worker(i); }); }
        void stop(){ if(!started.exchange(false))return; control_done.store(true,std::memory_order_relaxed); for(auto&t:pool)if(t.joinable())t.join(); pool.clear(); }
        void finish(){ stop(); if(err){ auto e=err; err=nullptr; std::rethrow_exception(e); } }
        // Publish one topology epoch. Called by the control participant while it walks its own timeline, i.e.
        // strictly before the workers are released, so the table is immutable while the workers read it.
        void broadcast_graph(const pptr::Graph&g,I t){
            need(!started.load(),5,"pdes","topology published after workers started");
            epochs.push_back(Epoch{t,std::make_shared<const pptr::Graph>(g)});
        }
        void seed(){
            Event e; e.time=std::max(begin,flow_start); e.kind=EventKind::Generate; e.phase=EventPhase::Traffic;
            push(owner(src),std::move(e));
        }
        void wait_quiescent(){
            while(live.load(std::memory_order_relaxed)!=0&&!failed.load(std::memory_order_relaxed))std::this_thread::yield();
            if(err){ auto e=err; err=nullptr; std::rethrow_exception(e); }
        }
        Totals totals(){
            Totals t;
            for(auto&w:workers){
                t.generated+=w->generated.load(); t.delivered+=w->delivered.load(); t.dropped+=w->dropped.load();
                t.queue_drop+=w->queue_drop.load(); t.link_drop+=w->link_drop.load();
                t.no_route_drop+=w->no_route_drop.load(); t.hop_drop+=w->hop_drop.load();
                t.censored+=U(w->queued.load())+U(w->busy.load());
            }
            t.inflight=U(inflight.load());
            t.censored+=t.inflight;
            return t;
        }
        Interval interval_totals(size_t idx){
            Interval v;
            for(auto&w:workers){
                v.generated+=w->iv_gen[idx].load(); v.delivered+=w->iv_del[idx].load(); v.dropped+=w->iv_drop[idx].load();
            }
            return v;
        }
    };
}
