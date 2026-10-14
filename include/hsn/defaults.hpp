#pragma once
#include <array>
#include <string_view>
namespace unified {
struct Default{std::string_view name,value;bool business;};
inline constexpr std::array defaults{
 Default{"output","auto",false},Default{"start","computed",true},Default{"duration","computed",true},
 Default{"bandwidth-model","fluid",true},Default{"traffic-protocol","udp",true},
 Default{"traffic-src","first-user",true},Default{"traffic-dst","first-ground",true},
 Default{"traffic-rate-bps","1000000",true},Default{"traffic-packet-bytes","1200",true},
 Default{"queue-capacity","100",true},Default{"mtu","1500",true},Default{"max-hops","32",true},
 Default{"traceroute-probes","3",true},Default{"traceroute-timeout-s","5",true},
 Default{"probe-interval-s","10",true},Default{"report-interval-s","1",true},
 Default{"probe-packet-bytes","60",true},Default{"probe-serialization","enabled",true},
 Default{"memory-budget-bytes","268435456",false},Default{"host-io-delay-ms","0",false},
 // Handover policy (god's-eye USS/GSS selection for one user/ground pair).
 Default{"policy","none",true},Default{"pair","auto",true},Default{"policy-seed","1",true},
 Default{"lifetime-threshold-s","120",true},Default{"times","1",false},
 // Output naming: <constellation>-<policy>-<tool>-<index>.<ext>
 Default{"constellation","auto",false},Default{"run-index","auto",false},Default{"policy-dump-links","disabled",false},
 Default{"iperf-schema","iperf3-compat-v1",false}
};
}
