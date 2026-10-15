// Included by simulation.cpp (single translation unit: reader_internal.hpp includes core.cpp).
#include "hsn/options.hpp"
#include "hsn/defaults.hpp"
namespace unified {
    using namespace sp;
    bool policy_known(const std::string&p){
        for(auto k:{"none","random","min-access-delay","min-full-delay","max-min-visibility","min-orbit-hops","max-access-rate","min-lifetime","all"})if(p==k)return true;
        return false;
    }
    // --policy accepts a comma/space separated list (or "all"); each entry runs --times repetitions.
    std::vector<std::string> policy_list(const std::string&spec){
        std::vector<std::string>out;
        std::string cur;
        auto flush=[&]{
            if(cur.empty())return;
            if(cur=="all"){
                for(auto k:{"random","min-access-delay","min-full-delay","max-min-visibility","min-orbit-hops","max-access-rate","min-lifetime"})if(std::find(out.begin(),out.end(),k)==out.end())out.emplace_back(k);
            }
            else{
                need(policy_known(cur),1,"policy","unknown handover policy "+cur+" (none|random|min-access-delay|min-full-delay|max-min-visibility|min-orbit-hops|max-access-rate|min-lifetime|all)");
                need(std::find(out.begin(),out.end(),cur)==out.end(),1,"policy","duplicate policy "+cur);
                out.push_back(cur);
            }
            cur.clear();
        };
        for(char c:spec){
            if(c==','||c==' '||c==';'||c=='+')flush();
            else cur+=c;
        }
        flush();
        need(!out.empty(),1,"policy","empty policy list");
        return out;
    }
    Options parse(int argc,char**argv){
        Options o;
        for(auto&p:defaults)o.v[std::string(p.name)]=std::string(p.value);
        for(int i=1;i<argc;++i){
            std::string a=argv[i],val;
            if(a=="--help"){
                o.help=true;
                continue;
            }
            if(a=="--dump-defaults"){
                o.dump=true;
                continue;
            }
            if(a=="--selftest"){
                o.self=true;
                continue;
            }
            if(a.rfind("--",0)!=0){
                need(o.input.empty(),1,"cli","one input directory/manifest/graph expected");
                o.input=fs::u8path(a);
                continue;
            }
            a.erase(0,2);
            auto e=a.find('=');
            if(e!=std::string::npos){
                val=a.substr(e+1);
                a.resize(e);
            }
            need(o.v.count(a)&&o.explicit_keys.insert(a).second,1,"cli","unknown/duplicate option "+a);
            if(e==std::string::npos){
                need(i+1<argc,1,"cli","missing value "+a);
                val=argv[++i];
                // --pair USER GROUND (two tokens) is accepted besides --pair USER,GROUND / USER:GROUND.
                // --policy a b c (several tokens that are policy names) is accepted besides --policy a,b,c.
                if(a=="policy")while(i+1<argc&&policy_known(argv[i+1])){
                    val+=std::string(",")+argv[++i];
                }
                if(a=="pair"&&val.find(',')==std::string::npos&&val.find(':')==std::string::npos&&i+1<argc){
                    std::string nxt=argv[i+1];
                    if(nxt.rfind("--",0)!=0&&nxt.find('/')==std::string::npos&&nxt.find('\\')==std::string::npos&&nxt.find('.')==std::string::npos){
                        val+=","+nxt;
                        ++i;
                    }
                }
            }
            o.v[a]=val;
        }
        return o;
    }
    J config(const Options&o){
        J j=J::obj();
        for(auto&[k,v]:o.v){
            J q=J::obj();
            q["value"]=v;
            q["source"]=o.explicit_keys.count(k)?"cli":"default/computed";
            j[k]=q;
        }
        return j;
    }
    U val(const Options&o,const std::string&k){
        return sp::uint(o.v.at(k));
    }
    I duration(const Options&o,const std::string&k){
        I n=seconds(o.v.at(k));
        need(n>0,1,"time",k+" must be positive");
        return n;
    }
    void validate(const Options&o){
        need(o.v.at("traffic-protocol")=="udp",1,"protocol","TCP not implemented");
        need(o.v.at("bandwidth-model")=="fluid"||o.v.at("bandwidth-model")=="packet",1,"model","fluid|packet expected");
        need(val(o,"traffic-rate-bps")>=1&&val(o,"traffic-rate-bps")<=U(INT64_MAX),1,"rate","range");
        need(val(o,"mtu")>=576&&val(o,"mtu")<=9000&&val(o,"traffic-packet-bytes")>0&&val(o,"traffic-packet-bytes")<=val(o,"mtu")-28,1,"packet","MTU/payload range");
        need(val(o,"queue-capacity")>=1&&val(o,"queue-capacity")<=UINT32_MAX,1,"queue","range");
        need(val(o,"max-hops")>=1&&val(o,"max-hops")<=32&&val(o,"traceroute-probes")>=1&&val(o,"traceroute-probes")<=3,1,"trace","bounds");
        need(val(o,"probe-packet-bytes")>=28&&val(o,"probe-packet-bytes")<=9000,1,"probe bytes","range");
        duration(o,"trace-retry-interval-s");
        need(val(o,"trace-retry-max")<=1000000,1,"trace retry","range (0..1000000)");
        need(o.v.at("tool-start-jitter")=="disabled"||o.v.at("tool-start-jitter")=="enabled",1,"tool start jitter","disabled|enabled");
        need(o.v.at("probe-serialization")=="enabled"||o.v.at("probe-serialization")=="disabled",1,"probe serialization","enabled|disabled");
        need(val(o,"memory-budget-bytes")>=16777216,1,"budget","minimum 16 MiB");
        need(val(o,"host-io-delay-ms")<=10000,1,"test delay","range");
        duration(o,"traceroute-timeout-s");
        duration(o,"probe-interval-s");
        duration(o,"report-interval-s");
        policy_list(o.v.at("policy"));
        duration(o,"lifetime-threshold-s");
        val(o,"policy-seed");
        need(val(o,"times")>=1&&val(o,"times")<=100000,1,"times","positive integer (1..100000)");
        need(!(o.explicit_keys.count("times")&&o.explicit_keys.count("run-index")),1,"times","--times and --run-index are exclusive");
        if(o.v.at("run-index")!="auto")need(val(o,"run-index")>=1,1,"run index","auto|positive integer");
        need(o.v.at("policy-dump-links")=="enabled"||o.v.at("policy-dump-links")=="disabled",1,"policy dump links","enabled|disabled");
        need(o.v.at("pair")=="auto"||o.v.at("pair").find(',')!=std::string::npos||o.v.at("pair").find(':')!=std::string::npos,1,"pair","auto|USER,GROUND");
        for(char c:o.v.at("constellation"))need(std::isalnum(static_cast<unsigned char>(c))||c=='_',1,"constellation","alphanumeric/underscore only");
    }
}
