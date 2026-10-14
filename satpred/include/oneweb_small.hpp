#pragma once
#include <cstdint>
#include <initializer_list>
#include <string>
namespace oneweb_small {
inline constexpr char name[] = "OneWebSmall";
inline constexpr std::uint32_t planes = 1;
inline constexpr std::uint32_t satellites_per_plane = 49;
inline constexpr double altitude_km = 1200.0;
inline constexpr double inclination_deg = 87.9;
inline constexpr std::uint64_t terminal_uplink_bps = 50000000ULL;
inline constexpr std::uint64_t terminal_downlink_bps = 50000000ULL;
inline constexpr std::uint64_t uplink_frequency_hz = 2100000000ULL;
inline constexpr std::uint64_t downlink_frequency_hz = 1300000000ULL;
inline constexpr char satellite_xml_sha256[] = "d2f5451b3d5509c95f8b5f580038c0229ea4f64e5faf8a9230b5e9985e029309";
inline constexpr char ground_xml_sha256[] = "e8d2bc5f07c921217fbbe318318bacbe0245992736b6623f9fbdc437afd7037b";
inline bool matches(const std::string& sat_sha, const std::string& ground_sha) {
    return sat_sha == satellite_xml_sha256 && ground_sha == ground_xml_sha256;
}
template<class Json>
Json rf_metadata() {
    Json root = Json::obj();
    root["format"] = "satnet-rf-annotation-1";
    root["case_name"] = name;
    root["rf_model_applied"] = false;
    root["parameter_source"] = "user-specified";
    root["frequency_interpretation"] = "nominal-carrier";
    root["frequency_units"] = "Hz";
    root["bitrate_units"] = "bit/s";
    root["oneweb_operational_parameters_certified"] = false;
    root["nodes"] = Json::arr();
    for (const auto id : {"u1", "gw1"}) {
        Json node = Json::obj();
        node["node_name"] = id;
        node["uplink_frequency_hz"] = uplink_frequency_hz;
        node["downlink_frequency_hz"] = downlink_frequency_hz;
        node["uplink_max_bps"] = terminal_uplink_bps;
        node["downlink_max_bps"] = terminal_downlink_bps;
        root["nodes"].a.push_back(node);
    }
    return root;
}
} // namespace oneweb_small
