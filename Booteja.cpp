// Booteja.cpp — Windows-only single-file CLI to manage UEFI boot vars
// Build (Developer Command Prompt for VS 2022):
//   cl /EHsc /W4 /O2 /DUNICODE /D_UNICODE Booteja.cpp
// Run as Administrator.

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <fcntl.h>
#include <iomanip>
#include <io.h>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// Link against Advapi32 for token privilege APIs
#pragma comment(lib, "Advapi32.lib")

// Some Windows SDKs don't expose EFI variable attribute flags in headers.
// Define them if missing so SetFirmwareEnvironmentVariableExW gets valid attrs.
#ifndef EFI_VARIABLE_NON_VOLATILE
#define EFI_VARIABLE_NON_VOLATILE 0x00000001
#endif
#ifndef EFI_VARIABLE_BOOTSERVICE_ACCESS
#define EFI_VARIABLE_BOOTSERVICE_ACCESS 0x00000002
#endif
#ifndef EFI_VARIABLE_RUNTIME_ACCESS
#define EFI_VARIABLE_RUNTIME_ACCESS 0x00000004
#endif

static const wchar_t* EFI_GLOBAL_VARIABLE_GUID = L"{8BE4DF61-93CA-11D2-AA0D-00E098032B8C}";

// UEFI Load Option Attributes (subset)
constexpr UINT32 LOAD_OPTION_ACTIVE = 0x00000001;
constexpr UINT32 LOAD_OPTION_FORCE_RECONNECT = 0x00000002;
constexpr UINT32 LOAD_OPTION_HIDDEN = 0x00000008;

constexpr DWORD DEFAULT_EFI_READ_BUFFER_SIZE = 4096;
constexpr size_t BOOT_OPTION_HEADER_SIZE = 6;

static DWORD g_varAttrsRW =
    EFI_VARIABLE_NON_VOLATILE |
    EFI_VARIABLE_BOOTSERVICE_ACCESS |
    EFI_VARIABLE_RUNTIME_ACCESS;

struct ParsedLoadOption {
    UINT32 Attributes = 0;
    UINT16 FilePathListLength = 0;
    std::wstring Description;
    std::vector<BYTE> DevicePath;
    std::vector<BYTE> OptionalData;
};

// ----------------- Utilities -----------------
std::wstring LastErrorMessage(DWORD err = GetLastError()) {
    LPWSTR buf = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                        FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;

    FormatMessageW(flags, nullptr, err, 0, (LPWSTR)&buf, 0, nullptr);

    std::wstring msg = buf ? buf : L"";
    if (buf) {
        LocalFree(buf);
    }

    while (!msg.empty() && (msg.back() == L'\n' || msg.back() == L'\r')) {
        msg.pop_back();
    }

    std::wstringstream ss;
    ss << L"(error " << err << L") " << msg;
    return ss.str();
}

bool EnableSystemEnvironmentPrivilege() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        std::wcerr << L"OpenProcessToken failed: " << LastErrorMessage() << L"\n";
        return false;
    }

    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;

    if (!LookupPrivilegeValueW(nullptr, SE_SYSTEM_ENVIRONMENT_NAME, &tp.Privileges[0].Luid)) {
        std::wcerr << L"LookupPrivilegeValueW failed: " << LastErrorMessage() << L"\n";
        CloseHandle(hToken);
        return false;
    }

    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr)) {
        std::wcerr << L"AdjustTokenPrivileges failed: " << LastErrorMessage() << L"\n";
        CloseHandle(hToken);
        return false;
    }

    CloseHandle(hToken);
    return GetLastError() != ERROR_NOT_ALL_ASSIGNED;
}

std::wstring MakeBootVarName(UINT16 id) {
    std::wstringstream var;
    var << L"Boot" << std::uppercase << std::hex << std::setw(4) << std::setfill(L'0') << id;
    return var.str();
}

bool ParseBootId(const std::wstring& text, UINT16& outId) {
    std::wstring token = text;

    if (token.rfind(L"Boot", 0) == 0 || token.rfind(L"boot", 0) == 0) {
        token = token.substr(4);
    }

    unsigned value = 0;
    std::wstringstream hs;
    hs << std::hex << token;
    if (!(hs >> value) || value > 0xFFFF) {
        return false;
    }

    outId = static_cast<UINT16>(value);
    return true;
}

