#include "json_util.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace fd {
namespace {

// Position right after the ':' that follows "key", or npos.
size_t value_pos(const std::string& text, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t p = text.find(needle);
    while (p != std::string::npos) {
        size_t q = p + needle.size();
        while (q < text.size() && std::isspace(static_cast<unsigned char>(text[q]))) ++q;
        if (q < text.size() && text[q] == ':') {
            ++q;
            while (q < text.size() && std::isspace(static_cast<unsigned char>(text[q]))) ++q;
            return q;
        }
        p = text.find(needle, p + 1);
    }
    return std::string::npos;
}

}  // namespace

std::string read_text_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open file: " + path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::optional<double> json_number(const std::string& text, const std::string& key) {
    const size_t p = value_pos(text, key);
    if (p == std::string::npos) return std::nullopt;
    char* end = nullptr;
    const double v = std::strtod(text.c_str() + p, &end);
    if (end == text.c_str() + p) return std::nullopt;
    return v;
}

std::optional<std::string> json_string(const std::string& text, const std::string& key) {
    const size_t p = value_pos(text, key);
    if (p == std::string::npos || text[p] != '"') return std::nullopt;
    const size_t e = text.find('"', p + 1);
    if (e == std::string::npos) return std::nullopt;
    return text.substr(p + 1, e - p - 1);
}

std::optional<std::vector<int>> json_int_list(const std::string& text, const std::string& key) {
    size_t p = value_pos(text, key);
    if (p == std::string::npos || text[p] != '[') return std::nullopt;
    std::vector<int> out;
    ++p;
    while (p < text.size() && text[p] != ']') {
        char* end = nullptr;
        const long v = std::strtol(text.c_str() + p, &end, 10);
        if (end == text.c_str() + p) { ++p; continue; }
        out.push_back(static_cast<int>(v));
        p = static_cast<size_t>(end - text.c_str());
    }
    return out;
}

std::string json_num(double v) {
    if (!std::isfinite(v)) return "null";
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.10g", v);
    return buf;
}

std::string json_str(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
        else if (c == '\n') out += "\\n";
        else out.push_back(c);
    }
    return out + "\"";
}

}  // namespace fd
