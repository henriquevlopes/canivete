// Computer maintenance: shared between the Maintenance window (maint.cpp) and the checks and jobs
// behind it (maint_ops.cpp).
#pragma once
#include "app.h"

#include <functional>
#include <string>
#include <vector>

namespace maint {

// Progress from a job. key: an item's key (its row shows the text), or "status" (the status line),
// "log" / "logp" (a line of tool output; "logp" = a progress line the next line replaces),
// "reboot" (a restart is needed now), and for the progress bar:
//   "pct"   overall progress in thousandths ("0".."1000"), "-1" = no number (moving bar)
//   "step"  "<i>|<n>|<item key>|<what>": item i of n is being worked on ("Downloading 45%")
//   "freed" bytes freed so far (cleanup)
//   "ok" / "fail" / "skip"  text = an item's key: how it ended ("skip": not done, the job was stopped)
//   "hello" the elevated copy has started (it lets the window take the foreground back)
using Sink = std::function<void(const std::wstring& key, const std::wstring& text)>;

constexpr ULONG_PTR kCopyProgress = 0x4D4E5450;  // WM_COPYDATA tag from the elevated copy: "key\ttext"
constexpr wchar_t kWindowClass[] = L"CaniveteMaintenance";

// Cancel: the window sets a named event (per window process); jobs stop after the current item.
std::wstring CancelEventName(DWORD windowPid);
void SetCancelEvent(HANDLE event);  // the event this process's jobs watch
bool CancelRequested();
// Test copies only (CANIVETE_TEST and CANIVETE_MAINT_FAKE=1): checks and jobs are simulated, nothing
// is installed or deleted, and jobs "with administrator rights" start the helper copy without a prompt.
bool FakeMode();

// ---- updates
struct UpdateItem {
    std::wstring key;      // "wu:<UpdateID>" or "app:<winget id>"
    std::wstring name;
    std::wstring kind;     // "Windows", "Driver", "App"
    std::wstring version;  // "5.2.3 → 5.3.0", "KB5031455", driver date
    ULONGLONG size = 0;    // download size, 0 = unknown
    bool optional = false; // Windows Update lists it as optional (not checked at first)
};
struct GpuInfo {
    std::wstring name, version, date;
    std::wstring open, openLabel;  // the maker's driver app (path or web page) and its button text
};
struct UpdateScan {
    std::vector<UpdateItem> items;
    std::wstring wuError, appError;  // why a source could not be checked
    std::vector<GpuInfo> gpus;
};
UpdateScan ScanUpdates(const Sink& progress);  // takes a minute or more: call on a worker thread
std::vector<GpuInfo> ScanGpus();

// ---- cleanup
struct CleanItem {
    std::wstring key, name, note;
    ULONGLONG size = 0;
    bool measured = false;  // false: needs administrator rights to measure (or never measured)
    bool admin = false;     // cleaning it needs administrator rights
    bool byDefault = false;
};
std::vector<CleanItem> ScanCleanup();

// ---- startup apps
struct StartupItem {
    std::wstring key;  // "<U|M>|<Run|Run32|StartupFolder>|<value name>"
    std::wstring name, where, command;
    bool on = true;
    bool admin = false;  // all users: changing it needs administrator rights
};
std::vector<StartupItem> ScanStartup();

// ---- health
bool RebootPending();
struct DriveSpace {
    std::wstring root;
    ULONGLONG free = 0, total = 0;
};
struct Health {
    ULONGLONG uptimeMs = 0;
    bool rebootPending = false;
    MEMORYSTATUSEX mem{sizeof(MEMORYSTATUSEX)};
    std::vector<DriveSpace> drives;
};
Health ReadHealth();

// ---- overview checks (maint_info.cpp)
struct SecurityInfo {
    bool known = false;     // an antivirus was found
    std::wstring product;   // "Microsoft Defender Antivirus", …
    bool on = false;        // real-time protection on
    bool upToDate = true;   // virus definitions current
    bool defender = false;  // the details below come from Defender
    int sigAgeDays = -1;    // definitions age; -1 unknown
    int scanAgeDays = -1;   // days since the last quick or full scan; -1 unknown / never
};
SecurityInfo ReadSecurity();
struct DeviceProblem {
    std::wstring name;
    ULONG code = 0;  // Device Manager's problem code
};
std::vector<DeviceProblem> ScanDeviceProblems();
struct BatteryInfo {
    bool present = false;
    ULONG design = 0, full = 0;  // mWh when new, and now
};
BatteryInfo ReadBattery();
int DaysSinceLastUpdate();  // last successful Windows Update install; -1 unknown

// ---- installed apps
struct InstalledApp {
    std::wstring key;  // "<M|W|U>|<Uninstall subkey>"
    std::wstring name, publisher, version, date, uninstall, msi;
    ULONGLONG size = 0;
    bool machine = false;
};
std::vector<InstalledApp> ScanInstalled();
bool SplitCommand(const std::wstring& cmd, std::wstring& exe, std::wstring& args);  // "exe" args

// ---- jobs. "install" (items: "restore", "wu:<id>", "app:<id>"), "clean" (cleanup keys), "startup"
// ("<on 0|1>|<StartupItem key>"), "repair", "optimize", "restore". Returns the process exit code.
int RunTask(const std::wstring& task, const std::vector<std::wstring>& args, const Sink& sink);
bool IsElevated();
bool ValidWingetId(const std::wstring& id);
std::wstring SizeText(ULONGLONG bytes);
std::wstring QuoteArg(const std::wstring& a);  // for a CreateProcess / ShellExecute command line

}  // namespace maint
