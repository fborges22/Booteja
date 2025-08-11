// Booteja.cpp — Windows-only single-file CLI to manage UEFI boot vars
// Build (Developer Command Prompt for VS 2022):
//   cl /EHsc /W4 /O2 /DUNICODE /D_UNICODE Booteja.cpp
// Run as Administrator.

#include <windows.h>
#include <iostream>
#include <vector>
#include <string>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <io.h>
#include <fcntl.h>

// Link against Advapi32 for token privilege APIs
#pragma comment(lib, "Advapi32.lib")

// Some Windows SDKs don't expose EFI variable attribute flags in headers.
// Define them if missing so SetFirmwareEnvironmentVariableExW gets valid attrs.
#ifndef EFI_VARIABLE_NON_VOLATILE
#define EFI_VARIABLE_NON_VOLATILE            0x00000001
#endif
#ifndef EFI_VARIABLE_BOOTSERVICE_ACCESS
#define EFI_VARIABLE_BOOTSERVICE_ACCESS      0x00000002
#endif
#ifndef EFI_VARIABLE_RUNTIME_ACCESS
#define EFI_VARIABLE_RUNTIME_ACCESS          0x00000004
#endif
// Define EFI variable attributes for Windows build
#ifndef EFI_VARIABLE_NON_VOLATILE
#define EFI_VARIABLE_NON_VOLATILE        0x00000001
#define EFI_VARIABLE_BOOTSERVICE_ACCESS  0x00000002
#define EFI_VARIABLE_RUNTIME_ACCESS      0x00000004
#endif

static const wchar_t* EFI_GLOBAL_VARIABLE_GUID = L"{8BE4DF61-93CA-11D2-AA0D-00E098032B8C}";

// UEFI Load Option Attributes (subset)
constexpr UINT32 LOAD_OPTION_ACTIVE = 0x00000001;
constexpr UINT32 LOAD_OPTION_FORCE_RECONNECT = 0x00000002;
constexpr UINT32 LOAD_OPTION_HIDDEN = 0x00000008;

// ----------------- Utilities -----------------
std::wstring LastErrorMessage(DWORD err = GetLastError()) {
    LPWSTR buf = nullptr;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    FormatMessageW(flags, nullptr, err, 0, (LPWSTR)&buf, 0, nullptr);
    std::wstring msg = buf ? buf : L"";
    if (buf) LocalFree(buf);
    while (!msg.empty() && (msg.back() == L'\n' || msg.back() == L'\r')) msg.pop_back();
    std::wstringstream ss; ss << L"(error " << err << L") " << msg; return ss.str();
}

bool EnableSystemEnvironmentPrivilege() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        std::wcerr << L"OpenProcessToken failed: " << LastErrorMessage() << L"\n"; return false;
    }
    TOKEN_PRIVILEGES tp{}; tp.PrivilegeCount = 1;
    if (!LookupPrivilegeValueW(nullptr, SE_SYSTEM_ENVIRONMENT_NAME, &tp.Privileges[0].Luid)) {
        std::wcerr << L"LookupPrivilegeValueW failed: " << LastErrorMessage() << L"\n"; CloseHandle(hToken); return false;
    }
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr)) {
        std::wcerr << L"AdjustTokenPrivileges failed: " << LastErrorMessage() << L"\n"; CloseHandle(hToken); return false;
    }
    CloseHandle(hToken);
    return GetLastError() != ERROR_NOT_ALL_ASSIGNED;
}

