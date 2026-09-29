// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pure gcode tool remapper for Snapmaker U1 / ACE.
//
// A sliced file bakes the logical->physical tool mapping into four command
// families. A remap of logical tool a -> physical head b must rewrite ALL FOUR
// consistently:
//   1. Prestart:  SM_PRINT_AUTO_FEED / SM_PRINT_EXTRUDER_PREHEAT /
//                 SM_PRINT_FLOW_CALIBRATE  with  EXTRUDER=<n>  (Snapmaker U1)
//   2. Body:      a bare "T<n>" toolchange line
//   3. Temps:     M104 / M109 lines carrying a "T<n>" tool token
//   4. Start:     an INITIAL_TOOL=<n> parameter on any command line, the name
//                 of the slicer placeholder a start macro is handed the first
//                 tool by
//
// Any other tool-naming parameter a start macro takes is the user's own
// convention; unremapped_tool_params() reports those instead of guessing.
//
// Matching is deliberately conservative so comment lines and unrelated commands
// are never touched. Each line is transformed from its ORIGINAL text only, so a
// swap (1<->2) does not chain. No regex callbacks are used (GCC 7.5 on some
// cross targets lacks a reliable functional regex_replace overload) -- the
// single-token splice is hand-rolled.

#include "gcode_tool_remapper.h"

#include "text_io.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace helix {

namespace {

int mapped(int n, const std::map<int, int>& remap) {
    auto it = remap.find(n);
    return it != remap.end() ? it->second : n;
}

// Parse an unsigned integer starting at `pos` (which must point at a digit).
// On return `pos` is advanced past the last digit. Caller guarantees a digit.
int parse_uint(const std::string& s, size_t& pos) {
    int value = 0;
    while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
        value = value * 10 + (s[pos] - '0');
        ++pos;
    }
    return value;
}

bool starts_with(const std::string& s, const char* prefix) {
    size_t i = 0;
    for (; prefix[i] != '\0'; ++i) {
        if (i >= s.size() || s[i] != prefix[i]) {
            return false;
        }
    }
    return true;
}

// --- Family 2: bare toolchange line "T<digits>" (optional trailing whitespace) ---
// Returns true and fills `out` if `line` is a bare toolchange whose index is
// remapped. The trailing whitespace (if any) is preserved.
bool try_bare_toolchange(const std::string& line, const std::map<int, int>& remap,
                         std::string& out) {
    if (line.size() < 2 || line[0] != 'T') {
        return false;
    }
    size_t pos = 1;
    if (!std::isdigit(static_cast<unsigned char>(line[pos]))) {
        return false;
    }
    int idx = parse_uint(line, pos);
    // Whatever remains must be whitespace only (e.g. trailing \r or spaces).
    std::string tail = line.substr(pos);
    for (char c : tail) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            return false; // e.g. "T1X" or "TOOL" -- not a bare toolchange
        }
    }
    int m = mapped(idx, remap);
    if (m == idx) {
        return false; // unmapped: leave untouched (preserves exact bytes)
    }
    out = "T" + std::to_string(m) + tail;
    return true;
}

// --- Family 1: prestart "SM_PRINT_<CMD> EXTRUDER=<digits><rest>" ---
bool try_prestart(const std::string& line, const std::map<int, int>& remap, std::string& out) {
    static const char* PREFIXES[] = {
        "SM_PRINT_AUTO_FEED EXTRUDER=",
        "SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=",
        "SM_PRINT_FLOW_CALIBRATE EXTRUDER=",
    };
    for (const char* prefix : PREFIXES) {
        if (!starts_with(line, prefix)) {
            continue;
        }
        size_t prefix_len = std::string(prefix).size();
        if (prefix_len >= line.size() ||
            !std::isdigit(static_cast<unsigned char>(line[prefix_len]))) {
            return false; // "EXTRUDER=" not followed by a number
        }
        size_t pos = prefix_len;
        int idx = parse_uint(line, pos);
        int m = mapped(idx, remap);
        if (m == idx) {
            return false;
        }
        out = line.substr(0, prefix_len) + std::to_string(m) + line.substr(pos);
        return true;
    }
    return false;
}