std::vector<BYTE> ReadEfiVar(const std::wstring& name, DWORD& attrsOut) {
    DWORD required = GetFirmwareEnvironmentVariableExW(
        name.c_str(), EFI_GLOBAL_VARIABLE_GUID, nullptr, 0, &attrsOut);

    if (required == 0 && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return {};
    }

    if (required == 0) {
        required = DEFAULT_EFI_READ_BUFFER_SIZE;
    }

    std::vector<BYTE> buf(required);
    DWORD gotAttrs = 0;

    DWORD read = GetFirmwareEnvironmentVariableExW(
        name.c_str(), EFI_GLOBAL_VARIABLE_GUID, buf.data(), required, &gotAttrs);

    if (read == 0 && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        const DWORD newSize = GetFirmwareEnvironmentVariableExW(
            name.c_str(), EFI_GLOBAL_VARIABLE_GUID, nullptr, 0, &gotAttrs);

        if (newSize > buf.size()) {
            buf.resize(newSize);
            read = GetFirmwareEnvironmentVariableExW(
                name.c_str(), EFI_GLOBAL_VARIABLE_GUID, buf.data(), newSize, &gotAttrs);
        }
    }

    if (read == 0) {
        return {};
    }

    buf.resize(read);
    attrsOut = gotAttrs;
    return buf;
}

bool WriteEfiVar(const std::wstring& name, const void* data, DWORD size, DWORD attrs) {
    if (!SetFirmwareEnvironmentVariableExW(name.c_str(), EFI_GLOBAL_VARIABLE_GUID, (PVOID)data, size, attrs)) {
        std::wcerr << L"Write '" << name << L"' failed: " << LastErrorMessage() << L"\n";
        return false;
    }
    return true;
}

std::wstring ReadUcs2String(const std::vector<BYTE>& data, size_t start, size_t& nextOffset) {
    std::wstring out;
    size_t i = start;

    while (i + 1 < data.size()) {
        const wchar_t ch = static_cast<wchar_t>(data[i] | (data[i + 1] << 8));
        i += 2;
        if (ch == L'\0') {
            break;
        }
        out.push_back(ch);
    }

    if (i & 1) {
        ++i;
    }

    nextOffset = i;
    return out;
}

std::wstring HexPreview(const BYTE* p, size_t n) {
    std::wstringstream ss;
    ss << std::hex << std::setfill(L'0');

    const size_t count = std::min<size_t>(n, 64);
    for (size_t i = 0; i < count; ++i) {
        ss << std::setw(2) << static_cast<unsigned>(p[i]) << L' ';
    }

    return ss.str();
}

bool ParseLoadOption(const std::vector<BYTE>& buf, ParsedLoadOption& plo) {
    if (buf.size() < BOOT_OPTION_HEADER_SIZE) {
        return false;
    }

    memcpy(&plo.Attributes, buf.data(), sizeof(UINT32));
    memcpy(&plo.FilePathListLength, buf.data() + sizeof(UINT32), sizeof(UINT16));

    size_t offset = BOOT_OPTION_HEADER_SIZE;
    size_t next = 0;
    plo.Description = ReadUcs2String(buf, offset, next);
    offset = next;

    if (offset + plo.FilePathListLength > buf.size()) {
        return false;
    }

    plo.DevicePath.assign(buf.begin() + offset, buf.begin() + offset + plo.FilePathListLength);
    offset += plo.FilePathListLength;

    if (offset <= buf.size()) {
        plo.OptionalData.assign(buf.begin() + offset, buf.end());
    }

    return true;
}

