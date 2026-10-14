#pragma once
// Host-side run directory, environment, sibling program lookup and input snapshot.
#include "hsn/reader_internal.hpp"
namespace unified {
sp::fs::path new_output(const std::string& setting);
void set_environment(const char* name, const std::string& value);
sp::fs::path program(const char* name);
sp::fs::path snapshot(const sp::fs::path& input, const sp::fs::path& output);
}
