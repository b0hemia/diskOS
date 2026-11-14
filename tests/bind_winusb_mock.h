/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SetupAPI mock compiled with the complete production helper. No device access. */
#include <stdint.h>
#include <stdlib.h>
#include <wctype.h>
#include <wchar.h>
#include <string.h>
typedef uint32_t DWORD;
typedef unsigned int UINT;
typedef int BOOL;
typedef unsigned char BYTE;
typedef void *HANDLE;
typedef void *HDEVINFO;
typedef void *HMODULE;
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x800
typedef struct { DWORD a; unsigned short b,c; BYTE d[8]; } GUID;
typedef struct { GUID fmtid; DWORD pid; } DEVPROPKEY;
typedef struct { DWORD cbSize, DevInst; } SP_DEVINFO_DATA;
typedef struct { DWORD cbSize, Flags, FlagsEx; void *hwndParent; wchar_t DriverPath[260]; } SP_DEVINSTALL_PARAMS_W;
typedef struct { DWORD cbSize, index; wchar_t ProviderName[256]; } SP_DRVINFO_DATA_W;
typedef struct {
    DWORD cbSize, CompatIDsOffset, CompatIDsLength;
    wchar_t SectionName[256], InfFileName[260], HardwareID[1];
} SP_DRVINFO_DETAIL_DATA_W;
typedef struct { DWORD TokenIsElevated; } TOKEN_ELEVATION;
#define CAP_MOCK 4096
#define MAX_PATH 260
#define INVALID_HANDLE_VALUE ((void *)(intptr_t)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
enum { ERROR_INVALID_DATA=13, ERROR_NOT_FOUND=1168, ERROR_NO_MORE_ITEMS=259,
       ERROR_ACCESS_DENIED=5, ERROR_INSUFFICIENT_BUFFER=122, DEVPROP_TYPE_STRING=18,
       REG_SZ=1, REG_MULTI_SZ=7, SPDRP_HARDWAREID=1, SPDRP_LOCATION_PATHS=35,
       SPDRP_SERVICE=4, CR_SUCCESS=0, DIGCF_ALLCLASSES=4, DIGCF_PRESENT=2,
       TOKEN_QUERY=8, TokenElevation=20, DI_ENUMSINGLEINF=0x10000,
       DI_QUIETINSTALL=0x800000, DI_FLAGSEX_ALLOWEXCLUDEDDRVS=0x800,
       DI_NEEDREBOOT=0x100, DI_NEEDRESTART=0x80, SPDIT_CLASSDRIVER=1,
       FILE_ATTRIBUTE_REPARSE_POINT=0x400, FILE_ATTRIBUTE_DIRECTORY=16 };