std::vector<BYTE> ReadEfiVar(const std::wstring& name, DWORD& attrsOut) {
    DWORD required = GetFirmwareEnvironmentVariableExW(name.c_str(), EFI_GLOBAL_VARIABLE_GUID, nullptr, 0, &attrsOut);
    if (required == 0 && GetLastError() != ERROR_INSUFFICIENT_BUFFER) return {};
    if (required == 0) required = 4096; // fallback
    std::vector<BYTE> buf(required);
    DWORD gotAttrs = 0;
    DWORD read = GetFirmwareEnvironmentVariableExW(name.c_str(), EFI_GLOBAL_VARIABLE_GUID, buf.data(), required, &gotAttrs);
    if (read == 0 && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        DWORD newSize = GetFirmwareEnvironmentVariableExW(name.c_str(), EFI_GLOBAL_VARIABLE_GUID, nullptr, 0, &gotAttrs);
        if (newSize > buf.size()) { buf.resize(newSize); read = GetFirmwareEnvironmentVariableExW(name.c_str(), EFI_GLOBAL_VARIABLE_GUID, buf.data(), newSize, &gotAttrs); }
    }
    if (read == 0) return {};
    buf.resize(read); attrsOut = gotAttrs; return buf;
}

bool WriteEfiVar(const std::wstring& name, const void* data, DWORD size, DWORD attrs) {
    if (!SetFirmwareEnvironmentVariableExW(name.c_str(), EFI_GLOBAL_VARIABLE_GUID, (PVOID)data, size, attrs)) {
        std::wcerr << L"Write '" << name << L"' failed: " << LastErrorMessage() << L"\n"; return false;
    }
    return true;
}

std::wstring ReadUcs2String(const std::vector<BYTE>& data, size_t start, size_t& nextOffset) {
    std::wstring out; size_t i = start;
    while (i + 1 < data.size()) { wchar_t ch = static_cast<wchar_t>(data[i] | (data[i + 1] << 8)); i += 2; if (ch == L'\0') break; out.push_back(ch); }
    if (i & 1) i++; nextOffset = i; return out;
}

std::wstring HexPreview(const BYTE* p, size_t n) {
    std::wstringstream ss; ss << std::hex << std::setfill(L'0'); size_t count = std::min<size_t>(n, 64);
    for (size_t i = 0; i < count; ++i) ss << std::setw(2) << static_cast<unsigned>(p[i]) << L' ';
    return ss.str();
}

struct ParsedLoadOption {
    UINT32 Attributes = 0; UINT16 FilePathListLength = 0; std::wstring Description; std::vector<BYTE> DevicePath; std::vector<BYTE> OptionalData;
};

bool ParseLoadOption(const std::vector<BYTE>& buf, ParsedLoadOption& plo) {
    if (buf.size() < 6) return false;
    plo.Attributes = *reinterpret_cast<const UINT32*>(&buf[0]);
    plo.FilePathListLength = *reinterpret_cast<const UINT16*>(&buf[4]);
    size_t offset = 6; size_t next = 0; plo.Description = ReadUcs2String(buf, offset, next); offset = next;
    if (offset + plo.FilePathListLength <= buf.size()) {
        plo.DevicePath.assign(buf.begin() + offset, buf.begin() + offset + plo.FilePathListLength); offset += plo.FilePathListLength;
    }
    else { return false; }
    if (offset <= buf.size()) plo.OptionalData.assign(buf.begin() + offset, buf.end());
    return true;
}

std::vector<BYTE> BuildLoadOption(const ParsedLoadOption& plo) {
    std::vector<BYTE> out; out.reserve(6 + (plo.Description.size() + 1) * 2 + plo.DevicePath.size() + plo.OptionalData.size());
    auto push32 = [&](UINT32 v) { size_t i = out.size(); out.resize(i + 4); memcpy(out.data() + i, &v, 4); };
    auto push16 = [&](UINT16 v) { size_t i = out.size(); out.resize(i + 2); memcpy(out.data() + i, &v, 2); };
    push32(plo.Attributes);
    push16(static_cast<UINT16>(plo.DevicePath.size()));
    // Description (UTF-16LE null-terminated)
    for (wchar_t ch : plo.Description) { push16(static_cast<UINT16>(ch)); }
    push16(0);
    // DevicePath
    out.insert(out.end(), plo.DevicePath.begin(), plo.DevicePath.end());
    // OptionalData
    out.insert(out.end(), plo.OptionalData.begin(), plo.OptionalData.end());
    return out;
}

