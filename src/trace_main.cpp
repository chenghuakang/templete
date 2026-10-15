#include "hsn/reader_internal.hpp"
namespace fixedtrace {
    int run(int argc,char**argv){
        std::map<std::string,std::string>opts={{"src","default"},{"dst","default"},{"start","default"},{"max-hops","30"},{"probes","3"},{"timeout-s","5"},{"packet-bytes","60"},{"serialization","enabled"},{"output","stdout"},{"json","none"},{"memory-budget-bytes","268435456"}};
        std::set<std::string>seen;
        fs::path input;
        for(int i=1;i<argc;++i){
            std::string a=argv[i];
            if(a=="--help"){
                std::cout<<"hsn-traceroute <SATNET-run|manifest|graph.ngb> [--src id] [--dst id] [--start ISO] [--max-hops 30] [--probes 3] [--timeout-s 5] [--serialization enabled|disabled] [--output stdout|file] [--json file]\n";
                return 0;
            }
            if(a.rfind("--",0)!=0){
                need(input.empty(),1,"cli","one input required");
                input=fs::u8path(a);
                continue;
            }
            a.erase(0,2);
            std::string value;
            auto eq=a.find('=');
            if(eq!=std::string::npos){
                value=a.substr(eq+1);
                a.resize(eq);
            }
            need(opts.count(a)&&seen.insert(a).second,1,"cli","unknown/duplicate option "+a);
            if(eq==std::string::npos){
                need(i+1<argc,1,"cli","missing value");
                value=argv[++i];
            }
            opts[a]=value;
        }
        need(!input.empty(),1,"cli","input required");
        U memory=sp::uint(opts.at("memory-budget-bytes"));
        need(memory>=16777216,1,"memory","minimum 16 MiB");
        Input reader(fs::absolute(input),memory);
        auto src=reader.node_id(opts["src"],1),dst=reader.node_id(opts["dst"],2);
        hsn::trace::Config c;
        U maxh=sp::uint(opts["max-hops"]),count=sp::uint(opts["probes"]),bytes=sp::uint(opts["packet-bytes"]);
        need(maxh>=1&&maxh<=32&&count>=1&&count<=3&&bytes>=28&&bytes<=9000,1,"probe","bounds");
        c.max_hops=unsigned(maxh);
        c.probes=unsigned(count);
        c.packet_bytes=unsigned(bytes);
        I timeout=seconds(opts["timeout-s"]);
        need(timeout>0,1,"timeout","positive seconds required");
        c.timeout_ns=U(timeout);
        need(opts["serialization"]=="enabled"||opts["serialization"]=="disabled",1,"serialization","enabled|disabled");
        c.serialization=opts["serialization"]=="enabled";
        I start=opts["start"]=="default"?reader.begin:utc(opts["start"]);
        need(start>=reader.begin&&start<reader.end,1,"time","start outside coverage");
        hsn::trace::Source source{reader.begin,reader.end,[&](I t){return reader.at(t);},[&](I t){return reader.next(t);}};
        hsn::trace::Engine engine(source,c);
        auto r=engine.run(std::uint32_t(src),std::uint32_t(dst),start);
        auto text=engine.text(r);
        if(opts["output"]=="stdout")std::cout<<text;
        else{
            auto out=fs::absolute(fs::u8path(opts["output"]));
            need(!fs::exists(out),1,"output","file exists");
            atomic(out,text);
        }
        if(opts["json"]!="none"){
            auto out=fs::absolute(fs::u8path(opts["json"]));
            need(!fs::exists(out),1,"output","file exists");
            atomic(out,encode(r).dump());
        }
        if(r.censored)std::cerr<<"traceroute interrupted at simulation run_end; unfinished probes are censored, not timeout\n";
        return 0;
    }
}
int main(int argc,char**argv){
    try{
        return fixedtrace::run(argc,argv);
    }
    catch(const sp::Err&e){
        std::cerr<<"{\"code\":"<<e.code<<",\"where\":"<<sp::quote(e.where)<<",\"message\":"<<sp::quote(e.what())<<"}\n";
        return e.code;
    }
    catch(const std::filesystem::filesystem_error&e){
        std::cerr<<e.what()<<'\n';
        return 3;
    }
    catch(const std::exception&e){
        std::cerr<<e.what()<<'\n';
        return 5;
    }
}