static DWORD last_error;
static int generation, calls, boundary, policy;
static SP_DEVINSTALL_PARAMS_W saved;
static SP_DRVINFO_DATA_W selected_driver;
static const char *scenario(void) { const char *v = getenv("BIND_CASE"); return v ? v : "ok"; }
static int is(const char *s) { return !strcmp(scenario(), s); }
static int changed(const char *s) {
    return (generation==2 && is(s)) ||
        (boundary && !strncmp(scenario(), "boundary_", 9) && !strcmp(scenario()+9, s));
}
static void trace(const char *s) { fprintf(stderr, "%s\n", s); }
static DWORD GetLastError(void) { return last_error; }
static int _wcsnicmp(const wchar_t *a, const wchar_t *b, size_t n) {
    while (n--) { wint_t x=towlower(*a++), y=towlower(*b++); if(x!=y)return x<y?-1:1; if(!x)return 0; } return 0;
}
static int _wcsicmp(const wchar_t *a, const wchar_t *b) { return _wcsnicmp(a,b,CAP_MOCK); }
static HANDLE GetCurrentProcess(void) { return (void *)1; }
static BOOL OpenProcessToken(HANDLE p, DWORD access, HANDLE *t) { (void)p;(void)access; *t=(void *)1; return !is("token_error"); }
static BOOL GetTokenInformation(HANDLE t, int kind, TOKEN_ELEVATION *v, DWORD cap, DWORD *size) {
    (void)t;(void)kind; *size=cap; v->TokenIsElevated=!is("not_admin"); return 1;
}
static BOOL CloseHandle(HANDLE t) { (void)t; return 1; }
static HDEVINFO SetupDiGetClassDevsW(void *g, const wchar_t *e, void *h, DWORD flags) {
    (void)g;(void)h; trace("inventory");
    if(wcscmp(e,L"USB") || flags!=(DIGCF_ALLCLASSES|DIGCF_PRESENT)) abort();
    ++generation; return is("enum_error") ? INVALID_HANDLE_VALUE : (void *)(intptr_t)generation;
}
static BOOL SetupDiEnumDeviceInfo(HDEVINFO s, DWORD i, SP_DEVINFO_DATA *d) {
    if(is("device_enum_error")) { last_error=5; return 0; }
    int count = is("ambiguous") || (generation==2 && is("new_device")) ? 2 : 1;
    if(is("absent") || (generation==2 && is("disappear"))) count=0;
    if(i>=(DWORD)count) { if(generation==2) boundary=1; last_error=ERROR_NO_MORE_ITEMS; return 0; }
    d->DevInst = 100+i+changed("devnode_change"); (void)s; return 1;
}
static BOOL SetupDiGetDeviceInstanceIdW(HDEVINFO s, SP_DEVINFO_DATA *d, wchar_t *out, DWORD cap, DWORD *need) {
    (void)d;(void)cap;(void)need;
    if(is("instance_read_error")) { last_error=5; return 0; }
    if(boundary && s==(void *)1) {
        trace("final_snapshot");
        if(is("boundary_read_error")) { last_error=ERROR_NOT_FOUND; return 0; }
    }
    wcscpy(out,is("wrong_instance") ? L"USB\\VID_A108&PID_EAEF0\\ONE" :
      changed("instance_change") ? L"USB\\VID_A108&PID_EAEF\\OTHER" : L"USB\\VID_A108&PID_EAEF\\ONE"); return 1;
}
static BOOL data_property(DWORD reg, const DEVPROPKEY *key, DWORD *type, BYTE *buf, DWORD cap, DWORD *size) {
    const wchar_t *value=L""; int multi=0;
    if(key) {
        *type=DEVPROP_TYPE_STRING;
        if(is("bound")) value=key->pid==5 ? L"winusb.inf" : L"Microsoft";
        else if(is("foreign_inf") || changed("inf_change")) value=key->pid==5 ? L"oem42.inf" : L"Microsoft";
        else if(is("foreign_provider") || changed("provider_change")) value=key->pid==5 ? L"winusb.inf" : L"Other";
        else { last_error=ERROR_NOT_FOUND; return 0; }
    } else if(reg==SPDRP_SERVICE) {
        *type=REG_SZ;
        if(is("bound") || is("foreign_inf") || is("foreign_provider")) value=L"WINUSB";
        else if(is("foreign_driver") || changed("driver_change")) value=L"libusbK";
        else { last_error=ERROR_INVALID_DATA; return 0; }
    } else {
        multi=1; *type=REG_MULTI_SZ;
        value=reg==SPDRP_HARDWAREID ?
          ((is("wrong_ids") || changed("ids_change")) ? L"USB\\VID_1234&PID_EAEF" :
           is("interface_id") ? L"USB\\VID_A108&PID_EAEF&MI_00" :
           is("revision_only") ? L"USB\\VID_A108&PID_EAEF&REV_0100" :
           is("revision") ? L"USB\\VID_A108&PID_EAEF&REV_0100\0USB\\VID_A108&PID_EAEF" : L"USB\\VID_A108&PID_EAEF") :
          (changed("location_change") ? L"PCIROOT(0)#USBROOT(0)#USB(2)" : L"PCIROOT(0)#USBROOT(0)#USB(1)");
        if(is("missing_location") && reg==SPDRP_LOCATION_PATHS) { last_error=ERROR_INVALID_DATA; return 0; }
    }
    size_t n=wcslen(value)+1;
    if(is("revision") && reg==SPDRP_HARDWAREID && !key) n+=wcslen(value+n)+1;
    *size=(DWORD)((n+multi)*sizeof(wchar_t)); if(*size>cap) abort();
    memset(buf,0,cap); memcpy(buf,value,n*sizeof(wchar_t));
    if(is("bad_type")) *type=999;
    if(is("bad_size")) *size=cap+2;
    if(is("unterminated")) ((wchar_t *)buf)[n-1]=L'X';
    return 1;
}
static BOOL SetupDiGetDevicePropertyW(HDEVINFO s, SP_DEVINFO_DATA *d, const DEVPROPKEY *k, DWORD *t, BYTE *b, DWORD c, DWORD *n, DWORD flags) {
    (void)s;(void)d;(void)flags; return data_property(0,k,t,b,c,n);
}
static BOOL SetupDiGetDeviceRegistryPropertyW(HDEVINFO s, SP_DEVINFO_DATA *d, DWORD r, DWORD *t, BYTE *b, DWORD c, DWORD *n) {
    (void)s;(void)d; return data_property(r,NULL,t,b,c,n);
}
static DWORD CM_Get_DevNode_Status(DWORD *s, DWORD *p, DWORD dev, DWORD flags) {
    (void)dev;(void)flags; *s=0; *p=is("bound") ? 0 : (is("problem_code") || changed("problem_change")) ? 10 : 28;
    return is("cm_error") || changed("cm_error") ? 1 : CR_SUCCESS;
}
static UINT GetSystemWindowsDirectoryW(wchar_t *b, UINT c) {
    (void)c; trace("system_path"); wcscpy(b,is("bad_system_path") ? L"relative" : L"C:\\Windows");
    if(is("inf_path_too_long")) { for(int i=3;i<280;++i) b[i]=L'X'; b[280]=0; }
    return is("system_path_too_long") ? CAP_MOCK : (UINT)wcslen(b);
}
static DWORD GetFileAttributesW(const wchar_t *p) {
    if(!is("inf_path_too_long") && wcscmp(p,L"C:\\Windows\\INF\\winusb.inf")) abort();
    return is("inf_reparse") ? FILE_ATTRIBUTE_REPARSE_POINT : is("inf_missing") ? INVALID_FILE_ATTRIBUTES :
        is("inf_directory") ? FILE_ATTRIBUTE_DIRECTORY : 0;
}
static BOOL SetupDiGetDeviceInstallParamsW(HDEVINFO s, SP_DEVINFO_DATA *d, SP_DEVINSTALL_PARAMS_W *p) {
    (void)s;(void)d; trace("get_params");
    if(!calls && is("get_params_error")) { last_error=5; return 0; }
    if(calls && (is("post_error") || is("api_reboot_post_error"))) { last_error=5; return 0; }
    if(calls && (is("reboot") || is("restart"))) p->Flags |= is("reboot") ? DI_NEEDREBOOT : DI_NEEDRESTART;
    return 1;
}
static BOOL SetupDiSetDeviceInstallParamsW(HDEVINFO s, SP_DEVINFO_DATA *d, SP_DEVINSTALL_PARAMS_W *p) {
    (void)s;(void)d; trace("set_params"); saved=*p;
    if(is("set_params_error")) { last_error=5; return 0; } return 1;
}
static BOOL SetupDiBuildDriverInfoList(HDEVINFO s, SP_DEVINFO_DATA *d, DWORD kind) {
    (void)s;(void)d; trace("build_list");
    if(kind!=SPDIT_CLASSDRIVER || !(saved.Flags&DI_ENUMSINGLEINF) || !(saved.Flags&DI_QUIETINSTALL) ||
       !(saved.FlagsEx&DI_FLAGSEX_ALLOWEXCLUDEDDRVS) || wcscmp(saved.DriverPath,L"C:\\Windows\\INF\\winusb.inf")) abort();
    if(is("build_error")) { last_error=5; return 0; } return 1;
}
static const wchar_t *model(DWORD i) {
    if(is("other_models")) return i ? L"ADB" : L"BILLBOARD";
    if(is("three_models_first")) return i==0 ? L"WINUSB" : i==1 ? L"BILLBOARD" : L"ADB";
    if(is("three_models_last")) return i==2 ? L"WINUSB" : i==0 ? L"BILLBOARD" : L"ADB";
    if(is("three_models")) return i==1 ? L"WINUSB" : i==0 ? L"BILLBOARD" : L"ADB";
    if(is("wrong_section")) return L"ADB";
    return is("node_case") ? L"winusb" : L"WINUSB";
}
static BOOL SetupDiEnumDriverInfoW(HDEVINFO s, SP_DEVINFO_DATA *d, DWORD kind, DWORD i, SP_DRVINFO_DATA_W *v) {
    (void)s;(void)d;(void)kind; trace("enum_driver");
    if(is("driver_enum_error") && i==1) { last_error=5; return 0; }
    DWORD count=is("no_driver") ? 0 : is("two_drivers") || is("other_models") ? 2 :
        !strncmp(scenario(),"three_models",12) ? 3 : 1;
    if(i>=count) {last_error=ERROR_NO_MORE_ITEMS; return 0;}
    v->index=i;
    wcscpy(v->ProviderName,is("node_provider") ? L"Other" : is("node_case") ? L"mICROSOFT" : L"Microsoft");
    if(is("provider_unterminated")) for(int j=0;j<256;++j) v->ProviderName[j]=L'X';
    return 1;
}
static BOOL SetupDiGetDriverInfoDetailW(HDEVINFO s, SP_DEVINFO_DATA *d, SP_DRVINFO_DATA_W *v, SP_DRVINFO_DETAIL_DATA_W *detail, DWORD cap, DWORD *needed) {
    (void)s;(void)d; trace("detail");
    if(detail->cbSize!=sizeof(*detail)) abort();
    wcscpy(detail->SectionName,model(v->index));
    wcscpy(detail->InfFileName,is("node_inf") ? L"C:\\evil.inf" :
        is("node_case") ? L"c:\\WINDOWS\\inf\\WINUSB.INF" : L"C:\\Windows\\INF\\winusb.inf");
    const wchar_t *ids=!_wcsicmp(model(v->index),L"BILLBOARD") ? L"USB\\Class_11&SubClass_00&Prot_00" :
        !_wcsicmp(model(v->index),L"ADB") && !is("wrong_section") ? L"USB\\Class_ff&SubClass_42&Prot_01" :
        is("wrong_node_id") ? L"USB\\Class_ff&SubClass_42&Prot_01" :
        is("node_id_suffix") ? L"USB\\MS_COMP_WINUSB_EXTRA" :
        is("node_id_prefix") ? L"XUSB\\MS_COMP_WINUSB" :
        is("node_case") ? L"usb\\ms_comp_winusb" : L"USB\\MS_COMP_WINUSB";
    const wchar_t *compat=NULL;
    size_t length=0;
    if(is("node_id_list") || is("both_ids") || is("compatible_only")) {
        ids=is("compatible_only") ? L"" : L"USB\\OTHER";
        compat=L"USB\\OTHER\0USB\\MS_COMP_WINUSB\0";
        length=sizeof(L"USB\\OTHER\0USB\\MS_COMP_WINUSB\0")/sizeof(wchar_t);
    }
    if(is("both_hardware_match") || is("both_no_match") || is("compat_padding")) {
        if(!is("both_hardware_match")) ids=L"USB\\OTHER";
        compat=L"USB\\OTHER\0";
        length=sizeof(L"USB\\OTHER\0")/sizeof(wchar_t);
    }
    if(!strncmp(scenario(),"compat_bad_",11)) {
        compat=L"USB\\MS_COMP_WINUSB\0";
        length=sizeof(L"USB\\MS_COMP_WINUSB\0")/sizeof(wchar_t);
    }
    if(is("hardware_padding")) ids=L"USB\\OTHER";
    size_t n=wcslen(ids)+1;
    size_t total=n+length;
    if(is("hardware_padding") || is("compat_padding")) total+=sizeof(L"USB\\MS_COMP_WINUSB")/sizeof(wchar_t);
    *needed=(DWORD)(offsetof(SP_DRVINFO_DETAIL_DATA_W,HardwareID)+total*sizeof(wchar_t));
    if(*needed>cap) { last_error=ERROR_INSUFFICIENT_BUFFER; return 0; }
    /* Poison unused capacity so a terminator outside RequiredSize cannot help. */
    size_t capacity=(cap - offsetof(SP_DRVINFO_DETAIL_DATA_W, HardwareID))/sizeof(wchar_t);
    for(size_t j=0;j<capacity;++j) detail->HardwareID[j]=L'X';
    memcpy(detail->HardwareID,ids,n*sizeof(wchar_t));
    detail->CompatIDsOffset=length ? (DWORD)n : is("hardware_offset_one") ? 1 : 0;
    detail->CompatIDsLength=(DWORD)length;
    if(length) memcpy(detail->HardwareID+n,compat,length*sizeof(wchar_t));
    if(is("hardware_padding") || is("compat_padding")) wcscpy(detail->HardwareID+n+length,L"USB\\MS_COMP_WINUSB");
    if(is("section_unterminated")) for(int j=0;j<256;++j) detail->SectionName[j]=L'X';
    if(is("inf_unterminated")) for(int j=0;j<260;++j) detail->InfFileName[j]=L'X';
    if(is("ids_unterminated")) detail->HardwareID[n-1]=L'X';
    if(is("ids_empty")) detail->HardwareID[0]=0;
    if(is("offset_beyond")) detail->CompatIDsOffset=(DWORD)total+1;
    if(is("offset_huge")) detail->CompatIDsOffset=UINT32_MAX;
    if(is("length_beyond") || is("length_huge")) {
        detail->CompatIDsOffset=1;
        detail->CompatIDsLength=is("length_huge") ? UINT32_MAX : (DWORD)total;
    }
    if(is("length_without_offset")) detail->CompatIDsLength=2;
    if(is("hardware_truncated")) detail->CompatIDsOffset=(DWORD)n-1;
    if(is("compat_bad_offset_one")) detail->CompatIDsOffset=1;
    if(is("compat_bad_single_null")) { --detail->CompatIDsLength; *needed-=sizeof(wchar_t); }
    if(is("compat_bad_unterminated")) for(size_t j=n;j<total;++j) detail->HardwareID[j]=L'X';
    if(is("compat_bad_empty")) detail->HardwareID[n]=0;
    if(is("compat_bad_trailing")) detail->HardwareID[total-1]=L'X';
    if(is("detail_short")) *needed=offsetof(SP_DRVINFO_DETAIL_DATA_W,HardwareID);
    if(is("detail_oversize")) *needed=cap+sizeof(wchar_t);
    if(is("detail_odd_size")) --*needed;
    if(is("detail_error") || is("detail_insufficient")) {
        last_error=is("detail_error") ? 5 : ERROR_INSUFFICIENT_BUFFER; return 0;
    }
    return 1;
}
static BOOL SetupDiSetSelectedDriverW(HDEVINFO s, SP_DEVINFO_DATA *d, SP_DRVINFO_DATA_W *v) {
    (void)s;(void)d; trace("select");
    if(_wcsicmp(model(v->index),L"WINUSB")) abort();
    selected_driver=*v;
    if(is("select_error")) { last_error=5; return 0; } return 1;
}
static BOOL DiInstallDevice(void *parent, HDEVINFO s, SP_DEVINFO_DATA *d,
                            SP_DRVINFO_DATA_W *driver, DWORD flags, BOOL *reboot) {
    trace("install"); ++calls;
    if(parent || s!=(void *)1 || !d || d->cbSize!=sizeof(*d) || d->DevInst!=100 ||
       generation!=2 || !boundary || calls!=1 || flags || !reboot ||
       !driver || driver->cbSize!=sizeof(*driver) ||
       memcmp(driver,&selected_driver,sizeof(*driver))) abort();
    *reboot=is("api_reboot") || is("api_reboot_post_error");
    if(is("install_error") || is("install_error_raw")) {
        last_error=is("install_error_raw") ? UINT32_C(0xe0000204) : 5; return 0;
    }
    return 1;
}
static BOOL SetupDiDestroyDriverInfoList(HDEVINFO s, SP_DEVINFO_DATA *d, DWORD k) { (void)s;(void)d;(void)k; trace("destroy_drivers"); return 1; }
static BOOL SetupDiDestroyDeviceInfoList(HDEVINFO s) { (void)s; trace("destroy_set"); return 1; }