static DWORD g_varAttrsRW = EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;

// ----------------- Helpers -----------------
std::vector<UINT16> GetBootOrder() {
    DWORD a = 0; auto v = ReadEfiVar(L"BootOrder", a); std::vector<UINT16> ids; if (v.size() % 2) return ids;
    ids.assign(reinterpret_cast<const UINT16*>(v.data()), reinterpret_cast<const UINT16*>(v.data() + v.size())); return ids;
}

bool SetBootOrder(const std::vector<UINT16>& order) {
    return WriteEfiVar(L"BootOrder", order.data(), static_cast<DWORD>(order.size() * sizeof(UINT16)), g_varAttrsRW);
}

bool ReadBootEntry(UINT16 id, ParsedLoadOption& plo, DWORD& attrsOut) {
    std::wstringstream var; var << L"Boot" << std::uppercase << std::hex << std::setw(4) << std::setfill(L'0') << id; std::wstring name = var.str();
    auto data = ReadEfiVar(name, attrsOut); if (data.empty()) return false; return ParseLoadOption(data, plo);
}

bool WriteBootEntry(UINT16 id, const ParsedLoadOption& plo) {
    std::wstringstream var; var << L"Boot" << std::uppercase << std::hex << std::setw(4) << std::setfill(L'0') << id; std::wstring name = var.str();
    auto blob = BuildLoadOption(plo); return WriteEfiVar(name, blob.data(), static_cast<DWORD>(blob.size()), g_varAttrsRW);
}

void PrintEntry(UINT16 id, size_t index, size_t total, const ParsedLoadOption& plo) {
    std::wcout << L"\n[" << index << L"/" << total << L"] Boot" << std::uppercase << std::hex << std::setw(4) << std::setfill(L'0') << id << std::nouppercase << std::dec << L"\n";
    std::wcout << L"    Attributes: 0x" << std::hex << plo.Attributes << std::dec << L"\n";
    std::wcout << L"      - Active: " << ((plo.Attributes & LOAD_OPTION_ACTIVE) ? L"yes" : L"no") << L"\n";
    std::wcout << L"      - ForceReconnect: " << ((plo.Attributes & LOAD_OPTION_FORCE_RECONNECT) ? L"yes" : L"no") << L"\n";
    std::wcout << L"      - Hidden: " << ((plo.Attributes & LOAD_OPTION_HIDDEN) ? L"yes" : L"no") << L"\n";
    std::wcout << L"    Description: " << (plo.Description.empty() ? L"(none)" : plo.Description) << L"\n";
    std::wcout << L"    DevicePath bytes: " << plo.DevicePath.size() << L"\n";
    std::wcout << L"    DevicePath hex preview: " << HexPreview(plo.DevicePath.data(), plo.DevicePath.size()) << L"\n";
    std::wcout << L"    OptionalData bytes: " << plo.OptionalData.size() << L"\n";
}

// ----------------- Commands -----------------
int cmd_list() {
    DWORD a = 0; auto order = ReadEfiVar(L"BootOrder", a); if (order.empty() || (order.size() % 2)) {
        std::wcerr << L"Could not read BootOrder: " << LastErrorMessage() << L"\n"; return 1;
    }
    const UINT16* ids = reinterpret_cast<const UINT16*>(order.data()); size_t n = order.size() / 2;

    // Show BootCurrent/BootNext
    auto showU16 = [&](const std::wstring& nm) { DWORD aa = 0; auto v = ReadEfiVar(nm, aa); if (v.size() >= 2) { UINT16 val = *reinterpret_cast<const UINT16*>(v.data()); std::wcout << nm << L": Boot" << std::uppercase << std::hex << std::setw(4) << std::setfill(L'0') << val << std::nouppercase << std::dec << L"\n"; } };
    showU16(L"BootCurrent"); showU16(L"BootNext");

    for (size_t i = 0; i < n; ++i) { UINT16 id = ids[i]; ParsedLoadOption plo; DWORD vA = 0; if (ReadBootEntry(id, plo, vA)) PrintEntry(id, i + 1, n, plo); else std::wcout << L"\n[" << i + 1 << L"/" << n << L"] Boot" << std::uppercase << std::hex << std::setw(4) << std::setfill(L'0') << id << std::nouppercase << std::dec << L": (unreadable)\n"; }
    return 0;
}