// --- Family 3: M104 / M109 line carrying the FIRST "T<digits>" token ---
// A tool token is a standalone parameter: preceded by whitespace (or line start
// of the param after the command) and the "T" immediately followed by digits.
// We rewrite only the first such token; gcode never carries two on one line.
bool try_temp(const std::string& line, const std::map<int, int>& remap, std::string& out) {
    if (!starts_with(line, "M104") && !starts_with(line, "M109")) {
        return false;
    }
    // Never look inside the trailing comment.
    size_t comment = line.find(';');
    size_t scan_end = (comment == std::string::npos) ? line.size() : comment;

    for (size_t i = 0; i + 1 < scan_end; ++i) {
        if (line[i] != 'T') {
            continue;
        }
        // Must start a token: previous char is whitespace (the command "M104"
        // itself guarantees the first 'T' we care about is never at index 0).
        if (i == 0 || !std::isspace(static_cast<unsigned char>(line[i - 1]))) {
            continue;
        }
        if (!std::isdigit(static_cast<unsigned char>(line[i + 1]))) {
            continue;
        }
        size_t pos = i + 1;
        int idx = parse_uint(line, pos);
        int m = mapped(idx, remap);
        if (m == idx) {
            return false; // first tool token is unmapped -> nothing to do
        }
        out = line.substr(0, i + 1) + std::to_string(m) + line.substr(pos);
        return true;
    }
    return false;
}

// --- Family 4: "INITIAL_TOOL=<digits>" parameter, key case-insensitive ---
constexpr std::string_view INITIAL_TOOL_KEY = "INITIAL_TOOL";

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(a[i])) !=
            std::toupper(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool icontains(std::string_view haystack, std::string_view needle) {
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        if (iequals(haystack.substr(i, needle.size()), needle)) {
            return true;
        }
    }
    return false;
}

// Visits each whitespace-delimited KEY=VALUE token before any ';' comment.
// `key_begin` is the token's offset in `line`.
template <typename Visit> void for_each_param(std::string_view line, Visit&& visit) {
    const size_t end = std::min(line.find(';'), line.size());
    size_t i = 0;
    while (i < end) {
        while (i < end && std::isspace(static_cast<unsigned char>(line[i]))) {
            ++i;
        }
        const size_t token_begin = i;
        while (i < end && !std::isspace(static_cast<unsigned char>(line[i]))) {
            ++i;
        }
        const std::string_view token = line.substr(token_begin, i - token_begin);
        const size_t eq = token.find('=');
        if (eq != std::string_view::npos && eq > 0) {
            visit(token.substr(0, eq), token.substr(eq + 1), token_begin);
        }
    }
}

bool try_initial_tool(const std::string& line, const std::map<int, int>& remap, std::string& out) {
    // The command word itself is never a parameter.
    const size_t first_space = line.find_first_of(" \t");
    if (line.empty() || line[0] == ';' || first_space == std::string::npos) {
        return false;
    }
    bool changed = false;
    for_each_param(std::string_view(line).substr(first_space),
                   [&](std::string_view key, std::string_view value, size_t key_begin) {
                       if (changed || !iequals(key, INITIAL_TOOL_KEY) || value.empty() ||
                           !std::isdigit(static_cast<unsigned char>(value[0]))) {
                           return;
                       }
                       const size_t value_begin = first_space + key_begin + key.size() + 1;
                       size_t pos = value_begin;
                       int idx = parse_uint(line, pos);
                       if (pos != value_begin + value.size()) {
                           return; // "INITIAL_TOOL=1x" is not a tool number
                       }
                       int m = mapped(idx, remap);
                       if (m == idx) {
                           return;
                       }
                       out = line.substr(0, value_begin) + std::to_string(m) + line.substr(pos);
                       changed = true;
                   });
    return changed;
}

