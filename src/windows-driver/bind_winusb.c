/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Fixed, elevated, one-instance in-box binding prototype. No command-line options. */
#include <stddef.h>
#ifdef BIND_MOCK
#include <stdio.h>
#include "bind_winusb_mock.h"
#else
#if !defined(_WIN64) || !defined(__x86_64__)
#error Requires native Windows x64
#endif
#if !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0a00
#elif _WIN32_WINNT < 0x0a00
#error Requires the Windows 10 SDK API level
#endif
/* DiInstallDevice uses the generic SP_DRVINFO_DATA type; our driver nodes are W. */
#ifndef UNICODE
#define UNICODE 1
#endif
#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#endif

/* Kernel32 is the sole static KnownDLL import. No CRT startup runs before this
 * policy; non-KnownDLL APIs and their dependencies resolve only from System32. */
#define SETUP_APIS(X) \
    X(SetupDiGetDevicePropertyW) X(SetupDiGetDeviceRegistryPropertyW) \
    X(SetupDiGetDeviceInstanceIdW) X(SetupDiGetClassDevsW) X(SetupDiEnumDeviceInfo) \
    X(SetupDiGetDeviceInstallParamsW) X(SetupDiSetDeviceInstallParamsW) \
    X(SetupDiBuildDriverInfoList) X(SetupDiEnumDriverInfoW) \
    X(SetupDiGetDriverInfoDetailW) X(SetupDiSetSelectedDriverW) \
    X(SetupDiDestroyDriverInfoList) X(SetupDiDestroyDeviceInfoList)
