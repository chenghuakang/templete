#pragma once
// God's-eye handover policy: per time slice, prune the user and ground station of one pair down to one
// satellite each, following a named rule. Remaining visibility is computed over the whole input horizon.
#include "hsn/reader_internal.hpp"
#include <random>
namespace unified {
using sp::U; using sp::I; using sp::J;
enum class Policy { None, Random, MinAccessDelay, MinFullDelay, MaxMinVisibility, MinOrbitHops, MaxAccessRate, MinLifetime };
Policy policy_from(const std::string& name);
std::string policy_name(Policy policy);
bool pair_policy(Policy policy);        // metric is defined on (USS,GSS) pairs, not per endpoint
bool expiry_triggered(Policy policy);   // re-select only when the current satellite is no longer visible

struct Access {                          // one endpoint<->satellite access link in one slice
    U sat = 0;
    U delay_ns = 0;                      // propagation delay (max of present directions)
    U capacity_bps = 0;                  // bottleneck of present directions
    bool up = false, down = false;       // endpoint->sat / sat->endpoint edge present
    U remaining_ns = 0;                  // consecutive visibility from this slice to data horizon
};
struct Pick {
    U sat = UINT64_MAX;                  // UINT64_MAX: no satellite
    std::string trigger = "none";        // initial|hold|expiry|better|none|unreachable_fallback
    U metric = 0;                        // policy metric of the chosen link/pair (0 for random)
    bool relaxed = false;                // min-lifetime: threshold could not be met
};
struct Selection {
    I t = 0;
    Pick user, ground;
    U pair_metric = 0;
    std::string pair_trigger;
};
struct HandoverConfig {
    Policy policy = Policy::None;
    U user = 0, ground = 0;
    U seed = 1;
    U lifetime_threshold_ns = 120000000000ULL;
    I begin = 0, end = 0;                // selection window (half open)
};
struct PairIds { U user, ground; };
PairIds resolve_pair(const J& nodes, const std::string& spec);
std::string node_name(const J& nodes, U id);

class HandoverPlan {
    HandoverConfig cfg;
    I step = 0, data_begin = 0, data_end = 0;
    std::vector<std::vector<Access>> user_access, ground_access;   // per data slice
    std::vector<Selection> sel;                                    // per window slice
    size_t window_first = 0;
    size_t slice(I t) const;
public:
    U handovers_user = 0, handovers_ground = 0, disconnects_user = 0, disconnects_ground = 0, relaxed_count = 0;
    HandoverPlan(fixedtrace::Input& input, HandoverConfig config);
    const HandoverConfig& config() const { return cfg; }
    const Selection& at(I t) const;
    const std::vector<Selection>& selections() const { return sel; }
    const std::vector<Access>& user_links(I t) const { return user_access[slice(t)]; }
    const std::vector<Access>& ground_links(I t) const { return ground_access[slice(t)]; }
    hsn::trace::Graph apply(const hsn::trace::Graph& g, I t) const;   // pruned graph for slice containing t
    J summary(const J& nodes) const;
};

// Topology seen by the simulation: raw input, or input pruned by a handover plan.
struct Topology {
    fixedtrace::Input& base;
    const HandoverPlan* plan = nullptr;
    I begin, end;
    J& nodes;
    hsn::trace::Graph at(I t) { auto g = base.at(t); return plan ? plan->apply(g, t) : g; }
    std::optional<I> next(I t) { return base.next(t); }
};
}
