// Tiny JSON helpers: read a few top-level values from the models' *.meta.json files (first occurrence
// of the key wins, which is the top-level one in the files written by the training code) and format
// numbers/strings for the result files.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace fd {

std::string read_text_file(const std::string& path);                          // throws if unreadable
std::optional<double> json_number(const std::string& text, const std::string& key);
std::optional<std::string> json_string(const std::string& text, const std::string& key);
std::optional<std::vector<int>> json_int_list(const std::string& text, const std::string& key);

std::string json_num(double v);                      // finite -> "%.10g", otherwise "null"
std::string json_str(const std::string& s);         // quoted + escaped

}  // namespace fd