int cmd_order_show() {
    auto order = GetBootOrder(); if (order.empty()) { std::wcerr << L"BootOrder empty: " << LastErrorMessage() << L"\n"; return 1; }
    std::wcout << L"BootOrder (" << order.size() << L"):";
    for (auto id : order) { std::wcout << L" Boot" << std::uppercase << std::hex << std::setw(4) << std::setfill(L'0') << id << std::nouppercase << std::dec; }
    std::wcout << L"\n"; return 0;
}

int cmd_order_set(const std::wstring& csv) {
    // parse ids like 0004,0001,... possibly with or without 0x
    std::vector<UINT16> newOrder; std::wstringstream ss(csv); std::wstring tok;
    while (std::getline(ss, tok, L',')) {
        tok.erase(std::remove_if(tok.begin(), tok.end(), ::iswspace), tok.end());
        if (tok.rfind(L"Boot", 0) == 0) tok = tok.substr(4);
        unsigned v = 0; std::wstringstream hs; hs << std::hex << tok; if (!(hs >> v) || v > 0xFFFF) { std::wcerr << L"Bad id: " << tok << L"\n"; return 2; }
        newOrder.push_back(static_cast<UINT16>(v));
    }
    if (newOrder.empty()) { std::wcerr << L"No IDs provided.\n"; return 2; }
    if (!SetBootOrder(newOrder)) return 3; std::wcout << L"BootOrder updated.\n"; return 0;
}

int cmd_select(const std::wstring& idhex) {
    auto order = GetBootOrder(); if (order.empty()) return 1;
    unsigned v = 0; { std::wstringstream hs; std::wstring tok = idhex; if (tok.rfind(L"Boot", 0) == 0) tok = tok.substr(4); hs << std::hex << tok; if (!(hs >> v) || v > 0xFFFF) { std::wcerr << L"Bad id.\n"; return 2; } }
    UINT16 target = static_cast<UINT16>(v);
    auto it = std::find(order.begin(), order.end(), target); if (it == order.end()) { std::wcerr << L"ID not found in BootOrder.\n"; return 3; }
    std::rotate(order.begin(), it, it + 1); // bring to front
    if (!SetBootOrder(order)) return 4; std::wcout << L"Default boot set to Boot" << std::uppercase << std::hex << std::setw(4) << std::setfill(L'0') << target << std::nouppercase << std::dec << L".\n"; return 0;
}

int cmd_next(const std::wstring& idhex) {
    unsigned v = 0; { std::wstringstream hs; std::wstring tok = idhex; if (tok.rfind(L"Boot", 0) == 0) tok = tok.substr(4); hs << std::hex << tok; if (!(hs >> v) || v > 0xFFFF) { std::wcerr << L"Bad id.\n"; return 2; } }
    UINT16 target = static_cast<UINT16>(v);
    if (!WriteEfiVar(L"BootNext", &target, sizeof(target), g_varAttrsRW)) return 3;
    std::wcout << L"BootNext set to Boot" << std::uppercase << std::hex << std::setw(4) << std::setfill(L'0') << target << std::nouppercase << std::dec << L" (one-time).\n"; return 0;
}