std::vector<BYTE> BuildLoadOption(const ParsedLoadOption& plo) {
    std::vector<BYTE> out;
    out.reserve(
        BOOT_OPTION_HEADER_SIZE +
        (plo.Description.size() + 1) * sizeof(UINT16) +
        plo.DevicePath.size() +
        plo.OptionalData.size());

    auto push32 = [&](UINT32 v) {
        const size_t i = out.size();
        out.resize(i + sizeof(UINT32));
        memcpy(out.data() + i, &v, sizeof(UINT32));
    };

    auto push16 = [&](UINT16 v) {
        const size_t i = out.size();
        out.resize(i + sizeof(UINT16));
        memcpy(out.data() + i, &v, sizeof(UINT16));
    };

    push32(plo.Attributes);
    push16(static_cast<UINT16>(plo.DevicePath.size()));

    // Description (UTF-16LE null-terminated)
    for (wchar_t ch : plo.Description) {
        push16(static_cast<UINT16>(ch));
    }
    push16(0);

    out.insert(out.end(), plo.DevicePath.begin(), plo.DevicePath.end());
    out.insert(out.end(), plo.OptionalData.begin(), plo.OptionalData.end());

    return out;
}

void PrintEntry(UINT16 id, size_t index, size_t total, const ParsedLoadOption& plo) {
    std::wcout << L"\n[" << index << L"/" << total << L"] " << MakeBootVarName(id) << L"\n";
    std::wcout << L"    Attributes: 0x" << std::hex << plo.Attributes << std::dec << L"\n";
    std::wcout << L"      - Active: " << ((plo.Attributes & LOAD_OPTION_ACTIVE) ? L"yes" : L"no") << L"\n";
    std::wcout << L"      - ForceReconnect: " << ((plo.Attributes & LOAD_OPTION_FORCE_RECONNECT) ? L"yes" : L"no") << L"\n";
    std::wcout << L"      - Hidden: " << ((plo.Attributes & LOAD_OPTION_HIDDEN) ? L"yes" : L"no") << L"\n";
    std::wcout << L"    Description: " << (plo.Description.empty() ? L"(none)" : plo.Description) << L"\n";
    std::wcout << L"    DevicePath bytes: " << plo.DevicePath.size() << L"\n";
    std::wcout << L"    DevicePath hex preview: " << HexPreview(plo.DevicePath.data(), plo.DevicePath.size()) << L"\n";
    std::wcout << L"    OptionalData bytes: " << plo.OptionalData.size() << L"\n";
}

// ----------------- Helpers -----------------
std::vector<UINT16> GetBootOrder() {
    DWORD attrs = 0;
    const auto v = ReadEfiVar(L"BootOrder", attrs);

    std::vector<UINT16> ids;
    if (v.size() % sizeof(UINT16) != 0) {
        return ids;
    }

    ids.reserve(v.size() / sizeof(UINT16));
    for (size_t i = 0; i < v.size(); i += sizeof(UINT16)) {
        UINT16 id = 0;
        memcpy(&id, v.data() + i, sizeof(UINT16));
        ids.push_back(id);
    }

    return ids;
}

bool SetBootOrder(const std::vector<UINT16>& order) {
    return WriteEfiVar(
        L"BootOrder",
        order.data(),
        static_cast<DWORD>(order.size() * sizeof(UINT16)),
        g_varAttrsRW);
}

bool ReadBootEntry(UINT16 id, ParsedLoadOption& plo, DWORD& attrsOut) {
    const auto name = MakeBootVarName(id);
    const auto data = ReadEfiVar(name, attrsOut);
    if (data.empty()) {
        return false;
    }
    return ParseLoadOption(data, plo);
}

bool WriteBootEntry(UINT16 id, const ParsedLoadOption& plo) {
    const auto name = MakeBootVarName(id);
    const auto blob = BuildLoadOption(plo);
    return WriteEfiVar(name, blob.data(), static_cast<DWORD>(blob.size()), g_varAttrsRW);
}

