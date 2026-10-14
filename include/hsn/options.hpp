#pragma once
// Command-line option model for hypersim: defaults, parsing, validation, effective config.
#include "hsn/reader_internal.hpp"
namespace unified {
struct Options {
    std::map<std::string, std::string> v;   // option name -> textual value
    std::set<std::string> explicit_keys;    // options given on the command line
    sp::fs::path input;                     // run directory, manifest or graph (may be empty)
    bool help = false;
    bool dump = false;
    bool self = false;
};
Options parse(int argc, char** argv);
sp::J config(const Options& options);                           // effective config with provenance
sp::U val(const Options& options, const std::string& key);      // unsigned numeric option
sp::I duration(const Options& options, const std::string& key); // positive seconds -> ns
bool policy_known(const std::string& policy);                   // handover policy name registry
std::vector<std::string> policy_list(const std::string& spec);  // --policy list: "a,b" / "a b" / "all"
void validate(const Options& options);                          // throws sp::Err code 1 on bad input
}
