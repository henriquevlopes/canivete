// Read-only checks for the Maintenance Overview and Installed apps tabs: virus protection (Security
// Center and Defender through WMI), devices with problems (Device Manager's yellow marks), battery
// wear, the last Windows Update install, and the installed apps list (the registry's Uninstall keys,
// the same list Windows' Apps settings show for desktop apps).
#include "maint.h"

#include <cfgmgr32.h>
#include <setupapi.h>
#include <oleauto.h>
#include <wbemidl.h>
#include <wuapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <set>

using Microsoft::WRL::ComPtr;

namespace maint {
namespace {

struct Bstr {
    BSTR b = nullptr;
    explicit Bstr(const wchar_t* s) : b(SysAllocString(s)) {}
    ~Bstr() { SysFreeString(b); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
};

struct Var {
    VARIANT v;
    Var() { VariantInit(&v); }
    ~Var() { VariantClear(&v); }
    Var(const Var&) = delete;
    Var& operator=(const Var&) = delete;
};

// Runs a WQL query and calls each(object) per row. False: the namespace or class isn't readable.
template <class F>
bool Wmi(const wchar_t* ns, const wchar_t* query, F each) {
    ComPtr<IWbemLocator> loc;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&loc)))) return false;
    ComPtr<IWbemServices> svc;
    Bstr nsb(ns);
    if (FAILED(loc->ConnectServer(nsb.b, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc))) return false;
    CoSetProxyBlanket(svc.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                      RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    ComPtr<IEnumWbemClassObject> en;
    Bstr lang(L"WQL"), q(query);
    if (FAILED(svc->ExecQuery(lang.b, q.b, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en)))
        return false;
    CoSetProxyBlanket(en.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                      RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    bool any = false;
    for (;;) {
        ComPtr<IWbemClassObject> o;
        ULONG n = 0;
        const HRESULT hr = en->Next(10000, 1, &o, &n);
        if (FAILED(hr)) return any;
        if (hr != S_OK || !n) break;
        any = true;
        each(o.Get());
    }
    return true;
}

std::wstring WStr(IWbemClassObject* o, const wchar_t* name) {
    Var v;
    if (FAILED(o->Get(name, 0, &v.v, nullptr, nullptr)) || v.v.vt != VT_BSTR || !v.v.bstrVal) return {};
    return v.v.bstrVal;
}

// Integers and booleans (WMI hands uint32 over as VT_I4, uint64 as a string). def when missing.
long long WNum(IWbemClassObject* o, const wchar_t* name, long long def) {
    Var v;
    if (FAILED(o->Get(name, 0, &v.v, nullptr, nullptr))) return def;
    switch (v.v.vt) {
    case VT_BOOL:
        return v.v.boolVal ? 1 : 0;
    case VT_I4:
        return static_cast<unsigned long>(v.v.lVal);
    case VT_UI4:
        return v.v.ulVal;
    case VT_I2:
        return v.v.iVal;
    case VT_UI1:
        return v.v.bVal;
    case VT_BSTR:
        return v.v.bstrVal ? _wtoi64(v.v.bstrVal) : def;
    default:
        return def;
    }
}

std::wstring RegStr(HKEY k, const wchar_t* name) {
    wchar_t buf[2048];
    DWORD size = sizeof(buf);
    if (RegGetValueW(k, nullptr, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, buf, &size) !=
        ERROR_SUCCESS)
        return {};
    return buf;
}

DWORD RegDword(HKEY k, const wchar_t* name, DWORD def) {
    DWORD v = 0, size = sizeof(v);
    return RegGetValueW(k, nullptr, name, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS ? v : def;
}

void UninstallKeys(std::vector<InstalledApp>& out, HKEY root, REGSAM view, wchar_t where) {
    constexpr wchar_t kPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    HKEY list;
    if (RegOpenKeyExW(root, kPath, 0, KEY_READ | view, &list) != ERROR_SUCCESS) return;
    wchar_t sub[256];
    for (DWORD i = 0;; ++i) {
        DWORD n = 256;
        if (RegEnumKeyExW(list, i, sub, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        HKEY k;
        if (RegOpenKeyExW(list, sub, 0, KEY_READ | view, &k) != ERROR_SUCCESS) continue;
        InstalledApp a;
        a.name = RegStr(k, L"DisplayName");
        a.uninstall = RegStr(k, L"UninstallString");
        const bool hidden = RegDword(k, L"SystemComponent", 0) == 1 || !RegStr(k, L"ParentKeyName").empty() ||
                            RegStr(k, L"ReleaseType") == L"Update" || RegStr(k, L"ReleaseType") == L"Hotfix" ||
                            RegStr(k, L"ReleaseType") == L"Security Update";
        if (!a.name.empty() && !a.uninstall.empty() && !hidden) {
            a.key = std::wstring(1, where) + L"|" + sub;
            a.publisher = RegStr(k, L"Publisher");
            a.version = RegStr(k, L"DisplayVersion");
            const std::wstring d = RegStr(k, L"InstallDate");  // yyyymmdd
            if (d.size() == 8 && std::all_of(d.begin(), d.end(), iswdigit) && d.substr(4, 2) >= L"01" &&
                d.substr(4, 2) <= L"12" && d.substr(6, 2) >= L"01" && d.substr(6, 2) <= L"31")
                a.date = d.substr(0, 4) + L"-" + d.substr(4, 2) + L"-" + d.substr(6, 2);
            a.size = static_cast<ULONGLONG>(RegDword(k, L"EstimatedSize", 0)) * 1024;
            a.machine = root == HKEY_LOCAL_MACHINE;
            if (RegDword(k, L"WindowsInstaller", 0) == 1 && sub[0] == L'{') a.msi = sub;
            out.push_back(std::move(a));
        }
        RegCloseKey(k);
    }
    RegCloseKey(list);
}

}  // namespace

SecurityInfo ReadSecurity() {
    SecurityInfo s;
    // Security Center: which antivirus is on and whether its definitions are current. productState
    // packs that into bytes: 0x1000 bit range = enabled, 0x10 = definitions out of date.
    struct Product {
        std::wstring name;
        bool on, current;
    };
    std::vector<Product> products;
    Wmi(L"ROOT\\SecurityCenter2", L"SELECT displayName, productState FROM AntiVirusProduct", [&](IWbemClassObject* o) {
        const auto st = static_cast<unsigned long>(WNum(o, L"productState", 0));
        products.push_back({WStr(o, L"displayName"), ((st >> 12) & 0xF) == 1, ((st >> 4) & 0xF) == 0});
    });
    for (const Product& p : products)
        if (p.on && (!s.on || p.name.find(L"Defender") == std::wstring::npos)) {
            s.known = true;
            s.on = true;
            s.product = p.name;
            s.upToDate = p.current;
        }
    if (!s.known && !products.empty()) {
        s.known = true;
        s.product = products[0].name;
    }
    // Defender's own details (signature age, last quick scan) when it is the active one.
    if (!s.known || s.product.find(L"Defender") != std::wstring::npos) {
        Wmi(L"ROOT\\Microsoft\\Windows\\Defender",
            L"SELECT AntivirusEnabled, RealTimeProtectionEnabled, AntivirusSignatureAge, QuickScanAge, FullScanAge "
            L"FROM MSFT_MpComputerStatus",
            [&](IWbemClassObject* o) {
                s.defender = true;
                if (!s.known) {
                    s.known = true;
                    s.product = L"Microsoft Defender Antivirus";
                }
                s.on = WNum(o, L"AntivirusEnabled", 0) && WNum(o, L"RealTimeProtectionEnabled", 0);
                auto age = [&](const wchar_t* n) {
                    const long long v = WNum(o, n, -1);
                    return v < 0 || v > 100000 ? -1 : static_cast<int>(v);
                };
                s.sigAgeDays = age(L"AntivirusSignatureAge");
                const int q = age(L"QuickScanAge"), f = age(L"FullScanAge");
                s.scanAgeDays = q < 0 ? f : f < 0 ? q : std::min(q, f);
                if (s.sigAgeDays > 2) s.upToDate = false;
            });
    }
    return s;
}

std::vector<DeviceProblem> ScanDeviceProblems() {
    std::vector<DeviceProblem> v;
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return v;
    SP_DEVINFO_DATA d{sizeof(d)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); ++i) {
        ULONG status = 0, problem = 0;
        if (CM_Get_DevNode_Status(&status, &problem, d.DevInst, 0) != CR_SUCCESS) continue;
        if (!(status & DN_HAS_PROBLEM) || problem == CM_PROB_DISABLED || problem == CM_PROB_HARDWARE_DISABLED) continue;
        wchar_t name[512] = L"";
        if (!SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_FRIENDLYNAME, nullptr, reinterpret_cast<PBYTE>(name),
                                               sizeof(name), nullptr))
            SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_DEVICEDESC, nullptr, reinterpret_cast<PBYTE>(name),
                                              sizeof(name), nullptr);
        v.push_back({name[0] ? name : L"Unknown device", problem});
    }
    SetupDiDestroyDeviceInfoList(set);
    return v;
}

