// La Booteja main entry point :O
// Enumerate EFI Boot entries on Windows (UEFI) using GetFirmwareEnvironmentVariableExW
// Build: cl /EHsc /W4 /DUNICODE /D_UNICODE Booteja.cpp

#include <windows.h>
#include <iostream>
#include <vector>
#include <string>
#include <iomanip>
#include <sstream>
#include <algorithm>

static const wchar_t* EFI_GLOBAL_VARIABLE_GUID = L"{8BE4DF61-93CA-11D2-AA0D-00E098032B8C}";

// UEFI Load Option Attributes (subset)
constexpr UINT32 LOAD_OPTION_ACTIVE = 0x00000001;
constexpr UINT32 LOAD_OPTION_FORCE_RECONNECT = 0x00000002;
constexpr UINT32 LOAD_OPTION_HIDDEN = 0x00000008;

// Simple helper to format LastError
std::wstring LastErrorMessage(DWORD err = GetLastError()) {
    LPWSTR buf = nullptr;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    FormatMessageW(flags, nullptr, err, 0, (LPWSTR)&buf, 0, nullptr);
    std::wstring msg = buf ? buf : L"";
    if (buf) LocalFree(buf);
    if (!msg.empty() && msg.back() == L'\n') msg.pop_back();
    if (!msg.empty() && msg.back() == L'\r') msg.pop_back();
    std::wstringstream ss;
    ss << L"(error " << err << L") " << msg;
    return ss.str();
}

// Enable SeSystemEnvironmentPrivilege for this process
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
    return GetLastError() != ERROR_NOT_ALL_ASSIGNED; // AdjustTokenPrivileges sets last error
}

// Read an EFI variable into a vector<BYTE>
std::vector<BYTE> ReadEfiVar(const std::wstring& name, DWORD& attrsOut) {
    // First call to get required size
    DWORD required = GetFirmwareEnvironmentVariableExW(name.c_str(), EFI_GLOBAL_VARIABLE_GUID, nullptr, 0, &attrsOut);
    if (required == 0) {
        DWORD err = GetLastError();
        if (err != ERROR_INSUFFICIENT_BUFFER) {
            // real error
            return {};
        }
    }
    std::vector<BYTE> buf;
    if (required == 0) {
        // Some firmware returns size only on second call; try a reasonable buffer
        required = 4096;
    }
    buf.resize(required);
    DWORD gotAttrs = 0;
    DWORD read = GetFirmwareEnvironmentVariableExW(name.c_str(), EFI_GLOBAL_VARIABLE_GUID, buf.data(), static_cast<DWORD>(buf.size()), &gotAttrs);
    if (read == 0) {
        // Try grow once if buffer too small
        DWORD err = GetLastError();
        if (err == ERROR_INSUFFICIENT_BUFFER) {
            DWORD newSize = GetFirmwareEnvironmentVariableExW(name.c_str(), EFI_GLOBAL_VARIABLE_GUID, nullptr, 0, &gotAttrs);
            if (newSize > buf.size()) {
                buf.resize(newSize);
                read = GetFirmwareEnvironmentVariableExW(name.c_str(), EFI_GLOBAL_VARIABLE_GUID, buf.data(), newSize, &gotAttrs);
            }
        }
    }
    if (read == 0) {
        buf.clear();
    }
    else {
        buf.resize(read);
        attrsOut = gotAttrs;
    }
    return buf;
}

// Decode UTF-16LE null-terminated string from a byte span starting at offset.
// Returns wstring and sets nextOffset just after the null terminator (aligned to 2 bytes).
std::wstring ReadUcs2String(const std::vector<BYTE>& data, size_t start, size_t& nextOffset) {
    std::wstring out;
    size_t i = start;
    while (i + 1 < data.size()) {
        wchar_t ch = static_cast<wchar_t>(data[i] | (data[i + 1] << 8));
        i += 2;
        if (ch == L'\0') break;
        out.push_back(ch);
    }
    // Align to 2 bytes if needed
    if (i & 1) i++;
    nextOffset = i;
    return out;
}

// Hex dump helper (first N bytes)
std::wstring HexPreview(const BYTE* p, size_t n) {
    std::wstringstream ss;
    ss << std::hex << std::setfill(L'0');
    size_t count = std::min<size_t>(n, 64); // cap preview
    for (size_t i = 0; i < count; ++i) {
        ss << std::setw(2) << static_cast<unsigned>(p[i]) << L' ';
    }
    return ss.str();
}