// Shared per-line transform. Returns the rewritten line, or `line` unchanged.
// `line` must NOT contain a trailing '\n' (callers split on newlines first).
std::string transform_line(const std::string& line, const std::map<int, int>& remap) {
    std::string out;
    if (try_bare_toolchange(line, remap, out)) {
        return out;
    }
    if (try_prestart(line, remap, out)) {
        return out;
    }
    if (try_temp(line, remap, out)) {
        return out;
    }
    if (try_initial_tool(line, remap, out)) {
        return out;
    }
    return line;
}

// Rewrites each record `next` yields and hands the output to `emit`. A newline
// is emitted BEFORE every line but the first, so the final one is written only
// when the source's last record carried its delimiter.
template <typename NextLine, typename Emit>
size_t remap_lines(NextLine&& next, Emit&& emit, const std::map<int, int>& remap) {
    size_t changed = 0;
    std::string line;
    bool first = true;
    bool had_delim = false;
    bool source_ended_with_newline = false;
    while (next(line, had_delim)) {
        source_ended_with_newline = had_delim;
        if (!first) {
            emit(std::string_view("\n"));
        }
        std::string rewritten = transform_line(line, remap);
        if (rewritten != line) {
            ++changed;
        }
        emit(std::string_view(rewritten));
        first = false;
    }
    if (source_ended_with_newline) {
        emit(std::string_view("\n"));
    }
    return changed;
}

} // namespace

std::optional<size_t> GcodeToolRemapper::apply_to_file(const std::string& in_path,
                                                       const std::string& out_path,
                                                       const std::map<int, int>& remap) {
    text_io::LineReader in(in_path);
    if (!in) {
        return std::nullopt;
    }
    text_io::File out = text_io::open_file(out_path, "wb");
    if (!out) {
        return std::nullopt;
    }
    bool write_ok = true;
    const size_t changed = remap_lines(
        [&](std::string& line, bool& had_delim) {
            if (!in.next(line)) {
                return false;
            }
            had_delim = in.last_had_delimiter();
            return true;
        },
        [&](std::string_view piece) {
            write_ok = text_io::write_all(out.get(), piece) && write_ok;
        },
        remap);
    if (!text_io::close(out) || !write_ok) {
        return std::nullopt;
    }
    return changed;
}

std::string GcodeToolRemapper::apply_to_string(const std::string& gcode,
                                               const std::map<int, int>& remap) {
    const std::string_view text(gcode);
    auto records = text_io::lines(text);
    auto it = records.begin();
    std::string out;
    out.reserve(gcode.size());
    remap_lines(
        [&](std::string& line, bool& had_delim) {
            if (it == records.end()) {
                return false;
            }
            const std::string_view rec = *it;
            line.assign(rec);
            had_delim = rec.data() + rec.size() < text.data() + text.size();
            ++it;
            return true;
        },
        [&](std::string_view piece) { out.append(piece); }, remap);
    return out;
}

std::vector<std::string> GcodeToolRemapper::unremapped_tool_params(std::string_view line) {
    std::vector<std::string> keys;
    for_each_param(line, [&](std::string_view key, std::string_view, size_t) {
        if (iequals(key, INITIAL_TOOL_KEY)) {
            return;
        }
        const bool t_numbered = key.size() >= 2 && (key[0] == 'T' || key[0] == 't') &&
                                std::isdigit(static_cast<unsigned char>(key[1]));
        size_t after_digits = 1;
        while (t_numbered && after_digits < key.size() &&
               std::isdigit(static_cast<unsigned char>(key[after_digits]))) {
            ++after_digits;
        }
        const bool t_key = t_numbered && (after_digits == key.size() || key[after_digits] == '_');
        if (t_key || icontains(key, "EXTRUDER") || icontains(key, "TOOL")) {
            keys.emplace_back(key);
        }
    });
    return keys;
}

} // namespace helix