int cmd_enable_disable(const std::wstring& idhex, bool enable) {
    unsigned v = 0; { std::wstringstream hs; std::wstring tok = idhex; if (tok.rfind(L"Boot", 0) == 0) tok = tok.substr(4); hs << std::hex << tok; if (!(hs >> v) || v > 0xFFFF) { std::wcerr << L"Bad id.\n"; return 2; } }
    UINT16 id = static_cast<UINT16>(v); ParsedLoadOption plo; DWORD a = 0; if (!ReadBootEntry(id, plo, a)) { std::wcerr << L"Entry not found.\n"; return 3; }
    if (enable) plo.Attributes |= LOAD_OPTION_ACTIVE; else plo.Attributes &= ~LOAD_OPTION_ACTIVE;
    if (!WriteBootEntry(id, plo)) return 4; std::wcout << (enable ? L"Enabled " : L"Disabled ") << L"Boot" << std::uppercase << std::hex << std::setw(4) << std::setfill(L'0') << id << std::nouppercase << std::dec << L".\n"; return 0;
}

int cmd_rename(const std::wstring& idhex, const std::wstring& newLabel) {
    unsigned v = 0; { std::wstringstream hs; std::wstring tok = idhex; if (tok.rfind(L"Boot", 0) == 0) tok = tok.substr(4); hs << std::hex << tok; if (!(hs >> v) || v > 0xFFFF) { std::wcerr << L"Bad id.\n"; return 2; } }
    UINT16 id = static_cast<UINT16>(v); ParsedLoadOption plo; DWORD a = 0; if (!ReadBootEntry(id, plo, a)) { std::wcerr << L"Entry not found.\n"; return 3; }
    ParsedLoadOption n = plo; n.Description = newLabel; if (!WriteBootEntry(id, n)) return 4; std::wcout << L"Renamed Boot" << std::uppercase << std::hex << std::setw(4) << std::setfill(L'0') << id << std::nouppercase << std::dec << L" to '" << newLabel << L"'.\n"; return 0;
}

int cmd_dump() {
    // Dump raw BootOrder + per-entry sizes
    DWORD a = 0; auto orderRaw = ReadEfiVar(L"BootOrder", a); if (orderRaw.empty()) { std::wcerr << L"BootOrder read failed: " << LastErrorMessage() << L"\n"; return 1; }
    std::wcout << L"BootOrder bytes: " << orderRaw.size() << L"\n";
    auto order = GetBootOrder(); size_t idx = 0; for (auto id : order) { DWORD va = 0; auto name = [&] { std::wstringstream s; s << L"Boot" << std::uppercase << std::hex << std::setw(4) << std::setfill(L'0') << id; return s.str(); }(); auto data = ReadEfiVar(name, va); std::wcout << L"[" << ++idx << L"] " << name << L" size=" << data.size() << L" attrs=0x" << std::hex << va << std::dec << L"\n"; }
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
        << L"\nExamples:\n  booteja list\n  booteja order\n  booteja select 0003\n  booteja next 0004\n  booteja order set 0004,0001,0003,0002\n  booteja rename 0002 \"Ubuntu NVMe\"\n";
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

    std::wstring cmd = argv[1]; std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::towlower);

    if (cmd == L"list") return cmd_list();
    if (cmd == L"order") {
        if (argc >= 3 && std::wstring(argv[2]) == L"set" && argc >= 4) return cmd_order_set(argv[3]);
        return cmd_order_show();
    }
    if (cmd == L"select" && argc >= 3) return cmd_select(argv[2]);
    if (cmd == L"next" && argc >= 3) return cmd_next(argv[2]);
    if (cmd == L"enable" && argc >= 3) return cmd_enable_disable(argv[2], true);
    if (cmd == L"disable" && argc >= 3) return cmd_enable_disable(argv[2], false);
    if (cmd == L"rename" && argc >= 4) {
        // join all remaining args as label
        std::wstring label = argv[3];
        for (int i = 4; i < argc; ++i) { label += L" "; label += argv[i]; }
        return cmd_rename(argv[2], label);
    }
    if (cmd == L"dump") return cmd_dump();

    PrintHelp();
    return 0;
}