static BOOL SetDefaultDllDirectories(DWORD flags) {
    trace("loader_policy");
    if(flags!=LOAD_LIBRARY_SEARCH_SYSTEM32 || policy++) abort();
    if(is("policy_error")) { last_error=5; return 0; }
    return 1;
}
static HMODULE LoadLibraryExW(const wchar_t *name, HANDLE file, DWORD flags) {
    if(policy!=1 || file || flags!=LOAD_LIBRARY_SEARCH_SYSTEM32) abort();
    if(wcscmp(name,L"setupapi.dll") && wcscmp(name,L"cfgmgr32.dll") &&
       wcscmp(name,L"advapi32.dll") && wcscmp(name,L"newdev.dll")) abort();
    trace("load_system_dll");
    if(is("load_error") || (is("newdev_load_error") && !wcscmp(name,L"newdev.dll"))) {
        last_error=5; return NULL;
    }
    return !wcscmp(name,L"newdev.dll") ? (void *)2 : (void *)1;
}
static void *GetProcAddress(HMODULE module, const char *name) {
    if(policy!=1) abort();
    if(!strcmp(name,"DiInstallDevice")) {
        if(module!=(void *)2) abort();
        if(is("newdev_resolve_error")) { last_error=127; return NULL; }
        return (void *)&DiInstallDevice;
    }
    if(module!=(void *)1) abort();
    if(is("resolve_error")) { last_error=127; return NULL; }
#define EXPORT(fn) if(!strcmp(name,#fn)) return (void *)&fn;
    EXPORT(SetupDiGetDevicePropertyW) EXPORT(SetupDiGetDeviceRegistryPropertyW)
    EXPORT(SetupDiGetDeviceInstanceIdW) EXPORT(SetupDiGetClassDevsW) EXPORT(SetupDiEnumDeviceInfo)
    EXPORT(SetupDiGetDeviceInstallParamsW) EXPORT(SetupDiSetDeviceInstallParamsW)
    EXPORT(SetupDiBuildDriverInfoList) EXPORT(SetupDiEnumDriverInfoW)
    EXPORT(SetupDiGetDriverInfoDetailW) EXPORT(SetupDiSetSelectedDriverW)
    EXPORT(SetupDiDestroyDriverInfoList) EXPORT(SetupDiDestroyDeviceInfoList)
    EXPORT(OpenProcessToken) EXPORT(GetTokenInformation) EXPORT(CM_Get_DevNode_Status)
#undef EXPORT
    abort();
}

