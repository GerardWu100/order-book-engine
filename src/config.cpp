// config.cpp - the small TOML subset reader declared in config.hpp.

#include "config.hpp"

#include <fstream>
#include <stdexcept>

namespace obe {
namespace {

// Remove spaces and tabs from both ends.
std::string trim(const std::string& text) {
    const std::size_t first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos) {
        return "";
    }
    const std::size_t last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

// Strip surrounding double quotes if both are there.
std::string unquote(const std::string& text) {
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        return text.substr(1, text.size() - 2);
    }
    return text;
}

}  // namespace

Config Config::from_file(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("cannot open config file: " + path);
    }

    Config      config;
    std::string section;  // current [section] prefix, empty at the top of the file
    std::string line;
    int         line_number = 0;

    while (std::getline(file, line)) {
        ++line_number;

        // Drop an end-of-line comment, but not a '#' inside a quoted string.
        bool inside_quotes = false;
        for (std::size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '"') {
                inside_quotes = !inside_quotes;
            } else if (line[i] == '#' && !inside_quotes) {
                line = line.substr(0, i);
                break;
            }
        }

        const std::string stripped = trim(line);
        if (stripped.empty()) {
            continue;
        }

        if (stripped.front() == '[' && stripped.back() == ']') {
            section = trim(stripped.substr(1, stripped.size() - 2));
            continue;
        }

        const std::size_t equals = stripped.find('=');
        if (equals == std::string::npos) {
            throw std::runtime_error(path + ":" + std::to_string(line_number) +
                                     " is not a comment, a section or key = value");
        }

        const std::string key   = trim(stripped.substr(0, equals));
        const std::string value = unquote(trim(stripped.substr(equals + 1)));
        config.values_[section.empty() ? key : section + "." + key] = value;
    }

    return config;
}

bool Config::has(const std::string& key) const {
    return values_.find(key) != values_.end();
}

std::string Config::text(const std::string& key) const {
    const auto found = values_.find(key);
    if (found == values_.end()) {
        throw std::runtime_error("missing config key: " + key);
    }
    return found->second;
}

std::int64_t Config::integer(const std::string& key) const {
    const std::string raw = text(key);
    try {
        std::size_t         used = 0;
        const std::int64_t  value = std::stoll(raw, &used);
        if (used != raw.size()) {
            throw std::invalid_argument("trailing characters");
        }
        return value;
    } catch (const std::exception&) {
        throw std::runtime_error("config key " + key + " is not a whole number: " + raw);
    }
}

double Config::decimal(const std::string& key) const {
    const std::string raw = text(key);
    try {
        std::size_t  used  = 0;
        const double value = std::stod(raw, &used);
        if (used != raw.size()) {
            throw std::invalid_argument("trailing characters");
        }
        return value;
    } catch (const std::exception&) {
        throw std::runtime_error("config key " + key + " is not a number: " + raw);
    }
}

}  // namespace obe
