#include "Presets.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdio>

#include "NetConfig.h"

namespace presets {
namespace {

std::wstring trim(const std::wstring& s) {
    size_t first = s.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    size_t last = s.find_last_not_of(L" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::wstring executableDirectory() {
    wchar_t path[MAX_PATH] = {};
    DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    std::wstring full(path, length);
    size_t slash = full.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : full.substr(0, slash + 1);
}

// The file is small and hand-edited, so it is read whole and decoded as UTF-8
// with a BOM tolerated -- that is what an editor on this machine will write.
bool readTextFile(const std::wstring& path, std::wstring& text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > (1 << 20)) {
        CloseHandle(file);
        return false;
    }

    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    bool ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) != 0;
    CloseHandle(file);
    if (!ok) return false;
    bytes.resize(read);

    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bytes.erase(0, 3);
    }

    int need = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()),
                                   nullptr, 0);
    if (need <= 0) { text.clear(); return true; }
    text.resize(static_cast<size_t>(need));
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()),
                        text.data(), need);
    return true;
}

bool isAddress(const std::wstring& text) {
    IN_ADDR a{};
    return !text.empty() && InetPtonW(AF_INET, text.c_str(), &a) == 1;
}

// "192.168.50.10/24" or "192.168.50.10/255.255.255.0". The prefix form is what
// people write; the dotted form is what the rest of the app deals in.
bool parseAddressAndMask(const std::wstring& spec, std::wstring& ip, std::wstring& mask) {
    size_t slash = spec.find(L'/');
    if (slash == std::wstring::npos) return false;
    ip = trim(spec.substr(0, slash));
    std::wstring suffix = trim(spec.substr(slash + 1));
    if (!isAddress(ip) || suffix.empty()) return false;

    if (suffix.find(L'.') != std::wstring::npos) {
        int prefix = 0;
        if (!net::prefixFromMask(suffix, prefix)) return false;   // rejects a gappy mask
        mask = suffix;
        return true;
    }

    int prefix = 0;
    for (wchar_t c : suffix) {
        if (c < L'0' || c > L'9') return false;
        prefix = prefix * 10 + (c - L'0');
        if (prefix > 32) return false;
    }
    if (prefix < 1) return false;
    mask = net::maskFromPrefix(prefix);
    return !mask.empty();
}

// Accepts either "Name = 10.0.0.5/24" or the barer "Name 10.0.0.5/24", with an
// optional gateway after a comma. Names may contain spaces in the '=' form.
bool parseLine(const std::wstring& line, Preset& preset) {
    std::wstring rest = line;
    std::wstring gateway;

    size_t comma = rest.find(L',');
    if (comma != std::wstring::npos) {
        gateway = trim(rest.substr(comma + 1));
        rest = rest.substr(0, comma);
        if (!gateway.empty() && !isAddress(gateway)) return false;
    }

    std::wstring name, spec;
    size_t equals = rest.find(L'=');
    if (equals != std::wstring::npos) {
        name = trim(rest.substr(0, equals));
        spec = trim(rest.substr(equals + 1));
    } else {
        std::wstring trimmed = trim(rest);
        size_t space = trimmed.find_last_of(L" \t");
        if (space == std::wstring::npos) return false;
        name = trim(trimmed.substr(0, space));
        spec = trim(trimmed.substr(space + 1));
    }
    if (name.empty()) return false;

    if (!parseAddressAndMask(spec, preset.ip, preset.mask)) return false;
    preset.name = name;
    preset.gateway = gateway;
    return true;
}

}  // namespace

std::vector<Preset> load(std::wstring& path, std::wstring& problems) {
    problems.clear();
    std::vector<Preset> out;

    std::wstring directory = executableDirectory();
    path = directory + L"BNET.presets.txt";

    std::wstring text;
    if (!readTextFile(path, text)) return out;   // absent is normal, not an error

    int skipped = 0;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = trim(text.substr(start, end - start));
        start = end + 1;

        if (line.empty() || line[0] == L'#' || line[0] == L';') continue;

        Preset preset;
        if (parseLine(line, preset))
            out.push_back(preset);
        else
            ++skipped;

        if (end == text.size()) break;
    }

    if (skipped > 0) {
        wchar_t buf[96];
        swprintf_s(buf, L"%d line%s in the presets file could not be read.",
                   skipped, skipped == 1 ? L"" : L"s");
        problems = buf;
    }
    return out;
}

}  // namespace presets
