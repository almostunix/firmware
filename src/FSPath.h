#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Resolve "//", "." and ".." the way littlefs does, so a check sees the file that will actually be opened.
inline std::string canonicalFsPath(const char *path, size_t maxLen = SIZE_MAX)
{
    std::vector<std::string> parts;
    std::string part;
    const size_t len = strnlen(path, maxLen);
    for (size_t i = 0; i <= len; i++) {
        if (i < len && path[i] != '/') {
            part += path[i];
            continue;
        }
        if (part == "..") {
            if (!parts.empty())
                parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        part.clear();
    }
    std::string out;
    for (const auto &p : parts)
        out += "/" + p;
    return out.empty() ? "/" : out;
}

// True if a canonical path is dir itself or inside it.
inline bool fsPathIsWithin(const std::string &canonical, const char *dir)
{
    const size_t n = strlen(dir);
    return canonical.compare(0, n, dir) == 0 && (canonical.size() == n || canonical[n] == '/');
}

// The node's Tailscale auth key and private keys (TailscaleSocket.cpp): never served, listed or overwritten.
inline bool isTailscaleSecretPath(const std::string &canonical)
{
    return canonical.rfind("/prefs/tailscale", 0) == 0;
}