BatteryInfo ReadBattery() {
    BatteryInfo b;
    if (SYSTEM_POWER_STATUS ps; GetSystemPowerStatus(&ps) && (ps.BatteryFlag == 128 || ps.BatteryFlag == 255)) return b;
    Wmi(L"ROOT\\WMI", L"SELECT DesignedCapacity FROM BatteryStaticData", [&](IWbemClassObject* o) {
        b.design += static_cast<ULONG>(WNum(o, L"DesignedCapacity", 0));
    });
    Wmi(L"ROOT\\WMI", L"SELECT FullChargedCapacity FROM BatteryFullChargedCapacity", [&](IWbemClassObject* o) {
        b.full += static_cast<ULONG>(WNum(o, L"FullChargedCapacity", 0));
    });
    b.present = b.design > 0 && b.full > 0;
    return b;
}

int DaysSinceLastUpdate() {
    ComPtr<IUpdateSession> session;
    ComPtr<IUpdateSearcher> searcher;
    if (FAILED(CoCreateInstance(CLSID_UpdateSession, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&session))) ||
        FAILED(session->CreateUpdateSearcher(&searcher)))
        return -1;
    LONG count = 0;
    if (FAILED(searcher->GetTotalHistoryCount(&count)) || count <= 0) return -1;
    ComPtr<IUpdateHistoryEntryCollection> hist;
    if (FAILED(searcher->QueryHistory(0, std::min<LONG>(count, 100), &hist))) return -1;
    LONG n = 0;
    hist->get_Count(&n);
    DATE latest = 0;
    for (LONG i = 0; i < n; ++i) {
        ComPtr<IUpdateHistoryEntry> e;
        if (FAILED(hist->get_Item(i, &e))) continue;
        UpdateOperation op{};
        OperationResultCode rc{};
        DATE d = 0;
        if (SUCCEEDED(e->get_Operation(&op)) && op == uoInstallation && SUCCEEDED(e->get_ResultCode(&rc)) &&
            (rc == orcSucceeded || rc == orcSucceededWithErrors) && SUCCEEDED(e->get_Date(&d)))
            latest = std::max(latest, d);
    }
    if (latest <= 0) return -1;
    SYSTEMTIME now;
    GetSystemTime(&now);
    DATE today = 0;
    SystemTimeToVariantTime(&now, &today);
    return std::max(0, static_cast<int>(today - latest));
}