// ----------------- Commands -----------------
int cmd_list() {
    DWORD attrs = 0;
    const auto order = ReadEfiVar(L"BootOrder", attrs);

    if (order.empty() || (order.size() % sizeof(UINT16) != 0)) {
        std::wcerr << L"Could not read BootOrder: " << LastErrorMessage() << L"\n";
        return 1;
    }

    const size_t n = order.size() / sizeof(UINT16);

    auto showU16 = [&](const std::wstring& varName) {
        DWORD varAttrs = 0;
        const auto value = ReadEfiVar(varName, varAttrs);
        if (value.size() < sizeof(UINT16)) {
            return;
        }

        UINT16 id = 0;
        memcpy(&id, value.data(), sizeof(UINT16));
        std::wcout << varName << L": " << MakeBootVarName(id) << L"\n";
    };

    showU16(L"BootCurrent");
    showU16(L"BootNext");

    for (size_t i = 0; i < n; ++i) {
        UINT16 id = 0;
        memcpy(&id, order.data() + i * sizeof(UINT16), sizeof(UINT16));

        ParsedLoadOption plo;
        DWORD entryAttrs = 0;
        if (ReadBootEntry(id, plo, entryAttrs)) {
            PrintEntry(id, i + 1, n, plo);
        } else {
            std::wcout << L"\n[" << i + 1 << L"/" << n << L"] " << MakeBootVarName(id) << L": (unreadable)\n";
        }
    }

    return 0;
}

int cmd_order_show() {
    const auto order = GetBootOrder();
    if (order.empty()) {
        std::wcerr << L"BootOrder empty: " << LastErrorMessage() << L"\n";
        return 1;
    }

    std::wcout << L"BootOrder (" << order.size() << L"):";
    for (const auto id : order) {
        std::wcout << L" " << MakeBootVarName(id);
    }
    std::wcout << L"\n";

    return 0;
}

int cmd_order_set(const std::wstring& csv) {
    std::vector<UINT16> newOrder;
    std::wstringstream ss(csv);
    std::wstring token;

    while (std::getline(ss, token, L',')) {
        token.erase(std::remove_if(token.begin(), token.end(), [](wchar_t ch) { return std::iswspace(ch) != 0; }), token.end());

        UINT16 id = 0;
        if (!ParseBootId(token, id)) {
            std::wcerr << L"Bad id: " << token << L"\n";
            return 2;
        }

        newOrder.push_back(id);
    }

    if (newOrder.empty()) {
        std::wcerr << L"No IDs provided.\n";
        return 2;
    }

    if (!SetBootOrder(newOrder)) {
        return 3;
    }

    std::wcout << L"BootOrder updated.\n";
    return 0;
}

int cmd_select(const std::wstring& idhex) {
    auto order = GetBootOrder();
    if (order.empty()) {
        return 1;
    }

    UINT16 target = 0;
    if (!ParseBootId(idhex, target)) {
        std::wcerr << L"Bad id.\n";
        return 2;
    }

    auto it = std::find(order.begin(), order.end(), target);
    if (it == order.end()) {
        std::wcerr << L"ID not found in BootOrder.\n";
        return 3;
    }

    std::rotate(order.begin(), it, it + 1); // bring to front
    if (!SetBootOrder(order)) {
        return 4;
    }

    std::wcout << L"Default boot set to " << MakeBootVarName(target) << L".\n";
    return 0;
}

int cmd_next(const std::wstring& idhex) {
    UINT16 target = 0;
    if (!ParseBootId(idhex, target)) {
        std::wcerr << L"Bad id.\n";
        return 2;
    }

    if (!WriteEfiVar(L"BootNext", &target, sizeof(target), g_varAttrsRW)) {
        return 3;
    }

    std::wcout << L"BootNext set to " << MakeBootVarName(target) << L" (one-time).\n";
    return 0;
}

int cmd_enable_disable(const std::wstring& idhex, bool enable) {
    UINT16 id = 0;
    if (!ParseBootId(idhex, id)) {
        std::wcerr << L"Bad id.\n";
        return 2;
    }

    ParsedLoadOption plo;
    DWORD attrs = 0;
    if (!ReadBootEntry(id, plo, attrs)) {
        std::wcerr << L"Entry not found.\n";
        return 3;
    }

    if (enable) {
        plo.Attributes |= LOAD_OPTION_ACTIVE;
    } else {
        plo.Attributes &= ~LOAD_OPTION_ACTIVE;
    }

    if (!WriteBootEntry(id, plo)) {
        return 4;
    }

    std::wcout << (enable ? L"Enabled " : L"Disabled ") << MakeBootVarName(id) << L".\n";
    return 0;
}

