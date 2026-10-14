// Included by simulation.cpp (single translation unit: reader_internal.hpp includes core.cpp).
#include "hsn/handover.hpp"
#include <queue>
namespace unified {
    using namespace sp;
    namespace tr=hsn::trace;
    Policy policy_from(const std::string&n){
        if(n=="none")return Policy::None;
        if(n=="random")return Policy::Random;
        if(n=="min-access-delay")return Policy::MinAccessDelay;
        if(n=="min-full-delay")return Policy::MinFullDelay;
        if(n=="max-min-visibility")return Policy::MaxMinVisibility;
        if(n=="min-orbit-hops")return Policy::MinOrbitHops;
        if(n=="max-access-rate")return Policy::MaxAccessRate;
        if(n=="min-lifetime")return Policy::MinLifetime;
        fail(1,"policy","unknown handover policy "+n);
    }
    std::string policy_name(Policy p){
        switch(p){
            case Policy::None:return "none";
            case Policy::Random:return "random";
            case Policy::MinAccessDelay:return "min-access-delay";
            case Policy::MinFullDelay:return "min-full-delay";
            case Policy::MaxMinVisibility:return "max-min-visibility";
            case Policy::MinOrbitHops:return "min-orbit-hops";
            case Policy::MaxAccessRate:return "max-access-rate";
            case Policy::MinLifetime:return "min-lifetime";
        }
        return "none";
    }
    bool pair_policy(Policy p){
        return p==Policy::MinFullDelay||p==Policy::MaxMinVisibility||p==Policy::MinOrbitHops;
    }
    bool expiry_triggered(Policy p){
        return p==Policy::Random||p==Policy::MaxMinVisibility||p==Policy::MinLifetime;
    }
    std::string node_name(const J&nodes,U id){
        if(id>=nodes.a.size())return "";
        auto&n=nodes.a[size_t(id)];
        return n.o.count("name")?n.at("name").str():std::to_string(id);
    }
    static U find_node(const J&nodes,const std::string&token){
        for(auto&n:nodes.a)if(n.o.count("name")&&n.at("name").str()==token)return n.at("id").u();
        bool digits=!token.empty()&&std::all_of(token.begin(),token.end(),[](unsigned char c){return std::isdigit(c);});
        need(digits,1,"pair","unknown node name "+token);
        U id=sp::uint(token);
        need(id<nodes.a.size(),1,"pair","node id outside graph: "+token);
        return id;
    }
    PairIds resolve_pair(const J&nodes,const std::string&spec){
        std::vector<U>users,grounds;
        for(auto&n:nodes.a){
            U k=n.at("kind").u();
            if(k==1)users.push_back(n.at("id").u());
            else if(k==2)grounds.push_back(n.at("id").u());
        }
        need(!users.empty()&&!grounds.empty(),1,"pair","handover policy needs at least one user and one ground station in the graph");
        if(spec=="auto"){
            need(users.size()==1&&grounds.size()==1,1,"pair","several users/ground stations: specify --pair USER,GROUND");
            return {users[0],grounds[0]};
        }
        auto sep=spec.find_first_of(",:");
        need(sep!=std::string::npos&&sep>0&&sep+1<spec.size(),1,"pair","expected USER,GROUND");
        U a=find_node(nodes,spec.substr(0,sep)),b=find_node(nodes,spec.substr(sep+1));
        U ka=nodes.a[size_t(a)].at("kind").u(),kb=nodes.a[size_t(b)].at("kind").u();
        if(ka==2&&kb==1)std::swap(a,b),std::swap(ka,kb);
        need(ka==1&&kb==2,1,"pair","pair must be one user terminal and one ground station");
        return {a,b};
    }
    // Dijkstra over the satellite network with the two pair endpoints removed.
    static std::vector<U> reach(const tr::Graph&g,U from,U ban1,U ban2,bool hops){
        std::vector<std::vector<std::pair<U,U>>>adj(g.node_count);
        for(auto&e:g.edges){
            if(e.src==ban1||e.src==ban2||e.dst==ban1||e.dst==ban2)continue;
            adj[e.src].push_back({e.dst,hops?U(1):e.delay_ns});
        }
        std::vector<U>d(g.node_count,UINT64_MAX);
        using Q=std::pair<U,U>;
        std::priority_queue<Q,std::vector<Q>,std::greater<Q>>q;
        d[size_t(from)]=0;
        q.push({0,from});
        while(!q.empty()){
            auto [dist,v]=q.top();
            q.pop();
            if(dist>d[size_t(v)])continue;
            for(auto&[w,c]:adj[size_t(v)]){
                U nd=add(dist,c);
                if(nd<d[size_t(w)]){
                    d[size_t(w)]=nd;
                    q.push({nd,w});
                }
            }
        }
        return d;
    }
    static const Access* find_access(const std::vector<Access>&v,U sat){
        for(auto&a:v)if(a.sat==sat)return &a;
        return nullptr;
    }
    size_t HandoverPlan::slice(I t)const{
        need(t>=data_begin&&t<data_end,1,"handover","time outside input coverage");
        return size_t((t-data_begin)/step);
    }
    const Selection& HandoverPlan::at(I t)const{
        need(t>=cfg.begin&&t<cfg.end,1,"handover","time outside selection window");
        size_t j=slice(t)-window_first;
        need(j<sel.size(),5,"handover","selection index");
        return sel[j];
    }
    HandoverPlan::HandoverPlan(fixedtrace::Input&input,HandoverConfig c):cfg(c){
        step=input.step;
        data_begin=input.begin;
        data_end=input.end;
        need(step>0,7,"handover","zero step");
        need(cfg.begin>=data_begin&&cfg.end<=data_end&&cfg.begin<cfg.end,1,"handover","selection window outside input");
        auto is_sat=[&](U id){return id<input.nodes.a.size()&&input.nodes.a[size_t(id)].at("kind").u()==0;};
        size_t M=size_t((data_end-data_begin+step-1)/step);
        user_access.assign(M,{});
        ground_access.assign(M,{});
        // Pass 1: collect every access link of the pair endpoints in every data slice.
        for(size_t j=0;j<M;++j){
            safe();
            auto g=input.at(data_begin+I(j)*step);
            auto collect=[&](U node,std::vector<Access>&out){
                std::map<U,Access>m;
                for(auto&e:g.edges){
                    U other;bool up;
                    if(e.src==node&&is_sat(e.dst)){other=e.dst;up=true;}
                    else if(e.dst==node&&is_sat(e.src)){other=e.src;up=false;}
                    else continue;
                    auto&a=m[other];
                    a.sat=other;
                    a.delay_ns=std::max(a.delay_ns,e.delay_ns);
                    a.capacity_bps=a.capacity_bps?std::min(a.capacity_bps,e.capacity_bps):e.capacity_bps;
                    (up?a.up:a.down)=true;
                }
                for(auto&[k,a]:m)out.push_back(a);
            };
            collect(cfg.user,user_access[j]);
            collect(cfg.ground,ground_access[j]);
        }
        // Pass 2: remaining visibility = consecutive future slices (data horizon is the limit).
        auto backward=[&](std::vector<std::vector<Access>>&v){
            for(size_t j=M;j-->0;){
                for(auto&a:v[j]){
                    const Access*n=j+1<M?find_access(v[j+1],a.sat):nullptr;
                    a.remaining_ns=add(U(step),n?n->remaining_ns:0);
                }
            }
        };
        backward(user_access);
        backward(ground_access);
        // Pass 3: apply the policy over the selection window.
        window_first=size_t((cfg.begin-data_begin)/step);
        size_t window_last=size_t((cfg.end-data_begin-1)/step);
        std::mt19937_64 rng(cfg.seed);
        Pick pu,pg;
        auto better_choice=[&](Policy p,const std::vector<Access>&cand,const Pick&prev)->Pick{
            Pick r;
            if(cand.empty())return r;
            const Access*best=nullptr;
            for(auto&a:cand){
                U m=p==Policy::MaxAccessRate?a.capacity_bps:a.delay_ns;
                if(!best){best=&a;continue;}
                U bm=p==Policy::MaxAccessRate?best->capacity_bps:best->delay_ns;
                bool improves=p==Policy::MaxAccessRate?m>bm:m<bm;
                bool tie=m==bm&&(a.sat==prev.sat||(best->sat!=prev.sat&&a.sat<best->sat));
                if(improves||tie)best=&a;
            }
            r.sat=best->sat;
            r.metric=p==Policy::MaxAccessRate?best->capacity_bps:best->delay_ns;
            bool prev_visible=prev.sat!=UINT64_MAX&&find_access(cand,prev.sat);
            r.trigger=prev.sat==UINT64_MAX?"initial":r.sat==prev.sat?"hold":prev_visible?"better":"expiry";
            return r;
        };
        auto expiry_choice=[&](Policy p,const std::vector<Access>&cand,const Pick&prev)->Pick{
            Pick r;
            if(cand.empty())return r;
            if(const Access*cur=prev.sat==UINT64_MAX?nullptr:find_access(cand,prev.sat)){
                r.sat=cur->sat;
                r.metric=cur->remaining_ns;
                r.trigger="hold";
                return r;
            }
            r.trigger=prev.sat==UINT64_MAX?"initial":"expiry";
            if(p==Policy::Random){
                std::uniform_int_distribution<size_t>pick(0,cand.size()-1);
                auto&a=cand[pick(rng)];
                r.sat=a.sat;
                r.metric=a.remaining_ns;
                return r;
            }
            // min-lifetime: drop links shorter than the threshold, then take the longest.
            const Access*best=nullptr;
            for(auto&a:cand)if(a.remaining_ns>=cfg.lifetime_threshold_ns&&(!best||a.remaining_ns>best->remaining_ns||(a.remaining_ns==best->remaining_ns&&a.sat<best->sat)))best=&a;
            if(!best){
                r.relaxed=true;
                for(auto&a:cand)if(!best||a.remaining_ns>best->remaining_ns||(a.remaining_ns==best->remaining_ns&&a.sat<best->sat))best=&a;
            }
            r.sat=best->sat;
            r.metric=best->remaining_ns;
            return r;
        };
        for(size_t j=window_first;j<=window_last;++j){
            safe();
            I t=data_begin+I(j)*step;
            Selection s;
            s.t=t;
            auto&cu=user_access[j];
            auto&cg=ground_access[j];
            if(!pair_policy(cfg.policy)){
                s.user=expiry_triggered(cfg.policy)?expiry_choice(cfg.policy,cu,pu):better_choice(cfg.policy,cu,pu);
                s.ground=expiry_triggered(cfg.policy)?expiry_choice(cfg.policy,cg,pg):better_choice(cfg.policy,cg,pg);
                s.pair_trigger=s.user.trigger+"/"+s.ground.trigger;
            }
            else if(cu.empty()||cg.empty()){
                s.user.trigger=cu.empty()?"none":"pair_incomplete";
                s.ground.trigger=cg.empty()?"none":"pair_incomplete";
                s.pair_trigger="none";
            }
            else if(cfg.policy==Policy::MaxMinVisibility){
                const Access*au=pu.sat==UINT64_MAX?nullptr:find_access(cu,pu.sat);
                const Access*ag=pg.sat==UINT64_MAX?nullptr:find_access(cg,pg.sat);
                std::string trig;
                if(au&&ag)trig="hold";
                else{
                    trig=pu.sat==UINT64_MAX&&pg.sat==UINT64_MAX?"initial":"expiry";
                    au=ag=nullptr;
                    U best=0;
                    for(auto&a:cu)for(auto&b:cg){
                        U m=std::min(a.remaining_ns,b.remaining_ns);
                        if(!au||m>best||(m==best&&std::tie(a.sat,b.sat)<std::tie(au->sat,ag->sat))){au=&a;ag=&b;best=m;}
                    }
                }
                s.user.sat=au->sat;s.ground.sat=ag->sat;
                s.user.metric=au->remaining_ns;s.ground.metric=ag->remaining_ns;
                s.pair_metric=std::min(au->remaining_ns,ag->remaining_ns);
                s.user.trigger=s.ground.trigger=s.pair_trigger=trig;
            }
            else{
                // min-full-delay / min-orbit-hops: metric of every reachable (USS,GSS) pair.
                bool hops=cfg.policy==Policy::MinOrbitHops;
                auto g=input.at(t);
                const Access*bu=nullptr;const Access*bg=nullptr;U best=UINT64_MAX;
                for(auto&a:cu){
                    auto d=reach(g,a.sat,cfg.user,cfg.ground,hops);
                    for(auto&b:cg){
                        if(d[size_t(b.sat)]==UINT64_MAX)continue;
                        U m=hops?add(d[size_t(b.sat)],2):add(add(a.delay_ns,d[size_t(b.sat)]),b.delay_ns);
                        bool is_prev=a.sat==pu.sat&&b.sat==pg.sat,best_prev=bu&&bu->sat==pu.sat&&bg->sat==pg.sat;
                        if(!bu||m<best||(m==best&&!best_prev&&(is_prev||std::tie(a.sat,b.sat)<std::tie(bu->sat,bg->sat)))){bu=&a;bg=&b;best=m;}
                    }
                }
                std::string trig;
                if(!bu){
                    // No end-to-end path: keep an access link anyway (shortest access delay), traffic will be unreachable.
                    for(auto&a:cu)if(!bu||a.delay_ns<bu->delay_ns)bu=&a;
                    for(auto&b:cg)if(!bg||b.delay_ns<bg->delay_ns)bg=&b;
                    trig="unreachable_fallback";
                    best=0;
                }
                else{
                    bool prev_visible=pu.sat!=UINT64_MAX&&pg.sat!=UINT64_MAX&&find_access(cu,pu.sat)&&find_access(cg,pg.sat);
                    trig=pu.sat==UINT64_MAX&&pg.sat==UINT64_MAX?"initial":(bu->sat==pu.sat&&bg->sat==pg.sat)?"hold":prev_visible?"better":"expiry";
                }
                s.user.sat=bu->sat;s.ground.sat=bg->sat;
                s.user.metric=bu->delay_ns;s.ground.metric=bg->delay_ns;
                s.pair_metric=best;
                s.user.trigger=s.ground.trigger=s.pair_trigger=trig;
            }
            auto account=[&](const Pick&prev,const Pick&now,U&ho,U&dc){
                if(now.sat==UINT64_MAX){if(prev.sat!=UINT64_MAX)++dc;}
                else if(prev.sat!=UINT64_MAX&&prev.sat!=now.sat)++ho;
            };
            account(pu,s.user,handovers_user,disconnects_user);
            account(pg,s.ground,handovers_ground,disconnects_ground);
            if(s.user.relaxed||s.ground.relaxed)++relaxed_count;
            pu=s.user;pg=s.ground;
            sel.push_back(std::move(s));
        }
    }
    tr::Graph HandoverPlan::apply(const tr::Graph&g,I t)const{
        auto&s=at(t);
        tr::Graph out{g.node_count,{}};
        out.edges.reserve(g.edges.size());
        for(auto&e:g.edges){
            bool touches_user=e.src==cfg.user||e.dst==cfg.user;
            bool touches_ground=e.src==cfg.ground||e.dst==cfg.ground;
            if(touches_user){
                U other=e.src==cfg.user?e.dst:e.src;
                if(other!=s.user.sat)continue;
            }
            if(touches_ground){
                U other=e.src==cfg.ground?e.dst:e.src;
                if(other!=s.ground.sat)continue;
            }
            out.edges.push_back(e);
        }
        return out;
    }
    J HandoverPlan::summary(const J&nodes)const{
        J j=J::obj();
        j["format"]="hsn-handover-summary-1";
        j["policy"]=policy_name(cfg.policy);
        j["trigger"]=expiry_triggered(cfg.policy)?"on-expiry":"on-better-metric";
        j["pair_metric"]=pair_policy(cfg.policy);
        j["user"]=cfg.user;j["user_name"]=node_name(nodes,cfg.user);
        j["ground"]=cfg.ground;j["ground_name"]=node_name(nodes,cfg.ground);
        j["seed"]=cfg.seed;
        j["lifetime_threshold_ns"]=cfg.lifetime_threshold_ns;
        j["window_start_ns"]=cfg.begin;j["window_end_ns"]=cfg.end;
        j["data_start_ns"]=data_begin;j["data_end_ns"]=data_end;j["step_ns"]=step;
        j["slices"]=U(sel.size());
        U cu=0,cg=0,both=0;
        for(auto&s:sel){
            if(s.user.sat!=UINT64_MAX)++cu;
            if(s.ground.sat!=UINT64_MAX)++cg;
            if(s.user.sat!=UINT64_MAX&&s.ground.sat!=UINT64_MAX)++both;
        }
        j["user_connected_slices"]=cu;j["ground_connected_slices"]=cg;j["both_connected_slices"]=both;
        j["user_handovers"]=handovers_user;j["ground_handovers"]=handovers_ground;
        j["user_disconnects"]=disconnects_user;j["ground_disconnects"]=disconnects_ground;
        j["lifetime_threshold_relaxed_slices"]=relaxed_count;
        j["visibility_horizon"]="remaining visibility counted to the end of the input data, not the simulation window";
        return j;
    }
}