std::vector<InstalledApp> ScanInstalled() {
    std::vector<InstalledApp> v;
    UninstallKeys(v, HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, L'M');
    UninstallKeys(v, HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, L'W');
    UninstallKeys(v, HKEY_CURRENT_USER, 0, L'U');
    // The same app can be listed twice (32- and 64-bit views); keep the first.
    std::set<std::wstring> seen;
    std::vector<InstalledApp> out;
    for (auto& a : v) {
        std::wstring id = a.name + L"\x1f" + a.version;
        for (auto& c : id) c = static_cast<wchar_t>(towlower(c));
        if (seen.insert(id).second) out.push_back(std::move(a));
    }
    std::sort(out.begin(), out.end(),
              [](const InstalledApp& a, const InstalledApp& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    return out;
}

bool SplitCommand(const std::wstring& cmd, std::wstring& exe, std::wstring& args) {
    std::wstring c = cmd;
    while (!c.empty() && iswspace(c.front())) c.erase(0, 1);
    if (c.empty()) return false;
    if (c[0] == L'"') {
        const size_t q = c.find(L'"', 1);
        if (q == std::wstring::npos) return false;
        exe = c.substr(1, q - 1);
        args = c.substr(q + 1);
    } else {
        std::wstring lower = c;
        for (auto& ch : lower) ch = static_cast<wchar_t>(towlower(ch));
        const size_t e = lower.find(L".exe");
        const size_t sp = c.find(L' ');
        const size_t end = e != std::wstring::npos ? e + 4 : sp;
        exe = c.substr(0, end);
        args = end < c.size() ? c.substr(end) : L"";
    }
    while (!args.empty() && iswspace(args.front())) args.erase(0, 1);
    exe = [&] {
        wchar_t buf[MAX_PATH * 2];
        const DWORD n = ExpandEnvironmentStringsW(exe.c_str(), buf, MAX_PATH * 2);
        return n && n <= MAX_PATH * 2 ? std::wstring(buf) : exe;
    }();
    return !exe.empty();
}

}  // namespace maint
