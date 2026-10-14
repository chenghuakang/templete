#pragma once
// Discrete-event kernel data types shared by the simulation and its tests.
#include "hsn/reader_internal.hpp"
#include <deque>
namespace unified {
    using sp::U;
    using sp::I;
    namespace tr = hsn::trace;
    // Event kinds dispatched by Simulation::dispatch.
    enum class EventKind : unsigned {
        Generate,  // packet generation (packet model)
        ServiceComplete,  // link finished serialising a packet
        Arrival,  // packet arrives at next node
        FlowBegin,  // fluid arrival rate segment starts
        FlowEnd,  // fluid arrival rate segment ends
        ProbeComplete  // traceroute probe reply/timeout
    };
    // Same-timestamp ordering: topology first, then completions, then timeouts, then traffic.
    enum class EventPhase : unsigned {
        Topology, Completion, Timeout, Traffic
    };
    struct Packet{
        U id=0,node=0,hops=0;
    };
    struct LinkState{
        tr::Link edge{};
        bool up=false,busy=false;
        U generation=0;
        U remaining_scaled=0;
        I last=0;
        Packet active;
        std::deque<Packet>queue;
    };
    struct Event{
        I time=0;
        EventPhase phase=EventPhase::Topology;
        U seq=0;
        EventKind kind=EventKind::Generate;
        U key=0,generation=0;
        Packet packet;
        long double rate=0;
        tr::Probe probe;
    };
    struct Compare{
        bool operator()(const Event&a,const Event&b)const{
            return std::tie(a.time,a.phase,a.seq)>std::tie(b.time,b.phase,b.seq);
        }
    };
}
