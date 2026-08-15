// config.hpp
//
// A very small reader for the subset of TOML this project uses: section
// headers, one `key = value` per line, `#` comments, and values that are
// either a number or a quoted string.
//
// Nothing here handles arrays, nested tables written inline, multi-line
// strings or dates. It exists so the random order generator can be retuned by
// editing config.toml instead of recompiling.
//
// A key is addressed by its full path, so
//
//   [flow]
//   seed = 7
//
// is read as config.integer("flow.seed").

#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace obe {

class Config {
public:
    // Read a config file. Throws std::runtime_error if the file cannot be
    // opened or a line is neither a comment, a section header nor key = value.
    static Config from_file(const std::string& path);

    // Typed lookups. Each throws std::runtime_error when the key is missing or
    // the value does not parse, so a typo in the file fails loudly instead of
    // silently falling back to a default.
    std::int64_t integer(const std::string& key) const;
    double       decimal(const std::string& key) const;
    std::string  text(const std::string& key) const;

    bool has(const std::string& key) const;

private:
    std::map<std::string, std::string> values_;  // full key path -> raw value
};

}  // namespace obe
