// Included by simulation.cpp (single translation unit: reader_internal.hpp includes core.cpp).
#include "hsn/runtime_io.hpp"
namespace unified {
    using namespace sp;
    fs::path new_output(const std::string&s){
        if(s!="auto"){
            auto p=fs::absolute(fs::u8path(s));
            need(!fs::exists(p)||fs::is_empty(p),1,"output","output exists and nonempty");
            fs::create_directories(p);
            return p;
        }
        const char*p=std::getenv("HSN_HOME");
        fs::path base;
        if(p)base=fs::u8path(p);
        else{
#ifdef _WIN32
            p=std::getenv("USERPROFILE");
#else
            p=std::getenv("HOME");
#endif
            need(p,3,"home","home unavailable");
            base=fs::u8path(p)/".hypersim";
        }
        fs::create_directories(base/"runs");
        for(U i=1;;++i){
            std::ostringstream x;
            x<<"run_"<<std::setfill('0')<<std::setw(6)<<i;
            auto out=base/"runs"/x.str();
            std::error_code ec;
            if(fs::create_directory(out,ec))return fs::absolute(out);
            need(!ec,3,"output",ec.message());
        }
    }
    void set_environment(const char*name,const std::string&value){
#ifdef _WIN32
        need(_putenv_s(name,value.c_str())==0,3,"environment",std::string("cannot set ")+name);
#else
        need(setenv(name,value.c_str(),1)==0,3,"environment",std::string("cannot set ")+name);
#endif
    }
    fs::path program(const char*name){
        auto p=executable().parent_path()/name;
#ifdef _WIN32
        p+=".exe";
#endif
        return p;
    }
    fs::path snapshot(const fs::path&input,const fs::path&out){
        auto base=fs::is_directory(input)?input:input.parent_path();
        auto manifest=fs::is_directory(input)?input/"manifest.json":input;
        if(input.extension()==".ngb")return input;
        // standalone graph mode validates bindings but has no run snapshot catalog
        auto m=json(readtext(manifest));
        need(m.at("format").str()=="SATNET-1.1",7,"manifest","format");
        auto target=out/"source_snapshot";
        fs::create_directories(target);
        for(auto&p:m.at("products").a)for(auto key:{"graph_file","source_file"}){
            auto src=fixedtrace::relative_path(base,p.at(key).str());
            auto dst=target/fs::u8path(p.at(key).str());
            fs::create_directories(dst.parent_path());
            need(!fs::exists(dst),7,"snapshot","duplicate product path");
            U size=fs::file_size(src);
            auto mt=fs::last_write_time(src);
            fs::copy_file(src,dst);
            need(fs::file_size(src)==size&&fs::last_write_time(src)==mt,7,"snapshot","source changed during copy");
        }
        atomic(target/"manifest.json",m.dump());
        return target;
    }
}