#ifdef BIND_NOCRT
/* Kernel32 boundary for exercising the production entry, strings and JSON. */
#define TRUE 1
#define CSTR_EQUAL 2
#define STD_OUTPUT_HANDLE ((DWORD)-11)
static int lstrlenW(const wchar_t *p) { return (int)wcslen(p); }
static int lstrlenA(const char *p) { return (int)strlen(p); }
static wchar_t *lstrcpyW(wchar_t *p, const wchar_t *q) { return wcscpy(p,q); }
static wchar_t *lstrcatW(wchar_t *p, const wchar_t *q) { return wcscat(p,q); }
static char *lstrcatA(char *p, const char *q) { return strcat(p,q); }
static int CompareStringOrdinal(const wchar_t *p, int pn, const wchar_t *q, int qn, BOOL insensitive) {
    int full=pn==-1 && qn==-1;
    if(pn==-1) pn=(int)wcslen(p);
    if(qn==-1) qn=(int)wcslen(q);
    if(!insensitive || pn<0 || qn<0) abort();
    int n=pn<qn ? pn : qn, result=full ? _wcsicmp(p,q) : _wcsnicmp(p,q,(size_t)n);
    if(!result) result=pn==qn ? 0 : pn<qn ? -1 : 1;
    return result<0 ? 1 : result>0 ? 3 : CSTR_EQUAL;
}
static const wchar_t *GetCommandLineW(void) {
    static wchar_t command[CAP_MOCK]; const char *p=getenv("BIND_COMMAND");
    if(!p || mbstowcs(command,p,CAP_MOCK)==(size_t)-1) abort();
    command[CAP_MOCK-1]=0; return command;
}
static HANDLE GetStdHandle(DWORD kind) { if(kind!=STD_OUTPUT_HANDLE) abort(); return (void *)1; }
static BOOL WriteFile(HANDLE h, const void *p, DWORD n, DWORD *written, void *overlapped) {
    if(h!=(void *)1 || overlapped) abort();
    *written=(DWORD)fwrite(p,1,n,stdout); return *written==n;
}
static void ExitProcess(UINT code) { exit((int)code); }
#endif
