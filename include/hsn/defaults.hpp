#pragma once
#include <array>
#include <string_view>
namespace unified {
// Typed canonical registry (SDD §14.3 / satpred §10.1): the single source of defaults.
// type: Bool/U64/I64/F64/Enum/Path/Seconds · stage: Load/Time/Network/Traffic/Measure/Resource/Output/Run
// valid: enum domain or numeric range (display) · business: affects_business_run_id
struct Default{std::string_view name,value,type,unit,stage,valid;bool business;};
inline constexpr std::array defaults{
 Default{"output","auto","Path","","Output","auto | empty directory",false},
 Default{"start","computed","Seconds","","Time","computed | UTC ISO/ns",true},
 Default{"duration","computed","Seconds","s","Time","computed | >0",true},
 Default{"bandwidth-model","fluid","Enum","","Network","fluid|packet",true},
 Default{"traffic-protocol","udp","Enum","","Traffic","udp",true},
 Default{"traffic-src","first-user","Path","","Traffic","first-user | node id",true},
 Default{"traffic-dst","first-ground","Path","","Traffic","first-ground | node id",true},
 Default{"traffic-rate-bps","1000000","U64","bps","Traffic","1..2^63-1",true},
 Default{"traffic-packet-bytes","1200","U64","B","Traffic","1..mtu-28",true},
 Default{"queue-capacity","100","U64","packets","Network","1..2^32-1",true},
 Default{"mtu","1500","U64","B","Network","576..9000",true},
 Default{"max-hops","32","U64","","Network","1..32",true},
 Default{"traceroute-probes","3","U64","","Measure","1..3",true},
 Default{"traceroute-timeout-s","5","Seconds","s","Measure",">0",true},
 Default{"probe-interval-s","10","Seconds","s","Measure",">0",true},
 Default{"report-interval-s","1","Seconds","s","Measure",">0",true},
 Default{"probe-packet-bytes","60","U64","B","Measure","28..9000",true},
 Default{"probe-serialization","enabled","Enum","","Measure","enabled|disabled",true},
 Default{"satellite-processing-us","0","U64","us","Network",">=0",true},
 Default{"trace-retry-interval-s","1","Seconds","s","Measure",">0",true},
 Default{"trace-retry-max","30","U64","","Measure","0..1000000",true},
 Default{"memory-budget-bytes","268435456","U64","B","Resource",">=16777216",false},
 Default{"host-io-delay-ms","0","U64","ms","Time","0..10000",false},
 // Handover policy (god's-eye USS/GSS selection for one user/ground pair).
 Default{"policy","none","Enum","","Network","none|random|min-access-delay|min-full-delay|max-min-visibility|min-orbit-hops|max-access-rate|min-lifetime",true},
 Default{"pair","auto","Enum","","Traffic","auto | USER,GROUND",true},
 Default{"policy-seed","1","U64","","Network",">=0",true},
 Default{"lifetime-threshold-s","120","Seconds","s","Network",">0",true},
 Default{"times","1","U64","","Run","1..100000",false},
 // Output naming: <constellation>-<policy>-<tool>-<index>.<ext>
 Default{"constellation","auto","Enum","","Output","auto | alphanumeric/underscore",false},
 Default{"run-index","auto","Enum","","Output","auto | >=1",false},
 Default{"policy-dump-links","disabled","Enum","","Output","enabled|disabled",false},
 Default{"iperf-schema","iperf3-compat-v1","Enum","","Measure","iperf3-compat-v1",false}
};
}