// Parse minimal EFI_LOAD_OPTION (per UEFI spec)
// struct {
//   UINT32 Attributes;
//   UINT16 FilePathListLength;
//   CHAR16 Description[];     // null-terminated
//   EFI_DEVICE_PATH FilePathList[FilePathListLength];
//   UINT8 OptionalData[];     // remainder
// }
struct ParsedLoadOption {
    UINT32 Attributes = 0;
    UINT16 FilePathListLength = 0;
    std::wstring Description;
    std::wstring DevicePathHexPreview;
    size_t OptionalDataLength = 0;
};

bool ParseLoadOption(const std::vector<BYTE>& buf, ParsedLoadOption& plo) {
    if (buf.size() < 6) return false;
    plo.Attributes = *reinterpret_cast<const UINT32*>(&buf[0]);
    plo.FilePathListLength = *reinterpret_cast<const UINT16*>(&buf[4]);

    size_t offset = 6;
    plo.Description = ReadUcs2String(buf, offset, offset);

    // Device path list
    size_t dplen = plo.FilePathListLength;
    if (offset + dplen <= buf.size()) {
        plo.DevicePathHexPreview = HexPreview(&buf[offset], dplen);
        offset += dplen;
    }
    else {
        plo.DevicePathHexPreview = L"(truncated/invalid)";
        offset = buf.size();
    }

    if (offset <= buf.size()) {
        plo.OptionalDataLength = buf.size() - offset;
    }
    return true;
}

void PrintAttributes(UINT32 a) {
    std::wcout << L"    Attributes: 0x" << std::hex << a << std::dec << L"\n";
    std::wcout << L"      - Active: " << ((a & LOAD_OPTION_ACTIVE) ? L"yes" : L"no") << L"\n";
    std::wcout << L"      - ForceReconnect: " << ((a & LOAD_OPTION_FORCE_RECONNECT) ? L"yes" : L"no") << L"\n";
    std::wcout << L"      - Hidden: " << ((a & LOAD_OPTION_HIDDEN) ? L"yes" : L"no") << L"\n";
}

int wmain() {
    std::wcout << L"EFI Entry Enumerator (Windows / MSVC)\n\n";

    if (!EnableSystemEnvironmentPrivilege()) {
        std::wcerr << L"Warning: Could not enable SeSystemEnvironmentPrivilege. Run as Administrator on a UEFI system.\n";
    }

    // Show BootCurrent and BootNext if available
    auto showUint16Var = [](const std::wstring& name) {
        DWORD attrs = 0;
        auto v = ReadEfiVar(name, attrs);
        if (v.size() >= 2) {
            UINT16 val = *reinterpret_cast<const UINT16*>(v.data());
            std::wcout << name << L": Boot" << std::setw(4) << std::setfill(L'0') << std::hex << std::uppercase
                << val << std::nouppercase << std::dec << std::setfill(L' ') << L"\n";
        }
        };
    showUint16Var(L"BootCurrent");
    showUint16Var(L"BootNext");

    // Read BootOrder
    DWORD attrs = 0;
    auto order = ReadEfiVar(L"BootOrder", attrs);
    if (order.empty() || (order.size() % 2) != 0) {
        std::wcerr << L"\nCould not read BootOrder (or firmware does not expose it). "
            L"Ensure: UEFI firmware, 64-bit Windows, elevated prompt. "
            L"System says: " << LastErrorMessage() << L"\n";
        return 1;
    }

    std::wcout << L"\nBootOrder (" << (order.size() / 2) << L" entries):\n";
    const UINT16* ids = reinterpret_cast<const UINT16*>(order.data());
    size_t count = order.size() / sizeof(UINT16);

    for (size_t i = 0; i < count; ++i) {
        UINT16 id = ids[i];
        std::wstringstream varName;
        varName << L"Boot" << std::uppercase << std::hex << std::setw(4) << std::setfill(L'0') << id;
        std::wstring name = varName.str();

        DWORD vAttrs = 0;
        auto data = ReadEfiVar(name, vAttrs);
        std::wcout << L"\n[" << (i + 1) << L"/" << count << L"] " << name << L"  (size=" << data.size() << L")\n";

        if (data.empty()) {
            std::wcout << L"    (not found / empty)  " << LastErrorMessage() << L"\n";
            continue;
        }

        ParsedLoadOption plo;
        if (!ParseLoadOption(data, plo)) {
            std::wcout << L"    Failed to parse load option.\n";
            continue;
        }

        PrintAttributes(plo.Attributes);
        std::wcout << L"    Description: " << (plo.Description.empty() ? L"(none)" : plo.Description) << L"\n";
        std::wcout << L"    DevicePath bytes: " << plo.FilePathListLength << L"\n";
        std::wcout << L"    DevicePath hex preview: " << plo.DevicePathHexPreview << L"\n";
        std::wcout << L"    OptionalData bytes: " << plo.OptionalDataLength << L"\n";
    }

    std::wcout << L"\nDone.\n";
    return 0;
}