int cmd_rename(const std::wstring& idhex, const std::wstring& newLabel) {
    UINT16 id = 0;
    if (!ParseBootId(idhex, id)) {
        std::wcerr << L"Bad id.\n";
        return 2;
    }

    ParsedLoadOption plo;
    DWORD attrs = 0;
    if (!ReadBootEntry(id, plo, attrs)) {
        std::wcerr << L"Entry not found.\n";
        return 3;
    }

    ParsedLoadOption updated = plo;
    updated.Description = newLabel;

    if (!WriteBootEntry(id, updated)) {
        return 4;
    }

    std::wcout << L"Renamed " << MakeBootVarName(id) << L" to '" << newLabel << L"'.\n";
    return 0;
}

int cmd_dump() {
    DWORD attrs = 0;
    const auto orderRaw = ReadEfiVar(L"BootOrder", attrs);
    if (orderRaw.empty()) {
        std::wcerr << L"BootOrder read failed: " << LastErrorMessage() << L"\n";
        return 1;
    }

    std::wcout << L"BootOrder bytes: " << orderRaw.size() << L"\n";

    const auto order = GetBootOrder();
    size_t index = 0;
    for (const auto id : order) {
        DWORD varAttrs = 0;
        const auto name = MakeBootVarName(id);
        const auto data = ReadEfiVar(name, varAttrs);

        std::wcout << L"[" << ++index << L"] "
                   << name
                   << L" size=" << data.size()
                   << L" attrs=0x" << std::hex << varAttrs << std::dec
                   << L"\n";
    }

    return 0;
}

void PrintHelp() {
    std::wcout
        << L"Booteja — Windows UEFI Boot utility\n\n"
        << L"Usage: booteja <command> [options]\n\n"
        << L"Commands:\n"
        << L"  list                              List Boot#### entries and BootOrder\n"
        << L"  order                             Show BootOrder\n"
        << L"  order set <id[,id,...]>           Set BootOrder (hex IDs or BootXXXX)\n"
        << L"  select <id>                       Make ID first in BootOrder (default)\n"
        << L"  next <id>                         Set BootNext one-time target\n"
        << L"  enable <id> / disable <id>        Toggle LOAD_OPTION_ACTIVE\n"
        << L"  rename <id> \"New Label\"          Rename entry description\n"
        << L"  dump                              Raw sizes/attrs diagnostic\n"
        << L"\nExamples:\n"
        << L"  booteja list\n"
        << L"  booteja order\n"
        << L"  booteja select 0003\n"
        << L"  booteja next 0004\n"
        << L"  booteja order set 0004,0001,0003,0002\n"
        << L"  booteja rename 0002 \"Ubuntu NVMe\"\n";

    std::wcout.flush();
}

int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_U16TEXT);
    _setmode(_fileno(stderr), _O_U16TEXT);

    std::wcout << L"Booteja (Windows / UEFI)\n";
    if (!EnableSystemEnvironmentPrivilege()) {
        std::wcerr << L"Warning: Could not enable SeSystemEnvironmentPrivilege. Run elevated on a UEFI system.\n";
    }

    if (argc < 2) {
        PrintHelp();
        return 0;
    }

    std::wstring cmd = argv[1];
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::towlower);

    if (cmd == L"list") {
        return cmd_list();
    }

    if (cmd == L"order") {
        if (argc >= 3) {
            std::wstring sub = argv[2];
            std::transform(sub.begin(), sub.end(), sub.begin(), ::towlower);
            if (sub == L"set" && argc >= 4) {
                return cmd_order_set(argv[3]);
            }
        }
        return cmd_order_show();
    }

    if (cmd == L"select" && argc >= 3) {
        return cmd_select(argv[2]);
    }

    if (cmd == L"next" && argc >= 3) {
        return cmd_next(argv[2]);
    }

    if (cmd == L"enable" && argc >= 3) {
        return cmd_enable_disable(argv[2], true);
    }

    if (cmd == L"disable" && argc >= 3) {
        return cmd_enable_disable(argv[2], false);
    }

    if (cmd == L"rename" && argc >= 4) {
        std::wstring label = argv[3];
        for (int i = 4; i < argc; ++i) {
            label += L" ";
            label += argv[i];
        }
        return cmd_rename(argv[2], label);
    }

    if (cmd == L"dump") {
        return cmd_dump();
    }

    PrintHelp();
    return 0;
}
