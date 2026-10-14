// Unit tests for option parsing/validation and event ordering (no simulation run).
#include "hsn/options.hpp"
#include "hsn/events.hpp"
#include "../src/options.inc"
#include <iostream>
#include <stdexcept>
#include <vector>
namespace {
unified::Options arguments(std::initializer_list<const char*> args) {
    std::vector<std::string> storage{"hypersim"};
    for (auto arg : args) storage.emplace_back(arg);
    std::vector<char*> argv;
    for (auto& arg : storage) argv.push_back(arg.data());
    return unified::parse(static_cast<int>(argv.size()), argv.data());
}
void expect(bool condition, const char* what) { if (!condition) throw std::runtime_error(what); }
void rejected(std::initializer_list<const char*> args, const char* what) {
    try { auto options = arguments(args); unified::validate(options); }
    catch (const sp::Err& error) { expect(error.code == 1, what); return; }
    throw std::runtime_error(std::string("accepted invalid options: ") + what);
}
}
int main() {
    try {
        auto defaults = arguments({}); unified::validate(defaults);
        expect(unified::val(defaults, "traffic-rate-bps") == 1000000, "default rate");
        expect(unified::duration(defaults, "probe-interval-s") == 10000000000LL, "default probe interval ns");
        auto equals = arguments({"--traffic-rate-bps=9600", "network"});
        unified::validate(equals);
        expect(unified::val(equals, "traffic-rate-bps") == 9600, "--key=value form");
        expect(equals.input == "network", "positional input");
        expect(unified::config(equals).at("traffic-rate-bps").at("source").str() == "cli", "cli provenance");
        expect(unified::config(equals).at("mtu").at("source").str() == "default/computed", "default provenance");
        expect(arguments({"--help"}).help && arguments({"--dump-defaults"}).dump && arguments({"--selftest"}).self, "flags");
        rejected({"--wrong", "1"}, "unknown option");
        rejected({"--mtu"}, "missing value");
        rejected({"--mtu", "1500", "--mtu=1500"}, "duplicate option");
        rejected({"first", "second"}, "two positional inputs");
        rejected({"--traffic-protocol", "tcp"}, "tcp not implemented");
        rejected({"--traffic-packet-bytes", "1473"}, "payload exceeds mtu-28");
        rejected({"--queue-capacity", "0"}, "zero queue");
        rejected({"--traceroute-timeout-s", "-1"}, "negative timeout");
        rejected({"--traceroute-probes", "4"}, "probes > 3");
        rejected({"--memory-budget-bytes", "1"}, "budget below 16 MiB");
        rejected({"--probe-serialization", "maybe"}, "serialization enum");
        auto pair2 = arguments({"--policy", "random", "--pair", "u1", "gw1", "network"});
        unified::validate(pair2);
        expect(pair2.v.at("pair") == "u1,gw1" && pair2.input == "network", "--pair USER GROUND two-token form");
        auto pair1 = arguments({"--pair", "u1:gw1", "--policy", "min-lifetime", "--lifetime-threshold-s", "60"});
        unified::validate(pair1);
        expect(pair1.v.at("pair") == "u1:gw1" && unified::duration(pair1, "lifetime-threshold-s") == 60000000000LL, "--pair USER:GROUND and threshold");
        expect(unified::policy_known("max-min-visibility") && !unified::policy_known("nearest"), "policy registry");
        rejected({"--policy", "nearest"}, "unknown policy");
        rejected({"--lifetime-threshold-s", "0"}, "zero lifetime threshold");
        rejected({"--pair", "u1"}, "pair without separator");
        auto multi = arguments({"--policy", "random", "min-lifetime", "network"});
        unified::validate(multi);
        expect(multi.v.at("policy") == "random,min-lifetime" && multi.input == "network", "--policy a b multi-token form");
        auto lst = unified::policy_list("random, min-access-delay;max-access-rate");
        expect(lst.size() == 3 && lst[0] == "random" && lst[2] == "max-access-rate", "policy list split");
        expect(unified::policy_list("all").size() == 7 && unified::policy_list("none").size() == 1, "policy list all/none");
        rejected({"--policy", "random,random"}, "duplicate policy in list");
        rejected({"--policy", "random,nearest"}, "unknown policy in list");
        rejected({"--times", "0"}, "zero times");
        rejected({"--times", "3", "--run-index", "2"}, "times with run-index");
        auto times = arguments({"--times", "5"}); unified::validate(times); expect(unified::val(times, "times") == 5, "--times value");
        rejected({"--run-index", "0"}, "zero run index");
        rejected({"--policy-dump-links", "yes"}, "dump links enum");
        rejected({"--constellation", "one web"}, "constellation slug");
        unified::Event a, b; a.time = b.time = 10;
        a.phase = unified::EventPhase::Completion; b.phase = unified::EventPhase::Timeout;
        expect(!unified::Compare{}(a, b) && unified::Compare{}(b, a), "completion before timeout at same time");
        a.phase = unified::EventPhase::Traffic; b.phase = unified::EventPhase::Topology;
        expect(unified::Compare{}(a, b), "topology before traffic at same time");
        b.phase = a.phase; a.seq = 1; b.seq = 2;
        expect(!unified::Compare{}(a, b) && unified::Compare{}(b, a), "FIFO by sequence within phase");
        b.time = 9; b.seq = 0; b.phase = unified::EventPhase::Traffic; a.phase = unified::EventPhase::Topology;
        expect(unified::Compare{}(a, b), "earlier time wins regardless of phase");
        std::cout << "options and event ordering: PASS (43 checks)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