#define TOKEN_APIS(X) X(OpenProcessToken) X(GetTokenInformation)
#define DECLARE(name) static __typeof__(&name) p_##name;
SETUP_APIS(DECLARE)
TOKEN_APIS(DECLARE)
DECLARE(CM_Get_DevNode_Status)
DECLARE(DiInstallDevice)
#undef DECLARE
static int load_system_apis(void)
{
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)) return 0;
    HMODULE setup = LoadLibraryExW(L"setupapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!setup) return 0;
    HMODULE cfg = LoadLibraryExW(L"cfgmgr32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!cfg) return 0;
    HMODULE token = LoadLibraryExW(L"advapi32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!token) return 0;
    HMODULE newdev = LoadLibraryExW(L"newdev.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!newdev) return 0;
    /* Keep modules loaded until process exit, including on partial failure. */
#define RESOLVE(module, name) \
    p_##name = (__typeof__(&name))(void *)GetProcAddress(module, #name); if (!p_##name) return 0;
#define SETUP(name) RESOLVE(setup, name)
#define TOKEN(name) RESOLVE(token, name)
    SETUP_APIS(SETUP)
    TOKEN_APIS(TOKEN)
    RESOLVE(cfg, CM_Get_DevNode_Status)
    RESOLVE(newdev, DiInstallDevice)
#undef TOKEN
#undef SETUP
#undef RESOLVE
    return 1;
}
#define SetupDiGetDevicePropertyW p_SetupDiGetDevicePropertyW
#define SetupDiGetDeviceRegistryPropertyW p_SetupDiGetDeviceRegistryPropertyW
#define SetupDiGetDeviceInstanceIdW p_SetupDiGetDeviceInstanceIdW
#define SetupDiGetClassDevsW p_SetupDiGetClassDevsW
#define SetupDiEnumDeviceInfo p_SetupDiEnumDeviceInfo
#define SetupDiGetDeviceInstallParamsW p_SetupDiGetDeviceInstallParamsW
#define SetupDiSetDeviceInstallParamsW p_SetupDiSetDeviceInstallParamsW
#define SetupDiBuildDriverInfoList p_SetupDiBuildDriverInfoList
#define SetupDiEnumDriverInfoW p_SetupDiEnumDriverInfoW
#define SetupDiGetDriverInfoDetailW p_SetupDiGetDriverInfoDetailW
#define SetupDiSetSelectedDriverW p_SetupDiSetSelectedDriverW
#define DiInstallDevice p_DiInstallDevice
#define SetupDiDestroyDriverInfoList p_SetupDiDestroyDriverInfoList
#define SetupDiDestroyDeviceInfoList p_SetupDiDestroyDeviceInfoList
#define OpenProcessToken p_OpenProcessToken
#define GetTokenInformation p_GetTokenInformation
#define CM_Get_DevNode_Status p_CM_Get_DevNode_Status

#if !defined(BIND_MOCK) || defined(BIND_NOCRT)
/* The CRT-free entry uses Kernel32 string APIs; supply the byte operations the
 * compiler may also emit for aggregate initialisation. */
void *memset(void *dst, int value, size_t n)
{
    unsigned char *p = dst;
    while (n--) *p++ = (unsigned char)value;
    return dst;
}
void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *p = dst; const unsigned char *q = src;
    while (n--) *p++ = *q++;
    return dst;
}
int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;
    while (n--) { if (*p != *q) return *p < *q ? -1 : 1; ++p; ++q; }
    return 0;
}
static const wchar_t *find_wide(const wchar_t *p, wchar_t c, size_t n)
{
    while (n--) { if (*p == c) return p; ++p; }
    return NULL;
}
static int prefix_compare(const wchar_t *a, const wchar_t *b, size_t n)
{
    size_t an = (size_t)lstrlenW(a), bn = (size_t)lstrlenW(b);
    return CompareStringOrdinal(a, (int)(an < n ? an : n),
                                b, (int)(bn < n ? bn : n), TRUE) - CSTR_EQUAL;
}
static int full_compare(const wchar_t *a, const wchar_t *b)
{
    return CompareStringOrdinal(a, -1, b, -1, TRUE) - CSTR_EQUAL;
}
#define wcslen(p) ((size_t)lstrlenW(p))
#define wcscpy lstrcpyW
#define wcscat lstrcatW
#define _wcsicmp full_compare
#define _wcsnicmp prefix_compare
#define wmemchr find_wide
#define wcschr(p, c) find_wide(p, c, wcslen(p) + 1)
#endif

#define CAP 4096
static const wchar_t hardware[] = L"USB\\VID_A108&PID_EAEF";
/* Avoid a dependency on a separate GUID definition library. */
static const DEVPROPKEY inf_key = {{0xa8b865dd,0x2e3d,0x4094,
    {0xad,0x97,0xe5,0x93,0xa7,0x0c,0x75,0xd6}},5};
static const DEVPROPKEY provider_key = {{0xa8b865dd,0x2e3d,0x4094,
    {0xad,0x97,0xe5,0x93,0xa7,0x0c,0x75,0xd6}},9};
struct target {
    wchar_t instance[CAP], ids[CAP], location[CAP], service[CAP], inf[CAP], provider[CAP];
    DWORD problem, devinst;
};

/* Reject truncated, mistyped, empty or unterminated properties. Optional absence
 * is permitted only for the three driver fields, never identity fields. */
static int property(HDEVINFO set, SP_DEVINFO_DATA *dev, DWORD reg,
                    const DEVPROPKEY *key, wchar_t *buf, int multi, int optional)
{
    DWORD type = 0, size = 0;
    BOOL ok;
    memset(buf, 0, CAP * sizeof(*buf));
    if (key)
        ok = SetupDiGetDevicePropertyW(set, dev, key, &type, (BYTE *)buf,
                                      CAP * sizeof(*buf), &size, 0);
    else
        ok = SetupDiGetDeviceRegistryPropertyW(set, dev, reg, &type, (BYTE *)buf,
                                               CAP * sizeof(*buf), &size);
    if (!ok) {
        DWORD error = GetLastError();
        return optional && error == (key ? ERROR_NOT_FOUND : ERROR_INVALID_DATA);
    }
    if (type != (key ? DEVPROP_TYPE_STRING : multi ? REG_MULTI_SZ : REG_SZ) ||
        size % sizeof(*buf) || size > CAP * sizeof(*buf) ||
        size < (DWORD)((multi ? 3 : 2) * sizeof(*buf))) return 0;
    size /= sizeof(*buf);
    if (!buf[0] || buf[size - 1] || (multi && buf[size - 2])) return 0;
    /* A string must have exactly one terminator; a list exactly two. */
    for (DWORD i = 0; i < size - 1; ++i)
        if (!buf[i] && (!multi || i == 0 || !buf[i - 1])) return 0;
    return 1;
}

static int snapshot(HDEVINFO set, SP_DEVINFO_DATA *dev, struct target *t)
{
    DWORD status;
    memset(t, 0, sizeof(*t));
    if (!SetupDiGetDeviceInstanceIdW(set, dev, t->instance, CAP, NULL) ||
        !wmemchr(t->instance, 0, CAP)) return 0;
    size_t n = wcslen(hardware);
    /* Parent USB device only: no MI interfaces, REV selectors or prefix matches. */
    if (_wcsnicmp(t->instance, hardware, n) || t->instance[n] != L'\\' ||
        !t->instance[n + 1]) return 2; /* unrelated */
    if (!property(set, dev, SPDRP_HARDWAREID, NULL, t->ids, 1, 0) ||
        !property(set, dev, SPDRP_LOCATION_PATHS, NULL, t->location, 1, 0) ||
        !property(set, dev, SPDRP_SERVICE, NULL, t->service, 0, 1) ||
        !property(set, dev, 0, &inf_key, t->inf, 0, 1) ||
        !property(set, dev, 0, &provider_key, t->provider, 0, 1) ||
        CM_Get_DevNode_Status(&status, &t->problem, dev->DevInst, 0) != CR_SUCCESS)
        return 0;
    /* Windows USB IDs may include the revision-specific ID before the base ID.
     * Every ID must be this parent device; the exact base ID must be present. */
    int base = 0;
    for (wchar_t *p = t->ids; *p; p += wcslen(p) + 1) {
        if (!_wcsicmp(p, hardware)) base = 1;
        else {
            if (_wcsnicmp(p, hardware, n) || _wcsnicmp(p + n, L"&REV_", 5) ||
                wcslen(p) != n + 9) return 0;
            for (size_t j = n + 5; j < n + 9; ++j)
                if (!wcschr(L"0123456789abcdefABCDEF", p[j])) return 0;
        }
    }
    t->devinst = dev->DevInst;
    return base ? 1 : 0;
}

/* The present-only inventory itself owns the selected SP_DEVINFO_DATA. */
static int unique(HDEVINFO *set, SP_DEVINFO_DATA *chosen, struct target *t,
                  const char **reason)
{
    *reason = "inventory_error";
    *set = SetupDiGetClassDevsW(NULL, L"USB", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (*set == INVALID_HANDLE_VALUE) return 0;
    int count = 0;
    for (DWORD i = 0; ; ++i) {
        SP_DEVINFO_DATA dev = {0}; dev.cbSize = sizeof(dev);
        if (!SetupDiEnumDeviceInfo(*set, i, &dev)) {
            if (GetLastError() != ERROR_NO_MORE_ITEMS) return 0;
            *reason = "not_unique";
            return count == 1;
        }
        struct target candidate;
        int result = snapshot(*set, &dev, &candidate);
        if (!result) { *reason = "invalid_target"; return 0; }
        if (result == 1) {
            if (++count > 1) { *reason = "not_unique"; return 0; }
            *chosen = dev; *t = candidate;
        }
    }
}

/* HardwareID is one string; only CompatIDsLength characters form a MULTI_SZ.
 * Both offsets are character counts relative to HardwareID, not byte counts. */
static int winusb_ids(const SP_DRVINFO_DETAIL_DATA_W *detail, size_t count)
{
    size_t offset = detail->CompatIDsOffset, length = detail->CompatIDsLength;
    if (offset > count || length > count - offset || (!offset && length)) return -1;
    const wchar_t *ids = detail->HardwareID;
    /* Offset 0 means no compatible IDs; 1 permits an empty hardware ID. */
    size_t hardware_count = offset > 1 ? offset : count;
    const wchar_t *end = wmemchr(ids, 0, hardware_count);
    if (!end || (!*ids && (offset != 1 || !length))) return -1;
    int match = !_wcsicmp(ids, L"USB\\MS_COMP_WINUSB");
    if (!length) return match;
    if (offset == 1 && *ids) return -1;
    ids += offset;
    count = length;
    while (count > 1 && *ids) {
        end = wmemchr(ids, 0, count);
        if (!end) return -1;
        if (!_wcsicmp(ids, L"USB\\MS_COMP_WINUSB")) match = 1;
        size_t used = (size_t)(end - ids) + 1;
        ids += used; count -= used;
        if (count == 1 && !*ids) return match;
    }
    return -1;
}

static int driver_state(const struct target *t)
{
    if (t->problem == 28 && !*t->service && !*t->inf && !*t->provider) return 1;
    if (!t->problem && !_wcsicmp(t->service, L"WINUSB") &&
        !_wcsicmp(t->inf, L"winusb.inf") && !_wcsicmp(t->provider, L"Microsoft")) return 2;
    return 0;
}

static int elevated(void)
{
    HANDLE token;
    TOKEN_ELEVATION info = {0}; DWORD size = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return 0;
    BOOL ok = GetTokenInformation(token, TokenElevation, &info, sizeof(info), &size);
    CloseHandle(token);
    return ok && size == sizeof(info) && info.TokenIsElevated;
}

static int run(int argc)
{
    const char *state = "refused";
    const char *reason = "none";
    DWORD error = ERROR_INVALID_DATA;
    int installed = 0, reboot = 0, rc = 1, built = 0;
    HDEVINFO set = INVALID_HANDLE_VALUE, fresh = INVALID_HANDLE_VALUE;
    SP_DEVINFO_DATA dev = {0}, other = {0};
    struct target before, after;
    SP_DEVINSTALL_PARAMS_W params = {0}; params.cbSize = sizeof(params);
    SP_DRVINFO_DATA_W selected = {0}; selected.cbSize = sizeof(selected);
    BOOL need_reboot = 0;
    if (!load_system_apis()) { reason = state = "loader_policy_refused"; error = GetLastError(); goto done; }
    if (argc != 1) { reason = state = "arguments_refused"; goto done; }
    if (!elevated()) { reason = state = "elevation_required"; error = ERROR_ACCESS_DENIED; goto done; }
    if (!unique(&set, &dev, &before, &reason)) goto done;
    if (!driver_state(&before)) { reason = "unfamiliar_driver"; goto done; }
    if (driver_state(&before) == 1) {
        wchar_t inf[CAP] = {0};
        UINT length = GetSystemWindowsDirectoryW(inf, CAP);
        if (length < 3 || length >= CAP - 16 || inf[1] != L':' || inf[2] != L'\\')
            { reason = state = "system_path_refused"; goto done; }
        wcscat(inf, L"\\INF\\winusb.inf");
        DWORD attr = GetFileAttributesW(inf);
        if (attr == INVALID_FILE_ATTRIBUTES || attr & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
            { reason = "inf_unavailable"; goto done; }
        reason = "get_params_failed";
        if (!SetupDiGetDeviceInstallParamsW(set, &dev, &params)) goto api_error;
        if (wcslen(inf) >= sizeof(params.DriverPath) / sizeof(wchar_t))
            { reason = "inf_path_too_long"; goto done; }
        wcscpy(params.DriverPath, inf);
        params.hwndParent = NULL;
        params.Flags |= DI_ENUMSINGLEINF | DI_QUIETINSTALL;
        params.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
        reason = "set_params_failed";
        if (!SetupDiSetDeviceInstallParamsW(set, &dev, &params)) goto api_error;
        reason = "build_list_failed";
        if (!SetupDiBuildDriverInfoList(set, &dev, SPDIT_CLASSDRIVER)) goto api_error;
        built = 1;
        int matches = 0;
        for (DWORD i = 0; ; ++i) {
            SP_DRVINFO_DATA_W node = {0}; node.cbSize = sizeof(node);
            reason = "driver_enum_failed";
            if (!SetupDiEnumDriverInfoW(set, &dev, SPDIT_CLASSDRIVER, i, &node)) {
                if (GetLastError() != ERROR_NO_MORE_ITEMS) goto api_error;
                break;
            }
            /* A bounded, aligned buffer includes hardware and compatible IDs.
             * Insufficient buffer is a refusal: partial IDs cannot select a node. */
            union {
                SP_DRVINFO_DETAIL_DATA_W detail;
                BYTE bytes[sizeof(SP_DRVINFO_DETAIL_DATA_W) + CAP * sizeof(wchar_t)];
            } buffer = {0};
            SP_DRVINFO_DETAIL_DATA_W *detail = &buffer.detail;
            detail->cbSize = sizeof(*detail);
            DWORD required = 0;
            reason = "driver_detail_failed";
            if (!SetupDiGetDriverInfoDetailW(set, &dev, &node, detail, sizeof(buffer), &required))
                goto api_error;
            size_t offset = offsetof(SP_DRVINFO_DETAIL_DATA_W, HardwareID);
            reason = "invalid_driver_detail";
            if (required < offset + sizeof(wchar_t) || required > sizeof(buffer) ||
                (required - offset) % sizeof(wchar_t) ||
                !wmemchr(detail->SectionName, 0, sizeof(detail->SectionName) / sizeof(wchar_t))) goto done;
            int ids = winusb_ids(detail, (required - offset) / sizeof(wchar_t));
            if (ids < 0) goto done;
            if (_wcsicmp(detail->SectionName, L"WINUSB") || !ids) continue;
            reason = "provider_mismatch";
            if (!wmemchr(node.ProviderName, 0, sizeof(node.ProviderName) / sizeof(wchar_t)) ||
                _wcsicmp(node.ProviderName, L"Microsoft")) goto done;
            reason = "inf_mismatch";
            if (!wmemchr(detail->InfFileName, 0, MAX_PATH) || _wcsicmp(detail->InfFileName, inf)) goto done;
            selected = node; ++matches;
        }
        if (matches != 1) {
            reason = matches ? "ambiguous_winusb_node" : "no_winusb_node";
            goto done;
        }
        reason = "select_driver_failed";
        if (!SetupDiSetSelectedDriverW(set, &dev, &selected)) goto api_error;
    }
    /* No caller-provided identity: preserve the sole observed instance, devnode,
     * all hardware IDs, physical location paths and existing binding. */
    if (!unique(&fresh, &other, &after, &reason) || !driver_state(&after) ||
        memcmp(&before, &after, sizeof(before))) { reason = state = "target_changed"; goto done; }
    if (driver_state(&after) == 2) { reason = "none"; state = "already_bound"; error = 0; rc = 0; goto done; }
    if (snapshot(set, &dev, &after) != 1 || !driver_state(&after) ||
        memcmp(&before, &after, sizeof(before))) { reason = state = "target_changed"; goto done; }
    /* There is no atomic compare-and-install operation. A concurrent
     * change during this final property read or before DiInstallDevice can
     * still race us; this re-read narrows, but cannot eliminate, that interval. */
    reason = "install_failed";
    /* Supply the validated class-driver node explicitly: its WINUSB compatible
     * ID does not match this device, so automatic driver ranking cannot bind it. */
    if (!DiInstallDevice(NULL, set, &dev, &selected, 0, &need_reboot)) goto api_error;
    installed = 1;
    if (!SetupDiGetDeviceInstallParamsW(set, &dev, &params)) {
        /* Installation occurred, but reboot status is unknown. Require a reboot. */
        reboot = 1; reason = state = "installed_status_unknown"; goto api_error_keep_state;
    }
    reboot = need_reboot || !!(params.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART));
    reason = "none"; state = reboot ? "reboot_required" : "installed"; error = 0; rc = 0;
    goto done;
api_error:
    state = "api_error";
api_error_keep_state:
    error = GetLastError();
done:
    if (built) SetupDiDestroyDriverInfoList(set, &dev, SPDIT_CLASSDRIVER);
    if (fresh != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(fresh);
    if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);
#if defined(BIND_MOCK) && !defined(BIND_NOCRT)
    printf("{\"schema\":1,\"status\":\"%s\",\"reason\":\"%s\",\"error\":%lu,\"installed\":%s,\"reboot_required\":%s}\n",
           state, reason, (unsigned long)error, installed ? "true" : "false", reboot ? "true" : "false");
#else
    char json[256] = "{\"schema\":1,\"status\":\"", number[11];
    unsigned int n = 0; DWORD value = error, written;
    do { number[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    lstrcatA(json, state); lstrcatA(json, "\",\"reason\":\"");
    lstrcatA(json, reason); lstrcatA(json, "\",\"error\":");
    size_t pos = (size_t)lstrlenA(json);
    while (n) json[pos++] = number[--n];
    json[pos] = 0;
    lstrcatA(json, installed ? ",\"installed\":true" : ",\"installed\":false");
    lstrcatA(json, reboot ? ",\"reboot_required\":true}\n" : ",\"reboot_required\":false}\n");
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), json, (DWORD)lstrlenA(json), &written, NULL);
#endif
    return rc;
}

#if defined(BIND_MOCK) && !defined(BIND_NOCRT)
int main(int argc, char **argv) { (void)argv; return run(argc); }
#else
/* Windows treats the executable token specially: quotes group spaces, and the
 * first unquoted space/tab ends it. Any subsequent token is an argument. */
void mainCRTStartup(void)
{
    const wchar_t *p = GetCommandLineW(); int quoted = 0;
    while (*p) {
        if (*p == L'"') quoted = !quoted;
        else if (!quoted && (*p == L' ' || *p == L'\t')) break;
        ++p;
    }
    while (*p == L' ' || *p == L'\t') ++p;
    ExitProcess((UINT)run(*p ? 2 : 1));
}
#ifdef BIND_NOCRT
int main(void) { mainCRTStartup(); return 1; }
#endif
#endif
