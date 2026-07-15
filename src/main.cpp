// FPS Overlay — Lightweight DirectX 11 + ImGui performance monitor
//
// Features:
//   - Real game FPS via ETW (Event Tracing for Windows)
//     Supports: DirectX 9/10/11/12, Vulkan, OpenGL via DXGI + DxgKrnl providers
//   - GPU usage & temperature via LibreHardwareMonitor (supports NVIDIA, AMD, Intel)
//   - CPU / RAM monitoring
//   - Hardware names (CPU model, GPU model)
//   - Custom hotkey binding
//   - System tray integration
//
// Requires: run as Administrator for game FPS capture (ETW needs it)

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi.h>
#include <shellapi.h>
#include <evntrace.h>
#include <evntcons.h>
#include <psapi.h>
#include <wbemidl.h>
#include <comdef.h>
#include <wincodec.h>
#include <tlhelp32.h>
#include <pdh.h>
#include <powrprof.h>
#include <iphlpapi.h>
#include <taskschd.h>
#include <chrono>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <ctime>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <map>
#include <algorithm>
#include <utility>
#include <fstream>
#include <filesystem>
#include <string>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "PowrProf.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "taskschd.lib")

// Note: Link with -lwbemuuid -lole32 -loleaut32 for WMI support
// Note: Link with lhwm-cpp-wrapper.lib and mscoree.lib for LibreHardwareMonitor support

#include "imgui.h"
#include "imgui_internal.h"
#include "lhwm-cpp-wrapper.h"
#include <tuple>
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "../features/FeatureRegistry.h"
#include "../features/SettingsUi.h"

// ═══════════════════════════════════════════════════════════════════════════
// Constants & safety defines
// ═══════════════════════════════════════════════════════════════════════════
#define WM_TRAYICON   (WM_USER + 1)
#define WM_APP_TRAY_COMMAND (WM_APP + 10)
#define WM_APP_TRAY_TOOLTIP (WM_APP + 11)
#define WM_APP_TRAY_RESTORE (WM_APP + 12)
#define WM_APP_TRAY_RECOVERY_STATUS (WM_APP + 13)
#define IDM_SETTINGS  1001
#define IDM_EXIT      1002
#define IDM_SHOW      1003
#define IDM_HIDE      1004
#define IDM_RESET_POS 1006
#define IDM_DESKTOP_ONLY 1007
#define IDM_POWER_SHOW_SYSTEM 1010
#define IDM_POWER_SHOW_KNOWN 1011
#define IDM_POWER_SHOW_BATTERY 1012
#define IDM_POWER_ALLOW_ESTIMATE 1013
#define IDM_RESCAN_SENSORS 1014
#define IDM_COPY_DIAGNOSTIC_SUMMARY 1015

#include "version.h"

// Current delivered build identifier. Kept as APP_VERSION so existing config,
// diagnostics and UI call sites remain backward-compatible.
#define APP_VERSION VER_DISPLAY_STRING

// Settings window defaults. Width and height are both freely resizable.
static const int kConfigDlgOuterW = 1120;
static const int kConfigDlgOuterH = 760;
static const int kConfigDlgMinOuterW = 720;
static const int kConfigDlgMinOuterH = 520;

// PawnIO installer resource ID (embedded executable)
#define IDR_PAWNIO_SETUP 101
#define IDR_GITHUB_ICON  102
#define IDR_KOFI_ICON    103

#ifndef PROCESS_TRACE_MODE_EVENT_RECORD
#define PROCESS_TRACE_MODE_EVENT_RECORD 0x10000000
#endif
#ifndef PROCESS_TRACE_MODE_REAL_TIME
#define PROCESS_TRACE_MODE_REAL_TIME    0x00000100
#endif
#ifndef EVENT_CONTROL_CODE_ENABLE_PROVIDER
#define EVENT_CONTROL_CODE_ENABLE_PROVIDER 1
#endif
#ifndef TRACE_LEVEL_INFORMATION
#define TRACE_LEVEL_INFORMATION 4
#endif

// Microsoft-Windows-DXGI provider  {CA11C036-0102-4A2D-A6AD-F03CFED5D3C9}
static const GUID DXGI_PROVIDER =
    { 0xCA11C036, 0x0102, 0x4A2D, { 0xA6, 0xAD, 0xF0, 0x3C, 0xFE, 0xD5, 0xD3, 0xC9 } };

// Microsoft-Windows-D3D9 provider  {783ACA0A-790E-4D7F-8451-AA850511C6B9}
static const GUID D3D9_PROVIDER =
    { 0x783ACA0A, 0x790E, 0x4D7F, { 0x84, 0x51, 0xAA, 0x85, 0x05, 0x11, 0xC6, 0xB9 } };

// Microsoft-Windows-DxgKrnl provider  {802EC45A-1E99-4B83-9920-87C98277BA9D}
// This captures presents at the kernel level for ALL graphics APIs: DX9/10/11/12, Vulkan, OpenGL
static const GUID DXGKRNL_PROVIDER =
    { 0x802EC45A, 0x1E99, 0x4B83, { 0x99, 0x20, 0x87, 0xC9, 0x82, 0x77, 0xBA, 0x9D } };

// DxgKrnl keywords for Present tracking
static const ULONGLONG DXGKRNL_KEYWORD_PRESENT = 0x8000000;  // Present keyword
static const ULONGLONG DXGKRNL_KEYWORD_BASE    = 0x1;        // Base keyword

// DxgKrnl event IDs for present tracking
static const USHORT DXGKRNL_EVENT_PRESENT_INFO = 0x00B8;  // Present::Info (184)
static const USHORT DXGKRNL_EVENT_FLIP_INFO    = 0x00A8;  // Flip::Info (168)
static const USHORT DXGKRNL_EVENT_BLIT_INFO    = 0x00A6;  // Blit::Info (166)

static const char* ETW_SESSION_NAME = "FPSOverlay_ETW";

// ═══════════════════════════════════════════════════════════════════════════
// Configuration
// ═══════════════════════════════════════════════════════════════════════════
// Layout: 0 = vertical stack, 1 = horizontal compact, 2 = Steam-style performance bar, 3 = Game++-style segmented bar
#define LAYOUT_VERTICAL   0
#define LAYOUT_HORIZONTAL 1
#define LAYOUT_STEAM      2
#define LAYOUT_GAMEPP     3

// Overlay corner / edge (work-area relative). Legacy INI 0..3 remapped on load via positionVer.
#define POS_TOP_LEFT       0
#define POS_TOP_CENTER     1
#define POS_TOP_RIGHT      2
#define POS_BOTTOM_LEFT    3
#define POS_BOTTOM_CENTER  4
#define POS_BOTTOM_RIGHT   5

#define TIME_FORMAT_24H    0
#define TIME_FORMAT_12H    1

#define FREQ_PATH_MAX   260
#define FREQ_SPARK_LEN  48
#define POWER_PATH_MAX  512

struct OverlayConfig {
    bool showFPS  = true;
    bool useAverageFPS = true; // show the rolling 60-second average in the FPS slot
    bool customFpsColors = false;
    int  fpsWarningThreshold = 30;
    int  fpsGoodThreshold = 60;
    bool showCpuUsage = true;
    bool showCpuTemp = true;
    bool showGpuUsage = true;
    bool showGpuTemp = true;
    bool showVRAMPercent = true;
    bool showVRAMUsage = true; // used / total
    bool showRAMPercent = true;
    bool showRAMUsage = true;  // used / total
    bool showNetworkDownload = false;
    bool showNetworkUpload = false;
    bool showProcessName = true; // tracked game / process label (all layouts)
    bool showTime = false;
    int  timeFormat = TIME_FORMAT_24H; // TIME_FORMAT_24H or TIME_FORMAT_12H
    bool timeShowSeconds = true;
    int  layoutStyle = LAYOUT_VERTICAL;
    bool useFahrenheit = false; // false = Celsius, true = Fahrenheit
    bool autoStart = true;    // skip config window and start overlay immediately
    bool startWithWindows = false; // launch elevated through Task Scheduler at logon
    int  position = POS_TOP_LEFT; // POS_* constants
    int  opacity  = 85;       // 30..100 % overlay background (all layouts)
    int  toggleKey = VK_INSERT;
    int  exitKey   = VK_END;
    int  layoutSwitchKey = VK_F9;
    float customX = -1.0f;    // custom position (-1 = use corner preset)
    float customY = -1.0f;
    int  selectedGpu = 0;     // selected GPU index (0 = first GPU)
    int  overlayScale = 100;  // 50..200 % UI scale for all layouts
    bool showCpuPower = false;
    bool showGpuPower = false;
    bool showCpuFan = true;
    bool showGpuFan = true;
    bool showSystemFan = true;
    bool showCpuFreq = true;
    bool showGpuCoreFreq = true;
    bool showGpuMemTemp = true;
    bool showDiskTemp = false;
    bool showMemoryTemp = false;
    char cpuFreqPath[FREQ_PATH_MAX] = "";
    char gpuCoreFreqPath[FREQ_PATH_MAX] = "";
    char acInputPowerPath[POWER_PATH_MAX] = "";
    char totalSystemPowerPath[POWER_PATH_MAX] = "";
    int  gameppFontPreset = 2;     // 0=480, 1=720, 2=1080, 3=2K, 4=4K
    int  refreshMs = 1000;         // Metrics refresh interval
    bool monitorSnap = true;       // Snap dragged overlay near work-area edges
    bool desktopOnlyMode = false;  // Hide temporarily while a foreground app fills its monitor
};

// ═══════════════════════════════════════════════════════════════════════════
// GPU list (for multi-GPU support via LHWM)
// ═══════════════════════════════════════════════════════════════════════════
#define MAX_GPUS 8

struct GpuInfo {
    char name[256];
    std::string tempPath;      // LHWM sensor path for temperature
    std::string loadPath;      // LHWM sensor path for GPU load
    int         loadPathPri  = -1; // higher = better match: 2=exact "GPU Core", 1=contains "GPU Core", 0=fallback
    std::string vramUsedPath;  // LHWM sensor path for VRAM used
    std::string vramTotalPath; // LHWM sensor path for VRAM total
    int         vramTotalPri = -1; // higher = better match (see VramTotalSensorPriority)
    std::string powerPath;         // LHWM sensor path for GPU power (Watts)
    std::string fanPath;           // LHWM sensor path for GPU fan speed (RPM)
    int         fanPathPri   = -1; // higher = better match (see GpuFanSensorPriority)
    bool        fanIsPercent = false; // true when path is a Control (%) sensor
    std::string memTempPath;       // GPU memory junction temperature
    std::vector<std::pair<std::string, std::string>> coreClockOpts; // display name, path
};
static GpuInfo g_gpuList[MAX_GPUS];
static int g_gpuCount = 0;

// Helper to convert Celsius to Fahrenheit
inline float ToDisplayTemp(float celsius, bool useFahrenheit) {
    return useFahrenheit ? (celsius * 9.0f / 5.0f + 32.0f) : celsius;
}

// Temperature thresholds (in Celsius) - adjust for F display comparison
inline float GetHighTempThreshold(bool useFahrenheit) { return useFahrenheit ? 185.0f : 85.0f; }
inline float GetMedTempThreshold(bool useFahrenheit) { return useFahrenheit ? 158.0f : 70.0f; }

static void FormatOverlayTime(const OverlayConfig& cfg, char* out, size_t outLen)
{
    if (!out || outLen == 0) return;
    out[0] = '\0';

    SYSTEMTIME st;
    GetLocalTime(&st);

    if (cfg.timeFormat == TIME_FORMAT_12H) {
        int h = (int)st.wHour % 12;
        if (h == 0) h = 12;
        const char* ampm = (st.wHour < 12) ? "AM" : "PM";
        if (cfg.timeShowSeconds)
            snprintf(out, outLen, "%d:%02u:%02u %s", h, st.wMinute, st.wSecond, ampm);
        else
            snprintf(out, outLen, "%d:%02u %s", h, st.wMinute, ampm);
    } else {
        if (cfg.timeShowSeconds)
            snprintf(out, outLen, "%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
        else
            snprintf(out, outLen, "%02u:%02u", st.wHour, st.wMinute);
    }
}

static double g_networkDownloadBps = 0.0;
static double g_networkUploadBps = 0.0;

static bool QueryNetworkByteTotals(ULONGLONG& received, ULONGLONG& sent)
{
    received = 0;
    sent = 0;
    PMIB_IF_TABLE2 table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || !table)
        return false;

    bool found = false;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2& row = table->Table[i];
        if (row.OperStatus != IfOperStatusUp ||
            row.MediaConnectState != MediaConnectStateConnected ||
            row.Type == IF_TYPE_SOFTWARE_LOOPBACK ||
            row.Type == IF_TYPE_TUNNEL) {
            continue;
        }

        const bool usable =
            row.InterfaceAndOperStatusFlags.HardwareInterface ||
            row.Type == IF_TYPE_PPP ||
            row.Type == IF_TYPE_WWANPP ||
            row.Type == IF_TYPE_WWANPP2;
        if (!usable)
            continue;

        received += row.InOctets;
        sent += row.OutOctets;
        found = true;
    }
    FreeMibTable(table);
    return found;
}

static void UpdateNetworkRates()
{
    static bool initialized = false;
    static ULONGLONG previousReceived = 0;
    static ULONGLONG previousSent = 0;
    static auto previousTime = std::chrono::steady_clock::now();

    ULONGLONG received = 0;
    ULONGLONG sent = 0;
    const auto now = std::chrono::steady_clock::now();
    if (!QueryNetworkByteTotals(received, sent)) {
        g_networkDownloadBps = 0.0;
        g_networkUploadBps = 0.0;
        initialized = false;
        return;
    }

    if (initialized) {
        const double seconds =
            std::chrono::duration<double>(now - previousTime).count();
        if (seconds > 0.05) {
            g_networkDownloadBps = received >= previousReceived
                ? (double)(received - previousReceived) / seconds : 0.0;
            g_networkUploadBps = sent >= previousSent
                ? (double)(sent - previousSent) / seconds : 0.0;
        }
    } else {
        g_networkDownloadBps = 0.0;
        g_networkUploadBps = 0.0;
        initialized = true;
    }

    previousReceived = received;
    previousSent = sent;
    previousTime = now;
}

static void FormatNetworkRate(char* out, size_t outLen, double bytesPerSecond)
{
    if (!out || outLen == 0) return;
    bytesPerSecond = (std::max)(0.0, bytesPerSecond);
    constexpr double kKiB = 1024.0;
    constexpr double kMiB = 1024.0 * 1024.0;
    constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
    if (bytesPerSecond >= kGiB)
        snprintf(out, outLen, "%.2f GB/s", bytesPerSecond / kGiB);
    else if (bytesPerSecond >= kMiB)
        snprintf(out, outLen, "%.2f MB/s", bytesPerSecond / kMiB);
    else if (bytesPerSecond >= kKiB)
        snprintf(out, outLen, "%.1f KB/s", bytesPerSecond / kKiB);
    else
        snprintf(out, outLen, "%.0f B/s", bytesPerSecond);
}

// ═══════════════════════════════════════════════════════════════════════════
// Configuration file (INI) - saved next to overlay.exe
// ═══════════════════════════════════════════════════════════════════════════
static char g_configPath[MAX_PATH] = "";
static const int CONFIG_SCHEMA_VERSION = 1;
static char g_lastConfigBackupPath[MAX_PATH] = "";
static char g_lastConfigRecoveryReason[128] = "";
static char g_logPath[MAX_PATH] = "";
static std::mutex g_logMutex;
static std::mutex g_lhwmStateMutex;
static std::mutex g_configIoMutex;
static thread_local const char* g_iniWritePath = nullptr;
static bool g_ConfigDirty = false;
static ULONGLONG g_ConfigDirtyTick = 0;
static FeatureRegistry g_FeatureRegistry;
static FeatureContext  g_FeatureContext;
static std::atomic<bool> g_trayShowSystemPower{true};
static std::atomic<bool> g_trayShowKnownPower{true};
static std::atomic<bool> g_trayShowBatteryPower{true};
static std::atomic<bool> g_trayAllowEstimatedPower{true};
static std::atomic<bool> g_trayDesktopOnlyMode{false};
// Written when PawnIO_setup succeeds; survives config.ini being rewritten or stripped.
static char g_pawnioRebootStatePath[MAX_PATH] = "";

static void InitConfigPath()
{
    if (g_configPath[0] != '\0') return; // already initialized
    
    // Get the directory where the executable is located
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    
    // Remove the executable name to get the directory
    char* lastSlash = strrchr(exePath, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';
    
    // Append the config filename
    snprintf(g_configPath, MAX_PATH, "%sconfig.ini", exePath);
    snprintf(g_pawnioRebootStatePath, MAX_PATH, "%sfpsoverlay-pawnio-reboot.state", exePath);
    snprintf(g_logPath, MAX_PATH, "%sfps-overlay.log", exePath);
}

static void LogLine(const char* fmt, ...)
{
    InitConfigPath();
    std::lock_guard<std::mutex> lock(g_logMutex);

    static unsigned int sizeCheckCounter = 0;
    if ((sizeCheckCounter++ & 0xffu) == 0) {
        WIN32_FILE_ATTRIBUTE_DATA data = {};
        if (GetFileAttributesExA(g_logPath, GetFileExInfoStandard, &data)) {
            ULARGE_INTEGER size = {};
            size.HighPart = data.nFileSizeHigh;
            size.LowPart = data.nFileSizeLow;
            constexpr ULONGLONG kMaxLogBytes = 16ull * 1024ull * 1024ull;
            if (size.QuadPart >= kMaxLogBytes) {
                char rotatedPath[MAX_PATH] = {};
                snprintf(rotatedPath, sizeof(rotatedPath), "%s.previous.log", g_logPath);
                MoveFileExA(g_logPath, rotatedPath,
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            }
        }
    }

    FILE* f = nullptr;
    if (fopen_s(&f, g_logPath, "ab") != 0 || !f)
        return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);

    fputc('\n', f);
    fclose(f);
}

static void BuildTimestampSuffix(char* out, size_t cap)
{
    if (!out || cap == 0) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(out, cap, "%04u%02u%02u-%02u%02u%02u",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

static bool ConfigFileLooksUsable(const char* path, char* reason, size_t reasonCap)
{
    if (reason && reasonCap) reason[0] = '\0';

    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        if (reason && reasonCap) snprintf(reason, reasonCap, "open_failed");
        return false;
    }

    const std::streamoff size = in.tellg();
    if (size <= 0) {
        if (reason && reasonCap) snprintf(reason, reasonCap, "empty_file");
        return false;
    }
    if (size > 1024 * 1024) {
        if (reason && reasonCap) snprintf(reason, reasonCap, "too_large");
        return false;
    }

    std::string data;
    data.resize((size_t)size);
    in.seekg(0, std::ios::beg);
    if (!in.read(data.data(), size)) {
        if (reason && reasonCap) snprintf(reason, reasonCap, "read_failed");
        return false;
    }

    if (data.find('\0') != std::string::npos) {
        if (reason && reasonCap) snprintf(reason, reasonCap, "binary_data");
        return false;
    }
    if (data.find('[') == std::string::npos || data.find('=') == std::string::npos) {
        if (reason && reasonCap) snprintf(reason, reasonCap, "not_ini");
        return false;
    }

    return true;
}

static bool BackupAndResetConfig(const char* reason)
{
    InitConfigPath();
    g_lastConfigBackupPath[0] = '\0';
    snprintf(g_lastConfigRecoveryReason, sizeof(g_lastConfigRecoveryReason),
             "%s", reason ? reason : "unknown");

    char stamp[32] = {};
    BuildTimestampSuffix(stamp, sizeof(stamp));

    char backupPath[MAX_PATH] = {};
    snprintf(backupPath, MAX_PATH, "%s.%s.%s.bak",
             g_configPath, reason && reason[0] ? reason : "recovered", stamp);

    if (!CopyFileA(g_configPath, backupPath, FALSE)) {
        if (!MoveFileExA(g_configPath, backupPath, MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING))
            return false;
    } else {
        DeleteFileA(g_configPath);
    }

    snprintf(g_lastConfigBackupPath, sizeof(g_lastConfigBackupPath), "%s", backupPath);
    return true;
}

static bool RecoverConfigIfNeeded()
{
    InitConfigPath();
    g_lastConfigBackupPath[0] = '\0';
    g_lastConfigRecoveryReason[0] = '\0';

    const DWORD attrib = GetFileAttributesA(g_configPath);
    if (attrib == INVALID_FILE_ATTRIBUTES)
        return false;

    char reason[128] = {};
    if (!ConfigFileLooksUsable(g_configPath, reason, sizeof(reason)))
        return BackupAndResetConfig(reason);

    const int schema = GetPrivateProfileIntA("App", "schema_version", 0, g_configPath);
    if (schema > CONFIG_SCHEMA_VERSION)
        return BackupAndResetConfig("future_schema");

    return false;
}

static int ReadIniInt(const char* section, const char* key, int defaultVal)
{
    return GetPrivateProfileIntA(section, key, defaultVal, g_configPath);
}

static float ReadIniFloat(const char* section, const char* key, float defaultVal)
{
    char buf[32];
    GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), g_configPath);
    if (buf[0] == '\0') return defaultVal;
    return (float)atof(buf);
}

static void WriteIniInt(const char* section, const char* key, int value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    WritePrivateProfileStringA(section, key, buf,
                               g_iniWritePath ? g_iniWritePath : g_configPath);
}

static void WriteIniFloat(const char* section, const char* key, float value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f", value);
    WritePrivateProfileStringA(section, key, buf,
                               g_iniWritePath ? g_iniWritePath : g_configPath);
}

static void ReadIniStr(const char* section, const char* key, char* out, size_t cap)
{
    if (!cap) return;
    GetPrivateProfileStringA(section, key, "", out, (DWORD)cap, g_configPath);
    out[cap - 1] = '\0';
}

static void WriteIniStr(const char* section, const char* key, const char* value)
{
    WritePrivateProfileStringA(section, key, value ? value : "",
                               g_iniWritePath ? g_iniWritePath : g_configPath);
}

// PawnIO reboot gate — sidecar state (see CommitPawnIORebootPending / CheckPawnIORebootGateOrExit).
static bool WritePawnIORebootPendingStateFile(const char* hex16)
{
    if (!hex16 || strlen(hex16) != 16) return false;
    InitConfigPath();
    char line[96];
    snprintf(line, sizeof(line), "FPSOVERLAY_PAWNIO_REBOOT 1 %s\n", hex16);
    const DWORD flags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH;
    HANDLE h = CreateFileA(g_pawnioRebootStatePath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           flags, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const size_t n = strlen(line);
    const BOOL ok = WriteFile(h, line, (DWORD)n, &written, nullptr) && written == (DWORD)n;
    FlushFileBuffers(h);
    CloseHandle(h);
    return ok != FALSE;
}

static bool ReadPawnIORebootPendingStateFile(char* hexOut, size_t cap)
{
    if (!hexOut || cap < 17) return false;
    hexOut[0] = '\0';
    InitConfigPath();
    HANDLE h = CreateFileA(g_pawnioRebootStatePath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    char buf[256] = {};
    DWORD rd = 0;
    const BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &rd, nullptr);
    CloseHandle(h);
    if (!ok || rd == 0) return false;
    buf[rd] = '\0';
    int ver = 0;
    char hexBuf[24] = {};
    if (sscanf_s(buf, "FPSOVERLAY_PAWNIO_REBOOT %d %23s", &ver, hexBuf, (unsigned)sizeof(hexBuf)) < 2 || ver != 1)
        return false;
    if (strlen(hexBuf) != 16) return false;
    unsigned long long v = 0;
    if (sscanf_s(hexBuf, "%llx", &v) != 1) return false;
    (void)v;
    snprintf(hexOut, cap, "%s", hexBuf);
    return true;
}

static void DeletePawnIORebootPendingStateFile()
{
    InitConfigPath();
    DeleteFileA(g_pawnioRebootStatePath);
}

static void LoadConfig(OverlayConfig& cfg)
{
    InitConfigPath();
    RecoverConfigIfNeeded();
    if (g_lastConfigBackupPath[0] != '\0') {
        LogLine("Config recovered: reason=%s backup=%s",
                g_lastConfigRecoveryReason, g_lastConfigBackupPath);
    }
    
    // Check if config file exists
    DWORD attrib = GetFileAttributesA(g_configPath);
    if (attrib == INVALID_FILE_ATTRIBUTES) {
        // No config file, use defaults
        return;
    }
    
    // Display settings
    cfg.showFPS       = ReadIniInt("Display", "showFPS", 1) != 0;
    cfg.useAverageFPS = ReadIniInt("Display", "useAverageFPS", 1) != 0;
    cfg.customFpsColors = ReadIniInt("Display", "customFpsColors", 0) != 0;
    cfg.fpsWarningThreshold = ReadIniInt("Display", "fpsWarningThreshold", 30);
    cfg.fpsGoodThreshold = ReadIniInt("Display", "fpsGoodThreshold", 60);
    {
        const int legacyCpu = ReadIniInt("Display", "showCPU", 1);
        int cu = ReadIniInt("Display", "showCpuUsage", -1);
        if (cu < 0) cu = legacyCpu;
        cfg.showCpuUsage = cu != 0;
        int ct = ReadIniInt("Display", "showCpuTemp", -1);
        if (ct < 0) ct = legacyCpu;
        cfg.showCpuTemp = ct != 0;
        const int legacyGpu = ReadIniInt("Display", "showGPU", 1);
        int gu = ReadIniInt("Display", "showGpuUsage", -1);
        if (gu < 0) gu = legacyGpu;
        cfg.showGpuUsage = gu != 0;
        int gt = ReadIniInt("Display", "showGpuTemp", -1);
        if (gt < 0) gt = legacyGpu;
        cfg.showGpuTemp = gt != 0;
    }
    {
        const int legacyVram = ReadIniInt("Display", "showVRAM", 1);
        const int legacyRam = ReadIniInt("Display", "showRAM", 1);
        int vramPercent = ReadIniInt("Display", "showVRAMPercent", -1);
        int vramUsage = ReadIniInt("Display", "showVRAMUsage", -1);
        int ramPercent = ReadIniInt("Display", "showRAMPercent", -1);
        int ramUsage = ReadIniInt("Display", "showRAMUsage", -1);
        cfg.showVRAMPercent = (vramPercent < 0 ? legacyVram : vramPercent) != 0;
        cfg.showVRAMUsage = (vramUsage < 0 ? legacyVram : vramUsage) != 0;
        cfg.showRAMPercent = (ramPercent < 0 ? legacyRam : ramPercent) != 0;
        cfg.showRAMUsage = (ramUsage < 0 ? legacyRam : ramUsage) != 0;
    }
    cfg.showNetworkDownload = ReadIniInt("Display", "showNetworkDownload", 0) != 0;
    cfg.showNetworkUpload = ReadIniInt("Display", "showNetworkUpload", 0) != 0;
    cfg.showProcessName = ReadIniInt("Display", "showProcessName", 1) != 0;
    cfg.showTime        = ReadIniInt("Display", "showTime", 0) != 0;
    cfg.timeFormat      = ReadIniInt("Display", "timeFormat", TIME_FORMAT_24H);
    cfg.timeShowSeconds = ReadIniInt("Display", "timeShowSeconds", 1) != 0;

    cfg.showCpuPower    = ReadIniInt("Display", "showCpuPower", 0) != 0;
    cfg.showGpuPower    = ReadIniInt("Display", "showGpuPower", 0) != 0;
    cfg.showCpuFan      = ReadIniInt("Display", "showCpuFan", cfg.showCpuFan ? 1 : 0) != 0;
    cfg.showGpuFan      = ReadIniInt("Display", "showGpuFan", cfg.showGpuFan ? 1 : 0) != 0;
    cfg.showSystemFan   = ReadIniInt("Display", "showSystemFan", cfg.showSystemFan ? 1 : 0) != 0;
    cfg.showGpuMemTemp  = ReadIniInt("Display", "showGpuMemTemp", cfg.showGpuMemTemp ? 1 : 0) != 0;
    cfg.showDiskTemp    = ReadIniInt("Display", "showDiskTemp", cfg.showDiskTemp ? 1 : 0) != 0;
    cfg.showMemoryTemp  = ReadIniInt("Display", "showMemoryTemp", cfg.showMemoryTemp ? 1 : 0) != 0;
    cfg.showCpuFreq     = ReadIniInt("Frequency", "showCpuFreq", cfg.showCpuFreq ? 1 : 0) != 0;
    cfg.showGpuCoreFreq = ReadIniInt("Frequency", "showGpuCoreFreq", cfg.showGpuCoreFreq ? 1 : 0) != 0;
    ReadIniStr("Frequency", "cpuFreqPath", cfg.cpuFreqPath, sizeof(cfg.cpuFreqPath));
    ReadIniStr("Frequency", "gpuCoreFreqPath", cfg.gpuCoreFreqPath, sizeof(cfg.gpuCoreFreqPath));
    ReadIniStr("PowerSensors", "acInputPath", cfg.acInputPowerPath, sizeof(cfg.acInputPowerPath));
    ReadIniStr("PowerSensors", "totalSystemPath", cfg.totalSystemPowerPath, sizeof(cfg.totalSystemPowerPath));
    
    // Layout settings (migrate old horizontal=1 -> layoutStyle=1)
    {
        int ls = ReadIniInt("Layout", "layoutStyle", -1);
        if (ls < 0)
            ls = ReadIniInt("Layout", "horizontal", 0) ? LAYOUT_HORIZONTAL : LAYOUT_VERTICAL;
        if (ls < LAYOUT_VERTICAL || ls > LAYOUT_GAMEPP)
            ls = LAYOUT_VERTICAL;
        cfg.layoutStyle = ls;
    }
    cfg.useFahrenheit = ReadIniInt("Layout", "useFahrenheit", 0) != 0;
    cfg.autoStart     = ReadIniInt("Layout", "autoStart", 1) != 0;
    {
        const int configured = ReadIniInt("App", "startWithWindows", -1);
        if (configured >= 0) {
            cfg.startWithWindows = configured != 0;
        } else {
            // Previous versions created the task without a visible switch.
            // Preserve that state during migration when the old path marker exists.
            char legacyTaskPath[MAX_PATH] = {};
            ReadIniStr("App", "AutoLaunchTaskPath", legacyTaskPath, sizeof(legacyTaskPath));
            cfg.startWithWindows = legacyTaskPath[0] != '\0';
        }
    }
    {
        const int posVer = ReadIniInt("Layout", "positionVer", 0);
        int pos = ReadIniInt("Layout", "position", 0);
        if (posVer < 2) {
            // Legacy: TL=0, TR=1, BL=2, BR=3  ->  new POS_* grid
            static const int kLegacyToPos[] = { POS_TOP_LEFT, POS_TOP_RIGHT, POS_BOTTOM_LEFT, POS_BOTTOM_RIGHT };
            if (pos >= 0 && pos <= 3)
                pos = kLegacyToPos[pos];
            WriteIniInt("Layout", "positionVer", 2);
            WriteIniInt("Layout", "position", pos);
        }
        cfg.position = pos;
    }
    cfg.opacity       = ReadIniInt("Layout", "opacity", 85);
    cfg.customX       = ReadIniFloat("Layout", "customX", -1.0f);
    cfg.customY       = ReadIniFloat("Layout", "customY", -1.0f);
    {
        int sc = ReadIniInt("Layout", "overlayScale", -1);
        if (sc < 0)
            sc = ReadIniInt("Layout", "steamBarScale", 100);
        cfg.overlayScale = sc;
    }
    cfg.gameppFontPreset = ReadIniInt("Layout", "gameppFontPreset", 2);
    cfg.refreshMs        = ReadIniInt("Layout", "refreshMs", 1000);
    cfg.monitorSnap      = ReadIniInt("Layout", "monitorSnap", 1) != 0;
    cfg.desktopOnlyMode  = ReadIniInt("Layout", "desktopOnlyMode", 0) != 0;
    
    // Hotkeys
    cfg.toggleKey     = ReadIniInt("Hotkeys", "toggleKey", VK_INSERT);
    cfg.exitKey       = ReadIniInt("Hotkeys", "exitKey", VK_END);
    cfg.layoutSwitchKey = ReadIniInt("Hotkeys", "layoutSwitchKey", VK_F9);
    
    // GPU selection
    cfg.selectedGpu   = ReadIniInt("GPU", "selectedGpu", 0);
    
    // Clamp values to valid ranges
    if (cfg.position < POS_TOP_LEFT || cfg.position > POS_BOTTOM_RIGHT) cfg.position = POS_TOP_LEFT;
    if (cfg.opacity < 30) cfg.opacity = 30;
    if (cfg.opacity > 100) cfg.opacity = 100;
    if (cfg.selectedGpu < 0) cfg.selectedGpu = 0;
    if (cfg.layoutStyle < LAYOUT_VERTICAL || cfg.layoutStyle > LAYOUT_GAMEPP)
        cfg.layoutStyle = LAYOUT_VERTICAL;
    if (cfg.overlayScale < 50) cfg.overlayScale = 50;
    if (cfg.overlayScale > 200) cfg.overlayScale = 200;
    if (cfg.gameppFontPreset < 0) cfg.gameppFontPreset = 0;
    if (cfg.gameppFontPreset > 4) cfg.gameppFontPreset = 4;
    if (cfg.refreshMs < 250) cfg.refreshMs = 250;
    if (cfg.refreshMs > 5000) cfg.refreshMs = 5000;
    if (cfg.timeFormat != TIME_FORMAT_24H && cfg.timeFormat != TIME_FORMAT_12H)
        cfg.timeFormat = TIME_FORMAT_24H;
    cfg.fpsWarningThreshold = std::clamp(cfg.fpsWarningThreshold, 15, 239);
    cfg.fpsGoodThreshold = std::clamp(cfg.fpsGoodThreshold,
                                      cfg.fpsWarningThreshold + 1, 240);

    FeatureSettings featureSettings;
    featureSettings.temperatureAlertEnabled = ReadIniInt("Features", "feature.temperature_alert", 1) != 0;
    featureSettings.lowFpsAlertEnabled = ReadIniInt("Features", "feature.low_fps_alert", 1) != 0;
    featureSettings.gameAutoOverlayEnabled = ReadIniInt("Features", "feature.game_auto_overlay", 0) != 0;
    featureSettings.gamePeakStatsEnabled = ReadIniInt("Features", "feature.game_peak_stats", 1) != 0;
    featureSettings.laptopPowerEnabled = ReadIniInt("Features", "feature.laptop_power", 1) != 0;
    featureSettings.temperatureThresholdC = ReadIniInt("Features", "feature.temperature_alert.threshold_c", 85);
    featureSettings.lowFpsThreshold = ReadIniInt("Features", "feature.low_fps_alert.threshold", 45);
    featureSettings.lowFpsSeconds = ReadIniFloat("Features", "feature.low_fps_alert.seconds", 3.0f);
    featureSettings.laptopPowerShowOverlay = ReadIniInt("Features", "feature.laptop_power.show_overlay", 1) != 0;
    featureSettings.laptopPowerShowDcInputPower = ReadIniInt("Features", "laptop_power.show_dc_input_power", 1) != 0;
    featureSettings.laptopPowerShowSystemPower = ReadIniInt("Features", "laptop_power.show_system_power", 1) != 0;
    featureSettings.laptopPowerShowKnownComponentsPower = ReadIniInt("Features", "laptop_power.show_known_components_power", 1) != 0;
    featureSettings.laptopPowerShowBatteryDischargePower = ReadIniInt("Features", "laptop_power.show_battery_discharge_power", 1) != 0;
    featureSettings.laptopPowerShowBatteryStatus = ReadIniInt("Features", "laptop_power.show_battery_status", 0) != 0;
    featureSettings.laptopPowerOnlyShowPowerItemsInGame =
        ReadIniInt("Features", "laptop_power.only_show_power_items_in_game", 0) != 0;
    featureSettings.laptopPowerNonGameShowSystemPower =
        ReadIniInt("Features", "laptop_power.non_game_show_system_power", 0) != 0;
    featureSettings.laptopPowerNonGameShowKnownComponentsPower =
        ReadIniInt("Features", "laptop_power.non_game_show_known_components_power", 0) != 0;
    featureSettings.laptopPowerNonGameShowBatteryDischargePower =
        ReadIniInt("Features", "laptop_power.non_game_show_battery_discharge_power", 0) != 0;
    featureSettings.laptopPowerAllowEstimatedSystemPower = ReadIniInt("Features", "laptop_power.allow_estimated_system_power", 1) != 0;
    featureSettings.laptopPowerShowEstimated = ReadIniInt("Features", "feature.laptop_power.show_estimated", 1) != 0;
    featureSettings.laptopPowerShowSource = ReadIniInt("Features", "feature.laptop_power.show_source", 1) != 0;
    featureSettings.laptopPowerSamplingMs = ReadIniInt("Features", "feature.laptop_power.sampling_ms", 2000);
    featureSettings.laptopPowerSmoothSeconds = ReadIniInt("Features", "feature.laptop_power.smooth_seconds", 5);
    featureSettings.laptopPowerWarningThresholdW = ReadIniInt("Features", "feature.laptop_power.warning_threshold_w", 100);
    featureSettings.laptopPowerAdapterWarningW = ReadIniInt("Features", "feature.laptop_power.adapter_warning_w", 65);
    featureSettings.laptopPowerDisplayMode = ReadIniInt("Features", "feature.laptop_power.display_mode", 1);
    featureSettings.laptopPowerBatteryAssistAlertEnabled =
        ReadIniInt("Features", "feature.laptop_power.battery_assist_alert", 1) != 0;
    featureSettings.laptopPowerBatteryAssistThresholdW =
        ReadIniInt("Features", "feature.laptop_power.battery_assist_threshold_w", 3);
    featureSettings.laptopPowerBatteryAssistSeconds =
        ReadIniInt("Features", "feature.laptop_power.battery_assist_seconds", 10);
    g_FeatureRegistry.LoadSettings(featureSettings);
    g_trayShowSystemPower.store(featureSettings.laptopPowerShowSystemPower, std::memory_order_relaxed);
    g_trayShowKnownPower.store(featureSettings.laptopPowerShowKnownComponentsPower, std::memory_order_relaxed);
    g_trayShowBatteryPower.store(featureSettings.laptopPowerShowBatteryDischargePower, std::memory_order_relaxed);
    g_trayAllowEstimatedPower.store(featureSettings.laptopPowerAllowEstimatedSystemPower, std::memory_order_relaxed);
}

// Check if welcome message has been shown (separate from config)
static bool HasWelcomeBeenShown()
{
    InitConfigPath();
    return ReadIniInt("App", "welcomeShown", 0) != 0;
}

static void MarkWelcomeShown()
{
    InitConfigPath();
    WriteIniInt("App", "welcomeShown", 1);
}

// Show welcome message on first run
[[maybe_unused]] static void ShowWelcomeMessage()
{
    MarkWelcomeShown();
    return;

    if (HasWelcomeBeenShown()) {
        return;  // Already shown before
    }
    
    MessageBoxW(
        nullptr,
        L"欢迎使用 FPS Overlay！\n\n"
        L"为了获得更稳定的体验，建议先关闭其他 FPS 覆盖层：\n\n"
        L"  - Steam 覆盖层（Steam > 设置 > 游戏中）\n"
        L"  - Xbox Game Bar（Windows 设置 > 游戏）\n"
        L"  - NVIDIA GeForce Experience / ShadowPlay / NVIDIA App 覆盖层\n"
        L"  - AMD Radeon Software 覆盖层\n"
        L"  - Discord 覆盖层\n\n"
        L"这样可以减少冲突，并让 FPS 读数更准确。\n\n"
        L"祝你游戏愉快！",
        L"FPS Overlay",
        MB_OK | MB_ICONINFORMATION | MB_TOPMOST
    );
    
    MarkWelcomeShown();
}

static bool SaveConfig(const OverlayConfig& cfg)
{
    std::lock_guard<std::mutex> configLock(g_configIoMutex);
    InitConfigPath();

    int pawnioRb = ReadIniInt("App", "PawnIORequiresReboot", 0);
    char pawnioHex[48] = {};
    ReadIniStr("App", "PawnIOInstallUtcHex", pawnioHex, sizeof(pawnioHex));
    char sidecarHex[48] = {};
    if (ReadPawnIORebootPendingStateFile(sidecarHex, sizeof(sidecarHex))) {
        pawnioRb = 1;
        snprintf(pawnioHex, sizeof(pawnioHex), "%s", sidecarHex);
    }

    char finalConfigPath[MAX_PATH] = {};
    char tempConfigPath[MAX_PATH] = {};
    snprintf(finalConfigPath, sizeof(finalConfigPath), "%s", g_configPath);
    snprintf(tempConfigPath, sizeof(tempConfigPath), "%s.tmp", finalConfigPath);
    DeleteFileA(tempConfigPath);
    CopyFileA(finalConfigPath, tempConfigPath, FALSE);
    g_iniWritePath = tempConfigPath;

    WriteIniInt("App", "schema_version", CONFIG_SCHEMA_VERSION);
    WriteIniStr("App", "app_version", APP_VERSION);
    WriteIniInt("App", "startWithWindows", cfg.startWithWindows ? 1 : 0);

    // Display settings
    WriteIniInt("Display", "showFPS", cfg.showFPS ? 1 : 0);
    WriteIniInt("Display", "useAverageFPS", cfg.useAverageFPS ? 1 : 0);
    WriteIniInt("Display", "customFpsColors", cfg.customFpsColors ? 1 : 0);
    WriteIniInt("Display", "fpsWarningThreshold", cfg.fpsWarningThreshold);
    WriteIniInt("Display", "fpsGoodThreshold", cfg.fpsGoodThreshold);
    WriteIniInt("Display", "showCpuUsage", cfg.showCpuUsage ? 1 : 0);
    WriteIniInt("Display", "showCpuTemp", cfg.showCpuTemp ? 1 : 0);
    WriteIniInt("Display", "showGpuUsage", cfg.showGpuUsage ? 1 : 0);
    WriteIniInt("Display", "showGpuTemp", cfg.showGpuTemp ? 1 : 0);
    // Legacy combined flags (older builds / hand-edited inis)
    WriteIniInt("Display", "showCPU", (cfg.showCpuUsage || cfg.showCpuTemp) ? 1 : 0);
    WriteIniInt("Display", "showGPU", (cfg.showGpuUsage || cfg.showGpuTemp) ? 1 : 0);
    WriteIniInt("Display", "showVRAMPercent", cfg.showVRAMPercent ? 1 : 0);
    WriteIniInt("Display", "showVRAMUsage", cfg.showVRAMUsage ? 1 : 0);
    WriteIniInt("Display", "showRAMPercent", cfg.showRAMPercent ? 1 : 0);
    WriteIniInt("Display", "showRAMUsage", cfg.showRAMUsage ? 1 : 0);
    WriteIniInt("Display", "showNetworkDownload", cfg.showNetworkDownload ? 1 : 0);
    WriteIniInt("Display", "showNetworkUpload", cfg.showNetworkUpload ? 1 : 0);
    WriteIniInt("Display", "showVRAM", (cfg.showVRAMPercent || cfg.showVRAMUsage) ? 1 : 0);
    WriteIniInt("Display", "showRAM", (cfg.showRAMPercent || cfg.showRAMUsage) ? 1 : 0);
    WriteIniInt("Display", "showProcessName", cfg.showProcessName ? 1 : 0);
    WriteIniInt("Display", "showTime", cfg.showTime ? 1 : 0);
    WriteIniInt("Display", "timeFormat", cfg.timeFormat);
    WriteIniInt("Display", "timeShowSeconds", cfg.timeShowSeconds ? 1 : 0);
    WriteIniInt("Display", "showCpuPower", cfg.showCpuPower ? 1 : 0);
    WriteIniInt("Display", "showGpuPower", cfg.showGpuPower ? 1 : 0);
    WriteIniInt("Display", "showCpuFan", cfg.showCpuFan ? 1 : 0);
    WriteIniInt("Display", "showGpuFan", cfg.showGpuFan ? 1 : 0);
    WriteIniInt("Display", "showSystemFan", cfg.showSystemFan ? 1 : 0);
    WriteIniInt("Display", "showGpuMemTemp", cfg.showGpuMemTemp ? 1 : 0);
    WriteIniInt("Display", "showDiskTemp", cfg.showDiskTemp ? 1 : 0);
    WriteIniInt("Display", "showMemoryTemp", cfg.showMemoryTemp ? 1 : 0);

    WriteIniInt("Frequency", "showCpuFreq", cfg.showCpuFreq ? 1 : 0);
    WriteIniInt("Frequency", "showGpuCoreFreq", cfg.showGpuCoreFreq ? 1 : 0);
    WriteIniStr("Frequency", "cpuFreqPath", cfg.cpuFreqPath);
    WriteIniStr("Frequency", "gpuCoreFreqPath", cfg.gpuCoreFreqPath);
    WriteIniStr("PowerSensors", "acInputPath", cfg.acInputPowerPath);
    WriteIniStr("PowerSensors", "totalSystemPath", cfg.totalSystemPowerPath);
    
    // Layout settings
    WriteIniInt("Layout", "layoutStyle", cfg.layoutStyle);
    WriteIniInt("Layout", "horizontal", cfg.layoutStyle == LAYOUT_HORIZONTAL ? 1 : 0);
    WriteIniInt("Layout", "useFahrenheit", cfg.useFahrenheit ? 1 : 0);
    WriteIniInt("Layout", "autoStart", cfg.autoStart ? 1 : 0);
    WriteIniInt("Layout", "position", cfg.position);
    WriteIniInt("Layout", "positionVer", 2);
    WriteIniInt("Layout", "opacity", cfg.opacity);
    WriteIniFloat("Layout", "customX", cfg.customX);
    WriteIniFloat("Layout", "customY", cfg.customY);
    WriteIniInt("Layout", "overlayScale", cfg.overlayScale);
    WriteIniInt("Layout", "gameppFontPreset", cfg.gameppFontPreset);
    WriteIniInt("Layout", "refreshMs", cfg.refreshMs);
    WriteIniInt("Layout", "monitorSnap", cfg.monitorSnap ? 1 : 0);
    WriteIniInt("Layout", "desktopOnlyMode", cfg.desktopOnlyMode ? 1 : 0);
    
    // Hotkeys
    WriteIniInt("Hotkeys", "toggleKey", cfg.toggleKey);
    WriteIniInt("Hotkeys", "exitKey", cfg.exitKey);
    WriteIniInt("Hotkeys", "layoutSwitchKey", cfg.layoutSwitchKey);
    
    // GPU selection
    WriteIniInt("GPU", "selectedGpu", cfg.selectedGpu);

    FeatureSettings featureSettings = g_FeatureRegistry.GetSettings();
    g_trayShowSystemPower.store(featureSettings.laptopPowerShowSystemPower, std::memory_order_relaxed);
    g_trayShowKnownPower.store(featureSettings.laptopPowerShowKnownComponentsPower, std::memory_order_relaxed);
    g_trayShowBatteryPower.store(featureSettings.laptopPowerShowBatteryDischargePower, std::memory_order_relaxed);
    g_trayAllowEstimatedPower.store(featureSettings.laptopPowerAllowEstimatedSystemPower, std::memory_order_relaxed);
    WriteIniInt("Features", "feature.temperature_alert", featureSettings.temperatureAlertEnabled ? 1 : 0);
    WriteIniInt("Features", "feature.low_fps_alert", featureSettings.lowFpsAlertEnabled ? 1 : 0);
    WriteIniInt("Features", "feature.game_auto_overlay", featureSettings.gameAutoOverlayEnabled ? 1 : 0);
    WriteIniInt("Features", "feature.game_peak_stats", featureSettings.gamePeakStatsEnabled ? 1 : 0);
    WriteIniInt("Features", "feature.laptop_power", featureSettings.laptopPowerEnabled ? 1 : 0);
    WriteIniInt("Features", "feature.temperature_alert.threshold_c", featureSettings.temperatureThresholdC);
    WriteIniInt("Features", "feature.low_fps_alert.threshold", featureSettings.lowFpsThreshold);
    WriteIniFloat("Features", "feature.low_fps_alert.seconds", featureSettings.lowFpsSeconds);
    WriteIniInt("Features", "feature.laptop_power.show_overlay", featureSettings.laptopPowerShowOverlay ? 1 : 0);
    WriteIniInt("Features", "laptop_power.show_dc_input_power", featureSettings.laptopPowerShowDcInputPower ? 1 : 0);
    WriteIniInt("Features", "laptop_power.show_system_power", featureSettings.laptopPowerShowSystemPower ? 1 : 0);
    WriteIniInt("Features", "laptop_power.show_known_components_power", featureSettings.laptopPowerShowKnownComponentsPower ? 1 : 0);
    WriteIniInt("Features", "laptop_power.show_battery_discharge_power", featureSettings.laptopPowerShowBatteryDischargePower ? 1 : 0);
    WriteIniInt("Features", "laptop_power.show_battery_status", featureSettings.laptopPowerShowBatteryStatus ? 1 : 0);
    WriteIniInt("Features", "laptop_power.only_show_power_items_in_game",
                featureSettings.laptopPowerOnlyShowPowerItemsInGame ? 1 : 0);
    WriteIniInt("Features", "laptop_power.non_game_show_system_power",
                featureSettings.laptopPowerNonGameShowSystemPower ? 1 : 0);
    WriteIniInt("Features", "laptop_power.non_game_show_known_components_power",
                featureSettings.laptopPowerNonGameShowKnownComponentsPower ? 1 : 0);
    WriteIniInt("Features", "laptop_power.non_game_show_battery_discharge_power",
                featureSettings.laptopPowerNonGameShowBatteryDischargePower ? 1 : 0);
    WriteIniInt("Features", "laptop_power.allow_estimated_system_power", featureSettings.laptopPowerAllowEstimatedSystemPower ? 1 : 0);
    WriteIniInt("Features", "feature.laptop_power.show_estimated", featureSettings.laptopPowerShowEstimated ? 1 : 0);
    WriteIniInt("Features", "feature.laptop_power.show_source", featureSettings.laptopPowerShowSource ? 1 : 0);
    WriteIniInt("Features", "feature.laptop_power.sampling_ms", featureSettings.laptopPowerSamplingMs);
    WriteIniInt("Features", "feature.laptop_power.smooth_seconds", featureSettings.laptopPowerSmoothSeconds);
    WriteIniInt("Features", "feature.laptop_power.warning_threshold_w", featureSettings.laptopPowerWarningThresholdW);
    WriteIniInt("Features", "feature.laptop_power.adapter_warning_w", featureSettings.laptopPowerAdapterWarningW);
    WriteIniInt("Features", "feature.laptop_power.display_mode", featureSettings.laptopPowerDisplayMode);
    WriteIniInt("Features", "feature.laptop_power.battery_assist_alert",
                featureSettings.laptopPowerBatteryAssistAlertEnabled ? 1 : 0);
    WriteIniInt("Features", "feature.laptop_power.battery_assist_threshold_w",
                featureSettings.laptopPowerBatteryAssistThresholdW);
    WriteIniInt("Features", "feature.laptop_power.battery_assist_seconds",
                featureSettings.laptopPowerBatteryAssistSeconds);

    if (pawnioRb != 0) {
        WriteIniInt("App", "PawnIORequiresReboot", 1);
        if (pawnioHex[0] != '\0')
            WriteIniStr("App", "PawnIOInstallUtcHex", pawnioHex);
    }

    // Flush the profile cache before replacing the live file. Some Windows
    // versions return FALSE here even though the temporary file was written,
    // so the replace operation is the authoritative success check.
    WritePrivateProfileStringA(nullptr, nullptr, nullptr, tempConfigPath);
    g_iniWritePath = nullptr;
    char validationReason[128] = {};
    if (!ConfigFileLooksUsable(tempConfigPath, validationReason,
                               sizeof(validationReason))) {
        DeleteFileA(tempConfigPath);
        LogLine("Temporary config validation failed: reason=%s",
                validationReason[0] ? validationReason : "unknown");
        g_ConfigDirty = true;
        g_ConfigDirtyTick = GetTickCount64();
        return false;
    }
    if (!MoveFileExA(tempConfigPath, finalConfigPath,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = GetLastError();
        DeleteFileA(tempConfigPath);
        LogLine("Atomic config save failed: err=%lu", (unsigned long)error);
        g_ConfigDirty = true;
        g_ConfigDirtyTick = GetTickCount64();
        return false;
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// App state
// ═══════════════════════════════════════════════════════════════════════════
enum AppMode    { MODE_CONFIG, MODE_OVERLAY };
enum PendingCmd { CMD_NONE, CMD_START_OVERLAY, CMD_SHOW_SETTINGS, CMD_EXIT };

static OverlayConfig g_Config;
static AppMode       g_Mode       = MODE_CONFIG;
static PendingCmd    g_Pending    = CMD_NONE;
static bool          g_Running    = true;
static bool          g_OvlVisible = true;
static bool          g_FullscreenAutoHidden = false;
enum DesktopVisibilityState {
    DESKTOP_VISIBILITY_DISABLED,
    DESKTOP_VISIBILITY_DESKTOP,
    DESKTOP_VISIBILITY_EVALUATING,
    DESKTOP_VISIBILITY_GAME,
    DESKTOP_VISIBILITY_APP_HIDDEN
};
static DesktopVisibilityState g_DesktopVisibilityState = DESKTOP_VISIBILITY_DISABLED;
static bool          g_ForegroundGameConfirmed = false;
static DWORD         g_RecentGamePid = 0;
static ULONGLONG     g_RecentGameSeenTick = 0;
static char          g_RecentGameExe[320] = "";
static bool          g_GameOverlayDisplayActive = false;
static DWORD         g_GameOverlayDisplayPid = 0;
static char          g_GameOverlayDisplayReason[96] = "";
static RECT          g_OverlayHostRect = {};
static bool          g_OverlayUsingGameMonitor = false;
static HWND          g_OverlayOwnerWindow = nullptr;
static bool          g_OverlayHostRecoveryPending = false;
static ULONGLONG     g_OverlayHostRecoveryRetryTick = 0;
static bool          g_PowerResumePending = false;
enum class PowerRecoveryStatus : int {
    Idle,
    Recovering,
    Recovered,
    Failed,
};
static std::atomic<PowerRecoveryStatus> g_PowerRecoveryStatus{PowerRecoveryStatus::Idle};
static ULONGLONG     g_PowerRecoveryStartedTick = 0;
static ULONGLONG     g_PowerRecoveryStatusTick = 0;
static bool          g_D3DRecoveryPending = false;
static ULONGLONG     g_D3DRecoveryRetryTick = 0;
static HRESULT       g_LastD3DError = S_OK;
static bool          g_ImGuiWin32Initialized = false;
static bool          g_ImGuiDx11Initialized = false;
static UINT          g_TaskbarCreatedMessage = 0;
static DWORD         g_DesktopForegroundPid = 0;
static char          g_DesktopForegroundExe[320] = "";
static char          g_DesktopVisibilityReason[160] = "disabled";
static float         g_dpiScale   = 1.0f;
static bool          g_ShowLiveSettings = false;
static ULONGLONG     g_SettingsRequestTick = 0;
static bool          g_SettingsFirstFramePending = false;

struct SettingsOpenTrace {
    bool active = false;
    const char* source = "unknown";
    LONGLONG startUs = 0;
    LONGLONG foregroundMenuStartUs = 0;
    LONGLONG foregroundMenuEndUs = 0;
    LONGLONG popupStartUs = 0;
    LONGLONG popupEndUs = 0;
    LONGLONG queuedUs = 0;
    LONGLONG commandStartUs = 0;
    LONGLONG clickThroughStartUs = 0;
    LONGLONG clickThroughEndUs = 0;
    LONGLONG foregroundSettingsStartUs = 0;
    LONGLONG foregroundSettingsEndUs = 0;
    LONGLONG commandEndUs = 0;
    LONGLONG newFrameStartUs = 0;
    LONGLONG newFrameEndUs = 0;
    LONGLONG settingsBeginStartUs = 0;
    LONGLONG settingsBeginEndUs = 0;
    LONGLONG imguiRenderStartUs = 0;
    LONGLONG imguiRenderEndUs = 0;
    LONGLONG drawSubmitEndUs = 0;
    LONGLONG presentStartUs = 0;
    LONGLONG presentEndUs = 0;
};
static SettingsOpenTrace g_SettingsTrace;

static LONGLONG PerfNowUs()
{
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return value;
    }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return (now.QuadPart * 1000000LL) / freq.QuadPart;
}

static double TraceMs(LONGLONG fromUs, LONGLONG toUs)
{
    if (fromUs == 0 || toUs == 0 || toUs < fromUs) return -1.0;
    return (double)(toUs - fromUs) / 1000.0;
}

static void StartSettingsTrace(const char* source)
{
    g_SettingsTrace = {};
    g_SettingsTrace.active = true;
    g_SettingsTrace.source = source;
    g_SettingsTrace.startUs = PerfNowUs();
}

static HINSTANCE      g_hInstance = nullptr;
static HWND           g_hwnd     = nullptr;
static std::atomic<HWND> g_trayHwnd{nullptr};
static NOTIFYICONDATAW g_nid     = {};
static std::thread     g_trayThread;
static HANDLE          g_trayReadyEvent = nullptr;
static bool            g_trayIconPublished = false;
static unsigned int    g_trayRestoreRetryIndex = 0;
static ULONGLONG       g_trayLastRestoreTick = 0;
static constexpr UINT_PTR kTrayRestoreTimerId = 0x4650;
static std::atomic<bool> g_trayOverlayVisible{true};
static RECT           g_overlayBounds = {0, 0, 0, 0};  // ImGui overlay bounds for hit-testing
static RECT           g_liveSettingsBounds = {0, 0, 0, 0}; // Live settings window bounds for hit-testing
static bool           g_isDragging = false;            // True when user is dragging the overlay
static bool           g_overlayForceCornerSnap = false; // one shot: snap to corner preset (after Reset Position)

// ── Hardware info ──
static char g_cpuName[256] = "Unknown";
static char g_gpuName[256] = "Unknown";

// ── GPU stats (from LHWM) ──
static float g_gpuUsage = 0.0f;
static float g_gpuTemp  = 0.0f;
static float g_vramUsed  = 0.0f;  // in GB
static float g_vramTotal = 0.0f;  // in GB

// ── ETW state ──
static TRACEHANDLE      g_etwSession = 0;
static TRACEHANDLE      g_etwTrace   = 0;
static std::thread      g_etwThread;
static std::atomic<bool>  g_etwRunning{false};
static std::atomic<float> g_gameFps{0.0f};
static std::atomic<float> g_autoGameFps{0.0f};
static std::atomic<DWORD> g_targetPid{0};
static std::atomic<DWORD> g_autoTargetPid{0};
static DWORD              g_lastTargetPid = 0;    // to detect PID change
static bool               g_etwAvailable = false;
static bool               g_isAdmin = false;      // running as administrator?
static double              g_qpcFreq     = 1.0;
static char               g_targetProcessName[768] = "";  // UTF-8: tracked process (exe + description)

// ── CPU temperature (WMI) ──
static float g_cpuTemp = 0.0f;
static bool  g_cpuTempAvailable = false;

// ── LibreHardwareMonitor (LHWM) state ──
static std::atomic<bool> g_lhwmAvailable{false};
static std::string g_lhwmCpuTempPath;      // e.g., "/amdcpu/0/temperature/3"
static std::string g_lhwmGpuTempPath;      // e.g., "/gpu-nvidia/0/temperature/0"
static std::string g_lhwmGpuLoadPath;      // e.g., "/gpu-nvidia/0/load/0"
static std::string g_lhwmGpuVramUsedPath;  // VRAM used
static std::string g_lhwmGpuVramTotalPath; // VRAM total
static std::string g_lhwmCpuPowerPath;     // CPU package power sensor
static std::string g_lhwmCpuMemoryPowerPath;
static std::string g_lhwmCpuPlatformPowerPath;
static std::string g_lhwmGpuPowerPath;     // Active GPU power sensor
static std::string g_lhwmDiscreteGpuPowerPath;
static std::string g_lhwmAcInputPowerPath; // AC adapter/input power
static std::string g_lhwmAutoAcInputPowerPath;
static std::string g_lhwmBatteryChargePowerPath;
static std::string g_lhwmBatteryDischargePowerPath;
static std::string g_lhwmTotalSystemPowerPath;
static std::string g_lhwmAutoTotalSystemPowerPath;
static std::string g_lhwmCpuFanPath;       // CPU fan sensor (from any hardware node)
static std::string g_lhwmGpuFanPath;       // Active GPU fan sensor
static std::string g_lhwmGpuMemTempPath;   // GPU memory junction temperature
static std::vector<std::string> g_lhwmDiskTempPaths;
static std::vector<std::string> g_lhwmMemoryTempPaths;
static std::vector<std::string> g_lhwmDiskActivityPaths;
static std::vector<std::string> g_lhwmNetworkLoadPaths;
static std::atomic<bool> g_lhwmCpuFanIsPercent{false};
static std::atomic<bool> g_lhwmGpuFanIsPercent{false};
static float g_lhwmCpuTemp = 0.0f;         // CPU temp from LHWM (used directly)
static std::atomic<bool> g_comparisonPowerPollInFlight{false};
static std::atomic<float> g_comparisonCpuPower{0.0f};
static std::atomic<float> g_comparisonCpuMemoryPower{0.0f};
static std::atomic<float> g_comparisonCpuPlatformPower{0.0f};
static std::atomic<float> g_comparisonGpuPower{0.0f};
static std::atomic<float> g_comparisonDiscreteGpuPower{0.0f};
static std::atomic<float> g_comparisonCpuFanRpm{0.0f};
static std::atomic<float> g_comparisonGpuFanRpm{0.0f};
static std::atomic<unsigned long long> g_comparisonPowerSampleTick{0};
static std::atomic<unsigned long long> g_comparisonPowerSampleSequence{0};
static float g_cpuPower = 0.0f;            // CPU power draw in Watts
static float g_cpuMemoryPower = 0.0f;
static float g_cpuPlatformPower = 0.0f;
static float g_gpuPower = 0.0f;            // GPU power draw in Watts
static float g_discreteGpuPower = 0.0f;
static float g_acInputPower = 0.0f;         // AC adapter/input power in Watts
static float g_batteryChargePower = 0.0f;   // Battery charge rate in Watts
static float g_batteryDischargePower = 0.0f;// Battery discharge rate in Watts
static float g_totalSystemPower = 0.0f;     // Total/platform power in Watts
static std::string g_totalSystemPowerSourceName;
static std::string g_acInputPowerSourceName;
static bool g_aidaPowerActive = false;
static float g_windowsRaplPackagePower = 0.0f;
static PDH_HQUERY g_powerCounterQuery = nullptr;
static PDH_HCOUNTER g_windowsSystemPowerCounter = nullptr;
static PDH_HCOUNTER g_windowsRaplPackageCounter = nullptr;
static bool g_powerCountersReady = false;
static bool g_loggedWindowsSystemPower = false;
static bool g_loggedWindowsRaplPower = false;
static float g_cpuFanRpm = 0.0f;           // CPU fan speed in RPM
static float g_gpuFanRpm = 0.0f;           // GPU fan speed in RPM
static float g_systemFanRpm = 0.0f;        // ASUS middle/system fan speed in RPM
static float g_gpuMemTemp = 0.0f;          // GPU memory temperature in Celsius
static float g_diskTemp = 0.0f;            // Max SSD/NVMe temperature in Celsius
static float g_memoryTemp = 0.0f;          // Max DIMM temperature in Celsius
static float g_diskActivityPercent = 0.0f;
static float g_networkUtilizationPercent = 0.0f;

struct PowerSensorCandidate {
    std::string hardwareName;
    std::string sensorName;
    std::string path;
};
static std::vector<PowerSensorCandidate> g_powerSensorCandidates;

struct LhwmStatsSnapshot {
    bool valid = false;
    float lhwmCpuTemp = 0.0f;
    float gpuTemp = 0.0f;
    float gpuUsage = 0.0f;
    float vramUsed = 0.0f;
    float vramTotal = 0.0f;
    float cpuPower = 0.0f;
    float cpuMemoryPower = 0.0f;
    float cpuPlatformPower = 0.0f;
    float gpuPower = 0.0f;
    float discreteGpuPower = 0.0f;
    float acInputPower = 0.0f;
    float batteryChargePower = 0.0f;
    float batteryDischargePower = 0.0f;
    float totalSystemPower = 0.0f;
    float cpuFanRpm = 0.0f;
    float gpuFanRpm = 0.0f;
    float systemFanRpm = 0.0f;
    float gpuMemTemp = 0.0f;
    float diskTemp = 0.0f;
    float memoryTemp = 0.0f;
    float diskActivityPercent = 0.0f;
    float networkUtilizationPercent = 0.0f;
    std::string acInputPowerSourceName;
    std::string totalSystemPowerSourceName;
};
static std::mutex g_asyncLhwmStatsMutex;
static LhwmStatsSnapshot g_asyncLhwmStatsSnapshot;
static std::atomic<bool> g_asyncLhwmPollInFlight{false};
static std::atomic<unsigned long long> g_asyncLhwmStatsSequence{0};
static std::atomic<unsigned long long> g_asyncLhwmStatsSampleTick{0};

// LibreHardwareMonitor ultimately calls vendor libraries such as NVIDIA NVML.
// They must not be entered concurrently, or while a GPU is transitioning through
// sleep/resume.  The worker polls remain asynchronous to the render loop, but all
// vendor calls are serialized here.
static std::mutex g_lhwmCallMutex;
static std::atomic<bool> g_lhwmReadsPaused{false};
static std::atomic<ULONGLONG> g_lhwmResumeReadyTick{0};
static std::atomic<bool> g_lhwmNativeFaulted{false};
static std::atomic<DWORD> g_lhwmNativeFaultCode{0};
static std::atomic<bool> g_lhwmRescanRequested{false};
static std::thread g_lhwmInitThread;
static std::thread g_asyncLhwmPollThread;
static std::thread g_comparisonPowerPollThread;
static constexpr ULONGLONG kLhwmResumeCooldownMs = 15000;

// CPU / GPU core clock options and live values (MHz) + sparkline history
static std::vector<std::pair<std::string, std::string>> g_cpuClockOpts;
static float g_cpuClockMHz = 0.f;
static float g_gpuCoreClockMHz = 0.f;
static float g_cpuSpark[FREQ_SPARK_LEN];
static int   g_cpuSparkN = 0;
static float g_gpuSpark[FREQ_SPARK_LEN];
static int   g_gpuSparkN = 0;

static float QueryCpuClockMHzFallback();
static bool ValidLaptopPowerValue(float value);

static void InitWindowsPowerCounters()
{
    if (g_powerCountersReady)
        return;
    g_powerCountersReady = true;

    if (PdhOpenQueryW(nullptr, 0, &g_powerCounterQuery) != ERROR_SUCCESS)
        return;

    if (PdhAddEnglishCounterW(g_powerCounterQuery,
                              L"\\Power Meter(Power Meter (0))\\Power",
                              0, &g_windowsSystemPowerCounter) != ERROR_SUCCESS) {
        g_windowsSystemPowerCounter = nullptr;
    }
    if (PdhAddEnglishCounterW(g_powerCounterQuery,
                              L"\\Energy Meter(RAPL_Package0_PKG)\\Power",
                              0, &g_windowsRaplPackageCounter) != ERROR_SUCCESS) {
        g_windowsRaplPackageCounter = nullptr;
    }

    if (!g_windowsSystemPowerCounter && !g_windowsRaplPackageCounter) {
        PdhCloseQuery(g_powerCounterQuery);
        g_powerCounterQuery = nullptr;
        return;
    }

    // Rate counters need a baseline sample before they return a formatted value.
    PdhCollectQueryData(g_powerCounterQuery);
}

static bool ReadPdhPowerCounter(PDH_HCOUNTER counter, float divisor, float* wattsOut)
{
    if (!counter || !wattsOut)
        return false;
    PDH_FMT_COUNTERVALUE value = {};
    DWORD type = 0;
    if (PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, &type, &value) != ERROR_SUCCESS ||
        value.CStatus != ERROR_SUCCESS || !std::isfinite(value.doubleValue)) {
        return false;
    }
    const float watts = static_cast<float>(value.doubleValue / divisor);
    if (!ValidLaptopPowerValue(watts))
        return false;
    *wattsOut = watts;
    return true;
}

static void PollWindowsPowerCounters()
{
    InitWindowsPowerCounters();
    if (!g_powerCounterQuery || PdhCollectQueryData(g_powerCounterQuery) != ERROR_SUCCESS)
        return;

    float watts = 0.0f;
    // Windows Power Meter reports watts. On this G815LR the counter is
    // registered but currently returns zero; keep the path active for BIOS or
    // driver revisions that expose the actual platform meter.
    if (ReadPdhPowerCounter(g_windowsSystemPowerCounter, 1.0f, &watts)) {
        g_totalSystemPower = watts;
        g_totalSystemPowerSourceName = "Windows Power Meter";
        if (!g_loggedWindowsSystemPower) {
            LogLine("Windows Power Meter active: %.1f W", watts);
            g_loggedWindowsSystemPower = true;
        }
    }

    // Energy Meter's RAPL Power counter is exposed in milliwatts.
    if (ReadPdhPowerCounter(g_windowsRaplPackageCounter, 1000.0f, &watts)) {
        g_windowsRaplPackagePower = watts;
        if (!ValidLaptopPowerValue(g_cpuPower))
            g_cpuPower = watts;
        if (!g_loggedWindowsRaplPower) {
            LogLine("Windows Energy Meter RAPL active: %.1f W", watts);
            g_loggedWindowsRaplPower = true;
        }
    }
}

static void ShutdownWindowsPowerCounters()
{
    if (g_powerCounterQuery)
        PdhCloseQuery(g_powerCounterQuery);
    g_powerCounterQuery = nullptr;
    g_windowsSystemPowerCounter = nullptr;
    g_windowsRaplPackageCounter = nullptr;
}

static bool FreqPathValid(const char* p, const std::vector<std::pair<std::string, std::string>>& opts)
{
    if (!p || !p[0]) return false;
    for (const auto& e : opts)
        if (e.second == p) return true;
    return false;
}

static void ValidateFrequencyPaths()
{
    if (!FreqPathValid(g_Config.cpuFreqPath, g_cpuClockOpts))
        g_Config.cpuFreqPath[0] = '\0';
    if (!g_Config.cpuFreqPath[0] && !g_cpuClockOpts.empty())
        snprintf(g_Config.cpuFreqPath, sizeof(g_Config.cpuFreqPath), "%s", g_cpuClockOpts[0].second.c_str());
    if (g_gpuCount > 0 && g_Config.selectedGpu >= 0 && g_Config.selectedGpu < g_gpuCount) {
        GpuInfo& g = g_gpuList[g_Config.selectedGpu];
        if (!FreqPathValid(g_Config.gpuCoreFreqPath, g.coreClockOpts))
            g_Config.gpuCoreFreqPath[0] = '\0';
        if (!g_Config.gpuCoreFreqPath[0] && !g.coreClockOpts.empty())
            snprintf(g_Config.gpuCoreFreqPath, sizeof(g_Config.gpuCoreFreqPath), "%s", g.coreClockOpts[0].second.c_str());
    } else {
        g_Config.gpuCoreFreqPath[0] = '\0';
    }
}

static void SparkPush(float* buf, int& n, int cap, float v)
{
    if (cap <= 0) return;
    if (n < cap)
        buf[n++] = v;
    else {
        memmove(buf, buf + 1, (size_t)(cap - 1) * sizeof(float));
        buf[cap - 1] = v;
    }
}

using LhwmSensorMap = decltype(LHWM::GetHardwareSensorMap());

static float CallLhwmSensorValueCpp(const char* path, bool* cppException)
{
    if (cppException) *cppException = false;
    try {
        return LHWM::GetSensorValue(std::string(path ? path : ""));
    } catch (...) {
        if (cppException) *cppException = true;
        return 0.0f;
    }
}

static float CallLhwmSensorValueGuarded(const char* path,
                                        bool* cppException,
                                        DWORD* exceptionCode)
{
    if (exceptionCode) *exceptionCode = 0;
#if defined(_MSC_VER)
    __try {
        return CallLhwmSensorValueCpp(path, cppException);
    } __except((exceptionCode ? *exceptionCode = GetExceptionCode() : 0,
                EXCEPTION_EXECUTE_HANDLER)) {
        return 0.0f;
    }
#else
    return CallLhwmSensorValueCpp(path, cppException);
#endif
}

static bool FillLhwmSensorMapCpp(void* output)
{
    try {
        *static_cast<LhwmSensorMap*>(output) = LHWM::GetHardwareSensorMap();
        return true;
    } catch (...) {
        return false;
    }
}

static bool FillLhwmSensorMapGuarded(void* output, DWORD* exceptionCode)
{
    if (exceptionCode) *exceptionCode = 0;
#if defined(_MSC_VER)
    __try {
        return FillLhwmSensorMapCpp(output);
    } __except((exceptionCode ? *exceptionCode = GetExceptionCode() : 0,
                EXCEPTION_EXECUTE_HANDLER)) {
        return false;
    }
#else
    return FillLhwmSensorMapCpp(output);
#endif
}

static void QuarantineLhwmAfterNativeFault(DWORD code, const char* operation)
{
    g_lhwmNativeFaultCode.store(code, std::memory_order_release);
    const bool firstFault = !g_lhwmNativeFaulted.exchange(true, std::memory_order_acq_rel);
    g_lhwmAvailable.store(false, std::memory_order_release);
    g_lhwmReadsPaused.store(true, std::memory_order_release);
    if (firstFault) {
        LogLine("LibreHardwareMonitor native fault quarantined: operation=%s code=0x%08lX",
                operation ? operation : "unknown", (unsigned long)code);
    }
}

static bool LhwmReadsAllowed()
{
    if (g_lhwmNativeFaulted.load(std::memory_order_acquire) ||
        g_lhwmReadsPaused.load(std::memory_order_acquire))
        return false;
    const ULONGLONG readyTick = g_lhwmResumeReadyTick.load(std::memory_order_acquire);
    return readyTick == 0 || GetTickCount64() >= readyTick;
}

static float ReadLhwmSensorValue(const std::string& path)
{
    if (path.empty() || !g_lhwmAvailable.load(std::memory_order_acquire) ||
        !LhwmReadsAllowed())
        return 0.0f;

    std::lock_guard<std::mutex> lock(g_lhwmCallMutex);
    if (!g_lhwmAvailable.load(std::memory_order_acquire) || !LhwmReadsAllowed())
        return 0.0f;

    try {
        bool cppException = false;
        DWORD nativeException = 0;
        const float value = CallLhwmSensorValueGuarded(
            path.c_str(), &cppException, &nativeException);
        if (nativeException != 0) {
            QuarantineLhwmAfterNativeFault(nativeException, "GetSensorValue");
            return 0.0f;
        }
        if (cppException)
            return 0.0f;
        return std::isfinite(value) ? value : 0.0f;
    } catch (...) {
        return 0.0f;
    }
}

static void PauseLhwmReadsForSuspend()
{
    g_lhwmReadsPaused.store(true, std::memory_order_release);
    const bool idle = g_lhwmCallMutex.try_lock();
    if (idle)
        g_lhwmCallMutex.unlock();
    LogLine("LibreHardwareMonitor polling paused for power suspend: activeCall=%d",
            idle ? 0 : 1);
}

static void SetPowerRecoveryStatus(PowerRecoveryStatus status, const char* reason)
{
    const PowerRecoveryStatus previous =
        g_PowerRecoveryStatus.exchange(status, std::memory_order_acq_rel);
    g_PowerRecoveryStatusTick = GetTickCount64();
    if (status == PowerRecoveryStatus::Recovering)
        g_PowerRecoveryStartedTick = g_PowerRecoveryStatusTick;
    if (previous != status) {
        LogLine("Power recovery status: state=%d reason=%s",
                static_cast<int>(status), reason ? reason : "N/A");
        if (HWND tray = g_trayHwnd.load(std::memory_order_acquire))
            PostMessageW(tray, WM_APP_TRAY_RECOVERY_STATUS,
                         static_cast<WPARAM>(status), 0);
    }
}

static void UpdatePowerRecoveryStatus()
{
    const PowerRecoveryStatus status =
        g_PowerRecoveryStatus.load(std::memory_order_acquire);
    const ULONGLONG now = GetTickCount64();
    if (status == PowerRecoveryStatus::Recovering &&
        g_PowerRecoveryStartedTick != 0 &&
        now - g_PowerRecoveryStartedTick >= 45000) {
        SetPowerRecoveryStatus(PowerRecoveryStatus::Failed,
                               "no fresh hardware snapshot within 45 seconds");
    } else if (status == PowerRecoveryStatus::Recovered &&
               g_PowerRecoveryStatusTick != 0 &&
               now - g_PowerRecoveryStatusTick >= 8000) {
        g_PowerRecoveryStatus.store(PowerRecoveryStatus::Idle,
                                    std::memory_order_release);
    }
}

static void DelayLhwmReadsAfterResume()
{
    SetPowerRecoveryStatus(PowerRecoveryStatus::Recovering,
                           "Windows power resume");
    if (g_lhwmNativeFaulted.load(std::memory_order_acquire)) {
        SetPowerRecoveryStatus(PowerRecoveryStatus::Failed,
                               "hardware provider remains quarantined");
        return;
    }
    g_lhwmReadsPaused.store(false, std::memory_order_release);
    g_lhwmResumeReadyTick.store(GetTickCount64() + kLhwmResumeCooldownMs,
                                std::memory_order_release);
    LogLine("LibreHardwareMonitor polling delayed %llu ms after power resume",
            kLhwmResumeCooldownMs);
}

static void ResumeLhwmReadsWhenReady()
{
    const ULONGLONG readyTick = g_lhwmResumeReadyTick.load(std::memory_order_acquire);
    if (readyTick != 0 && GetTickCount64() >= readyTick) {
        g_lhwmResumeReadyTick.store(0, std::memory_order_release);
        LogLine("LibreHardwareMonitor polling resumed after power recovery");
    }
}

static void PollClockSensors()
{
    try {
        if (g_Config.showCpuFreq && g_lhwmAvailable && g_Config.cpuFreqPath[0])
            g_cpuClockMHz = ReadLhwmSensorValue(std::string(g_Config.cpuFreqPath));
        else if (g_Config.showCpuFreq)
            g_cpuClockMHz = QueryCpuClockMHzFallback();
        else
            g_cpuClockMHz = 0.f;

        if (g_Config.showGpuCoreFreq && g_lhwmAvailable && g_Config.gpuCoreFreqPath[0])
            g_gpuCoreClockMHz = ReadLhwmSensorValue(std::string(g_Config.gpuCoreFreqPath));
        else
            g_gpuCoreClockMHz = 0.f;

        SparkPush(g_cpuSpark, g_cpuSparkN, FREQ_SPARK_LEN, g_cpuClockMHz > 0.f ? g_cpuClockMHz : 0.f);
        SparkPush(g_gpuSpark, g_gpuSparkN, FREQ_SPARK_LEN, g_gpuCoreClockMHz > 0.f ? g_gpuCoreClockMHz : 0.f);
    } catch (...) {
    }
}

static void DrawMiniSpark(const char* id, const float* hist, int n, float mhz, ImVec2 sz)
{
    if (n >= 2) {
        ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.33f, 0.82f, 0.52f, 1.f));
        ImGui::PlotLines(id, hist, n, 0, nullptr, FLT_MAX, FLT_MAX, sz);
        ImGui::PopStyleColor();
        ImGui::SameLine();
    }
    if (mhz > 0.f)
        ImGui::TextColored(ImVec4(.72f, .72f, .76f, 1), "%.0f MHz", mhz);
    else
        ImGui::TextColored(ImVec4(.45f, .45f, .50f, 1), "--- MHz");
}

// Inline spark + MHz for horizontal / Steam rows: ImGui SameLine top-aligns widgets, so we
// vertically center the framed PlotLines and the Text block against each other.
static void InlineFreqSparkMHz(const char* plotId, const float* hist, int n, float mhz,
                               ImVec2 plotInnerPx, float gapAfterPlot, const ImVec4& txtCol)
{
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.f, 2.f));
    const ImGuiStyle& st = ImGui::GetStyle();
    const float plotFrameH = plotInnerPx.y + st.FramePadding.y * 2.f;
    const float textH = ImGui::GetTextLineHeight();
    const float rowH = plotFrameH > textH ? plotFrameH : textH;
    const float rowTop = ImGui::GetCursorPosY();
    ImGui::SetCursorPosY(rowTop + (rowH - plotFrameH) * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.33f, 0.82f, 0.52f, 1.f));
    ImGui::PlotLines(plotId, hist, n, 0, nullptr, FLT_MAX, FLT_MAX, plotInnerPx);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::SameLine(0, gapAfterPlot);
    ImGui::SetCursorPosY(rowTop + (rowH - textH) * 0.5f);
    ImGui::TextColored(txtCol, "%.0f MHz", mhz);
}

static float GameppPresetScale(int preset)
{
    static const float kScales[] = { 0.76f, 0.88f, 1.00f, 1.16f, 1.34f };
    if (preset < 0) preset = 0;
    if (preset > 4) preset = 4;
    return kScales[preset];
}

// ── DX11 ──
static ID3D11Device*           g_pd3dDevice        = nullptr;
static ID3D11DeviceContext*    g_pd3dDeviceContext  = nullptr;
static IDXGISwapChain*         g_pSwapChain        = nullptr;
static ID3D11RenderTargetView* g_pRenderTargetView = nullptr;

static ID3D11ShaderResourceView* g_texGitHub = nullptr;
static ID3D11ShaderResourceView* g_texKofi   = nullptr;

static bool LoadTextureFromMemory(const unsigned char* data, size_t dataSize,
                                  ID3D11ShaderResourceView** outSrv)
{
    if (!data || dataSize == 0 || !g_pd3dDevice || !outSrv) return false;

    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return false;

    IWICStream* stream = nullptr;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr)) { factory->Release(); return false; }

    hr = stream->InitializeFromMemory(const_cast<BYTE*>(data), (DWORD)dataSize);
    if (FAILED(hr)) { stream->Release(); factory->Release(); return false; }

    IWICBitmapDecoder* decoder = nullptr;
    hr = factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    stream->Release();
    if (FAILED(hr)) { factory->Release(); return false; }

    IWICBitmapFrameDecode* frame = nullptr;
    hr = decoder->GetFrame(0, &frame);
    decoder->Release();
    if (FAILED(hr)) { factory->Release(); return false; }

    IWICFormatConverter* converter = nullptr;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr)) { frame->Release(); factory->Release(); return false; }

    hr = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                               nullptr, 0.f, WICBitmapPaletteTypeCustom);
    frame->Release();
    if (FAILED(hr)) { converter->Release(); factory->Release(); return false; }

    UINT w = 0, h = 0;
    converter->GetSize(&w, &h);
    if (w == 0 || h == 0) {
        converter->Release();
        factory->Release();
        return false;
    }

    std::vector<BYTE> pixels((size_t)w * (size_t)h * 4);
    hr = converter->CopyPixels(nullptr, w * 4, (UINT)pixels.size(), pixels.data());
    converter->Release();
    factory->Release();
    if (FAILED(hr)) return false;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sub = {};
    sub.pSysMem = pixels.data();
    sub.SysMemPitch = w * 4;

    ID3D11Texture2D* tex = nullptr;
    hr = g_pd3dDevice->CreateTexture2D(&desc, &sub, &tex);
    if (FAILED(hr)) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    hr = g_pd3dDevice->CreateShaderResourceView(tex, &srvDesc, outSrv);
    tex->Release();
    return SUCCEEDED(hr);
}

static bool LoadTextureFromResource(int resId, ID3D11ShaderResourceView** outSrv)
{
    if (!g_hInstance || !outSrv) return false;
    HRSRC res = FindResource(g_hInstance, MAKEINTRESOURCE(resId), RT_RCDATA);
    if (!res) return false;
    HGLOBAL mem = LoadResource(g_hInstance, res);
    if (!mem) return false;
    DWORD size = SizeofResource(g_hInstance, res);
    const void* data = LockResource(mem);
    if (!data || size == 0) return false;
    return LoadTextureFromMemory(static_cast<const unsigned char*>(data), size, outSrv);
}

[[maybe_unused]] static void InitHeaderIconTextures()
{
    if (!g_texGitHub)
        LoadTextureFromResource(IDR_GITHUB_ICON, &g_texGitHub);
    if (!g_texKofi)
        LoadTextureFromResource(IDR_KOFI_ICON, &g_texKofi);
}

static void ReleaseHeaderIconTextures()
{
    if (g_texGitHub) { g_texGitHub->Release(); g_texGitHub = nullptr; }
    if (g_texKofi)   { g_texKofi->Release();   g_texKofi   = nullptr; }
}

// ── Hotkey listener state ──
static int  g_listeningFor = 0;   // 0=none, 1=toggle, 2=exit, 3=layout switch

// ═══════════════════════════════════════════════════════════════════════════
// Forward declarations
// ═══════════════════════════════════════════════════════════════════════════
bool    CreateDeviceD3D(HWND);
void    CleanupDeviceD3D();
bool    CreateRenderTarget();
void    CleanupRenderTarget();
void    AddTrayIcon();
void    RemoveTrayIcon();
void    UpdateTrayTooltip();
void    SwitchToOverlay();
void    SwitchToConfig();
void    ShutdownBackends();
bool    InitBackends();
void    ApplyStyle();
static float GetCpuUsage();

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK TrayWndProc(HWND, UINT, WPARAM, LPARAM);
static void TrayThreadMain();
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND, UINT, WPARAM, LPARAM);

// ═══════════════════════════════════════════════════════════════════════════
// Utility: key name from VK code
// ═══════════════════════════════════════════════════════════════════════════
static const char* GetKeyName(int vk)
{
    static char buf[64];
    switch (vk) {
        case VK_INSERT: return "插入键";
        case VK_DELETE: return "删除键";
        case VK_HOME: return "主页键";
        case VK_END: return "结束键";
        case VK_PRIOR: return "向上翻页";
        case VK_NEXT: return "向下翻页";
        case VK_ESCAPE: return "退出键";
        case VK_SPACE: return "空格键";
        case VK_RETURN: return "回车键";
        case VK_TAB: return "制表键";
        case VK_BACK: return "退格键";
        case VK_LEFT: return "左方向键";
        case VK_RIGHT: return "右方向键";
        case VK_UP: return "上方向键";
        case VK_DOWN: return "下方向键";
    }
    UINT sc = MapVirtualKey(vk, MAPVK_VK_TO_VSC);
    LONG lp = sc << 16;

    // Extended-key flag for nav keys
    switch (vk) {
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
        case VK_PRIOR:  case VK_NEXT:
        case VK_LEFT:   case VK_RIGHT:  case VK_UP:   case VK_DOWN:
        case VK_NUMLOCK: case VK_SNAPSHOT: case VK_CANCEL:
            lp |= (1 << 24);
            break;
    }

    if (GetKeyNameTextA(lp, buf, sizeof(buf)) > 0)
        return buf;
    snprintf(buf, sizeof(buf), "0x%02X", vk);
    return buf;
}

// ═══════════════════════════════════════════════════════════════════════════
// Admin check
// ═══════════════════════════════════════════════════════════════════════════
static bool IsRunningAsAdmin()
{
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
    
    if (AllocateAndInitializeSid(&ntAuth, 2,
            SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0, &adminGroup))
    {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin != FALSE;
}

// ═══════════════════════════════════════════════════════════════════════════
// Windows logon auto-start task
// ═══════════════════════════════════════════════════════════════════════════
static constexpr wchar_t kAutoLaunchTaskName[] = L"FPS Overlay";
static bool g_autoLaunchTaskReady = false;
static char g_autoLaunchStatus[160] = "尚未检查";

template <typename T>
static void ReleaseCom(T*& value)
{
    if (value) {
        value->Release();
        value = nullptr;
    }
}

static bool OpenTaskScheduler(ITaskService** serviceOut,
                              ITaskFolder** rootOut,
                              bool* uninitComOut)
{
    if (!serviceOut || !rootOut || !uninitComOut)
        return false;
    *serviceOut = nullptr;
    *rootOut = nullptr;
    *uninitComOut = false;

    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(init)) {
        *uninitComOut = true;
    } else if (init != RPC_E_CHANGED_MODE) {
        return false;
    }

    ITaskService* service = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITaskService, reinterpret_cast<void**>(&service));
    if (FAILED(hr) || !service) {
        if (*uninitComOut) CoUninitialize();
        *uninitComOut = false;
        return false;
    }

    hr = service->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t());
    if (FAILED(hr)) {
        ReleaseCom(service);
        if (*uninitComOut) CoUninitialize();
        *uninitComOut = false;
        return false;
    }

    ITaskFolder* root = nullptr;
    hr = service->GetFolder(_bstr_t(L"\\"), &root);
    if (FAILED(hr) || !root) {
        ReleaseCom(service);
        if (*uninitComOut) CoUninitialize();
        *uninitComOut = false;
        return false;
    }

    *serviceOut = service;
    *rootOut = root;
    return true;
}

static bool QueryAutoLaunchTask(bool* enabledOut)
{
    if (enabledOut)
        *enabledOut = false;
    ITaskService* service = nullptr;
    ITaskFolder* root = nullptr;
    bool uninitCom = false;
    if (!OpenTaskScheduler(&service, &root, &uninitCom))
        return false;

    IRegisteredTask* task = nullptr;
    const HRESULT hr = root->GetTask(_bstr_t(kAutoLaunchTaskName), &task);
    VARIANT_BOOL enabled = VARIANT_FALSE;
    const bool exists = SUCCEEDED(hr) && task;
    if (exists && enabledOut && SUCCEEDED(task->get_Enabled(&enabled)))
        *enabledOut = enabled == VARIANT_TRUE;

    ReleaseCom(task);
    ReleaseCom(root);
    ReleaseCom(service);
    if (uninitCom) CoUninitialize();
    return exists;
}

static bool IsAutoLaunchTaskEnabled()
{
    bool enabled = false;
    return QueryAutoLaunchTask(&enabled) && enabled;
}

static bool IsAutoLaunchTaskCurrentExecutable()
{
    wchar_t currentExePath[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, currentExePath, ARRAYSIZE(currentExePath)))
        return false;

    ITaskService* service = nullptr;
    ITaskFolder* root = nullptr;
    bool uninitCom = false;
    if (!OpenTaskScheduler(&service, &root, &uninitCom))
        return false;

    IRegisteredTask* task = nullptr;
    const HRESULT hr = root->GetTask(_bstr_t(kAutoLaunchTaskName), &task);
    BSTR xml = nullptr;
    const bool current = SUCCEEDED(hr) && task &&
        SUCCEEDED(task->get_Xml(&xml)) && xml &&
        wcsstr(xml, currentExePath) != nullptr;

    if (xml) SysFreeString(xml);
    ReleaseCom(task);
    ReleaseCom(root);
    ReleaseCom(service);
    if (uninitCom) CoUninitialize();
    return current;
}

static bool SetAutoLaunchTaskEnabled(bool enabled)
{
    if (!IsRunningAsAdmin())
        return false;

    ITaskService* service = nullptr;
    ITaskFolder* root = nullptr;
    bool uninitCom = false;
    if (!OpenTaskScheduler(&service, &root, &uninitCom))
        return false;

    bool success = false;
    if (!enabled) {
        if (!QueryAutoLaunchTask(nullptr)) {
            success = true;
        } else {
            success = SUCCEEDED(root->DeleteTask(_bstr_t(kAutoLaunchTaskName), 0));
        }
        if (success) {
            WriteIniStr("App", "AutoLaunchTaskPath", "");
            LogLine("Windows auto-start disabled");
        }
        ReleaseCom(root);
        ReleaseCom(service);
        if (uninitCom) CoUninitialize();
        return success;
    }

    wchar_t exePath[MAX_PATH] = {};
    char exePathA[MAX_PATH] = {};
    wchar_t userName[256] = {};
    DWORD userNameChars = ARRAYSIZE(userName);
    if (!GetModuleFileNameW(nullptr, exePath, ARRAYSIZE(exePath)) ||
        !GetModuleFileNameA(nullptr, exePathA, ARRAYSIZE(exePathA)) ||
        !GetUserNameW(userName, &userNameChars)) {
        ReleaseCom(root);
        ReleaseCom(service);
        if (uninitCom) CoUninitialize();
        return false;
    }

    wchar_t workingDirectory[MAX_PATH] = {};
    wcsncpy_s(workingDirectory, exePath, _TRUNCATE);
    if (wchar_t* slash = wcsrchr(workingDirectory, L'\\'))
        *slash = L'\0';

    ITaskDefinition* definition = nullptr;
    IRegistrationInfo* registration = nullptr;
    IPrincipal* principal = nullptr;
    ITaskSettings* settings = nullptr;
    ITriggerCollection* triggers = nullptr;
    ITrigger* trigger = nullptr;
    ILogonTrigger* logonTrigger = nullptr;
    IActionCollection* actions = nullptr;
    IAction* action = nullptr;
    IExecAction* execAction = nullptr;
    IRegisteredTask* registered = nullptr;

    HRESULT hr = service->NewTask(0, &definition);
    if (SUCCEEDED(hr))
        hr = definition->get_RegistrationInfo(&registration);
    if (SUCCEEDED(hr))
        hr = registration->put_Author(_bstr_t(L"FPS Overlay"));

    if (SUCCEEDED(hr))
        hr = definition->get_Principal(&principal);
    if (SUCCEEDED(hr))
        hr = principal->put_UserId(_bstr_t(userName));
    if (SUCCEEDED(hr))
        hr = principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
    if (SUCCEEDED(hr))
        hr = principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST);

    if (SUCCEEDED(hr))
        hr = definition->get_Settings(&settings);
    if (SUCCEEDED(hr))
        hr = settings->put_StartWhenAvailable(VARIANT_TRUE);
    if (SUCCEEDED(hr))
        hr = settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
    if (SUCCEEDED(hr))
        hr = settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
    if (SUCCEEDED(hr))
        hr = settings->put_ExecutionTimeLimit(_bstr_t(L"PT0S"));
    if (SUCCEEDED(hr))
        hr = settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);

    if (SUCCEEDED(hr))
        hr = definition->get_Triggers(&triggers);
    if (SUCCEEDED(hr))
        hr = triggers->Create(TASK_TRIGGER_LOGON, &trigger);
    if (SUCCEEDED(hr))
        hr = trigger->QueryInterface(IID_ILogonTrigger,
                                     reinterpret_cast<void**>(&logonTrigger));
    if (SUCCEEDED(hr))
        hr = logonTrigger->put_Id(_bstr_t(L"UserLogon"));
    if (SUCCEEDED(hr))
        hr = logonTrigger->put_UserId(_bstr_t(userName));

    if (SUCCEEDED(hr))
        hr = definition->get_Actions(&actions);
    if (SUCCEEDED(hr))
        hr = actions->Create(TASK_ACTION_EXEC, &action);
    if (SUCCEEDED(hr))
        hr = action->QueryInterface(IID_IExecAction,
                                    reinterpret_cast<void**>(&execAction));
    if (SUCCEEDED(hr))
        hr = execAction->put_Path(_bstr_t(exePath));
    if (SUCCEEDED(hr))
        hr = execAction->put_WorkingDirectory(_bstr_t(workingDirectory));

    if (SUCCEEDED(hr)) {
        hr = root->RegisterTaskDefinition(
            _bstr_t(kAutoLaunchTaskName),
            definition,
            TASK_CREATE_OR_UPDATE,
            _variant_t(userName),
            _variant_t(),
            TASK_LOGON_INTERACTIVE_TOKEN,
            _variant_t(L""),
            &registered);
    }
    success = SUCCEEDED(hr) && registered != nullptr;
    if (success) {
        WriteIniStr("App", "AutoLaunchTaskPath", exePathA);
        LogLine("Windows auto-start enabled: %s", exePathA);
    }

    ReleaseCom(registered);
    ReleaseCom(execAction);
    ReleaseCom(action);
    ReleaseCom(actions);
    ReleaseCom(logonTrigger);
    ReleaseCom(trigger);
    ReleaseCom(triggers);
    ReleaseCom(settings);
    ReleaseCom(principal);
    ReleaseCom(registration);
    ReleaseCom(definition);
    ReleaseCom(root);
    ReleaseCom(service);
    if (uninitCom) CoUninitialize();
    return success;
}

static void ReconcileAutoLaunchTask()
{
    bool success = true;
    if (g_Config.startWithWindows) {
        char previous[MAX_PATH] = {};
        char current[MAX_PATH] = {};
        ReadIniStr("App", "AutoLaunchTaskPath", previous, sizeof(previous));
        GetModuleFileNameA(nullptr, current, sizeof(current));
        if (!IsAutoLaunchTaskEnabled() || !IsAutoLaunchTaskCurrentExecutable() ||
            strcmp(previous, current) != 0)
            success = SetAutoLaunchTaskEnabled(true);
    } else if (QueryAutoLaunchTask(nullptr)) {
        success = SetAutoLaunchTaskEnabled(false);
    }

    g_autoLaunchTaskReady = IsAutoLaunchTaskEnabled();
    if (g_Config.startWithWindows != g_autoLaunchTaskReady)
        g_Config.startWithWindows = g_autoLaunchTaskReady;
    snprintf(g_autoLaunchStatus, sizeof(g_autoLaunchStatus), "%s",
             success
                 ? (g_autoLaunchTaskReady ? "已启用，用户登录后以最高权限启动"
                                          : "已关闭")
                 : "计划任务更新失败，请确认管理员权限");
    WriteIniInt("App", "startWithWindows", g_Config.startWithWindows ? 1 : 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// Hardware detection
// ═══════════════════════════════════════════════════════════════════════════
static void QueryCpuName()
{
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
            "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
            0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        DWORD sz = sizeof(g_cpuName);
        RegQueryValueExA(hKey, "ProcessorNameString", nullptr, nullptr,
                         reinterpret_cast<LPBYTE>(g_cpuName), &sz);
        RegCloseKey(hKey);

        // trim leading spaces
        char* p = g_cpuName;
        while (*p == ' ') p++;
        if (p != g_cpuName) memmove(g_cpuName, p, strlen(p) + 1);
    }
}

static float QueryCpuClockMHzFallback()
{
    HKEY hKey = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
            "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
            0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return 0.0f;

    DWORD mhz = 0;
    DWORD sz = sizeof(mhz);
    DWORD type = 0;
    const bool ok = (RegQueryValueExA(hKey, "~MHz", nullptr, &type,
                         reinterpret_cast<LPBYTE>(&mhz), &sz) == ERROR_SUCCESS &&
                     (type == REG_DWORD || type == REG_DWORD_LITTLE_ENDIAN));
    RegCloseKey(hKey);
    return ok && mhz > 0 ? (float)mhz : 0.0f;
}

static void FormatFanValue(char* out, size_t outLen, float value, bool percent)
{
    if (!out || outLen == 0) return;
    if (value > 0.0f)
        snprintf(out, outLen, percent ? "%.0f%%" : "%.0frpm", value);
    else
        snprintf(out, outLen, "N/A");
}

static void FormatTempCompact(char* out, size_t outLen, float celsius, bool useFahrenheit)
{
    if (!out || outLen == 0) return;
    if (celsius > 0.0f) {
        const float dispTemp = ToDisplayTemp(celsius, useFahrenheit);
        snprintf(out, outLen, "%.0f\xC2\xB0%s", dispTemp, useFahrenheit ? "F" : "C");
    } else {
        snprintf(out, outLen, "N/A");
    }
}

static void QueryGpuName()
{
    if (!g_pd3dDevice) return;

    IDXGIDevice* dxgiDev = nullptr;
    g_pd3dDevice->QueryInterface(__uuidof(IDXGIDevice),
                                 reinterpret_cast<void**>(&dxgiDev));
    if (!dxgiDev) return;

    IDXGIAdapter* adapter = nullptr;
    dxgiDev->GetAdapter(&adapter);
    if (adapter) {
        DXGI_ADAPTER_DESC desc;
        adapter->GetDesc(&desc);
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
                            g_gpuName, sizeof(g_gpuName), nullptr, nullptr);
        adapter->Release();
    }
    dxgiDev->Release();
}

// ═══════════════════════════════════════════════════════════════════════════
// Process name and description from PID (UTF-8 for ImGui / Unicode paths)
// ═══════════════════════════════════════════════════════════════════════════
static void WideToUtf8(const wchar_t* w, char* out, size_t outBytes)
{
    if (!out || outBytes == 0) return;
    out[0] = '\0';
    if (!w || !w[0]) return;
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, -1, out, (int)outBytes, nullptr, nullptr);
    if (n <= 0)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)outBytes, nullptr, nullptr);
}

static void GetFileDescriptionUtf8FromPathW(const wchar_t* filePathW, char* outDesc, size_t maxLen)
{
    outDesc[0] = '\0';
    if (!filePathW || !filePathW[0]) return;

    DWORD dummy = 0;
    DWORD size = GetFileVersionInfoSizeW(filePathW, &dummy);
    if (size == 0) return;

    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(filePathW, 0, size, data.data())) return;

    struct LANGANDCODEPAGE {
        WORD wLanguage;
        WORD wCodePage;
    } *lpTranslate = nullptr;
    UINT cbTranslate = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
                         reinterpret_cast<LPVOID*>(&lpTranslate), &cbTranslate) ||
        !lpTranslate || cbTranslate < sizeof(LANGANDCODEPAGE))
        return;

    wchar_t subBlock[72];
    _snwprintf_s(subBlock, _TRUNCATE, L"\\StringFileInfo\\%04x%04x\\FileDescription",
                 lpTranslate[0].wLanguage, lpTranslate[0].wCodePage);

    wchar_t* description = nullptr;
    UINT descLen = 0;
    if (!VerQueryValueW(data.data(), subBlock, reinterpret_cast<LPVOID*>(&description), &descLen) ||
        !description || !description[0])
        return;

    WideToUtf8(description, outDesc, maxLen);
}

static void GetProcessName(DWORD pid, char* outName, size_t maxLen)
{
    outName[0] = '\0';
    if (pid == 0 || maxLen == 0) return;

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProc)
        hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return;

    wchar_t fullPathW[MAX_PATH] = {};
    DWORD dw = MAX_PATH;
    if (QueryFullProcessImageNameW(hProc, 0, fullPathW, &dw)) {
        const wchar_t* slash = wcsrchr(fullPathW, L'\\');
        const wchar_t* exeW = slash ? (slash + 1) : fullPathW;

        char exeUtf8[320] = {};
        char descUtf8[512] = {};
        WideToUtf8(exeW, exeUtf8, sizeof(exeUtf8));
        GetFileDescriptionUtf8FromPathW(fullPathW, descUtf8, sizeof(descUtf8));

        if (descUtf8[0])
            snprintf(outName, maxLen, "%s (%s)", exeUtf8, descUtf8);
        else
            snprintf(outName, maxLen, "%s", exeUtf8);
    }
    CloseHandle(hProc);
}

// ═══════════════════════════════════════════════════════════════════════════
// CPU Temperature via WMI (works on some systems)
// ═══════════════════════════════════════════════════════════════════════════
static bool GetProcessExeName(DWORD pid, char* outName, size_t maxLen)
{
    if (!outName || maxLen == 0) return false;
    outName[0] = '\0';
    if (pid == 0) return false;

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return false;

    wchar_t fullPathW[MAX_PATH] = {};
    DWORD dw = MAX_PATH;
    bool ok = false;
    if (QueryFullProcessImageNameW(hProc, 0, fullPathW, &dw)) {
        const wchar_t* slash = wcsrchr(fullPathW, L'\\');
        const wchar_t* exeW = slash ? (slash + 1) : fullPathW;
        WideToUtf8(exeW, outName, maxLen);
        ok = outName[0] != '\0';
    }
    CloseHandle(hProc);
    return ok;
}

static bool GetProcessImagePath(DWORD pid, char* outPath, size_t maxLen)
{
    if (!outPath || maxLen == 0) return false;
    outPath[0] = '\0';
    if (pid == 0) return false;

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return false;

    wchar_t fullPathW[1024] = {};
    DWORD length = ARRAYSIZE(fullPathW);
    bool ok = false;
    if (QueryFullProcessImageNameW(hProc, 0, fullPathW, &length)) {
        WideToUtf8(fullPathW, outPath, maxLen);
        ok = outPath[0] != '\0';
    }
    CloseHandle(hProc);
    return ok;
}

static std::string LowerAscii(const char* value)
{
    std::string result = value ? value : "";
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return result;
}

static bool IsIgnoredForegroundProcess(const char* exeName)
{
    if (!exeName || !exeName[0]) return true;
    static const char* ignored[] = {
        "explorer.exe", "dwm.exe", "taskmgr.exe", "systemsettings.exe",
        "applicationframehost.exe", "shellexperiencehost.exe",
        "startmenuexperiencehost.exe", "searchhost.exe", "textinputhost.exe",
        "lockapp.exe", "winlogon.exe", "csrss.exe", "fontdrvhost.exe"
    };
    for (const char* name : ignored) {
        if (_stricmp(exeName, name) == 0)
            return true;
    }
    return false;
}

static bool IsKnownDesktopProcess(const char* exeName)
{
    if (!exeName || !exeName[0]) return false;
    static const char* desktopApps[] = {
        "msedge.exe", "chrome.exe", "firefox.exe", "brave.exe",
        "opera.exe", "vivaldi.exe", "iexplore.exe",
        "codex.exe", "chatgpt.exe", "code.exe", "devenv.exe", "notepad.exe",
        "notepad++.exe", "wordpad.exe", "winword.exe", "excel.exe",
        "powerpnt.exe", "outlook.exe", "acrord32.exe", "acrobat.exe",
        "discord.exe", "telegram.exe", "qq.exe", "wechat.exe", "weixin.exe",
        "steam.exe", "steamwebhelper.exe", "epicgameslauncher.exe",
        "battle.net.exe", "riotclientservices.exe", "ubisoftconnect.exe",
        "eadesktop.exe", "wegame.exe", "browser.exe", "krwebview.exe",
        "krsdkexternal.exe", "crashreportclient.exe",
        "unrealcefsubprocess.exe", "cefsharp.browsersubprocess.exe",
        "clash-verge.exe", "clash-verge-service.exe", "baidunetdisk.exe",
        "leigod.exe", "ace-tray.exe", "ace-service.exe",
        "bilibili.exe", "potplayermini64.exe", "potplayermini.exe", "vlc.exe",
        "wallpaper64.exe", "wallpaper32.exe", "wallpaperservice32.exe"
    };
    for (const char* name : desktopApps) {
        if (_stricmp(exeName, name) == 0)
            return true;
    }
    return false;
}

static bool HasStrongGameIdentity(const char* exeName, const char* imagePath)
{
    // Wallpaper Engine and launchers may live below a game-library directory
    // and present continuously, but must never become a game target.
    if (IsKnownDesktopProcess(exeName) || IsIgnoredForegroundProcess(exeName))
        return false;

    const std::string exe = LowerAscii(exeName);
    const std::string path = LowerAscii(imagePath);

    if (exe.find("-win64-shipping.exe") != std::string::npos ||
        exe.find("-win32-shipping.exe") != std::string::npos ||
        exe.find("deltaforceclient") != std::string::npos ||
        exe.find("deltaforce") != std::string::npos ||
        exe.find("3dmark") != std::string::npos) {
        return true;
    }

    static const char* gamePathMarkers[] = {
        "\\steamapps\\common\\",
        "\\xboxgames\\",
        "\\epic games\\",
        "\\riot games\\",
        "\\wegameapps\\",
        "\\deltaforce\\",
        "\\gog galaxy\\games\\",
        "\\ea games\\",
        "\\ubisoft game launcher\\games\\"
    };
    for (const char* marker : gamePathMarkers) {
        if (path.find(marker) != std::string::npos)
            return true;
    }
    return false;
}

static bool IsProcessAlive(DWORD pid)
{
    if (pid == 0)
        return false;
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProc) {
        DWORD exitCode = 0;
        const bool alive = GetExitCodeProcess(hProc, &exitCode) && exitCode == STILL_ACTIVE;
        CloseHandle(hProc);
        return alive;
    }

    const DWORD openError = GetLastError();
    if (openError == ERROR_ACCESS_DENIED)
        return true;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == pid) {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

static bool GetProcessIdentity(DWORD pid, char* exeName, size_t exeLen,
                               char* imagePath, size_t pathLen)
{
    bool requested = false;
    bool ok = false;
    if (exeName && exeLen > 0) {
        requested = true;
        exeName[0] = '\0';
        ok = GetProcessExeName(pid, exeName, exeLen) || ok;
    }
    if (imagePath && pathLen > 0) {
        requested = true;
        imagePath[0] = '\0';
        ok = GetProcessImagePath(pid, imagePath, pathLen) || ok;
    }
    return requested ? ok : false;
}

static bool IsStrongGameProcess(DWORD pid, char* exeOut = nullptr, size_t exeOutLen = 0)
{
    char exeName[320] = {};
    char imagePath[1024] = {};
    if (!GetProcessIdentity(pid, exeName, sizeof(exeName), imagePath, sizeof(imagePath)))
        return false;
    if (exeOut && exeOutLen > 0)
        snprintf(exeOut, exeOutLen, "%s", exeName);
    if (IsKnownDesktopProcess(exeName) || IsIgnoredForegroundProcess(exeName))
        return false;
    return HasStrongGameIdentity(exeName, imagePath);
}

static void RememberRecentGame(DWORD pid, const char* exeName)
{
    if (pid == 0)
        return;
    g_RecentGamePid = pid;
    g_RecentGameSeenTick = GetTickCount64();
    if (exeName && exeName[0])
        snprintf(g_RecentGameExe, sizeof(g_RecentGameExe), "%s", exeName);
    else
        GetProcessExeName(pid, g_RecentGameExe, sizeof(g_RecentGameExe));
}

static void RefreshRecentGame(DWORD pid)
{
    if (pid != 0 && pid == g_RecentGamePid)
        g_RecentGameSeenTick = GetTickCount64();
}

static void RememberGameOverlayDisplay(DWORD pid, const char* reason)
{
    if (pid == 0)
        return;
    g_GameOverlayDisplayActive = true;
    g_GameOverlayDisplayPid = pid;
    snprintf(g_GameOverlayDisplayReason, sizeof(g_GameOverlayDisplayReason),
             "%s", reason && reason[0] ? reason : "game target");
}

static void ClearGameOverlayDisplay()
{
    g_GameOverlayDisplayActive = false;
    g_GameOverlayDisplayPid = 0;
    g_GameOverlayDisplayReason[0] = '\0';
    if (g_hwnd && IsWindow(g_hwnd) &&
        (g_OverlayOwnerWindow || GetWindow(g_hwnd, GW_OWNER))) {
        SetWindowLongPtr(g_hwnd, GWLP_HWNDPARENT, 0);
        g_OverlayOwnerWindow = nullptr;
    }
}

static bool RecentGameStillUsable(ULONGLONG now, ULONGLONG keepMs)
{
    if (g_RecentGamePid == 0 || g_RecentGameSeenTick == 0)
        return false;
    if (now - g_RecentGameSeenTick > keepMs)
        return false;
    if (!IsProcessAlive(g_RecentGamePid)) {
        g_RecentGamePid = 0;
        g_RecentGameSeenTick = 0;
        g_RecentGameExe[0] = '\0';
        return false;
    }
    return true;
}

static bool IsIgnoredForegroundClass(const char* className)
{
    if (!className || !className[0]) return true;
    static const char* ignored[] = {
        "Shell_TrayWnd", "Shell_SecondaryTrayWnd", "Progman", "WorkerW",
        "DV2ControlHost", "Windows.UI.Core.CoreWindow"
    };
    for (const char* name : ignored) {
        if (_stricmp(className, name) == 0)
            return true;
    }
    return false;
}

// Rewritten from the foreground-window filtering idea in G-Helper
// app/Overlay/HardwareOverlay.cs at commit d7eb9cbfc2a38d43239dece00e3f5b9a165235c6.
// This project keeps only read-only process recognition; no ASUS control logic is imported.
static bool IsLikelyGameForegroundWindow(HWND hwnd, DWORD pid,
                                         const char* exeName)
{
    if (!hwnd || pid == 0 || pid == GetCurrentProcessId())
        return false;
    if (!IsWindowVisible(hwnd))
        return false;

    LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) || (exStyle & WS_EX_NOACTIVATE))
        return false;

    char className[128] = {};
    GetClassNameA(hwnd, className, sizeof(className));
    if (IsIgnoredForegroundClass(className))
        return false;

    RECT rc = {};
    if (!GetWindowRect(hwnd, &rc))
        return false;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w < 320 || h < 200)
        return false;

    if (!exeName || !exeName[0])
        return false;
    if (IsIgnoredForegroundProcess(exeName))
        return false;

    return true;
}

static bool IsWindowMaximizedOrFullscreen(HWND foreground)
{
    if (!foreground || foreground == g_hwnd ||
        !IsWindowVisible(foreground) || IsIconic(foreground)) {
        return false;
    }

    char className[128] = {};
    GetClassNameA(foreground, className, ARRAYSIZE(className));
    if (_stricmp(className, "Progman") == 0 ||
        _stricmp(className, "WorkerW") == 0 ||
        _stricmp(className, "Shell_TrayWnd") == 0 ||
        _stricmp(className, "Shell_SecondaryTrayWnd") == 0) {
        return false;
    }

    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(
            foreground, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
        cloaked) {
        return false;
    }

    RECT windowRect = {};
    if (FAILED(DwmGetWindowAttribute(
            foreground, DWMWA_EXTENDED_FRAME_BOUNDS,
            &windowRect, sizeof(windowRect))) &&
        !GetWindowRect(foreground, &windowRect)) {
        return false;
    }

    HMONITOR monitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTONULL);
    if (!monitor)
        return false;
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfo(monitor, &info))
        return false;

    constexpr int tolerancePx = 4;
    const auto coversRect = [&](const RECT& target) {
        return windowRect.left <= target.left + tolerancePx &&
               windowRect.top <= target.top + tolerancePx &&
               windowRect.right >= target.right - tolerancePx &&
               windowRect.bottom >= target.bottom - tolerancePx;
    };

    // IsZoomed covers normal Windows maximization (for example Edge with the
    // taskbar still visible). Rectangle checks cover snapped/custom windows
    // that fill the work area and borderless/exclusive full-screen apps.
    return IsZoomed(foreground) ||
           coversRect(info.rcWork) ||
           coversRect(info.rcMonitor);
}

struct ForegroundSnapshot {
    HWND window = nullptr;
    DWORD pid = 0;
    char exeName[320] = {};
    char imagePath[1024] = {};
    bool hasIdentity = false;
    bool likelyGameWindow = false;
    bool knownDesktop = false;
    bool strongGame = false;
    bool fullscreen = false;
};

static ForegroundSnapshot CaptureForegroundSnapshot()
{
    ForegroundSnapshot snapshot;
    snapshot.window = GetForegroundWindow();
    if (!snapshot.window || snapshot.window == g_hwnd)
        return snapshot;

    GetWindowThreadProcessId(snapshot.window, &snapshot.pid);
    if (snapshot.pid == 0)
        return snapshot;

    snapshot.hasIdentity = GetProcessIdentity(
        snapshot.pid,
        snapshot.exeName, sizeof(snapshot.exeName),
        snapshot.imagePath, sizeof(snapshot.imagePath));
    snapshot.likelyGameWindow = snapshot.hasIdentity &&
        IsLikelyGameForegroundWindow(snapshot.window, snapshot.pid,
                                     snapshot.exeName);
    snapshot.knownDesktop = snapshot.hasIdentity &&
        IsKnownDesktopProcess(snapshot.exeName);
    snapshot.strongGame = snapshot.hasIdentity &&
        HasStrongGameIdentity(snapshot.exeName, snapshot.imagePath);
    snapshot.fullscreen = IsWindowMaximizedOrFullscreen(snapshot.window);
    return snapshot;
}

static void UpdateDesktopVisibilityDecision(
    const ForegroundSnapshot& foreground)
{
    static DWORD s_previousPid = 0;
    static ULONGLONG s_foregroundSince = 0;
    static ULONGLONG s_renderEvidenceSince = 0;

    const ULONGLONG now = GetTickCount64();
    if (foreground.pid != s_previousPid) {
        s_previousPid = foreground.pid;
        s_foregroundSince = now;
        s_renderEvidenceSince = 0;
    }

    g_DesktopForegroundPid = foreground.pid;
    snprintf(g_DesktopForegroundExe, sizeof(g_DesktopForegroundExe), "%s",
             foreground.exeName);

    const bool knownDesktop = foreground.knownDesktop;
    const bool strongGame = foreground.strongGame;
    const bool activeGameTarget =
        g_GameOverlayDisplayActive && g_GameOverlayDisplayPid != 0;
    const float directFps =
        foreground.pid != 0 &&
        foreground.pid == g_targetPid.load(std::memory_order_relaxed)
            ? g_gameFps.load(std::memory_order_relaxed)
            : 0.0f;
    const float automaticFps =
        foreground.pid != 0 &&
        foreground.pid == g_autoTargetPid.load(std::memory_order_relaxed)
            ? g_autoGameFps.load(std::memory_order_relaxed)
            : 0.0f;
    const bool hasRenderEvidence =
        !knownDesktop && (directFps >= 12.0f || automaticFps >= 12.0f);
    if (hasRenderEvidence) {
        if (s_renderEvidenceSince == 0)
            s_renderEvidenceSince = now;
    } else {
        s_renderEvidenceSince = 0;
    }
    const bool sustainedRender =
        s_renderEvidenceSince != 0 && now - s_renderEvidenceSince >= 600;
    g_ForegroundGameConfirmed = strongGame || sustainedRender || activeGameTarget;
    if (strongGame || sustainedRender) {
        RememberRecentGame(foreground.pid, g_DesktopForegroundExe);
    } else if (activeGameTarget) {
        // The foreground window can briefly be Explorer, Steam, or another
        // desktop helper while the actual game keeps rendering in the background.
        // Refresh the confirmed game instead of overwriting it with that helper.
        RefreshRecentGame(g_GameOverlayDisplayPid);
    }

    const bool fullscreen = foreground.fullscreen;
    if (!g_Config.desktopOnlyMode) {
        g_FullscreenAutoHidden = false;
        g_DesktopVisibilityState = DESKTOP_VISIBILITY_DISABLED;
        snprintf(g_DesktopVisibilityReason, sizeof(g_DesktopVisibilityReason),
                 "%s", "desktop-only mode disabled");
        return;
    }
    if (!fullscreen) {
        g_FullscreenAutoHidden = false;
        g_DesktopVisibilityState = DESKTOP_VISIBILITY_DESKTOP;
        snprintf(g_DesktopVisibilityReason, sizeof(g_DesktopVisibilityReason),
                 "%s", "desktop or windowed foreground");
        return;
    }
    if (g_ForegroundGameConfirmed) {
        g_FullscreenAutoHidden = false;
        g_DesktopVisibilityState = DESKTOP_VISIBILITY_GAME;
        snprintf(g_DesktopVisibilityReason, sizeof(g_DesktopVisibilityReason),
                 "%s", strongGame ? "strong game identity"
                     : (activeGameTarget ? g_GameOverlayDisplayReason
                                         : "sustained foreground rendering"));
        return;
    }

    const ULONGLONG foregroundAge =
        s_foregroundSince == 0 ? 0 : now - s_foregroundSince;
    const ULONGLONG graceMs = knownDesktop ? 500 : 8000;
    if (foregroundAge < graceMs) {
        g_FullscreenAutoHidden = false;
        g_DesktopVisibilityState = DESKTOP_VISIBILITY_EVALUATING;
        snprintf(g_DesktopVisibilityReason, sizeof(g_DesktopVisibilityReason),
                 "%s", knownDesktop ? "desktop app debounce"
                                    : "unknown app game-detection grace period");
        return;
    }

    g_FullscreenAutoHidden = true;
    g_DesktopVisibilityState = DESKTOP_VISIBILITY_APP_HIDDEN;
    snprintf(g_DesktopVisibilityReason, sizeof(g_DesktopVisibilityReason),
             "%s", knownDesktop ? "confirmed desktop application"
                                : "no foreground game rendering detected");
}

static IWbemLocator*   g_pWbemLocator  = nullptr;
static IWbemServices*  g_pWbemServices = nullptr;
static bool            g_wmiInitialized = false;
static IWbemLocator*   g_pAsusWmiLocator  = nullptr;
static IWbemServices*  g_pAsusWmiServices = nullptr;
static bool            g_asusWmiInitialized = false;
static HANDLE          g_hAsusAtkAcpi = INVALID_HANDLE_VALUE;
static ULONGLONG       g_lastAsusAtkOpenAttemptMs = 0;

static const DWORD ASUS_WMI_DEVID_CPU_FAN_CTRL = 0x00110013;
static const DWORD ASUS_WMI_DEVID_GPU_FAN_CTRL = 0x00110014;
static const DWORD ASUS_WMI_DEVID_MID_FAN_CTRL = 0x00110031;
static const DWORD ASUS_WMI_DEVID_LEGACY_FAN_CTRL = 0x00110012;
static const DWORD ASUS_WMI_DSTS_PRESENCE_BIT  = 0x00010000;
static const DWORD ASUS_ATKACPI_IOCTL = 0x0022240C;
static const DWORD ASUS_ATKACPI_METHOD_DSTS = 0x53545344; // "DSTS"

static void WriteLe32(BYTE* dst, DWORD value)
{
    dst[0] = static_cast<BYTE>(value & 0xff);
    dst[1] = static_cast<BYTE>((value >> 8) & 0xff);
    dst[2] = static_cast<BYTE>((value >> 16) & 0xff);
    dst[3] = static_cast<BYTE>((value >> 24) & 0xff);
}

static DWORD ReadLe32(const BYTE* src)
{
    return static_cast<DWORD>(src[0]) |
           (static_cast<DWORD>(src[1]) << 8) |
           (static_cast<DWORD>(src[2]) << 16) |
           (static_cast<DWORD>(src[3]) << 24);
}

static bool InitAsusAtkAcpi()
{
    if (g_hAsusAtkAcpi != INVALID_HANDLE_VALUE)
        return true;

    const ULONGLONG now = GetTickCount64();
    if (g_lastAsusAtkOpenAttemptMs != 0 && now - g_lastAsusAtkOpenAttemptMs < 30000)
        return false;
    g_lastAsusAtkOpenAttemptMs = now;

    g_hAsusAtkAcpi = CreateFileW(L"\\\\.\\ATKACPI",
                                 GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr,
                                 OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL,
                                 nullptr);
    if (g_hAsusAtkAcpi == INVALID_HANDLE_VALUE)
        return false;
    return true;
}

static bool QueryAsusAtkAcpiDeviceStatus(DWORD deviceId, DWORD* firstDwordOut, DWORD* adjustedStatusOut)
{
    if (!firstDwordOut || !adjustedStatusOut || !InitAsusAtkAcpi())
        return false;

    BYTE inBuf[16] = {};
    BYTE outBuf[16] = {};
    DWORD returned = 0;

    WriteLe32(inBuf + 0, ASUS_ATKACPI_METHOD_DSTS);
    WriteLe32(inBuf + 4, 8);
    WriteLe32(inBuf + 8, deviceId);
    WriteLe32(inBuf + 12, 0);

    if (!DeviceIoControl(g_hAsusAtkAcpi,
                         ASUS_ATKACPI_IOCTL,
                         inBuf,
                         sizeof(inBuf),
                         outBuf,
                         sizeof(outBuf),
                         &returned,
                         nullptr) || returned < 4) {
        CloseHandle(g_hAsusAtkAcpi);
        g_hAsusAtkAcpi = INVALID_HANDLE_VALUE;
        return false;
    }

    const DWORD first = ReadLe32(outBuf);
    *firstDwordOut = first;
    *adjustedStatusOut = static_cast<DWORD>(static_cast<int>(first) - static_cast<int>(ASUS_WMI_DSTS_PRESENCE_BIT));
    return true;
}

static bool DecodeAsusFanRpm(DWORD rawStatus, float* rpmOut)
{
    if (!rpmOut)
        return false;

    const DWORD rpmDiv100 = rawStatus & 0xffff;
    if (rpmDiv100 > 0 && rpmDiv100 <= 200) {
        *rpmOut = static_cast<float>(rpmDiv100 * 100);
        return true;
    }

    if (rawStatus >= 300 && rawStatus <= 20000) {
        *rpmOut = static_cast<float>(rawStatus);
        return true;
    }

    return false;
}

static bool InitWMI()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;
    
    hr = CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
                              RPC_C_AUTHN_LEVEL_DEFAULT,
                              RPC_C_IMP_LEVEL_IMPERSONATE,
                              nullptr, EOAC_NONE, nullptr);
    // Ignore if already initialized
    
    hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IWbemLocator, reinterpret_cast<void**>(&g_pWbemLocator));
    if (FAILED(hr)) return false;
    
    // Try OpenHardwareMonitor WMI namespace first (most reliable)
    hr = g_pWbemLocator->ConnectServer(
        _bstr_t(L"ROOT\\OpenHardwareMonitor"), nullptr, nullptr, nullptr,
        0, nullptr, nullptr, &g_pWbemServices);
    
    if (FAILED(hr)) {
        // Try standard WMI namespace (works on some systems)
        hr = g_pWbemLocator->ConnectServer(
            _bstr_t(L"ROOT\\WMI"), nullptr, nullptr, nullptr,
            0, nullptr, nullptr, &g_pWbemServices);
    }
    
    if (FAILED(hr)) {
        g_pWbemLocator->Release();
        g_pWbemLocator = nullptr;
        return false;
    }
    
    hr = CoSetProxyBlanket(g_pWbemServices, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
                           nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                           nullptr, EOAC_NONE);
    
    g_wmiInitialized = true;
    return true;
}

static bool InitAsusWMI()
{
    if (g_asusWmiInitialized && g_pAsusWmiServices)
        return true;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
        return false;

    CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
                         RPC_C_AUTHN_LEVEL_DEFAULT,
                         RPC_C_IMP_LEVEL_IMPERSONATE,
                         nullptr, EOAC_NONE, nullptr);

    hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IWbemLocator, reinterpret_cast<void**>(&g_pAsusWmiLocator));
    if (FAILED(hr))
        return false;

    hr = g_pAsusWmiLocator->ConnectServer(
        _bstr_t(L"ROOT\\WMI"), nullptr, nullptr, nullptr,
        0, nullptr, nullptr, &g_pAsusWmiServices);
    if (FAILED(hr)) {
        g_pAsusWmiLocator->Release();
        g_pAsusWmiLocator = nullptr;
        return false;
    }

    hr = CoSetProxyBlanket(g_pAsusWmiServices, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
                           nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                           nullptr, EOAC_NONE);
    if (FAILED(hr)) {
        g_pAsusWmiServices->Release();
        g_pAsusWmiServices = nullptr;
        g_pAsusWmiLocator->Release();
        g_pAsusWmiLocator = nullptr;
        return false;
    }

    g_asusWmiInitialized = true;
    return true;
}

static bool QueryAsusWmiDeviceStatus(DWORD deviceId, DWORD* statusOut)
{
    if (!statusOut || !InitAsusWMI())
        return false;
    *statusOut = 0;

    IEnumWbemClassObject* pEnum = nullptr;
    HRESULT hr = g_pAsusWmiServices->ExecQuery(
        _bstr_t(L"WQL"),
        _bstr_t(L"SELECT * FROM AsusAtkWmi_WMNB"),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        nullptr, &pEnum);
    if (FAILED(hr) || !pEnum)
        return false;

    IWbemClassObject* pObj = nullptr;
    ULONG returned = 0;
    hr = pEnum->Next(250, 1, &pObj, &returned);
    pEnum->Release();
    if (FAILED(hr) || returned == 0 || !pObj)
        return false;

    VARIANT vtPath;
    VariantInit(&vtPath);
    hr = pObj->Get(L"__PATH", 0, &vtPath, nullptr, nullptr);
    pObj->Release();
    if (FAILED(hr) || vtPath.vt != VT_BSTR || !vtPath.bstrVal) {
        VariantClear(&vtPath);
        return false;
    }

    IWbemClassObject* pClass = nullptr;
    IWbemClassObject* pInDef = nullptr;
    IWbemClassObject* pInInst = nullptr;
    IWbemClassObject* pOut = nullptr;

    hr = g_pAsusWmiServices->GetObject(_bstr_t(L"AsusAtkWmi_WMNB"), 0, nullptr, &pClass, nullptr);
    if (SUCCEEDED(hr) && pClass)
        hr = pClass->GetMethod(L"DSTS", 0, &pInDef, nullptr);
    if (SUCCEEDED(hr) && pInDef)
        hr = pInDef->SpawnInstance(0, &pInInst);
    if (SUCCEEDED(hr) && pInInst) {
        VARIANT vtDev;
        VariantInit(&vtDev);
        vtDev.vt = VT_UI4;
        vtDev.ulVal = deviceId;
        hr = pInInst->Put(L"Device_ID", 0, &vtDev, 0);
        VariantClear(&vtDev);
    }
    if (SUCCEEDED(hr) && pInInst)
        hr = g_pAsusWmiServices->ExecMethod(vtPath.bstrVal, _bstr_t(L"DSTS"), 0, nullptr, pInInst, &pOut, nullptr);

    bool ok = false;
    if (SUCCEEDED(hr) && pOut) {
        VARIANT vtStatus;
        VariantInit(&vtStatus);
        hr = pOut->Get(L"device_status", 0, &vtStatus, nullptr, nullptr);
        if (SUCCEEDED(hr)) {
            if (vtStatus.vt == VT_UI4) {
                *statusOut = vtStatus.ulVal;
                ok = true;
            } else if (vtStatus.vt == VT_I4 || vtStatus.vt == VT_INT) {
                *statusOut = static_cast<DWORD>(vtStatus.lVal);
                ok = true;
            }
        }
        VariantClear(&vtStatus);
    }

    if (pOut) pOut->Release();
    if (pInInst) pInInst->Release();
    if (pInDef) pInDef->Release();
    if (pClass) pClass->Release();
    VariantClear(&vtPath);
    return ok;
}

static bool QueryAsusFanRpmFromDevices(const DWORD* ids, int idCount, float* rpmOut)
{
    if (!rpmOut)
        return false;
    *rpmOut = 0.0f;

    for (int i = 0; i < idCount; ++i) {
        DWORD devid = ids[i];
        DWORD atkFirst = 0;
        DWORD atkStatus = 0;
        if (QueryAsusAtkAcpiDeviceStatus(devid, &atkFirst, &atkStatus) &&
            (DecodeAsusFanRpm(atkStatus, rpmOut) || DecodeAsusFanRpm(atkFirst, rpmOut))) {
            return true;
        }

        DWORD status = 0;
        if (!QueryAsusWmiDeviceStatus(devid, &status))
            continue;

        // ASUS fan reads return RPM/100 in the low 16 bits on most BIOSes.
        // Some BIOSes do not set the generic presence bit for this read path.
        if (DecodeAsusFanRpm(status, rpmOut))
            return true;
    }
    return false;
}

static bool QueryAsusWmiFanRpm(bool gpuFan, float* rpmOut)
{
    if (gpuFan) {
        const DWORD gpuIds[] = { ASUS_WMI_DEVID_GPU_FAN_CTRL };
        return QueryAsusFanRpmFromDevices(gpuIds, static_cast<int>(sizeof(gpuIds) / sizeof(gpuIds[0])), rpmOut);
    }

    const DWORD cpuIds[] = { ASUS_WMI_DEVID_CPU_FAN_CTRL, ASUS_WMI_DEVID_LEGACY_FAN_CTRL };
    return QueryAsusFanRpmFromDevices(cpuIds, static_cast<int>(sizeof(cpuIds) / sizeof(cpuIds[0])), rpmOut);
}

static bool QueryAsusSystemFanRpm(float* rpmOut)
{
    const DWORD systemIds[] = { ASUS_WMI_DEVID_MID_FAN_CTRL };
    return QueryAsusFanRpmFromDevices(systemIds, static_cast<int>(sizeof(systemIds) / sizeof(systemIds[0])), rpmOut);
}

static void WriteAsusWmiDiagnostics()
{
    char exePath[MAX_PATH] = {};
    if (!GetModuleFileNameA(nullptr, exePath, MAX_PATH))
        return;
    char* slash = strrchr(exePath, '\\');
    if (!slash)
        return;
    *(slash + 1) = '\0';
    strcat_s(exePath, MAX_PATH, "asus-wmi-diagnostics.txt");

    std::ofstream out(exePath, std::ios::out | std::ios::trunc);
    if (!out.is_open())
        return;

    struct Probe { const char* name; DWORD devid; };
    const Probe probes[] = {
        {"CPU fan", ASUS_WMI_DEVID_CPU_FAN_CTRL},
        {"GPU fan", ASUS_WMI_DEVID_GPU_FAN_CTRL},
        {"Middle fan", ASUS_WMI_DEVID_MID_FAN_CTRL},
        {"Legacy fan", ASUS_WMI_DEVID_LEGACY_FAN_CTRL},
    };

    out << "Name\tDeviceId\tATKOk\tATKFirstHex\tATKAdjustedHex\tATKLow16\tATKRPM\tWmiOk\tWmiRawHex\tWmiLow16\tWmiRPM\n";
    for (const Probe& p : probes) {
        DWORD atkFirst = 0;
        DWORD atkStatus = 0;
        bool atkOk = QueryAsusAtkAcpiDeviceStatus(p.devid, &atkFirst, &atkStatus);
        float atkRpmFloat = 0.0f;
        bool atkRpmOk = atkOk && (DecodeAsusFanRpm(atkStatus, &atkRpmFloat) || DecodeAsusFanRpm(atkFirst, &atkRpmFloat));

        DWORD status = 0;
        bool ok = QueryAsusWmiDeviceStatus(p.devid, &status);
        float rpmFloat = 0.0f;
        bool rpmOk = ok && DecodeAsusFanRpm(status, &rpmFloat);

        char atkFirstHex[32];
        char atkAdjustedHex[32];
        char wmiHex[32];
        snprintf(atkFirstHex, sizeof(atkFirstHex), "0x%08X", atkFirst);
        snprintf(atkAdjustedHex, sizeof(atkAdjustedHex), "0x%08X", atkStatus);
        snprintf(wmiHex, sizeof(wmiHex), "0x%08X", status);
        out << p.name << '\t' << "0x" << std::hex << p.devid << std::dec << '\t'
            << (atkOk ? "1" : "0") << '\t' << atkFirstHex << '\t' << atkAdjustedHex << '\t'
            << (atkStatus & 0xffff) << '\t' << (atkRpmOk ? static_cast<DWORD>(atkRpmFloat) : 0) << '\t'
            << (ok ? "1" : "0") << '\t' << wmiHex << '\t'
            << (status & 0xffff) << '\t' << (rpmOk ? static_cast<DWORD>(rpmFloat) : 0) << '\n';
    }
}

static void ShutdownWMI()
{
    if (g_hAsusAtkAcpi != INVALID_HANDLE_VALUE) {
        CloseHandle(g_hAsusAtkAcpi);
        g_hAsusAtkAcpi = INVALID_HANDLE_VALUE;
    }
    if (g_pWbemServices) { g_pWbemServices->Release(); g_pWbemServices = nullptr; }
    if (g_pWbemLocator)  { g_pWbemLocator->Release();  g_pWbemLocator  = nullptr; }
    if (g_pAsusWmiServices) { g_pAsusWmiServices->Release(); g_pAsusWmiServices = nullptr; }
    if (g_pAsusWmiLocator)  { g_pAsusWmiLocator->Release();  g_pAsusWmiLocator  = nullptr; }
    g_wmiInitialized = false;
    g_asusWmiInitialized = false;
}

static float QueryCpuTemperature()
{
    if (!g_wmiInitialized || !g_pWbemServices) return 0.0f;
    
    IEnumWbemClassObject* pEnumerator = nullptr;
    HRESULT hr;
    
    // Try OpenHardwareMonitor sensor query
    hr = g_pWbemServices->ExecQuery(
        _bstr_t(L"WQL"),
        _bstr_t(L"SELECT Value FROM Sensor WHERE SensorType='Temperature' AND Name LIKE '%CPU%'"),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        nullptr, &pEnumerator);
    
    if (FAILED(hr)) {
        // Try MSAcpi_ThermalZoneTemperature (built-in, but less reliable)
        hr = g_pWbemServices->ExecQuery(
            _bstr_t(L"WQL"),
            _bstr_t(L"SELECT CurrentTemperature FROM MSAcpi_ThermalZoneTemperature"),
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
            nullptr, &pEnumerator);
    }
    
    if (FAILED(hr)) return 0.0f;
    
    float temp = 0.0f;
    IWbemClassObject* pObj = nullptr;
    ULONG returned = 0;
    
    if (pEnumerator->Next(WBEM_INFINITE, 1, &pObj, &returned) == S_OK && returned > 0) {
        VARIANT vtProp;
        VariantInit(&vtProp);
        
        // Try "Value" first (OpenHardwareMonitor)
        hr = pObj->Get(L"Value", 0, &vtProp, nullptr, nullptr);
        if (SUCCEEDED(hr) && vtProp.vt == VT_R4) {
            temp = vtProp.fltVal;
        } else {
            // Try "CurrentTemperature" (MSAcpi - returns in tenths of Kelvin)
            VariantClear(&vtProp);
            hr = pObj->Get(L"CurrentTemperature", 0, &vtProp, nullptr, nullptr);
            if (SUCCEEDED(hr) && (vtProp.vt == VT_I4 || vtProp.vt == VT_UI4)) {
                // Convert from tenths of Kelvin to Celsius
                temp = (vtProp.lVal / 10.0f) - 273.15f;
            }
        }
        VariantClear(&vtProp);
        pObj->Release();
    }
    
    pEnumerator->Release();
    return temp;
}

// ═══════════════════════════════════════════════════════════════════════════
// PawnIO Driver Installation — required for LibreHardwareMonitor
// ═══════════════════════════════════════════════════════════════════════════

static HKEY OpenPawnIOUninstallKeyRead()
{
    HKEY hKey = nullptr;
    if (RegOpenKeyExA(
            HKEY_LOCAL_MACHINE,
            "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PawnIO",
            0, KEY_READ, &hKey) == ERROR_SUCCESS)
        return hKey;
    if (RegOpenKeyExA(
            HKEY_LOCAL_MACHINE,
            "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PawnIO",
            0, KEY_READ | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS)
        return hKey;
    return nullptr;
}

static bool RegQuerySzA(HKEY hKey, const char* valueName, char* buf, DWORD cap)
{
    if (!cap) return false;
    DWORD sz = cap;
    DWORD typ = 0;
    if (RegQueryValueExA(hKey, valueName, nullptr, &typ,
                         reinterpret_cast<LPBYTE>(buf), &sz) != ERROR_SUCCESS)
        return false;
    if (typ != REG_SZ && typ != REG_EXPAND_SZ)
        return false;
    buf[cap - 1] = '\0';
    return buf[0] != '\0';
}

// Check if PawnIO driver is installed via registry
// LibreHardwareMonitor checks: SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PawnIO
static bool IsPawnIOInstalled()
{
    HKEY hKey = OpenPawnIOUninstallKeyRead();
    if (!hKey)
        return false;
    char versionStr[64] = {0};
    bool ok = RegQuerySzA(hKey, "DisplayVersion", versionStr, sizeof(versionStr));
    RegCloseKey(hKey);
    return ok;
}

static bool GetFileVersionQuad(const char* path, DWORD* verMS, DWORD* verLS)
{
    if (!path || !path[0] || !verMS || !verLS)
        return false;
    DWORD dummy = 0;
    DWORD verSize = GetFileVersionInfoSizeA(path, &dummy);
    if (!verSize)
        return false;
    std::vector<BYTE> data(verSize);
    if (!GetFileVersionInfoA(path, 0, verSize, data.data()))
        return false;
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT ffiLen = 0;
    if (!VerQueryValueA(data.data(), "\\", reinterpret_cast<void**>(&ffi), &ffiLen) || !ffi ||
        ffiLen < sizeof(VS_FIXEDFILEINFO))
        return false;
    *verMS = ffi->dwFileVersionMS;
    *verLS = ffi->dwFileVersionLS;
    return (*verMS | *verLS) != 0;
}

// Write embedded PawnIO_setup.exe bytes to an absolute path (overwrites).
static bool WriteEmbeddedPawnIOSetupToPath(const char* destPath)
{
    HMODULE hModule = GetModuleHandle(nullptr);
    HRSRC hResource = FindResource(hModule, MAKEINTRESOURCE(IDR_PAWNIO_SETUP), RT_RCDATA);
    if (!hResource)
        return false;
    HGLOBAL hLoadedResource = LoadResource(hModule, hResource);
    if (!hLoadedResource)
        return false;
    LPVOID pResourceData = LockResource(hLoadedResource);
    DWORD dwResourceSize = SizeofResource(hModule, hResource);
    if (!pResourceData || dwResourceSize == 0)
        return false;
    HANDLE hFile = CreateFileA(destPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;
    DWORD bytesWritten = 0;
    BOOL writeResult = WriteFile(hFile, pResourceData, dwResourceSize, &bytesWritten, nullptr);
    CloseHandle(hFile);
    if (!writeResult || bytesWritten != dwResourceSize) {
        DeleteFileA(destPath);
        return false;
    }
    return true;
}

static bool GetBundledPawnIOSetupVersion(DWORD* verMS, DWORD* verLS)
{
    char tempPath[MAX_PATH];
    char tempFile[MAX_PATH];
    if (GetTempPathA(MAX_PATH, tempPath) == 0)
        return false;
    snprintf(tempFile, MAX_PATH, "%sFPSOverlay_PawnIO_setup_%llu.exe", tempPath,
             (unsigned long long)GetTickCount64());
    if (!WriteEmbeddedPawnIOSetupToPath(tempFile))
        return false;
    bool ok = GetFileVersionQuad(tempFile, verMS, verLS);
    DeleteFileA(tempFile);
    return ok;
}

static void StripPathFromDisplayIcon(const char* raw, char* out, size_t cap)
{
    if (!cap) return;
    out[0] = '\0';
    if (!raw || !raw[0]) return;
    const char* p = raw;
    if (*p == '"') {
        ++p;
        const char* q = strchr(p, '"');
        if (q) {
            size_t n = (size_t)(q - p);
            if (n >= cap) n = cap - 1;
            memcpy(out, p, n);
            out[n] = '\0';
        }
        return;
    }
    const char* comma = strchr(p, ',');
    size_t n = comma ? (size_t)(comma - p) : strlen(p);
    while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

static void ExtractFirstQuotedPath(const char* raw, char* out, size_t cap)
{
    if (!cap) return;
    out[0] = '\0';
    if (!raw || !raw[0]) return;
    while (*raw == ' ' || *raw == '\t') ++raw;
    if (*raw == '"') {
        StripPathFromDisplayIcon(raw, out, cap);
        return;
    }
    const char* sp = raw;
    const char* end = raw;
    while (*end && *end != ' ' && *end != '\t') ++end;
    size_t n = (size_t)(end - sp);
    if (n >= cap) n = cap - 1;
    memcpy(out, sp, n);
    out[n] = '\0';
}

static bool PathFileExistsA_(const char* p)
{
    return p && p[0] && GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

static bool TryJoinExe(char* out, size_t cap, const char* dir, const char* exeName)
{
    if (!dir || !dir[0]) return false;
    size_t len = strlen(dir);
    while (len > 0 && (dir[len - 1] == '\\' || dir[len - 1] == '/')) len--;
    int n = snprintf(out, cap, "%.*s\\%s", (int)len, dir, exeName);
    return n > 0 && (size_t)n < cap && PathFileExistsA_(out);
}

static bool ParseDisplayVersionToQuad(const char* s, DWORD* verMS, DWORD* verLS)
{
    if (!s || !verMS || !verLS) return false;
    while (*s == ' ' || *s == '\t') ++s;
    if (*s == 'v' || *s == 'V') ++s;
    int v[4] = {0, 0, 0, 0};
    int n = 0;
    const char* p = s;
    while (n < 4 && *p) {
        char* end = nullptr;
        long x = strtol(p, &end, 10);
        if (end == p) break;
        v[n++] = (int)x;
        p = end;
        if (*p == '.' || *p == ',') ++p;
        else if (*p == '-' || *p == '+') break;
        else if (*p && (*p < '0' || *p > '9')) break;
    }
    if (n == 0) return false;
    WORD maj = (WORD)(n >= 1 ? v[0] : 0);
    WORD minr = (WORD)(n >= 2 ? v[1] : 0);
    WORD pat = (WORD)(n >= 3 ? v[2] : 0);
    WORD bld = (WORD)(n >= 4 ? v[3] : 0);
    *verMS = ((DWORD)maj << 16) | (DWORD)minr;
    *verLS = ((DWORD)pat << 16) | (DWORD)bld;
    return true;
}

// Installed version: prefer file version from uninstall-related paths, else DisplayVersion string.
static bool GetInstalledPawnIOVersionQuad(DWORD* verMS, DWORD* verLS)
{
    if (!verMS || !verLS) return false;
    *verMS = *verLS = 0;
    HKEY hKey = OpenPawnIOUninstallKeyRead();
    if (!hKey)
        return false;

    char buf512[512];
    char path[MAX_PATH];

    auto tryPath = [&](const char* candidate) -> bool {
        if (!candidate || !candidate[0]) return false;
        if (!PathFileExistsA_(candidate)) return false;
        return GetFileVersionQuad(candidate, verMS, verLS);
    };

    bool got = false;
    if (RegQuerySzA(hKey, "DisplayIcon", buf512, sizeof(buf512))) {
        StripPathFromDisplayIcon(buf512, path, sizeof(path));
        got = tryPath(path);
    }
    if (!got) {
        DWORD typ = 0;
        DWORD sz = sizeof(buf512);
        if (RegQueryValueExA(hKey, "InstallLocation", nullptr, &typ,
                             reinterpret_cast<LPBYTE>(buf512), &sz) == ERROR_SUCCESS &&
            (typ == REG_SZ || typ == REG_EXPAND_SZ) && buf512[0]) {
            buf512[sizeof(buf512) - 1] = '\0';
            if (typ == REG_EXPAND_SZ) {
                char expanded[512];
                if (ExpandEnvironmentStringsA(buf512, expanded, sizeof(expanded)) > 1)
                    snprintf(buf512, sizeof(buf512), "%s", expanded);
            }
            if (TryJoinExe(path, sizeof(path), buf512, "PawnIO.exe") && tryPath(path)) got = true;
            if (!got && TryJoinExe(path, sizeof(path), buf512, "PawnIO_setup.exe") && tryPath(path)) got = true;
        }
    }
    if (!got && RegQuerySzA(hKey, "UninstallString", buf512, sizeof(buf512))) {
        ExtractFirstQuotedPath(buf512, path, sizeof(path));
        if (!got && tryPath(path)) got = true;
    }
    if (!got)
        got = tryPath("C:\\Windows\\System32\\drivers\\PawnIO.sys");

    char disp[64] = {0};
    if (!got && RegQuerySzA(hKey, "DisplayVersion", disp, sizeof(disp)))
        got = ParseDisplayVersionToQuad(disp, verMS, verLS);

    RegCloseKey(hKey);
    return got && (*verMS != 0 || *verLS != 0);
}

static int CompareFileVersionQuad(DWORD aMS, DWORD aLS, DWORD bMS, DWORD bLS)
{
    if (aMS != bMS) return (aMS > bMS) ? 1 : -1;
    if (aLS != bLS) return (aLS > bLS) ? 1 : -1;
    return 0;
}

// Win32_OperatingSystem.LastBootUpTime as CIM datetime -> FILETIME (UTC).
// The trailing +/-minutes is the WMI UTC offset; ignoring it made last boot look "newer" than the
// install marker and cleared PawnIORequiresReboot incorrectly on some locales.
static bool CimDateTimeStringToFileTimeUtc(const wchar_t* wsz, FILETIME* pft)
{
    if (!wsz || !pft) return false;
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (swscanf_s(wsz, L"%4d%2d%2d%2d%2d%2d", &y, &mo, &d, &h, &mi, &s) != 6)
        return false;

    int biasMin = 0;
    const wchar_t* dot = wcschr(wsz, L'.');
    if (dot) {
        const wchar_t* p = dot + 1;
        while (*p && iswdigit(*p)) ++p;
        if (*p == L'+' || *p == L'-')
            biasMin = _wtoi(p);
    } else {
        const wchar_t* q = wsz + 14;
        while (*q && *q != L'+' && *q != L'-') ++q;
        if (*q == L'+' || *q == L'-')
            biasMin = _wtoi(q);
    }

    struct tm t = {};
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = s;
    time_t tt = _mkgmtime(&t);
    if (tt == (time_t)-1) return false;
    tt -= (time_t)biasMin * 60;

    ULARGE_INTEGER u;
    u.QuadPart = (unsigned long long)(tt + 11644473600LL) * 10000000ULL;
    *pft = *(FILETIME*)&u;
    return true;
}

static bool WmiQueryLastBootUtcFileTime(FILETIME* pft)
{
    if (!pft) return false;
    pft->dwLowDateTime = pft->dwHighDateTime = 0;
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comNeedsUninit = SUCCEEDED(hrInit);
    if (FAILED(hrInit) && hrInit != RPC_E_CHANGED_MODE)
        return false;

    IWbemLocator* pLoc = nullptr;
    IWbemServices* pSvc = nullptr;
    IEnumWbemClassObject* pEnum = nullptr;
    IWbemClassObject* pObj = nullptr;
    VARIANT vt;
    VariantInit(&vt);
    ULONG ret = 0;
    bool ok = false;

    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator,
                                  reinterpret_cast<void**>(&pLoc));
    if (FAILED(hr) || !pLoc) goto wmi_cleanup;

    hr = pLoc->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), nullptr, nullptr, nullptr, 0, nullptr, nullptr, &pSvc);
    if (FAILED(hr) || !pSvc) goto wmi_cleanup;

    hr = CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                           RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    if (FAILED(hr)) goto wmi_cleanup;

    hr = pSvc->ExecQuery(_bstr_t(L"WQL"), _bstr_t(L"SELECT LastBootUpTime FROM Win32_OperatingSystem"),
                         WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnum);
    if (FAILED(hr) || !pEnum) goto wmi_cleanup;

    if (pEnum->Next(WBEM_INFINITE, 1, &pObj, &ret) != S_OK || !pObj) goto wmi_cleanup;

    hr = pObj->Get(L"LastBootUpTime", 0, &vt, nullptr, nullptr);
    if (FAILED(hr)) goto wmi_cleanup;
    if (vt.vt == VT_BSTR && vt.bstrVal)
        ok = CimDateTimeStringToFileTimeUtc(vt.bstrVal, pft);

wmi_cleanup:
    VariantClear(&vt);
    if (pObj) pObj->Release();
    if (pEnum) pEnum->Release();
    if (pSvc) pSvc->Release();
    if (pLoc) pLoc->Release();
    if (comNeedsUninit) CoUninitialize();
    return ok;
}

static bool CommitPawnIORebootPendingToIni()
{
    InitConfigPath();
    FILETIME ft = {};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    char hex[20];
    snprintf(hex, sizeof(hex), "%016llX", (unsigned long long)u.QuadPart);

    for (int attempt = 0; attempt < 12; attempt++) {
        if (!WritePawnIORebootPendingStateFile(hex)) {
            Sleep(30);
            continue;
        }
        WriteIniStr("App", "PawnIOInstallUtcHex", hex);
        WriteIniInt("App", "PawnIORequiresReboot", 1);

        char verifyFile[48] = {};
        const bool fileOk =
            ReadPawnIORebootPendingStateFile(verifyFile, sizeof(verifyFile)) && (_stricmp(verifyFile, hex) == 0);
        if (!fileOk) {
            Sleep(30);
            continue;
        }

        if (ReadIniInt("App", "PawnIORequiresReboot", 0) == 0) {
            WriteIniStr("App", "PawnIOInstallUtcHex", hex);
            WriteIniInt("App", "PawnIORequiresReboot", 1);
        }
        char verify[40] = {};
        ReadIniStr("App", "PawnIOInstallUtcHex", verify, sizeof(verify));
        if (_stricmp(verify, hex) != 0) {
            WriteIniStr("App", "PawnIOInstallUtcHex", hex);
            WriteIniInt("App", "PawnIORequiresReboot", 1);
            ReadIniStr("App", "PawnIOInstallUtcHex", verify, sizeof(verify));
        }
        if (ReadIniInt("App", "PawnIORequiresReboot", 0) != 0 && _stricmp(verify, hex) == 0)
            return true;

        // Sidecar is authoritative; INI can be flaky with some tools.
        if (fileOk)
            return true;

        Sleep(30);
    }
    return false;
}

static void ClearPawnIORebootPendingAll()
{
    InitConfigPath();
    WriteIniInt("App", "PawnIORequiresReboot", 0);
    WriteIniStr("App", "PawnIOInstallUtcHex", "");
    DeletePawnIORebootPendingStateFile();
}

// Approximate last boot in the same UTC FILETIME domain as GetSystemTimeAsFileTime (install marker).
// Avoids WMI/CIM timezone mismatches that kept the reboot gate latched after a real restart.
static bool ApproxLastBootFromUptimeFileTime(FILETIME* pft)
{
    if (!pft) return false;
    FILETIME nowFt = {};
    GetSystemTimeAsFileTime(&nowFt);
    ULARGE_INTEGER now;
    now.LowPart = nowFt.dwLowDateTime;
    now.HighPart = nowFt.dwHighDateTime;
    const ULONGLONG uptime100ns = GetTickCount64() * 10000ULL;
    if (uptime100ns == 0 || now.QuadPart <= uptime100ns)
        return false;
    ULARGE_INTEGER boot;
    boot.QuadPart = now.QuadPart - uptime100ns;
    pft->dwLowDateTime = boot.LowPart;
    pft->dwHighDateTime = boot.HighPart;
    return true;
}

// bootFt and markerFt must be comparable (same epoch). minSlack100ns avoids borderline ties.
static bool LastBootUtcPlausiblyAfterPawnIOInstall(const FILETIME* bootFt, const FILETIME* markerFt,
                                                   ULONGLONG minSlack100ns)
{
    ULARGE_INTEGER b, m;
    b.LowPart = bootFt->dwLowDateTime;
    b.HighPart = bootFt->dwHighDateTime;
    m.LowPart = markerFt->dwLowDateTime;
    m.HighPart = markerFt->dwHighDateTime;
    if (b.QuadPart <= m.QuadPart)
        return false;
    return (b.QuadPart - m.QuadPart) >= minSlack100ns;
}

static bool ReadPawnIOInstallMarkerFileTime(FILETIME* pft)
{
    if (!pft) return false;
    char hex[32] = {};
    ReadIniStr("App", "PawnIOInstallUtcHex", hex, sizeof(hex));
    if (strlen(hex) != 16) return false;
    unsigned long long v = 0;
    if (sscanf_s(hex, "%llx", &v) != 1) return false;
    ULARGE_INTEGER u;
    u.QuadPart = v;
    pft->dwLowDateTime = u.LowPart;
    pft->dwHighDateTime = u.HighPart;
    return true;
}

static bool AcquireShutdownPrivilege()
{
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return false;
    TOKEN_PRIVILEGES tkp = {};
    tkp.PrivilegeCount = 1;
    tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueA(nullptr, "SeShutdownPrivilege", &tkp.Privileges[0].Luid)) {
        CloseHandle(hToken);
        return false;
    }
    BOOL ok = AdjustTokenPrivileges(hToken, FALSE, &tkp, 0, nullptr, nullptr);
    CloseHandle(hToken);
    return ok != FALSE;
}

// Same restart choice whenever a reboot is required (post-install / gate / WMI / bad marker).
// MB_SYSTEMMODAL + topmost + foreground so it is not lost behind other windows.
static void ForceShowPawnIORestartRequiredDialogThenExit(const wchar_t* situationLead)
{
    const UINT kMb =
        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_TOPMOST | MB_SETFOREGROUND | MB_SYSTEMMODAL;

    wchar_t body[2048];
    _snwprintf_s(body, _TRUNCATE,
                 L"%s\n\n"
                 L"重要提示：重启前请先保存其他应用中的工作，未保存的数据可能会丢失。\n\n"
                 L"FPS Overlay 需要完整重启系统后才能运行。\n\n"
                 L"选择“是”：现在重启此电脑（FPS Overlay 会先关闭）\n"
                 L"选择“否”：稍后重启（FPS Overlay 会关闭；准备好后请使用“开始”>“电源”>“重启”）\n\n",
                 situationLead ? situationLead : L"");

    const int r = MessageBoxW(nullptr, body, L"FPS Overlay - 需要重启", kMb);

    if (r == IDYES) {
        if (AcquireShutdownPrivilege()) {
            ExitWindowsEx(EWX_REBOOT | EWX_FORCEIFHUNG,
                          SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_MINOR_INSTALLATION |
                              SHTDN_REASON_FLAG_PLANNED);
        }
        MessageBoxW(nullptr,
                      L"无法自动重启。请手动重启电脑（开始 > 电源 > 重启），然后再次启动 FPS Overlay。",
                      L"FPS Overlay",
                      MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND | MB_SYSTEMMODAL);
    }
    ExitProcess(0);
}

// Block startup until Windows has rebooted since the last PawnIO install/update (config.ini [App] + sidecar state).
static void CheckPawnIORebootGateOrExit()
{
    InitConfigPath();

    char hex[48] = {};
    ReadIniStr("App", "PawnIOInstallUtcHex", hex, sizeof(hex));
    char fileHex[48] = {};
    const bool fileOk = ReadPawnIORebootPendingStateFile(fileHex, sizeof(fileHex));
    int rb = ReadIniInt("App", "PawnIORequiresReboot", 0);

    if (fileOk) {
        if (rb == 0 || _stricmp(hex, fileHex) != 0 || strlen(hex) != 16) {
            WriteIniStr("App", "PawnIOInstallUtcHex", fileHex);
            WriteIniInt("App", "PawnIORequiresReboot", 1);
        }
        snprintf(hex, sizeof(hex), "%s", fileHex);
        rb = 1;
    }

    if (rb == 0)
        return;

    FILETIME marker = {};
    if (!ReadPawnIOInstallMarkerFileTime(&marker)) {
        ForceShowPawnIORestartRequiredDialogThenExit(
            L"PawnIO 安装或更新后，FPS Overlay 正在等待系统重启，"
            L"但 config.ini 中的重启标记缺失或无效。\n\n"
            L"如果重启 Windows 后仍然出现此提示，请删除 config.ini 中 [App] 下的 PawnIORequiresReboot 和 "
            L"PawnIOInstallUtcHex，并删除 overlay.exe 旁边的 fpsoverlay-pawnio-reboot.state。");
        return;
    }

    FILETIME bootApprox = {};
    const bool haveBootApprox = ApproxLastBootFromUptimeFileTime(&bootApprox);
    if (haveBootApprox && LastBootUtcPlausiblyAfterPawnIOInstall(&bootApprox, &marker, 3ULL * 10000000ULL)) {
        ClearPawnIORebootPendingAll();
        return;
    }

    FILETIME bootWmi = {};
    const bool haveBootWmi = WmiQueryLastBootUtcFileTime(&bootWmi);
    if (haveBootWmi && LastBootUtcPlausiblyAfterPawnIOInstall(&bootWmi, &marker, 45ULL * 10000000ULL)) {
        ClearPawnIORebootPendingAll();
        return;
    }

    if (!haveBootApprox && !haveBootWmi) {
        ForceShowPawnIORestartRequiredDialogThenExit(
            L"FPS Overlay 无法确认此电脑是否已在 PawnIO 安装或更新后重启"
            L"（Windows 未能报告上次启动时间）。仍然需要完整重启一次。");
        return;
    }

    ForceShowPawnIORestartRequiredDialogThenExit(
        L"使用 FPS Overlay 前必须先重启 Windows。\n\n"
        L"PawnIO 之前已经安装或更新，但当前系统会话还没有完成完整重启。");
}

// Called after PawnIO_setup exits 0 and reboot pending is already committed to config.ini.
static void RequireSystemRestartAfterPawnIOSetup()
{
    ForceShowPawnIORestartRequiredDialogThenExit(L"PawnIO 已成功安装或更新。");
}

// Extract embedded PawnIO_setup.exe and run it (-install). Success only if the process exits with code 0.
static bool ExtractAndRunPawnIOSetup()
{
    char tempPath[MAX_PATH];
    char tempFile[MAX_PATH];
    if (GetTempPathA(MAX_PATH, tempPath) == 0)
        return false;
    snprintf(tempFile, MAX_PATH, "%sFPSOverlay_PawnIO_setup_run_%llu.exe", tempPath,
             (unsigned long long)GetTickCount64());
    if (!WriteEmbeddedPawnIOSetupToPath(tempFile))
        return false;

    SHELLEXECUTEINFOA sei = { sizeof(sei) };
    sei.lpVerb = "runas";
    sei.lpFile = tempFile;
    sei.lpParameters = "-install";
    sei.nShow = SW_HIDE;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;

    if (!ShellExecuteExA(&sei)) {
        DeleteFileA(tempFile);
        return false;
    }

    if (!sei.hProcess) {
        DeleteFileA(tempFile);
        return false;
    }

    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD exitCode = (DWORD)-1;
    if (!GetExitCodeProcess(sei.hProcess, &exitCode)) {
        CloseHandle(sei.hProcess);
        DeleteFileA(tempFile);
        return false;
    }
    CloseHandle(sei.hProcess);
    DeleteFileA(tempFile);

    if (exitCode == STILL_ACTIVE)
        return false;
    return exitCode == 0;
}

// True if bundled PawnIO_setup.exe is newer than the installed build (skip if we cannot read bundled version).
static bool IsPawnIOOutdatedVsBundled()
{
    DWORD bundledMS = 0, bundledLS = 0;
    if (!GetBundledPawnIOSetupVersion(&bundledMS, &bundledLS))
        return false;
    DWORD installedMS = 0, installedLS = 0;
    if (!GetInstalledPawnIOVersionQuad(&installedMS, &installedLS))
        return false;
    return CompareFileVersionQuad(installedMS, installedLS, bundledMS, bundledLS) < 0;
}

// Block until PawnIO is present and at least as new as the bundled installer, or exit the app.
[[maybe_unused]] static void EnforcePawnIOOrExit()
{
    CheckPawnIORebootGateOrExit();

    for (;;) {
        if (!IsPawnIOInstalled()) {
            int r = MessageBoxW(
                nullptr,
                L"FPS Overlay 需要 PawnIO 驱动。\n\n"
                L"LibreHardwareMonitor 会通过它读取 CPU 和 GPU 温度。"
                L"缺少该驱动时，应用无法继续运行。\n\n"
                L"点击“确定”安装 PawnIO。\n"
                L"点击“取消”退出。",
                L"FPS Overlay - 需要 PawnIO",
                MB_OKCANCEL | MB_ICONWARNING | MB_TOPMOST);
            if (r != IDOK)
                ExitProcess(1);
            if (!ExtractAndRunPawnIOSetup()) {
                MessageBoxW(nullptr,
                              L"PawnIO 安装没有成功完成。安装程序返回了错误"
                              L"（例如，需要先移除现有 PawnIO 版本）。\n\n"
                              L"请在 Windows 设置 > 应用 > 已安装的应用中卸载 PawnIO，然后回到这里再次点击“确定”。",
                              L"FPS Overlay", MB_OK | MB_ICONERROR | MB_TOPMOST);
                continue;
            }
            if (!CommitPawnIORebootPendingToIni()) {
                MessageBoxW(nullptr,
                              L"FPS Overlay 无法保存重启要求（overlay.exe 旁边的 config.ini 或 "
                              L"fpsoverlay-pawnio-reboot.state）。请确认此文件夹可写，然后再次尝试安装 PawnIO。",
                              L"FPS Overlay",
                              MB_OK | MB_ICONERROR | MB_TOPMOST);
                ExitProcess(1);
            }
            RequireSystemRestartAfterPawnIOSetup();
        }

        if (IsPawnIOOutdatedVsBundled()) {
            int r = MessageBoxW(
                nullptr,
                L"你的 PawnIO 驱动版本低于 FPS Overlay 随附版本。\n\n"
                L"过旧的 PawnIO 可能导致 LibreHardwareMonitor 无法正常工作（温度缺失或错误）。"
                L"必须更新后才能继续。\n\n"
                L"点击“确定”立即更新（会替换现有安装）。\n"
                L"点击“取消”退出。",
                L"FPS Overlay - 需要更新 PawnIO",
                MB_OKCANCEL | MB_ICONWARNING | MB_TOPMOST);
            if (r != IDOK)
                ExitProcess(1);
            if (!ExtractAndRunPawnIOSetup()) {
                MessageBoxW(nullptr,
                              L"PawnIO 更新没有成功完成。安装程序返回了错误"
                              L"（例如，需要先移除旧版本再重新安装）。\n\n"
                              L"请在 Windows 设置 > 应用 > 已安装的应用中卸载 PawnIO，然后回到这里再次点击“确定”。",
                              L"FPS Overlay", MB_OK | MB_ICONERROR | MB_TOPMOST);
                continue;
            }
            if (!CommitPawnIORebootPendingToIni()) {
                MessageBoxW(nullptr,
                              L"FPS Overlay 无法保存重启要求（overlay.exe 旁边的 config.ini 或 "
                              L"fpsoverlay-pawnio-reboot.state）。请确认此文件夹可写，然后再次尝试更新 PawnIO。",
                              L"FPS Overlay",
                              MB_OK | MB_ICONERROR | MB_TOPMOST);
                ExitProcess(1);
            }
            RequireSystemRestartAfterPawnIOSetup();
        }
        break;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// LibreHardwareMonitor (LHWM) — cross-vendor hardware monitoring
// ═══════════════════════════════════════════════════════════════════════════
// True for LHWM "GPU" hardware nodes we should bind sensors to (discrete + integrated).
// Names like "AMD Radeon(TM) Graphics" (ROG Ally / APUs) never matched only "Radeon RX" / GeForce,
// so g_gpuCount stayed 0 and GPU stats were N/A.
static std::string LowerCopy(const std::string& s);

static bool IsLhwmGpuHardwareNode(const std::string& name)
{
    if (name.find("GeForce") != std::string::npos) return true;
    if (name.find("RTX") != std::string::npos) return true;
    if (name.find("GTX") != std::string::npos) return true;
    if (name.find("Radeon RX") != std::string::npos) return true;
    if (name.find("Radeon Pro") != std::string::npos) return true;
    if (name.find("NVIDIA") != std::string::npos) return true;
    if (name.find("Intel") != std::string::npos && name.find("Arc") != std::string::npos) return true;

    // AMD integrated: "Radeon" present but not discrete RX / Pro product lines
    if (name.find("Radeon") != std::string::npos &&
        name.find("Radeon RX") == std::string::npos && name.find("Radeon Pro") == std::string::npos) {
        if (name.find("Graphics") != std::string::npos) return true;
        if (name.find("780M") != std::string::npos || name.find("760M") != std::string::npos ||
            name.find("740M") != std::string::npos ||
            name.find("680M") != std::string::npos || name.find("660M") != std::string::npos)
            return true;
    }

    if (name.find("Intel") != std::string::npos && name.find("Graphics") != std::string::npos) {
        if (name.find("Intel(R) Graphics") != std::string::npos) return true;
        if (name.find("UHD") != std::string::npos || name.find("HD Graphics") != std::string::npos ||
            name.find("Iris") != std::string::npos)
            return true;
    }

    return false;
}

// Find an existing GPU in the list by name, or return -1
static int FindGpuByName(const char* name) {
    for (int i = 0; i < g_gpuCount; i++) {
        if (strcmp(g_gpuList[i].name, name) == 0) return i;
    }
    return -1;
}

static bool PathContainsFragment(const std::string& path, const char* fragment)
{
    return fragment && fragment[0] && path.find(fragment) != std::string::npos;
}

static bool GpuInfoHasPathFragment(const GpuInfo& g, const char* fragment)
{
    if (PathContainsFragment(g.tempPath, fragment) || PathContainsFragment(g.loadPath, fragment) ||
        PathContainsFragment(g.vramUsedPath, fragment) || PathContainsFragment(g.vramTotalPath, fragment) ||
        PathContainsFragment(g.powerPath, fragment) || PathContainsFragment(g.fanPath, fragment))
        return true;
    for (const auto& e : g.coreClockOpts)
        if (PathContainsFragment(e.second, fragment))
            return true;
    return false;
}

static bool GpuInfoHasExactPath(const GpuInfo& g, const char* path)
{
    if (!path || !path[0])
        return false;
    if (g.tempPath == path || g.loadPath == path || g.vramUsedPath == path || g.vramTotalPath == path ||
        g.powerPath == path || g.fanPath == path)
        return true;
    for (const auto& e : g.coreClockOpts)
        if (e.second == path)
            return true;
    return false;
}

static bool GpuLooksDiscrete(const GpuInfo& g)
{
    std::string n = LowerCopy(g.name);
    return n.find("nvidia") != std::string::npos || n.find("geforce") != std::string::npos ||
           n.find("rtx") != std::string::npos || n.find("gtx") != std::string::npos ||
           n.find("radeon rx") != std::string::npos || n.find("radeon pro") != std::string::npos;
}

static int GpuTelemetryScore(const GpuInfo& g)
{
    int score = 0;
    if (GpuLooksDiscrete(g)) score += 30;
    if (!g.tempPath.empty()) score += 40;
    if (!g.loadPath.empty()) score += 10;
    if (!g.vramUsedPath.empty() && !g.vramTotalPath.empty()) score += 25;
    if (!g.powerPath.empty()) score += 20;
    if (!g.coreClockOpts.empty()) score += 5;
    if (!g.memTempPath.empty()) score += 5;
    if (!g.fanPath.empty()) score += 2;
    return score;
}

static int ChooseBestTelemetryGpuIndex()
{
    int best = -1;
    int bestScore = -1;
    for (int i = 0; i < g_gpuCount; ++i) {
        const int score = GpuTelemetryScore(g_gpuList[i]);
        if (score > bestScore) {
            best = i;
            bestScore = score;
        }
    }
    return best;
}

static bool ShouldPreferTelemetryGpu(int configuredIndex, int bestIndex)
{
    if (configuredIndex < 0 || configuredIndex >= g_gpuCount ||
        bestIndex < 0 || bestIndex >= g_gpuCount ||
        configuredIndex == bestIndex)
        return false;

    const GpuInfo& configured = g_gpuList[configuredIndex];
    const GpuInfo& best = g_gpuList[bestIndex];

    const bool configuredMissingCore =
        configured.tempPath.empty() ||
        configured.vramUsedPath.empty() ||
        configured.vramTotalPath.empty() ||
        configured.powerPath.empty();

    const bool bestHasCore =
        !best.tempPath.empty() &&
        !best.vramUsedPath.empty() &&
        !best.vramTotalPath.empty();

    if (!configuredMissingCore || !bestHasCore)
        return false;

    return GpuTelemetryScore(best) >= GpuTelemetryScore(configured) + 35;
}

static int ChooseBestGpuIndex()
{
    if (g_gpuCount <= 0)
        return 0;

    const int telemetryBest = ChooseBestTelemetryGpuIndex();

    if (g_Config.gpuCoreFreqPath[0]) {
        for (int i = 0; i < g_gpuCount; ++i) {
            if (GpuInfoHasExactPath(g_gpuList[i], g_Config.gpuCoreFreqPath)) {
                if (ShouldPreferTelemetryGpu(i, telemetryBest))
                    return telemetryBest;
                return i;
            }
        }
        if (strstr(g_Config.gpuCoreFreqPath, "/gpu-nvidia/")) {
            for (int i = 0; i < g_gpuCount; ++i)
                if (GpuInfoHasPathFragment(g_gpuList[i], "/gpu-nvidia/"))
                    return i;
        }
        if (strstr(g_Config.gpuCoreFreqPath, "/gpu-amd/")) {
            for (int i = 0; i < g_gpuCount; ++i)
                if (GpuInfoHasPathFragment(g_gpuList[i], "/gpu-amd/"))
                    return i;
        }
    }

    if (g_Config.selectedGpu >= 0 && g_Config.selectedGpu < g_gpuCount) {
        if (ShouldPreferTelemetryGpu(g_Config.selectedGpu, telemetryBest))
            return telemetryBest;
        if (GpuLooksDiscrete(g_gpuList[g_Config.selectedGpu]))
            return g_Config.selectedGpu;
    }

    if (telemetryBest >= 0 && GpuLooksDiscrete(g_gpuList[telemetryBest]))
        return telemetryBest;

    for (int i = 0; i < g_gpuCount; ++i)
        if (GpuLooksDiscrete(g_gpuList[i]))
            return i;

    if (telemetryBest >= 0)
        return telemetryBest;

    if (g_Config.selectedGpu >= 0 && g_Config.selectedGpu < g_gpuCount)
        return g_Config.selectedGpu;
    return 0;
}

static bool IsGpuMemoryClockSensor(const std::string& name)
{
    std::string n;
    n.reserve(name.size());
    for (char c : name)
        n += (char)tolower((unsigned char)c);
    if (n.find("memory") != std::string::npos && n.find("clock") != std::string::npos)
        return true;
    if (n.find("hbm") != std::string::npos)
        return true;
    if (n.find("vram") != std::string::npos && n.find("clock") != std::string::npos)
        return true;
    return false;
}

// LHWM exposes several "*Memory Total*" sensors on APUs (e.g. ROG Ally). "Shared Memory Total"
// contains the substring "Memory Total" and can overwrite "GPU Memory Total" (last match wins).
// Prefer the same row users see in LHM: GPU Memory Total, then dedicated/D3D, not shared pool.
static int VramTotalSensorPriority(const std::string& sensorName)
{
    std::string n;
    n.reserve(sensorName.size());
    for (char c : sensorName)
        n += (char)tolower((unsigned char)c);
    if (n.find("shared") != std::string::npos)
        return -1;
    if (n.find("gpu memory total") != std::string::npos)
        return 100;
    if (n.find("dedicated memory total") != std::string::npos || n.find("d3d dedicated") != std::string::npos)
        return 80;
    if (n.find("memory total") != std::string::npos)
        return 50;
    return -1;
}

// LHM AMD "Soc" (SoC) — must not match "Socket" (substring "Soc" inside "Socket").
static std::string LowerCopy(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        out += (char)tolower((unsigned char)c);
    return out;
}

static bool IsRogStrixG815LR()
{
    static int cached = -1;
    if (cached >= 0)
        return cached != 0;
    char model[256] = {};
    DWORD size = sizeof(model);
    const LSTATUS status = RegGetValueA(
        HKEY_LOCAL_MACHINE,
        "HARDWARE\\DESCRIPTION\\System\\BIOS",
        "SystemProductName",
        RRF_RT_REG_SZ,
        nullptr,
        model,
        &size);
    const std::string lower = status == ERROR_SUCCESS ? LowerCopy(model) : std::string();
    cached = lower.find("g815lr") != std::string::npos ? 1 : 0;
    return cached != 0;
}

static bool QueryDisplayBrightnessPercent(bool acOnline, float* percentOut)
{
    if (!percentOut)
        return false;
    static ULONGLONG lastQueryTick = 0;
    static bool cachedAvailable = false;
    static float cachedPercent = 50.0f;
    static bool cachedAcOnline = true;
    const ULONGLONG now = GetTickCount64();
    if (lastQueryTick != 0 && now - lastQueryTick < 1000 && cachedAcOnline == acOnline) {
        if (cachedAvailable)
            *percentOut = cachedPercent;
        return cachedAvailable;
    }
    lastQueryTick = now;
    cachedAcOnline = acOnline;
    GUID* activeScheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &activeScheme) != ERROR_SUCCESS || !activeScheme) {
        cachedAvailable = false;
        return false;
    }
    DWORD value = 0;
    const DWORD status = acOnline
        ? PowerReadACValueIndex(nullptr, activeScheme, &GUID_VIDEO_SUBGROUP,
                                &GUID_VIDEO_CURRENT_MONITOR_BRIGHTNESS, &value)
        : PowerReadDCValueIndex(nullptr, activeScheme, &GUID_VIDEO_SUBGROUP,
                                &GUID_VIDEO_CURRENT_MONITOR_BRIGHTNESS, &value);
    LocalFree(activeScheme);
    if (status != ERROR_SUCCESS || value > 100) {
        cachedAvailable = false;
        return false;
    }
    cachedPercent = static_cast<float>(value);
    cachedAvailable = true;
    *percentOut = cachedPercent;
    return true;
}

static bool ContainsAnyLower(const std::string& haystack, const char* const* needles, int count)
{
    for (int i = 0; i < count; i++) {
        if (haystack.find(needles[i]) != std::string::npos)
            return true;
    }
    return false;
}

static bool IsFanLikeSensorName(const std::string& sensorName)
{
    std::string n = LowerCopy(sensorName);
    return n.find("fan") != std::string::npos ||
           n.find("pump") != std::string::npos ||
           n.find("aio") != std::string::npos ||
           n.find("cooler") != std::string::npos;
}

static int CpuClockSensorPriority(const std::string& sensorName)
{
    std::string n = LowerCopy(sensorName);
    if (n.find("bus") != std::string::npos || n.find("memory") != std::string::npos ||
        n.find("fabric") != std::string::npos || n.find("ref") != std::string::npos ||
        n.find("bclk") != std::string::npos)
        return -1;

    int pri = 10;
    if (n.find("effective") != std::string::npos) pri = (std::max)(pri, 110);
    if (n.find("core max") != std::string::npos) pri = (std::max)(pri, 100);
    if (n.find("core #1") != std::string::npos || n.find("core 1") != std::string::npos) pri = (std::max)(pri, 95);
    if (n.find("cpu core") != std::string::npos) pri = (std::max)(pri, 90);
    if (n.find("package") != std::string::npos || n.find("processor") != std::string::npos) pri = (std::max)(pri, 70);
    return pri;
}

static int GpuCoreClockSensorPriority(const std::string& sensorName)
{
    std::string n = LowerCopy(sensorName);
    if (n.find("memory") != std::string::npos || n.find("vram") != std::string::npos ||
        n.find("hbm") != std::string::npos || n.find("video") != std::string::npos)
        return -1;

    int pri = 10;
    if (n == "gpu core") pri = 120;
    else if (n.find("gpu core") != std::string::npos) pri = 110;
    else if (n.find("graphics") != std::string::npos) pri = 80;
    else if (n.find("core") != std::string::npos) pri = 70;
    return pri;
}

static void SortClockOptions()
{
    std::stable_sort(g_cpuClockOpts.begin(), g_cpuClockOpts.end(),
        [](const auto& a, const auto& b) {
            return CpuClockSensorPriority(a.first) > CpuClockSensorPriority(b.first);
        });

    for (int i = 0; i < g_gpuCount; ++i) {
        std::stable_sort(g_gpuList[i].coreClockOpts.begin(), g_gpuList[i].coreClockOpts.end(),
            [](const auto& a, const auto& b) {
                return GpuCoreClockSensorPriority(a.first) > GpuCoreClockSensorPriority(b.first);
            });
    }
}

static void WriteSensorDiagnostics(
    const std::map<std::string, std::vector<std::tuple<std::string, std::string, std::string>>>& sensors)
{
    wchar_t exePath[32768] = {};
    const DWORD exeLength = GetModuleFileNameW(nullptr, exePath, ARRAYSIZE(exePath));
    if (exeLength == 0 || exeLength >= ARRAYSIZE(exePath))
        return;
    const std::filesystem::path outputPath =
        std::filesystem::path(exePath).parent_path() / L"sensor-diagnostics.txt";
    if (outputPath.empty())
        return;

    std::ofstream out(outputPath,
                      std::ios::out | std::ios::trunc | std::ios::binary);
    if (!out.is_open())
        return;

    auto cleanField = [](const std::string& value) {
        std::string clean(value.c_str());
        std::replace(clean.begin(), clean.end(), '\t', ' ');
        std::replace(clean.begin(), clean.end(), '\r', ' ');
        std::replace(clean.begin(), clean.end(), '\n', ' ');
        return clean;
    };

    out.write("\xEF\xBB\xBF", 3);
    out << "Hardware\tSensor\tType\tPath\n";
    for (const auto& [hardwareName, sensorList] : sensors) {
        for (const auto& sensorInfo : sensorList) {
            const auto& [sensorName, sensorType, sensorPath] = sensorInfo;
            out << cleanField(hardwareName) << '\t'
                << cleanField(sensorName) << '\t'
                << cleanField(sensorType) << '\t'
                << cleanField(sensorPath) << '\n';
        }
    }
}

static int CpuPowerSensorPriority(const std::string& sensorName, const std::string& hardwareName, bool knownCpuNode)
{
    std::string n = LowerCopy(sensorName);
    std::string h = LowerCopy(hardwareName);
    if (n.find("dram") != std::string::npos || n.find("gpu") != std::string::npos)
        return -1;

    const char* cpuTerms[] = { "cpu", "processor", "package", "core", "ppt", "tdp", "soc", "apu" };
    int pri = knownCpuNode ? 10 : 0;
    if (ContainsAnyLower(n, cpuTerms, ARRAYSIZE(cpuTerms)) || ContainsAnyLower(h, cpuTerms, ARRAYSIZE(cpuTerms)))
        pri = (std::max)(pri, 40);
    if (n.find("package") != std::string::npos || n.find("cpu total") != std::string::npos)
        pri = (std::max)(pri, 100);
    if (n.find("ppt") != std::string::npos || n.find("apu") != std::string::npos || n.find("soc") != std::string::npos)
        pri = (std::max)(pri, 80);
    return pri > 0 ? pri : -1;
}

static bool ValidLaptopPowerValue(float value)
{
    return std::isfinite(value) && value > 1.0f;
}

struct Aida64PowerValues {
    float inputW = 0.0f;
    float systemW = 0.0f;
    float batteryChargeW = 0.0f;
    float batteryDischargeW = 0.0f;
    std::string inputLabel;
    std::string systemLabel;
};

static std::string AidaXmlField(const std::string& block, const char* field)
{
    const std::string open = std::string("<") + field + ">";
    const std::string close = std::string("</") + field + ">";
    const size_t begin = block.find(open);
    if (begin == std::string::npos)
        return {};
    const size_t valueBegin = begin + open.size();
    const size_t end = block.find(close, valueBegin);
    if (end == std::string::npos)
        return {};
    return block.substr(valueBegin, end - valueBegin);
}

static bool ReadAida64PowerValues(Aida64PowerValues* out)
{
    if (!out)
        return false;
    *out = {};

    HANDLE mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, "AIDA64_SensorValues");
    if (!mapping)
        return false;

    const char* mapped = static_cast<const char*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    if (!mapped) {
        CloseHandle(mapping);
        return false;
    }

    MEMORY_BASIC_INFORMATION memoryInfo = {};
    const size_t mappedSize = VirtualQuery(mapped, &memoryInfo, sizeof(memoryInfo))
        ? memoryInfo.RegionSize
        : 10 * 1024;
    const size_t maxLength = (std::min)(mappedSize, static_cast<size_t>(256 * 1024));
    size_t length = 0;
    while (length < maxLength && mapped[length] != '\0')
        ++length;
    std::string xml(mapped, length);
    UnmapViewOfFile(mapped);
    CloseHandle(mapping);
    if (xml.empty())
        return false;

    int inputPriority = -1;
    int systemPriority = -1;
    size_t pos = 0;
    while ((pos = xml.find("<pwr>", pos)) != std::string::npos) {
        const size_t end = xml.find("</pwr>", pos);
        if (end == std::string::npos)
            break;
        const std::string block = xml.substr(pos, end + 6 - pos);
        pos = end + 6;

        const std::string id = AidaXmlField(block, "id");
        const std::string label = AidaXmlField(block, "label");
        const std::string valueText = AidaXmlField(block, "value");
        const float value = valueText.empty() ? 0.0f : static_cast<float>(strtod(valueText.c_str(), nullptr));
        if (!ValidLaptopPowerValue(std::fabs(value)))
            continue;

        const std::string idLower = LowerCopy(id);
        const std::string labelLower = LowerCopy(label);
        int inputPri = -1;
        if (idLower == "pdcin")
            inputPri = 1000;
        else if (labelLower.find("dc input") != std::string::npos ||
                 labelLower.find("ac input") != std::string::npos)
            inputPri = 900;
        else if (labelLower.find("adapter") != std::string::npos ||
                 labelLower.find("charger") != std::string::npos ||
                 labelLower.find("power supply") != std::string::npos)
            inputPri = 800;
        if (inputPri > inputPriority) {
            inputPriority = inputPri;
            out->inputW = std::fabs(value);
            out->inputLabel = label.empty() ? id : label;
        }

        const bool component =
            labelLower.find("cpu") != std::string::npos ||
            labelLower.find("gpu") != std::string::npos ||
            labelLower.find("battery") != std::string::npos;
        int systemPri = -1;
        if (!component && labelLower.find("total system") != std::string::npos)
            systemPri = 1000;
        else if (!component && labelLower.find("system power") != std::string::npos)
            systemPri = 900;
        else if (!component && labelLower.find("platform power") != std::string::npos)
            systemPri = 800;
        else if (!component && labelLower.find("board power") != std::string::npos)
            systemPri = 700;
        else if (!component && labelLower.find("ec power") != std::string::npos)
            systemPri = 600;
        if (systemPri > systemPriority) {
            systemPriority = systemPri;
            out->systemW = std::fabs(value);
            out->systemLabel = label.empty() ? id : label;
        }

        if (idLower == "pbattchr" || labelLower.find("battery charge") != std::string::npos)
            out->batteryChargeW = std::fabs(value);
        if (idLower == "pbattoutp" || labelLower.find("battery discharge") != std::string::npos)
            out->batteryDischargeW = std::fabs(value);
    }

    return ValidLaptopPowerValue(out->inputW) ||
           ValidLaptopPowerValue(out->systemW) ||
           ValidLaptopPowerValue(out->batteryChargeW) ||
           ValidLaptopPowerValue(out->batteryDischargeW);
}

static void PollExternalPowerStats()
{
    bool hasLhwmBatteryChargePath = false;
    bool hasLhwmBatteryDischargePath = false;
    {
        std::lock_guard<std::mutex> lock(g_lhwmStateMutex);
        hasLhwmBatteryChargePath = !g_lhwmBatteryChargePowerPath.empty();
        hasLhwmBatteryDischargePath = !g_lhwmBatteryDischargePowerPath.empty();
    }
    Aida64PowerValues aida;
    if (!ReadAida64PowerValues(&aida)) {
        if (g_aidaPowerActive) {
            if (g_acInputPowerSourceName.rfind("AIDA64", 0) == 0) {
                g_acInputPower = 0.0f;
                g_acInputPowerSourceName.clear();
            }
            if (g_totalSystemPowerSourceName.rfind("AIDA64", 0) == 0) {
                g_totalSystemPower = 0.0f;
                g_totalSystemPowerSourceName.clear();
            }
            if (!hasLhwmBatteryChargePath)
                g_batteryChargePower = 0.0f;
            if (!hasLhwmBatteryDischargePath)
                g_batteryDischargePower = 0.0f;
            g_aidaPowerActive = false;
            LogLine("AIDA64 power shared memory unavailable; using fallback sources");
        }
        return;
    }

    if (g_acInputPowerSourceName.rfind("AIDA64", 0) == 0) {
        g_acInputPower = 0.0f;
        g_acInputPowerSourceName.clear();
    }
    if (g_totalSystemPowerSourceName.rfind("AIDA64", 0) == 0) {
        g_totalSystemPower = 0.0f;
        g_totalSystemPowerSourceName.clear();
    }
    if (g_aidaPowerActive && !hasLhwmBatteryChargePath)
        g_batteryChargePower = 0.0f;
    if (g_aidaPowerActive && !hasLhwmBatteryDischargePath)
        g_batteryDischargePower = 0.0f;

    if (ValidLaptopPowerValue(aida.inputW)) {
        g_acInputPower = aida.inputW;
        g_acInputPowerSourceName =
            aida.inputLabel.empty() ? "AIDA64 / DC Input" : "AIDA64 / " + aida.inputLabel;
    }
    if (ValidLaptopPowerValue(aida.systemW)) {
        g_totalSystemPower = aida.systemW;
        g_totalSystemPowerSourceName =
            aida.systemLabel.empty() ? "AIDA64 / System Power" : "AIDA64 / " + aida.systemLabel;
    }
    if (ValidLaptopPowerValue(aida.batteryChargeW))
        g_batteryChargePower = aida.batteryChargeW;
    if (ValidLaptopPowerValue(aida.batteryDischargeW))
        g_batteryDischargePower = aida.batteryDischargeW;

    if (!g_aidaPowerActive) {
        LogLine("AIDA64 power shared memory active: input=%.1fW system=%.1fW charge=%.1fW discharge=%.1fW",
                aida.inputW, aida.systemW, aida.batteryChargeW, aida.batteryDischargeW);
    }
    g_aidaPowerActive = true;
}

static bool PowerCandidatePathValid(const char* path)
{
    if (!path || !path[0])
        return false;
    for (const auto& candidate : g_powerSensorCandidates)
        if (candidate.path == path)
            return true;
    return false;
}

[[maybe_unused]] static void ApplyManualPowerSensorBindings()
{
    if (PowerCandidatePathValid(g_Config.acInputPowerPath))
        g_lhwmAcInputPowerPath = g_Config.acInputPowerPath;
    else if (g_Config.acInputPowerPath[0])
        g_Config.acInputPowerPath[0] = '\0';

    if (PowerCandidatePathValid(g_Config.totalSystemPowerPath))
        g_lhwmTotalSystemPowerPath = g_Config.totalSystemPowerPath;
    else if (g_Config.totalSystemPowerPath[0])
        g_Config.totalSystemPowerPath[0] = '\0';
}

static int LhwmAcInputPowerPriority(const std::string& sensorName, const std::string& hardwareName)
{
    std::string n = LowerCopy(sensorName);
    std::string h = LowerCopy(hardwareName);
    int pri = -1;
    if (n.find("ac adapter") != std::string::npos || h.find("ac adapter") != std::string::npos)
        pri = 100;
    if (n.find("adapter") != std::string::npos || h.find("adapter") != std::string::npos)
        pri = (std::max)(pri, 80);
    if ((n.find("input") != std::string::npos || h.find("input") != std::string::npos) &&
        n.find("power") != std::string::npos)
        pri = (std::max)(pri, 70);
    return pri;
}

static int LhwmBatteryPowerPriority(const std::string& sensorName, const std::string& hardwareName, bool charge)
{
    std::string n = LowerCopy(sensorName);
    std::string h = LowerCopy(hardwareName);
    if (n.find("battery") == std::string::npos && h.find("battery") == std::string::npos)
        return -1;
    if (charge) {
        if (n.find("discharge") != std::string::npos)
            return -1;
        if (n.find("charge rate") != std::string::npos)
            return 100;
        if (n.find("charge") != std::string::npos)
            return 80;
    } else {
        if (n.find("discharge rate") != std::string::npos)
            return 100;
        if (n.find("discharge") != std::string::npos)
            return 80;
    }
    return -1;
}

static int LhwmTotalSystemPowerPriority(const std::string& sensorName, const std::string& hardwareName)
{
    std::string n = LowerCopy(sensorName);
    std::string h = LowerCopy(hardwareName);
    const std::string combined = h + " " + n + " " + n + " " + h;
    const bool componentNode =
        n.find("cpu") != std::string::npos || n.find("gpu") != std::string::npos ||
        h.find("cpu") != std::string::npos || h.find("gpu") != std::string::npos ||
        h.find("processor") != std::string::npos || h.find("graphics") != std::string::npos ||
        h.find("geforce") != std::string::npos || h.find("radeon") != std::string::npos;
    if (componentNode)
        return -1;
    if (combined.find("total system power") != std::string::npos)
        return 1000;
    if (combined.find("system power") != std::string::npos)
        return 900;
    if (combined.find("platform power") != std::string::npos)
        return 800;
    if (combined.find("board power") != std::string::npos)
        return 700;
    if (combined.find("asus ec power") != std::string::npos)
        return 650;
    if (combined.find("ec power") != std::string::npos)
        return 600;
    // Adapter/input sensors are not direct system meters. They must stay in
    // the AC-input path so LaptopPowerFeature can add battery assistance and
    // subtract battery charging before presenting system power.
    return -1;
}

static void SelectBestLiveSystemPowerSensor()
{
    int bestPriority = -1;
    float bestValue = 0.0f;
    const PowerSensorCandidate* best = nullptr;

    for (const auto& candidate : g_powerSensorCandidates) {
        const int priority = LhwmTotalSystemPowerPriority(candidate.sensorName, candidate.hardwareName);
        if (priority < 0 || priority <= bestPriority)
            continue;
        const float value = ReadLhwmSensorValue(candidate.path);
        if (!ValidLaptopPowerValue(value))
            continue;
        bestPriority = priority;
        bestValue = value;
        best = &candidate;
    }

    if (!best) {
        g_lhwmTotalSystemPowerPath.clear();
        g_totalSystemPower = 0.0f;
        g_totalSystemPowerSourceName.clear();
        return;
    }

    g_lhwmTotalSystemPowerPath = best->path;
    g_totalSystemPower = bestValue;
    std::string hardware = best->hardwareName;
    const size_t marker = hardware.find(" : /");
    if (marker != std::string::npos)
        hardware.resize(marker);
    g_totalSystemPowerSourceName =
        hardware.empty() ? best->sensorName : hardware + " / " + best->sensorName;
}

static int CpuFanSensorPriority(const std::string& sensorName, const std::string& hardwareName)
{
    std::string n = LowerCopy(sensorName);
    std::string h = LowerCopy(hardwareName);
    if (n.find("gpu") != std::string::npos || h.find("gpu") != std::string::npos)
        return -1;

    int pri = 10;
    if (n.find("cpu") != std::string::npos || h.find("cpu") != std::string::npos)
        pri = 100;
    if (n.find("pump") != std::string::npos || n.find("aio") != std::string::npos)
        pri = (std::max)(pri, 85);
    if (n.find("processor") != std::string::npos)
        pri = (std::max)(pri, 80);
    if (n.find("system") != std::string::npos || n.find("chassis") != std::string::npos ||
        n.find("case") != std::string::npos || n.find("fan #") != std::string::npos)
        pri = (std::max)(pri, 35);
    return pri;
}

static int GpuFanSensorPriority(const std::string& sensorName)
{
    std::string n = LowerCopy(sensorName);
    if (n.find("gpu fan") != std::string::npos)
        return 100;
    if (n.find("gpu") != std::string::npos && n.find("fan") != std::string::npos)
        return 85;
    if (n.find("fan") != std::string::npos)
        return 50;
    return 10;
}

static bool IsAmdSocCpuTempSensorName(const std::string& sensorName)
{
    std::string n;
    n.reserve(sensorName.size());
    for (char c : sensorName)
        n += (char)tolower((unsigned char)c);
    if (n.find("socket") != std::string::npos)
        return false;
    if (n == "soc")
        return true;
    if (n.find("soc") != std::string::npos)
        return true;
    return false;
}

// When SoC / Package is absent, prefer the same die reading users watch in LHM (Core Tctl/Tdie).
static int CpuTempFallbackPriority(const std::string& sensorName)
{
    std::string n;
    n.reserve(sensorName.size());
    for (char c : sensorName)
        n += (char)tolower((unsigned char)c);
    const bool hasCore = (n.find("core") != std::string::npos);
    const bool hasTctl = (n.find("tctl") != std::string::npos);
    const bool hasTdie = (n.find("tdie") != std::string::npos);
    const bool hasCcd  = (n.find("ccd") != std::string::npos);
    if (hasCore && (hasTctl || hasTdie))
        return 100;
    if (hasCcd)
        return 60;
    if (hasTctl || hasTdie)
        return 80;
    return 10;
}

static int CpuTempCandidatePriority(const std::string& sensorName, const std::string& hardwareName, bool knownCpuNode)
{
    std::string n = LowerCopy(sensorName);
    std::string h = LowerCopy(hardwareName);
    const char* rejectTerms[] = { "gpu", "vram", "ssd", "hdd", "drive", "disk", "nvme", "memory", "ambient" };
    if (ContainsAnyLower(n, rejectTerms, ARRAYSIZE(rejectTerms)) || ContainsAnyLower(h, rejectTerms, ARRAYSIZE(rejectTerms)))
        return -1;

    int pri = knownCpuNode ? CpuTempFallbackPriority(sensorName) : 0;
    if (n.find("package") != std::string::npos)
        pri = (std::max)(pri, 120);
    if (IsAmdSocCpuTempSensorName(sensorName))
        pri = (std::max)(pri, 110);

    const char* cpuTerms[] = { "cpu", "processor", "core", "tctl", "tdie", "ccd", "apu", "soc" };
    if (ContainsAnyLower(n, cpuTerms, ARRAYSIZE(cpuTerms)) || ContainsAnyLower(h, cpuTerms, ARRAYSIZE(cpuTerms)))
        pri = (std::max)(pri, 50);
    return pri > 0 ? pri : -1;
}

static bool InitLHWM()
{
    try {
        LhwmSensorMap sensors;
        DWORD nativeException = 0;
        bool mapLoaded = false;
        {
            std::lock_guard<std::mutex> lock(g_lhwmCallMutex);
            if (!LhwmReadsAllowed())
                return false;
            mapLoaded = FillLhwmSensorMapGuarded(&sensors, &nativeException);
        }
        if (nativeException != 0) {
            QuarantineLhwmAfterNativeFault(nativeException, "GetHardwareSensorMap");
            return false;
        }
        if (!mapLoaded)
            return false;
        if (sensors.empty()) return false;
        WriteSensorDiagnostics(sensors);

        std::lock_guard<std::mutex> stateLock(g_lhwmStateMutex);

        g_lhwmCpuTempPath.clear();
        g_lhwmGpuTempPath.clear();
        g_lhwmGpuLoadPath.clear();
        g_lhwmGpuVramUsedPath.clear();
        g_lhwmGpuVramTotalPath.clear();
        g_lhwmCpuPowerPath.clear();
        g_lhwmCpuMemoryPowerPath.clear();
        g_lhwmCpuPlatformPowerPath.clear();
        g_lhwmGpuPowerPath.clear();
        g_lhwmDiscreteGpuPowerPath.clear();
        g_lhwmAcInputPowerPath.clear();
        g_lhwmAutoAcInputPowerPath.clear();
        g_lhwmBatteryChargePowerPath.clear();
        g_lhwmBatteryDischargePowerPath.clear();
        g_lhwmTotalSystemPowerPath.clear();
        g_lhwmAutoTotalSystemPowerPath.clear();
        g_totalSystemPowerSourceName.clear();
        g_lhwmCpuFanPath.clear();
        g_lhwmGpuFanPath.clear();
        g_lhwmGpuMemTempPath.clear();
        g_lhwmDiskTempPaths.clear();
        g_lhwmMemoryTempPaths.clear();
        g_lhwmDiskActivityPaths.clear();
        g_lhwmNetworkLoadPaths.clear();
        g_powerSensorCandidates.clear();
        g_discreteGpuPower = 0.0f;
        g_lhwmCpuFanIsPercent = false;
        g_lhwmGpuFanIsPercent = false;
        g_cpuClockOpts.clear();
        for (int gi = 0; gi < MAX_GPUS; gi++) {
            g_gpuList[gi].fanIsPercent = false;
            g_gpuList[gi].coreClockOpts.clear();
        }
        
        // The map structure from lhwm-cpp-wrapper is:
        // Key (map key) = Hardware name (e.g., "AMD Ryzen 9 5900HS...")
        // Value = vector<tuple<sensorName, sensorType, sensorPath>>
        //   tuple[0] = Sensor name (e.g., "CPU Core #1", "GPU Core")
        //   tuple[1] = Sensor type (e.g., "Temperature", "Load")
        //   tuple[2] = Sensor path (e.g., "/amdcpu/0/temperature/0")
        
        std::string cpuTempFallbackPath;
        int         cpuTempFallbackPri = -1;
        std::string cpuPowerFallbackPath;
        int         cpuPowerFallbackPri = -1;
        int         acInputPowerPri = -1;
        int         batteryChargePowerPri = -1;
        int         batteryDischargePowerPri = -1;
        int         totalSystemPowerPri = -1;
        std::string cpuFanFallbackPath;
        int         cpuFanFallbackPri = -1;
        bool        cpuFanFallbackIsPercent = false;
        g_gpuCount = 0;  // Reset GPU count
        
        for (const auto& [hardwareName, sensorList] : sensors) {
            // Check if this is CPU or GPU hardware by hardware name
            std::string hardwareLower = LowerCopy(hardwareName);
            bool isCpuHardware = (hardwareLower.find("ryzen") != std::string::npos ||
                                  hardwareLower.find("intel") != std::string::npos ||
                                  hardwareLower.find("amd") != std::string::npos ||
                                  hardwareLower.find("cpu") != std::string::npos ||
                                  hardwareLower.find("processor") != std::string::npos ||
                                  hardwareLower.find("core") != std::string::npos);
            
            bool isGpuHardware = IsLhwmGpuHardwareNode(hardwareName);
            
            // If this is a GPU hardware node (discrete or integrated), find or create list entry
            int gpuIndex = -1;
            if (isGpuHardware && g_gpuCount < MAX_GPUS) {
                // Clean the hardware name - LHWM may return "Name : /path", we only want the name
                std::string cleanName = hardwareName;
                size_t colonPos = cleanName.find(" : ");
                if (colonPos != std::string::npos) {
                    cleanName = cleanName.substr(0, colonPos);
                }
                
                gpuIndex = FindGpuByName(cleanName.c_str());
                if (gpuIndex < 0) {
                    // New GPU - add to list
                    gpuIndex = g_gpuCount;
                    snprintf(g_gpuList[gpuIndex].name, sizeof(g_gpuList[gpuIndex].name), "%s", cleanName.c_str());
                    g_gpuList[gpuIndex].tempPath.clear();
                    g_gpuList[gpuIndex].loadPath.clear();
                    g_gpuList[gpuIndex].loadPathPri = -1;
                    g_gpuList[gpuIndex].vramUsedPath.clear();
                    g_gpuList[gpuIndex].vramTotalPath.clear();
                    g_gpuList[gpuIndex].vramTotalPri = -1;
                    g_gpuList[gpuIndex].powerPath.clear();
                    g_gpuList[gpuIndex].fanPath.clear();
                    g_gpuList[gpuIndex].fanPathPri = -1;
                    g_gpuList[gpuIndex].fanIsPercent = false;
                    g_gpuList[gpuIndex].memTempPath.clear();
                    g_gpuList[gpuIndex].coreClockOpts.clear();
                    g_gpuCount++;
                }
            }
            
            // Iterate through all sensors for this hardware
            for (const auto& sensorInfo : sensorList) {
                const auto& [sensorName, sensorType, sensorPath] = sensorInfo;
                
                // Also detect by path pattern
                bool isCpuPath = (sensorPath.find("/amdcpu/") != std::string::npos ||
                                  sensorPath.find("/intelcpu/") != std::string::npos);
                bool isGpuPath = (sensorPath.find("/gpu-nvidia/") != std::string::npos ||
                                  sensorPath.find("/gpu-amd/") != std::string::npos ||
                                  sensorPath.find("/gpu-intel/") != std::string::npos ||
                                  sensorPath.find("/gpu-intel-integrated/") != std::string::npos);
                if (sensorType == "Power") {
                    bool duplicate = false;
                    for (const auto& candidate : g_powerSensorCandidates) {
                        if (candidate.path == sensorPath) {
                            duplicate = true;
                            break;
                        }
                    }
                    if (!duplicate)
                        g_powerSensorCandidates.push_back({ hardwareName, sensorName, sensorPath });

                    if (isCpuPath && sensorName == "CPU Memory")
                        g_lhwmCpuMemoryPowerPath = sensorPath;
                    if (isCpuPath && sensorName == "CPU Platform")
                        g_lhwmCpuPlatformPowerPath = sensorPath;

                    int acPri = LhwmAcInputPowerPriority(sensorName, hardwareName);
                    if (acPri > acInputPowerPri) {
                        g_lhwmAcInputPowerPath = sensorPath;
                        acInputPowerPri = acPri;
                    }
                    int chPri = LhwmBatteryPowerPriority(sensorName, hardwareName, true);
                    if (chPri > batteryChargePowerPri) {
                        g_lhwmBatteryChargePowerPath = sensorPath;
                        batteryChargePowerPri = chPri;
                    }
                    int disPri = LhwmBatteryPowerPriority(sensorName, hardwareName, false);
                    if (disPri > batteryDischargePowerPri) {
                        g_lhwmBatteryDischargePowerPath = sensorPath;
                        batteryDischargePowerPri = disPri;
                    }
                    int totalPri = LhwmTotalSystemPowerPriority(sensorName, hardwareName);
                    if (totalPri > totalSystemPowerPri) {
                        g_lhwmTotalSystemPowerPath = sensorPath;
                        totalSystemPowerPri = totalPri;
                    }
                }
                if (sensorType == "Temperature") {
                    if (sensorPath.find("/nvme/") != std::string::npos && sensorName == "Temperature") {
                        if (std::find(g_lhwmDiskTempPaths.begin(), g_lhwmDiskTempPaths.end(), sensorPath) == g_lhwmDiskTempPaths.end())
                            g_lhwmDiskTempPaths.push_back(sensorPath);
                    }
                    if (sensorPath.find("/memory/dimm/") != std::string::npos &&
                        sensorName.find("DIMM") != std::string::npos) {
                        if (std::find(g_lhwmMemoryTempPaths.begin(), g_lhwmMemoryTempPaths.end(), sensorPath) == g_lhwmMemoryTempPaths.end())
                            g_lhwmMemoryTempPaths.push_back(sensorPath);
                    }
                }
                if (sensorType == "Load" &&
                    sensorPath.find("/nvme/") != std::string::npos &&
                    sensorName == "Total Activity") {
                    if (std::find(g_lhwmDiskActivityPaths.begin(), g_lhwmDiskActivityPaths.end(), sensorPath) ==
                        g_lhwmDiskActivityPaths.end())
                        g_lhwmDiskActivityPaths.push_back(sensorPath);
                }
                if (sensorType == "Load" &&
                    sensorPath.find("/nic/") != std::string::npos &&
                    sensorName == "Network Utilization") {
                    const bool virtualAdapter =
                        hardwareLower.find("tunnel") != std::string::npos ||
                        hardwareLower.find("virtual") != std::string::npos ||
                        hardwareLower.find("vpn") != std::string::npos ||
                        hardwareLower.find("wintun") != std::string::npos ||
                        hardwareLower.find("meta") != std::string::npos;
                    if (!virtualAdapter &&
                        std::find(g_lhwmNetworkLoadPaths.begin(), g_lhwmNetworkLoadPaths.end(), sensorPath) ==
                            g_lhwmNetworkLoadPaths.end())
                        g_lhwmNetworkLoadPaths.push_back(sensorPath);
                }
                if (gpuIndex < 0 && isGpuPath && g_gpuCount < MAX_GPUS) {
                    std::string cleanName = hardwareName;
                    size_t colonPos = cleanName.find(" : ");
                    if (colonPos != std::string::npos)
                        cleanName = cleanName.substr(0, colonPos);
                    if (cleanName.empty()) {
                        if (sensorPath.find("/gpu-nvidia/") != std::string::npos)
                            cleanName = "NVIDIA GPU";
                        else if (sensorPath.find("/gpu-amd/") != std::string::npos)
                            cleanName = "AMD GPU";
                        else
                            cleanName = "Intel GPU";
                    }

                    gpuIndex = FindGpuByName(cleanName.c_str());
                    if (gpuIndex < 0) {
                        gpuIndex = g_gpuCount;
                        snprintf(g_gpuList[gpuIndex].name, sizeof(g_gpuList[gpuIndex].name), "%s", cleanName.c_str());
                        g_gpuList[gpuIndex].tempPath.clear();
                        g_gpuList[gpuIndex].loadPath.clear();
                        g_gpuList[gpuIndex].loadPathPri = -1;
                        g_gpuList[gpuIndex].vramUsedPath.clear();
                        g_gpuList[gpuIndex].vramTotalPath.clear();
                        g_gpuList[gpuIndex].vramTotalPri = -1;
                        g_gpuList[gpuIndex].powerPath.clear();
                        g_gpuList[gpuIndex].fanPath.clear();
                        g_gpuList[gpuIndex].fanPathPri = -1;
                        g_gpuList[gpuIndex].fanIsPercent = false;
                        g_gpuList[gpuIndex].memTempPath.clear();
                        g_gpuList[gpuIndex].coreClockOpts.clear();
                        g_gpuCount++;
                    }
                }
                
                // CPU fan — search all non-GPU hardware for a fan sensor named "CPU"
                const bool isFanSensor = (sensorType == "Fan");
                const bool isFanControl = (sensorType == "Control" && IsFanLikeSensorName(sensorName));
                if ((isFanSensor || isFanControl) && !isGpuPath) {
                    int p = CpuFanSensorPriority(sensorName, hardwareName);
                    if (isFanControl)
                        p -= 5;
                    if (p > cpuFanFallbackPri) {
                        cpuFanFallbackPath = sensorPath;
                        cpuFanFallbackPri = p;
                        cpuFanFallbackIsPercent = isFanControl;
                    }
                }

                // CPU power sensors
                if (sensorType == "Power" && !isGpuPath) {
                    int p = CpuPowerSensorPriority(sensorName, hardwareName, isCpuHardware || isCpuPath);
                    if (p > cpuPowerFallbackPri) {
                        cpuPowerFallbackPath = sensorPath;
                        cpuPowerFallbackPri = p;
                    }

                }

                // CPU temperature sensors
                if (sensorType == "Temperature" && !isGpuPath) {
                    // Priority order for CPU temp (matching Task Manager):
                    // 1. AMD SoC ("Soc") — not "Socket" (false positive on substring "Soc")
                    // 2. "Package" - Intel package temp
                    // 3. Fallback: prefer Core (Tctl/Tdie), then other Tdie/Tctl, then CCD, else any
                    int p = CpuTempCandidatePriority(sensorName, hardwareName, isCpuHardware || isCpuPath);
                    if (p > cpuTempFallbackPri) {
                        cpuTempFallbackPath = sensorPath;
                        cpuTempFallbackPri = p;
                    }
                }
                
                // GPU sensors - store in the GPU's entry
                if (gpuIndex >= 0) {
                    if (sensorType == "Temperature") {
                        if (sensorName.find("Memory Junction") != std::string::npos ||
                            sensorName.find("Hot Spot") != std::string::npos ||
                            sensorName.find("Hotspot") != std::string::npos) {
                            if (g_gpuList[gpuIndex].memTempPath.empty() ||
                                sensorName.find("Memory Junction") != std::string::npos)
                                g_gpuList[gpuIndex].memTempPath = sensorPath;
                        }
                        // Prefer "GPU Core" exactly, avoid "Hot Spot" (matches Task Manager)
                        bool isHotSpot = (sensorName.find("Hot Spot") != std::string::npos ||
                                          sensorName.find("Hotspot") != std::string::npos);
                        bool isGpuCore = (sensorName == "GPU Core" || 
                                          sensorName.find("GPU Core") != std::string::npos);
                        
                        if (isGpuCore && !isHotSpot) {
                            g_gpuList[gpuIndex].tempPath = sensorPath;  // Best choice
                        } else if (g_gpuList[gpuIndex].tempPath.empty() && !isHotSpot) {
                            g_gpuList[gpuIndex].tempPath = sensorPath;  // Fallback (not hotspot)
                        }
                    }
                    else if (sensorType == "Load") {
                        // Priority: 2=exact "GPU Core", 1=contains "GPU Core", 0=any other (first fallback)
                        // This prevents "GPU Memory Controller", "GPU Bus", "GPU Video Engine", etc.
                        // from overwriting the correct 3D-engine load sensor.
                        int pri = (sensorName == "GPU Core") ? 2
                                : (sensorName.find("GPU Core") != std::string::npos) ? 1
                                : 0;
                        if (pri > g_gpuList[gpuIndex].loadPathPri) {
                            g_gpuList[gpuIndex].loadPath    = sensorPath;
                            g_gpuList[gpuIndex].loadPathPri = pri;
                        }
                    }
                    else if (sensorType == "SmallData" || sensorType == "Data") {
                        if (sensorName.find("Memory Used") != std::string::npos ||
                            sensorName.find("GPU Memory Used") != std::string::npos) {
                            g_gpuList[gpuIndex].vramUsedPath = sensorPath;
                        }
                        else {
                            int totPri = VramTotalSensorPriority(sensorName);
                            if (totPri >= 0 && totPri > g_gpuList[gpuIndex].vramTotalPri) {
                                g_gpuList[gpuIndex].vramTotalPath = sensorPath;
                                g_gpuList[gpuIndex].vramTotalPri = totPri;
                            }
                        }
                    }
                    else if (sensorType == "Clock") {
                        // VRAM/memory clocks are inconsistent in LHWM; only list GPU core clocks.
                        if (IsGpuMemoryClockSensor(sensorName) || GpuCoreClockSensorPriority(sensorName) < 0)
                            continue;
                        bool dup = false;
                        for (const auto& e : g_gpuList[gpuIndex].coreClockOpts)
                            if (e.second == sensorPath) { dup = true; break; }
                        if (!dup)
                            g_gpuList[gpuIndex].coreClockOpts.push_back({ sensorName, sensorPath });
                    }
                    else if (sensorType == "Power") {
                        // Prefer Package power; fall back to first available power sensor
                        if (sensorName.find("Package") != std::string::npos ||
                            g_gpuList[gpuIndex].powerPath.empty()) {
                            g_gpuList[gpuIndex].powerPath = sensorPath;
                        }
                    }
                    else if (sensorType == "Fan" || (sensorType == "Control" && IsFanLikeSensorName(sensorName))) {
                        int pri = GpuFanSensorPriority(sensorName);
                        const bool fanIsPercent = (sensorType == "Control");
                        if (fanIsPercent)
                            pri -= 5;
                        if (pri > g_gpuList[gpuIndex].fanPathPri) {
                            g_gpuList[gpuIndex].fanPath = sensorPath;
                            g_gpuList[gpuIndex].fanPathPri = pri;
                            g_gpuList[gpuIndex].fanIsPercent = fanIsPercent;
                        }
                    }
                }

                if (sensorType == "Clock") {
                    if ((isCpuHardware || isCpuPath) && !isGpuPath) {
                        if (CpuClockSensorPriority(sensorName) < 0)
                            continue;
                        bool dup = false;
                        for (const auto& e : g_cpuClockOpts)
                            if (e.second == sensorPath) { dup = true; break; }
                        if (!dup)
                            g_cpuClockOpts.push_back({ sensorName, sensorPath });
                    }
                }
            }
        }
        
        // Use fallback CPU temp if needed
        if (g_lhwmCpuTempPath.empty() && !cpuTempFallbackPath.empty()) {
            g_lhwmCpuTempPath = cpuTempFallbackPath;
        }
        if (g_lhwmCpuPowerPath.empty() && !cpuPowerFallbackPath.empty()) {
            g_lhwmCpuPowerPath = cpuPowerFallbackPath;
        }
        if (g_lhwmCpuFanPath.empty() && !cpuFanFallbackPath.empty()) {
            g_lhwmCpuFanPath = cpuFanFallbackPath;
            g_lhwmCpuFanIsPercent = cpuFanFallbackIsPercent;
        }
        
        // Clamp selected GPU to valid range
        if (g_Config.selectedGpu < 0 || g_Config.selectedGpu >= g_gpuCount)
            g_Config.selectedGpu = 0;
        if (g_gpuCount > 0) {
            const int previousGpu = g_Config.selectedGpu;
            g_Config.selectedGpu = ChooseBestGpuIndex();
            if (previousGpu != g_Config.selectedGpu) {
                LogLine("GPU auto-select: %d (%s) -> %d (%s)",
                        previousGpu,
                        (previousGpu >= 0 && previousGpu < g_gpuCount) ? g_gpuList[previousGpu].name : "N/A",
                        g_Config.selectedGpu,
                        (g_Config.selectedGpu >= 0 && g_Config.selectedGpu < g_gpuCount) ? g_gpuList[g_Config.selectedGpu].name : "N/A");
            }
        }
        
        // Set active GPU paths and name
        if (g_gpuCount > 0) {
            int idx = g_Config.selectedGpu;
            g_lhwmGpuTempPath = g_gpuList[idx].tempPath;
            g_lhwmGpuLoadPath = g_gpuList[idx].loadPath;
            g_lhwmGpuVramUsedPath = g_gpuList[idx].vramUsedPath;
            g_lhwmGpuVramTotalPath = g_gpuList[idx].vramTotalPath;
            g_lhwmGpuPowerPath = g_gpuList[idx].powerPath;
            g_lhwmGpuFanPath   = g_gpuList[idx].fanPath;
            g_lhwmGpuMemTempPath = g_gpuList[idx].memTempPath;
            g_lhwmGpuFanIsPercent = g_gpuList[idx].fanIsPercent;
            snprintf(g_gpuName, sizeof(g_gpuName), "%s", g_gpuList[idx].name);
        }
        for (int i = 0; i < g_gpuCount; ++i) {
            const std::string nameLower = LowerCopy(g_gpuList[i].name);
            if (!g_gpuList[i].powerPath.empty() &&
                (nameLower.find("nvidia") != std::string::npos ||
                 nameLower.find("geforce") != std::string::npos ||
                 nameLower.find("radeon") != std::string::npos)) {
                g_lhwmDiscreteGpuPowerPath = g_gpuList[i].powerPath;
                break;
            }
        }

        SortClockOptions();
        ValidateFrequencyPaths();
        g_lhwmAutoAcInputPowerPath = g_lhwmAcInputPowerPath;
        g_lhwmAutoTotalSystemPowerPath = g_lhwmTotalSystemPowerPath;
        LogLine("Power sensor scan: candidates=%zu ac=%s total=%s",
                g_powerSensorCandidates.size(),
                g_lhwmAcInputPowerPath.empty() ? "N/A" : g_lhwmAcInputPowerPath.c_str(),
                g_lhwmTotalSystemPowerPath.empty() ? "N/A" : g_lhwmTotalSystemPowerPath.c_str());
        
        return !g_lhwmCpuTempPath.empty() || !g_lhwmCpuPowerPath.empty() ||
               !g_lhwmCpuFanPath.empty() || g_gpuCount > 0;
    }
    catch (...) {
        return false;
    }
}

static std::atomic<bool> g_lhwmInitFinished{false};

static void LhwmBackgroundInitThread()
{
    bool ok = false;
    try {
        ok = InitLHWM();
    } catch (...) {
        ok = false;
    }
    ok = ok && !g_lhwmNativeFaulted.load(std::memory_order_acquire);
    g_lhwmAvailable.store(ok, std::memory_order_release);
    g_lhwmInitFinished.store(true, std::memory_order_release);
    std::string cpuTempPath;
    std::string cpuFanPath;
    int gpuCount = 0;
    {
        std::lock_guard<std::mutex> lock(g_lhwmStateMutex);
        cpuTempPath = g_lhwmCpuTempPath;
        cpuFanPath = g_lhwmCpuFanPath;
        gpuCount = g_gpuCount;
    }
    LogLine("LibreHardwareMonitor init: %s cpuTemp=%s cpuFan=%s gpuCount=%d nativeFault=0x%08lX",
            ok ? "ok" : "failed",
            cpuTempPath.empty() ? "N/A" : cpuTempPath.c_str(),
            cpuFanPath.empty() ? "N/A" : cpuFanPath.c_str(),
            gpuCount,
            (unsigned long)g_lhwmNativeFaultCode.load(std::memory_order_acquire));
}

static void StartLhwmInitializationThread()
{
    if (g_lhwmInitThread.joinable())
        g_lhwmInitThread.join();
    g_lhwmAvailable.store(false, std::memory_order_release);
    g_lhwmInitFinished.store(false, std::memory_order_release);
    g_lhwmInitThread = std::thread([]() {
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        LhwmBackgroundInitThread();
        if (SUCCEEDED(comResult))
            CoUninitialize();
    });
}

[[maybe_unused]] static void PollLHWMStats()
{
    try {
        if (g_lhwmAvailable) {
            // CPU temperature (stored in g_lhwmCpuTemp, used directly elsewhere)
            if (!g_lhwmCpuTempPath.empty()) {
                g_lhwmCpuTemp = ReadLhwmSensorValue(g_lhwmCpuTempPath);
            }
            
            // GPU stats - write directly to unified variables
            if (!g_lhwmGpuTempPath.empty()) {
                g_gpuTemp = ReadLhwmSensorValue(g_lhwmGpuTempPath);
            }
            if (!g_lhwmGpuLoadPath.empty()) {
                g_gpuUsage = ReadLhwmSensorValue(g_lhwmGpuLoadPath);
            }
            if (!g_lhwmGpuVramUsedPath.empty()) {
                // Value is in MB, convert to GB
                g_vramUsed = ReadLhwmSensorValue(g_lhwmGpuVramUsedPath) / 1024.0f;
            }
            if (!g_lhwmGpuVramTotalPath.empty()) {
                g_vramTotal = ReadLhwmSensorValue(g_lhwmGpuVramTotalPath) / 1024.0f;
            }
            if (!g_lhwmCpuPowerPath.empty()) {
                g_cpuPower = ReadLhwmSensorValue(g_lhwmCpuPowerPath);
            }
            g_cpuMemoryPower = 0.0f;
            if (!g_lhwmCpuMemoryPowerPath.empty()) {
                const float value = ReadLhwmSensorValue(g_lhwmCpuMemoryPowerPath);
                if (ValidLaptopPowerValue(value))
                    g_cpuMemoryPower = value;
            }
            g_cpuPlatformPower = 0.0f;
            if (!g_lhwmCpuPlatformPowerPath.empty()) {
                const float value = ReadLhwmSensorValue(g_lhwmCpuPlatformPowerPath);
                if (ValidLaptopPowerValue(value))
                    g_cpuPlatformPower = value;
            }
            if (!g_lhwmGpuPowerPath.empty()) {
                g_gpuPower = ReadLhwmSensorValue(g_lhwmGpuPowerPath);
            }
            g_discreteGpuPower = 0.0f;
            if (!g_lhwmDiscreteGpuPowerPath.empty()) {
                const float value = ReadLhwmSensorValue(g_lhwmDiscreteGpuPowerPath);
                if (ValidLaptopPowerValue(value))
                    g_discreteGpuPower = value;
            }
            g_acInputPower = 0.0f;
            g_acInputPowerSourceName.clear();
            if (!g_lhwmAcInputPowerPath.empty()) {
                float v = ReadLhwmSensorValue(g_lhwmAcInputPowerPath);
                if (ValidLaptopPowerValue(v)) {
                    g_acInputPower = v;
                    g_acInputPowerSourceName = "LibreHardwareMonitor";
                }
            }
            g_batteryChargePower = 0.0f;
            if (!g_lhwmBatteryChargePowerPath.empty()) {
                float v = ReadLhwmSensorValue(g_lhwmBatteryChargePowerPath);
                g_batteryChargePower = ValidLaptopPowerValue(fabsf(v)) ? fabsf(v) : 0.0f;
            }
            g_batteryDischargePower = 0.0f;
            if (!g_lhwmBatteryDischargePowerPath.empty()) {
                float v = ReadLhwmSensorValue(g_lhwmBatteryDischargePowerPath);
                g_batteryDischargePower = ValidLaptopPowerValue(fabsf(v)) ? fabsf(v) : 0.0f;
            }
            SelectBestLiveSystemPowerSensor();
            if (!g_lhwmCpuFanPath.empty()) {
                g_cpuFanRpm = ReadLhwmSensorValue(g_lhwmCpuFanPath);
            }
            if (!g_lhwmGpuFanPath.empty()) {
                g_gpuFanRpm = ReadLhwmSensorValue(g_lhwmGpuFanPath);
            }
            if (!g_lhwmGpuMemTempPath.empty()) {
                g_gpuMemTemp = ReadLhwmSensorValue(g_lhwmGpuMemTempPath);
            }
            if (!g_lhwmDiskTempPaths.empty()) {
                float maxTemp = 0.0f;
                for (const auto& p : g_lhwmDiskTempPaths)
                    maxTemp = (std::max)(maxTemp, ReadLhwmSensorValue(p));
                g_diskTemp = maxTemp;
            }
            if (!g_lhwmMemoryTempPaths.empty()) {
                float maxTemp = 0.0f;
                for (const auto& p : g_lhwmMemoryTempPaths)
                    maxTemp = (std::max)(maxTemp, ReadLhwmSensorValue(p));
                g_memoryTemp = maxTemp;
            }
            g_diskActivityPercent = 0.0f;
            for (const auto& p : g_lhwmDiskActivityPaths) {
                const float value = ReadLhwmSensorValue(p);
                if (std::isfinite(value) && value > 0.0f)
                    g_diskActivityPercent += (std::min)(100.0f, value);
            }
            g_networkUtilizationPercent = 0.0f;
            for (const auto& p : g_lhwmNetworkLoadPaths) {
                const float value = ReadLhwmSensorValue(p);
                if (std::isfinite(value))
                    g_networkUtilizationPercent = (std::max)(
                        g_networkUtilizationPercent, (std::clamp)(value, 0.0f, 100.0f));
            }
        }

        float asusFan = 0.0f;
        if ((g_lhwmCpuFanPath.empty() || g_cpuFanRpm <= 0.0f) &&
            QueryAsusWmiFanRpm(false, &asusFan)) {
            g_cpuFanRpm = asusFan;
            g_lhwmCpuFanIsPercent = false;
        }
        if ((g_lhwmGpuFanPath.empty() || g_gpuFanRpm <= 0.0f) &&
            QueryAsusWmiFanRpm(true, &asusFan)) {
            g_gpuFanRpm = asusFan;
            g_lhwmGpuFanIsPercent = false;
        }
        if (QueryAsusSystemFanRpm(&asusFan)) {
            g_systemFanRpm = asusFan;
        }
    }
    catch (...) {
        // Silently ignore polling errors
    }
}

static void ScheduleAsyncLHWMStatsPoll()
{
    if (!g_lhwmAvailable || !LhwmReadsAllowed() ||
        g_comparisonPowerPollInFlight.load(std::memory_order_acquire) ||
        g_asyncLhwmPollInFlight.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    std::string cpuTempPath;
    std::string gpuTempPath;
    std::string gpuLoadPath;
    std::string gpuVramUsedPath;
    std::string gpuVramTotalPath;
    std::string cpuPowerPath;
    std::string cpuMemoryPowerPath;
    std::string cpuPlatformPowerPath;
    std::string gpuPowerPath;
    std::string discreteGpuPowerPath;
    std::string acInputPowerPath;
    std::string batteryChargePowerPath;
    std::string batteryDischargePowerPath;
    std::string totalSystemPowerPath;
    std::string totalSystemPowerSourceName;
    std::string cpuFanPath;
    std::string gpuFanPath;
    std::string gpuMemTempPath;
    std::vector<std::string> diskTempPaths;
    std::vector<std::string> memoryTempPaths;
    std::vector<std::string> diskActivityPaths;
    std::vector<std::string> networkLoadPaths;
    {
        std::lock_guard<std::mutex> lock(g_lhwmStateMutex);
        cpuTempPath = g_lhwmCpuTempPath;
        gpuTempPath = g_lhwmGpuTempPath;
        gpuLoadPath = g_lhwmGpuLoadPath;
        gpuVramUsedPath = g_lhwmGpuVramUsedPath;
        gpuVramTotalPath = g_lhwmGpuVramTotalPath;
        cpuPowerPath = g_lhwmCpuPowerPath;
        cpuMemoryPowerPath = g_lhwmCpuMemoryPowerPath;
        cpuPlatformPowerPath = g_lhwmCpuPlatformPowerPath;
        gpuPowerPath = g_lhwmGpuPowerPath;
        discreteGpuPowerPath = g_lhwmDiscreteGpuPowerPath;
        acInputPowerPath = g_lhwmAcInputPowerPath;
        batteryChargePowerPath = g_lhwmBatteryChargePowerPath;
        batteryDischargePowerPath = g_lhwmBatteryDischargePowerPath;
        totalSystemPowerPath = g_lhwmTotalSystemPowerPath;
        totalSystemPowerSourceName = g_totalSystemPowerSourceName;
        cpuFanPath = g_lhwmCpuFanPath;
        gpuFanPath = g_lhwmGpuFanPath;
        gpuMemTempPath = g_lhwmGpuMemTempPath;
        diskTempPaths = g_lhwmDiskTempPaths;
        memoryTempPaths = g_lhwmMemoryTempPaths;
        diskActivityPaths = g_lhwmDiskActivityPaths;
        networkLoadPaths = g_lhwmNetworkLoadPaths;
    }

    if (g_asyncLhwmPollThread.joinable())
        g_asyncLhwmPollThread.join();
    g_asyncLhwmPollThread = std::thread([
        cpuTempPath,
        gpuTempPath,
        gpuLoadPath,
        gpuVramUsedPath,
        gpuVramTotalPath,
        cpuPowerPath,
        cpuMemoryPowerPath,
        cpuPlatformPowerPath,
        gpuPowerPath,
        discreteGpuPowerPath,
        acInputPowerPath,
        batteryChargePowerPath,
        batteryDischargePowerPath,
        totalSystemPowerPath,
        totalSystemPowerSourceName,
        cpuFanPath,
        gpuFanPath,
        gpuMemTempPath,
        diskTempPaths,
        memoryTempPaths,
        diskActivityPaths,
        networkLoadPaths
    ]() {
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        LhwmStatsSnapshot snap;
        auto readSensor = [](const std::string& path) {
            if (path.empty())
                return 0.0f;
            const float value = ReadLhwmSensorValue(path);
            return std::isfinite(value) ? value : 0.0f;
        };
        auto readPower = [](const std::string& path) {
            if (path.empty())
                return 0.0f;
            const float value = ReadLhwmSensorValue(path);
            return ValidLaptopPowerValue(value) ? value : 0.0f;
        };
        auto readPositive = [&readSensor](const std::string& path) {
            const float value = readSensor(path);
            return value > 0.0f ? value : 0.0f;
        };

        try {
            snap.lhwmCpuTemp = readPositive(cpuTempPath);
            snap.gpuTemp = readPositive(gpuTempPath);
            snap.gpuUsage = readSensor(gpuLoadPath);
            snap.vramUsed = readPositive(gpuVramUsedPath) / 1024.0f;
            snap.vramTotal = readPositive(gpuVramTotalPath) / 1024.0f;
            snap.cpuPower = readPower(cpuPowerPath);
            snap.cpuMemoryPower = readPower(cpuMemoryPowerPath);
            snap.cpuPlatformPower = readPower(cpuPlatformPowerPath);
            snap.gpuPower = readPower(gpuPowerPath);
            snap.discreteGpuPower =
                discreteGpuPowerPath == gpuPowerPath
                    ? snap.gpuPower
                    : readPower(discreteGpuPowerPath);
            snap.acInputPower = readPower(acInputPowerPath);
            if (snap.acInputPower > 0.0f)
                snap.acInputPowerSourceName = "LibreHardwareMonitor";
            snap.batteryChargePower = fabsf(readSensor(batteryChargePowerPath));
            if (!ValidLaptopPowerValue(snap.batteryChargePower))
                snap.batteryChargePower = 0.0f;
            snap.batteryDischargePower = fabsf(readSensor(batteryDischargePowerPath));
            if (!ValidLaptopPowerValue(snap.batteryDischargePower))
                snap.batteryDischargePower = 0.0f;
            snap.totalSystemPower = readPower(totalSystemPowerPath);
            if (snap.totalSystemPower > 0.0f) {
                snap.totalSystemPowerSourceName =
                    totalSystemPowerSourceName.empty()
                        ? "LibreHardwareMonitor"
                        : totalSystemPowerSourceName;
            }
            snap.cpuFanRpm = readPositive(cpuFanPath);
            snap.gpuFanRpm = readPositive(gpuFanPath);
            snap.gpuMemTemp = readPositive(gpuMemTempPath);

            for (const auto& p : diskTempPaths)
                snap.diskTemp = (std::max)(snap.diskTemp, readPositive(p));
            for (const auto& p : memoryTempPaths)
                snap.memoryTemp = (std::max)(snap.memoryTemp, readPositive(p));
            for (const auto& p : diskActivityPaths) {
                const float value = readSensor(p);
                if (value > 0.0f)
                    snap.diskActivityPercent += (std::min)(100.0f, value);
            }
            for (const auto& p : networkLoadPaths) {
                const float value = readSensor(p);
                snap.networkUtilizationPercent =
                    (std::max)(snap.networkUtilizationPercent,
                               (std::clamp)(value, 0.0f, 100.0f));
            }

            float asusFan = 0.0f;
            if ((cpuFanPath.empty() || snap.cpuFanRpm <= 0.0f) &&
                QueryAsusWmiFanRpm(false, &asusFan)) {
                snap.cpuFanRpm = asusFan;
            }
            if ((gpuFanPath.empty() || snap.gpuFanRpm <= 0.0f) &&
                QueryAsusWmiFanRpm(true, &asusFan)) {
                snap.gpuFanRpm = asusFan;
            }
            if (QueryAsusSystemFanRpm(&asusFan)) {
                snap.systemFanRpm = asusFan;
            }

            snap.valid = true;
            {
                std::lock_guard<std::mutex> lock(g_asyncLhwmStatsMutex);
                g_asyncLhwmStatsSnapshot = snap;
            }
            g_asyncLhwmStatsSampleTick.store(GetTickCount64(), std::memory_order_release);
            g_asyncLhwmStatsSequence.fetch_add(1, std::memory_order_release);
        } catch (...) {
            // Keep the previous complete snapshot if the async poll fails.
        }

        if (SUCCEEDED(comResult))
            CoUninitialize();
        g_asyncLhwmPollInFlight.store(false, std::memory_order_release);
    });
}

static void ApplyAsyncLHWMStatsSnapshot()
{
    static unsigned long long appliedSequence = 0;
    const unsigned long long sequence =
        g_asyncLhwmStatsSequence.load(std::memory_order_acquire);
    if (sequence == 0 || sequence == appliedSequence)
        return;

    LhwmStatsSnapshot snap;
    {
        std::lock_guard<std::mutex> lock(g_asyncLhwmStatsMutex);
        snap = g_asyncLhwmStatsSnapshot;
    }
    appliedSequence = sequence;
    if (!snap.valid)
        return;

    g_lhwmCpuTemp = snap.lhwmCpuTemp;
    if (snap.lhwmCpuTemp > 0.0f)
        g_cpuTemp = snap.lhwmCpuTemp;
    g_gpuTemp = snap.gpuTemp;
    g_gpuUsage = snap.gpuUsage;
    g_vramUsed = snap.vramUsed;
    g_vramTotal = snap.vramTotal;
    g_cpuPower = snap.cpuPower;
    g_cpuMemoryPower = snap.cpuMemoryPower;
    g_cpuPlatformPower = snap.cpuPlatformPower;
    g_gpuPower = snap.gpuPower;
    g_discreteGpuPower = snap.discreteGpuPower;
    if (g_acInputPowerSourceName.empty() ||
        g_acInputPowerSourceName == "LibreHardwareMonitor") {
        g_acInputPower = snap.acInputPower;
        g_acInputPowerSourceName = snap.acInputPowerSourceName;
    }
    g_batteryChargePower = snap.batteryChargePower;
    g_batteryDischargePower = snap.batteryDischargePower;
    const bool externalSystemPower =
        g_totalSystemPowerSourceName.rfind("AIDA64", 0) == 0 ||
        g_totalSystemPowerSourceName == "Windows Power Meter";
    if (!externalSystemPower) {
        g_totalSystemPower = snap.totalSystemPower;
        g_totalSystemPowerSourceName = snap.totalSystemPowerSourceName;
    }
    if (snap.cpuFanRpm > 0.0f)
        g_cpuFanRpm = snap.cpuFanRpm;
    if (snap.gpuFanRpm > 0.0f)
        g_gpuFanRpm = snap.gpuFanRpm;
    if (snap.systemFanRpm > 0.0f)
        g_systemFanRpm = snap.systemFanRpm;
    g_gpuMemTemp = snap.gpuMemTemp;
    g_diskTemp = snap.diskTemp;
    g_memoryTemp = snap.memoryTemp;
    g_diskActivityPercent = snap.diskActivityPercent;
    g_networkUtilizationPercent = snap.networkUtilizationPercent;
    if (g_PowerRecoveryStatus.load(std::memory_order_acquire) ==
            PowerRecoveryStatus::Recovering &&
        g_lhwmResumeReadyTick.load(std::memory_order_acquire) == 0) {
        SetPowerRecoveryStatus(PowerRecoveryStatus::Recovered,
                               "fresh hardware snapshot applied");
    }
}

static void ScheduleComparisonPowerPoll()
{
    if (!g_lhwmAvailable || !LhwmReadsAllowed() ||
        g_asyncLhwmPollInFlight.load(std::memory_order_acquire) ||
        g_comparisonPowerPollInFlight.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    std::string cpuPowerPath;
    std::string cpuMemoryPowerPath;
    std::string cpuPlatformPowerPath;
    std::string gpuPowerPath;
    std::string discreteGpuPowerPath;
    std::string cpuFanPath;
    std::string gpuFanPath;
    {
        std::lock_guard<std::mutex> lock(g_lhwmStateMutex);
        cpuPowerPath = g_lhwmCpuPowerPath;
        cpuMemoryPowerPath = g_lhwmCpuMemoryPowerPath;
        cpuPlatformPowerPath = g_lhwmCpuPlatformPowerPath;
        gpuPowerPath = g_lhwmGpuPowerPath;
        discreteGpuPowerPath = g_lhwmDiscreteGpuPowerPath;
        cpuFanPath = g_lhwmCpuFanPath;
        gpuFanPath = g_lhwmGpuFanPath;
    }

    if (g_comparisonPowerPollThread.joinable())
        g_comparisonPowerPollThread.join();
    g_comparisonPowerPollThread = std::thread([
        cpuPowerPath,
        cpuMemoryPowerPath,
        cpuPlatformPowerPath,
        gpuPowerPath,
        discreteGpuPowerPath,
        cpuFanPath,
        gpuFanPath
    ]() {
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto readPower = [](const std::string& path) {
            if (path.empty())
                return 0.0f;
            const float value = ReadLhwmSensorValue(path);
            return ValidLaptopPowerValue(value) ? value : 0.0f;
        };
        auto readPositive = [](const std::string& path) {
            if (path.empty())
                return 0.0f;
            const float value = ReadLhwmSensorValue(path);
            return std::isfinite(value) && value > 0.0f ? value : 0.0f;
        };

        try {
            g_comparisonCpuPower.store(
                readPower(cpuPowerPath), std::memory_order_relaxed);
            g_comparisonCpuMemoryPower.store(
                readPower(cpuMemoryPowerPath), std::memory_order_relaxed);
            g_comparisonCpuPlatformPower.store(
                readPower(cpuPlatformPowerPath), std::memory_order_relaxed);
            g_comparisonGpuPower.store(
                readPower(gpuPowerPath), std::memory_order_relaxed);
            g_comparisonDiscreteGpuPower.store(
                discreteGpuPowerPath == gpuPowerPath
                    ? g_comparisonGpuPower.load(std::memory_order_relaxed)
                    : readPower(discreteGpuPowerPath),
                std::memory_order_relaxed);
            g_comparisonCpuFanRpm.store(
                readPositive(cpuFanPath), std::memory_order_relaxed);
            g_comparisonGpuFanRpm.store(
                readPositive(gpuFanPath), std::memory_order_relaxed);
            g_comparisonPowerSampleTick.store(
                GetTickCount64(), std::memory_order_release);
            g_comparisonPowerSampleSequence.fetch_add(
                1, std::memory_order_release);
        } catch (...) {
            // Keep the previous complete snapshot if the fast poll fails.
        }
        if (SUCCEEDED(comResult))
            CoUninitialize();
        g_comparisonPowerPollInFlight.store(false, std::memory_order_release);
    });
}

static void ApplyComparisonPowerPollSnapshot()
{
    static unsigned long long appliedSequence = 0;
    const unsigned long long sequence =
        g_comparisonPowerSampleSequence.load(std::memory_order_acquire);
    if (sequence == 0 || sequence == appliedSequence)
        return;

    appliedSequence = sequence;
    g_cpuPower = g_comparisonCpuPower.load(std::memory_order_relaxed);
    g_cpuMemoryPower =
        g_comparisonCpuMemoryPower.load(std::memory_order_relaxed);
    g_cpuPlatformPower =
        g_comparisonCpuPlatformPower.load(std::memory_order_relaxed);
    g_gpuPower = g_comparisonGpuPower.load(std::memory_order_relaxed);
    g_discreteGpuPower =
        g_comparisonDiscreteGpuPower.load(std::memory_order_relaxed);
    const float cpuFan =
        g_comparisonCpuFanRpm.load(std::memory_order_relaxed);
    const float gpuFan =
        g_comparisonGpuFanRpm.load(std::memory_order_relaxed);
    if (cpuFan > 0.0f)
        g_cpuFanRpm = cpuFan;
    if (gpuFan > 0.0f)
        g_gpuFanRpm = gpuFan;
}

// Switch to a different GPU by index
static void SelectGpu(int index)
{
    std::lock_guard<std::mutex> lock(g_lhwmStateMutex);
    if (index < 0 || index >= g_gpuCount) return;
    
    g_Config.selectedGpu = index;
    
    // Update active sensor paths
    g_lhwmGpuTempPath = g_gpuList[index].tempPath;
    g_lhwmGpuLoadPath = g_gpuList[index].loadPath;
    g_lhwmGpuVramUsedPath = g_gpuList[index].vramUsedPath;
    g_lhwmGpuVramTotalPath = g_gpuList[index].vramTotalPath;
    g_lhwmGpuPowerPath = g_gpuList[index].powerPath;
    g_lhwmGpuFanPath   = g_gpuList[index].fanPath;
    g_lhwmGpuMemTempPath = g_gpuList[index].memTempPath;
    g_lhwmGpuFanIsPercent = g_gpuList[index].fanIsPercent;

    snprintf(g_gpuName, sizeof(g_gpuName), "%s", g_gpuList[index].name);

    ValidateFrequencyPaths();
}

static std::string GetGpuNameSnapshot()
{
    std::lock_guard<std::mutex> lock(g_lhwmStateMutex);
    return g_gpuName;
}

// ═══════════════════════════════════════════════════════════════════════════
// ETW — game FPS capture (hooks DXGI Present events system-wide)
// ═══════════════════════════════════════════════════════════════════════════
static std::string PowerCandidateDisplayName(const PowerSensorCandidate& candidate)
{
    std::string hardware = candidate.hardwareName;
    const size_t marker = hardware.find(" : /");
    if (marker != std::string::npos)
        hardware.resize(marker);
    std::string sensor = candidate.sensorName;
    const std::string lower = LowerCopy(sensor);
    if (lower == "cpu package") sensor = "CPU 封装功耗";
    else if (lower == "cpu cores") sensor = "CPU 核心功耗";
    else if (lower == "cpu memory") sensor = "CPU 内存控制器功耗";
    else if (lower == "cpu platform") sensor = "CPU 平台功耗";
    else if (lower == "gpu power") sensor = "GPU 功耗";
    else if (lower == "gpu package") sensor = "GPU 封装功耗";
    else if (lower.find("total system") != std::string::npos) sensor = "整机总功耗";
    else if (lower.find("ac adapter") != std::string::npos) sensor = "适配器输入功率";
    return hardware + " / " + sensor;
}

[[maybe_unused]] static bool DrawPowerSensorBindingCombo(const char* label,
                                        const char* id,
                                        char* configuredPath,
                                        size_t configuredPathCap,
                                        std::string& activePath,
                                        const std::string& automaticPath)
{
    bool changed = false;
    const char* preview = "自动匹配";
    std::string previewStorage;
    if (configuredPath && configuredPath[0]) {
        for (const auto& candidate : g_powerSensorCandidates) {
            if (candidate.path == configuredPath) {
                previewStorage = PowerCandidateDisplayName(candidate);
                preview = previewStorage.c_str();
                break;
            }
        }
    }

    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo(id, preview)) {
        const bool automatic = !configuredPath || configuredPath[0] == '\0';
        if (ImGui::Selectable("自动匹配", automatic)) {
            if (configuredPath)
                configuredPath[0] = '\0';
            activePath = automaticPath;
            changed = true;
        }
        if (automatic)
            ImGui::SetItemDefaultFocus();

        for (const auto& candidate : g_powerSensorCandidates) {
            const bool selected = configuredPath && strcmp(configuredPath, candidate.path.c_str()) == 0;
            const std::string display = PowerCandidateDisplayName(candidate);
            if (ImGui::Selectable(display.c_str(), selected)) {
                snprintf(configuredPath, configuredPathCap, "%s", candidate.path.c_str());
                activePath = candidate.path;
                changed = true;
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
            if (ImGui::IsItemHovered()) {
                const float value = ReadLhwmSensorValue(candidate.path);
                ImGui::SetTooltip("路径：%s\n当前值：%.2f W", candidate.path.c_str(), value);
            }
        }
        ImGui::EndCombo();
    }

    const float currentValue = activePath.empty() ? 0.0f : ReadLhwmSensorValue(activePath);
    ImGui::TextColored(ImVec4(.52f, .58f, .66f, 1.0f), "当前路径：%s",
                       activePath.empty() ? "未找到" : activePath.c_str());
    ImGui::SameLine();
    if (ValidLaptopPowerValue(currentValue))
        ImGui::TextColored(ImVec4(.95f, .76f, .36f, 1.0f), "%.2f W", currentValue);
    else
        ImGui::TextColored(ImVec4(.55f, .55f, .60f, 1.0f), "N/A");
    return changed;
}

[[maybe_unused]] static bool DrawPowerSensorBindingSettings()
{
    return false;
}

static void RescanPowerSensors()
{
    if (!g_lhwmInitFinished.load(std::memory_order_acquire))
        return;
    const bool recoveringFault =
        g_lhwmNativeFaulted.exchange(false, std::memory_order_acq_rel);
    g_lhwmNativeFaultCode.store(0, std::memory_order_release);
    g_lhwmReadsPaused.store(false, std::memory_order_release);
    g_Config.acInputPowerPath[0] = '\0';
    g_Config.totalSystemPowerPath[0] = '\0';
    g_lhwmRescanRequested.store(true, std::memory_order_release);
    if (g_PowerRecoveryStatus.load(std::memory_order_acquire) ==
        PowerRecoveryStatus::Failed) {
        SetPowerRecoveryStatus(PowerRecoveryStatus::Recovering,
                               "manual sensor rescan");
    }
    LogLine("Hardware sensor rescan requested: recoveringFault=%d",
            recoveringFault ? 1 : 0);
}

static void ProcessLhwmRescanRequest()
{
    if (!g_lhwmRescanRequested.load(std::memory_order_acquire) ||
        !LhwmReadsAllowed() ||
        !g_lhwmInitFinished.load(std::memory_order_acquire) ||
        g_asyncLhwmPollInFlight.load(std::memory_order_acquire) ||
        g_comparisonPowerPollInFlight.load(std::memory_order_acquire)) {
        return;
    }

    if (!g_lhwmRescanRequested.exchange(false, std::memory_order_acq_rel))
        return;
    if (g_asyncLhwmPollThread.joinable())
        g_asyncLhwmPollThread.join();
    if (g_comparisonPowerPollThread.joinable())
        g_comparisonPowerPollThread.join();
    LogLine("LibreHardwareMonitor rescan started");
    StartLhwmInitializationThread();
}

static void GetExeDirectoryA(char* out, size_t cap)
{
    if (!out || cap == 0) return;
    InitConfigPath();
    snprintf(out, cap, "%s", g_configPath);
    char* slash = strrchr(out, '\\');
    if (slash) *(slash + 1) = '\0';
}

static void CopyFileIfExistsA(const char* src, const char* dst)
{
    if (!src || !dst || !src[0] || !dst[0]) return;
    if (GetFileAttributesA(src) != INVALID_FILE_ATTRIBUTES)
        CopyFileA(src, dst, FALSE);
}

static bool ExportDiagnosticsPackage(char* outDir, size_t cap)
{
    InitConfigPath();

    char exeDir[MAX_PATH] = {};
    GetExeDirectoryA(exeDir, sizeof(exeDir));

    char stamp[32] = {};
    BuildTimestampSuffix(stamp, sizeof(stamp));

    char diagDir[MAX_PATH] = {};
    snprintf(diagDir, sizeof(diagDir), "%sFPSOverlay_Diagnostics_%s", exeDir, stamp);
    if (!CreateDirectoryA(diagDir, nullptr)) {
        if (GetLastError() != ERROR_ALREADY_EXISTS)
            return false;
    }

    char summaryPath[MAX_PATH] = {};
    snprintf(summaryPath, sizeof(summaryPath), "%s\\summary.txt", diagDir);
    std::ofstream out(summaryPath, std::ios::out | std::ios::trunc);
    if (!out)
        return false;

    out << "FPS Overlay diagnostics\n";
    out << "Generated: " << stamp << "\n";
    out << "App version: " << APP_VERSION << "\n";
    out << "Config schema_version: " << CONFIG_SCHEMA_VERSION << "\n";
    out << "Config path: " << g_configPath << "\n";
    out << "Log path: " << g_logPath << "\n";
    if (g_lastConfigBackupPath[0] != '\0')
        out << "Last config recovery: " << g_lastConfigRecoveryReason << " -> " << g_lastConfigBackupPath << "\n";
    out << "\nRuntime\n";
    out << "Admin: " << (g_isAdmin ? "yes" : "no") << "\n";
    out << "Overlay visible: " << (g_OvlVisible ? "yes" : "no") << "\n";
    out << "Desktop-only mode: " << (g_Config.desktopOnlyMode ? "yes" : "no") << "\n";
    out << "Fullscreen auto-hidden: " << (g_FullscreenAutoHidden ? "yes" : "no") << "\n";
    out << "Foreground game confirmed: " << (g_ForegroundGameConfirmed ? "yes" : "no") << "\n";
    out << "Game overlay display: "
        << (g_GameOverlayDisplayActive ? "yes" : "no")
        << " pid=" << g_GameOverlayDisplayPid
        << " reason="
        << (g_GameOverlayDisplayReason[0] ? g_GameOverlayDisplayReason : "N/A")
        << "\n";
    out << "Overlay host rect: "
        << g_OverlayHostRect.left << "," << g_OverlayHostRect.top << ","
        << g_OverlayHostRect.right << "," << g_OverlayHostRect.bottom
        << " gameMonitor=" << (g_OverlayUsingGameMonitor ? "yes" : "no")
        << " owner=" << g_OverlayOwnerWindow
        << "\n";
    out << "Desktop visibility reason: " << g_DesktopVisibilityReason << "\n";
    out << "Desktop foreground PID: " << g_DesktopForegroundPid << "\n";
    out << "Desktop foreground exe: "
        << (g_DesktopForegroundExe[0] ? g_DesktopForegroundExe : "N/A") << "\n";
    out << "Live settings: " << (g_ShowLiveSettings ? "yes" : "no") << "\n";
    out << "Start with Windows: " << (g_Config.startWithWindows ? "yes" : "no") << "\n";
    out << "Auto-start task active: " << (g_autoLaunchTaskReady ? "yes" : "no") << "\n";
    out << "ETW available: " << (g_etwAvailable ? "yes" : "no") << "\n";
    out << "ETW running: " << (g_etwRunning.load(std::memory_order_relaxed) ? "yes" : "no") << "\n";
    out << "Target PID: " << g_targetPid.load(std::memory_order_relaxed) << "\n";
    out << "Target process: " << (g_targetProcessName[0] ? g_targetProcessName : "N/A") << "\n";
    std::string gpuNameForDiagnostics;
    std::string cpuTempPathForDiagnostics;
    std::string cpuPowerPathForDiagnostics;
    std::string cpuFanPathForDiagnostics;
    std::string gpuTempPathForDiagnostics;
    std::string gpuLoadPathForDiagnostics;
    std::string gpuPowerPathForDiagnostics;
    std::string gpuFanPathForDiagnostics;
    std::string gpuVramUsedPathForDiagnostics;
    std::string gpuVramTotalPathForDiagnostics;
    std::string acInputPathForDiagnostics;
    std::string totalSystemPathForDiagnostics;
    std::vector<PowerSensorCandidate> powerCandidatesForDiagnostics;
    int gpuCountForDiagnostics = 0;
    {
        std::lock_guard<std::mutex> lock(g_lhwmStateMutex);
        gpuNameForDiagnostics = g_gpuName;
        cpuTempPathForDiagnostics = g_lhwmCpuTempPath;
        cpuPowerPathForDiagnostics = g_lhwmCpuPowerPath;
        cpuFanPathForDiagnostics = g_lhwmCpuFanPath;
        gpuTempPathForDiagnostics = g_lhwmGpuTempPath;
        gpuLoadPathForDiagnostics = g_lhwmGpuLoadPath;
        gpuPowerPathForDiagnostics = g_lhwmGpuPowerPath;
        gpuFanPathForDiagnostics = g_lhwmGpuFanPath;
        gpuVramUsedPathForDiagnostics = g_lhwmGpuVramUsedPath;
        gpuVramTotalPathForDiagnostics = g_lhwmGpuVramTotalPath;
        acInputPathForDiagnostics = g_lhwmAcInputPowerPath;
        totalSystemPathForDiagnostics = g_lhwmTotalSystemPowerPath;
        powerCandidatesForDiagnostics = g_powerSensorCandidates;
        gpuCountForDiagnostics = g_gpuCount;
    }

    out << "\nHardware\n";
    out << "CPU: " << g_cpuName << "\n";
    out << "GPU: " << gpuNameForDiagnostics << "\n";
    out << "LHWM init finished: " << (g_lhwmInitFinished.load(std::memory_order_acquire) ? "yes" : "no") << "\n";
    out << "LHWM available: " << (g_lhwmAvailable ? "yes" : "no") << "\n";
    const unsigned long long hardwareSampleTick =
        g_asyncLhwmStatsSampleTick.load(std::memory_order_acquire);
    out << "Hardware sensor sample age: "
        << (hardwareSampleTick > 0
                ? std::to_string(GetTickCount64() - hardwareSampleTick) + " ms"
                : "N/A")
        << "\n";
    out << "GPU count: " << gpuCountForDiagnostics << "\n";
    out << "CPU temp path: " << (cpuTempPathForDiagnostics.empty() ? "N/A" : cpuTempPathForDiagnostics) << "\n";
    out << "CPU power path: " << (cpuPowerPathForDiagnostics.empty() ? "N/A" : cpuPowerPathForDiagnostics) << "\n";
    out << "CPU fan path: " << (cpuFanPathForDiagnostics.empty() ? "N/A" : cpuFanPathForDiagnostics) << "\n";
    out << "GPU temp path: " << (gpuTempPathForDiagnostics.empty() ? "N/A" : gpuTempPathForDiagnostics) << "\n";
    out << "GPU load path: " << (gpuLoadPathForDiagnostics.empty() ? "N/A" : gpuLoadPathForDiagnostics) << "\n";
    out << "GPU power path: " << (gpuPowerPathForDiagnostics.empty() ? "N/A" : gpuPowerPathForDiagnostics) << "\n";
    out << "GPU fan path: " << (gpuFanPathForDiagnostics.empty() ? "N/A" : gpuFanPathForDiagnostics) << "\n";
    out << "AC input power path: " << (acInputPathForDiagnostics.empty() ? "N/A" : acInputPathForDiagnostics) << "\n";
    out << "Total system power path: " << (totalSystemPathForDiagnostics.empty() ? "N/A" : totalSystemPathForDiagnostics) << "\n";
    out << "Manual AC input path: " << (g_Config.acInputPowerPath[0] ? g_Config.acInputPowerPath : "automatic") << "\n";
    out << "Manual total system path: " << (g_Config.totalSystemPowerPath[0] ? g_Config.totalSystemPowerPath : "automatic") << "\n";
    out << "Power sensor candidates: " << powerCandidatesForDiagnostics.size() << "\n";
    for (const auto& candidate : powerCandidatesForDiagnostics) {
        const float value = ReadLhwmSensorValue(candidate.path);
        out << "  [" << candidate.hardwareName << "] " << candidate.sensorName
            << " = " << value << " W | " << candidate.path << "\n";
    }
    out << "Current AC state: " << (g_FeatureContext.acOnline ? "AC" : "battery") << "\n";
    out << "Current AC input: "
        << (g_FeatureContext.hasAcInputPower ? std::to_string(g_FeatureContext.acInputPowerW) : "N/A")
        << " W\n";
    out << "Current battery charge: "
        << (g_FeatureContext.hasBatteryChargePower ? std::to_string(g_FeatureContext.batteryChargePowerW) : "N/A")
        << " W\n";
    out << "Current battery discharge: "
        << (g_FeatureContext.hasBatteryDischargePower ? std::to_string(g_FeatureContext.batteryDischargePowerW) : "N/A")
        << " W\n";
    out << "Current system power: "
        << (g_FeatureContext.hasEstimatedSystemPower ? std::to_string(g_FeatureContext.estimatedSystemPowerW) : "N/A")
        << " W\n";
    out << "Current system source: "
        << (g_FeatureContext.automaticSystemPowerSource[0]
                ? g_FeatureContext.automaticSystemPowerSource
                : "N/A")
        << "\n";
    out << "Current model confidence: " << g_FeatureContext.systemPowerEstimateConfidence
        << "% uncertainty=" << g_FeatureContext.systemPowerEstimateUncertaintyW << " W\n";
    if (g_FeatureContext.systemPowerEstimateBreakdown[0])
        out << "Current model breakdown: " << g_FeatureContext.systemPowerEstimateBreakdown << "\n";
    out << "VRAM used path: " << (gpuVramUsedPathForDiagnostics.empty() ? "N/A" : gpuVramUsedPathForDiagnostics) << "\n";
    out << "VRAM total path: " << (gpuVramTotalPathForDiagnostics.empty() ? "N/A" : gpuVramTotalPathForDiagnostics) << "\n";
    out << "\nG-Helper reference\n";
    out << "Repository: https://github.com/seerge/g-helper\n";
    out << "Commit: d7eb9cbfc2a38d43239dece00e3f5b9a165235c6\n";
    out << "Referenced files: app/HardwareControl.cs, app/Overlay/HardwareOverlay.cs\n";
    out << "Rewrite note: diagnostics follow the lightweight snapshot idea; no ASUS control logic was imported.\n";
    out.close();

    char dst[MAX_PATH] = {};
    snprintf(dst, sizeof(dst), "%s\\config.ini", diagDir);
    CopyFileIfExistsA(g_configPath, dst);
    snprintf(dst, sizeof(dst), "%s\\fps-overlay.log", diagDir);
    CopyFileIfExistsA(g_logPath, dst);

    char asusPath[MAX_PATH] = {};
    snprintf(asusPath, sizeof(asusPath), "%sasus-wmi-diagnostics.txt", exeDir);
    snprintf(dst, sizeof(dst), "%s\\asus-wmi-diagnostics.txt", diagDir);
    CopyFileIfExistsA(asusPath, dst);

    if (outDir && cap)
        snprintf(outDir, cap, "%s", diagDir);
    LogLine("Diagnostics exported: %s", diagDir);
    return true;
}

static bool SetClipboardTextUtf8(const char* text)
{
    if (!text || !text[0])
        return false;
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                     text, -1, nullptr, 0);
    if (length <= 0)
        length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (length <= 0)
        return false;

    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE,
                                 static_cast<SIZE_T>(length) * sizeof(wchar_t));
    if (!memory)
        return false;
    wchar_t* destination = static_cast<wchar_t*>(GlobalLock(memory));
    if (!destination) {
        GlobalFree(memory);
        return false;
    }
    MultiByteToWideChar(CP_UTF8, 0, text, -1, destination, length);
    GlobalUnlock(memory);

    if (!OpenClipboard(g_hwnd)) {
        GlobalFree(memory);
        return false;
    }
    EmptyClipboard();
    const bool success = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
    CloseClipboard();
    if (!success)
        GlobalFree(memory);
    return success;
}

static bool CopyDiagnosticSummaryToClipboard()
{
    const std::string gpuName = GetGpuNameSnapshot();
    const unsigned long long sampleTick =
        g_asyncLhwmStatsSampleTick.load(std::memory_order_acquire);
    const unsigned long long sampleAge = sampleTick > 0
        ? GetTickCount64() - sampleTick : 0;
    const float fps = g_gameFps.load(std::memory_order_relaxed);
    const float autoFps = g_autoGameFps.load(std::memory_order_relaxed);
    const DWORD targetPid = g_targetPid.load(std::memory_order_relaxed);

    char summary[4096] = {};
    snprintf(
        summary, sizeof(summary),
        "FPS Overlay 诊断摘要\r\n"
        "版本：%s\r\n"
        "Overlay：%s，布局=%d，仅桌面=%s\r\n"
        "目标：PID %lu，%s\r\n"
        "FPS：实时 %.1f，自动 %.1f，ETW=%s\r\n"
        "GPU：#%d %s\r\n"
        "LHWM：%s，样本年龄=%s%llu ms，隔离=%s\r\n"
        "CPU：%.0f%% %.0f°C %.1fW\r\n"
        "GPU：%.0f%% %.0f°C %.1fW\r\n"
        "整机功耗：%s%.1fW，来源=%s\r\n"
        "恢复状态：%d\r\n",
        APP_VERSION,
        g_OvlVisible ? "显示" : "隐藏",
        g_Config.layoutStyle,
        g_Config.desktopOnlyMode ? "是" : "否",
        static_cast<unsigned long>(targetPid),
        g_targetProcessName[0] ? g_targetProcessName : "N/A",
        fps, autoFps, g_etwAvailable ? "可用" : "不可用",
        g_Config.selectedGpu, gpuName.empty() ? "N/A" : gpuName.c_str(),
        g_lhwmAvailable.load(std::memory_order_acquire) ? "可用" : "不可用",
        sampleTick > 0 ? "" : "N/A ", sampleAge,
        g_lhwmNativeFaulted.load(std::memory_order_acquire) ? "是" : "否",
        g_FeatureContext.cpuUsage, g_FeatureContext.cpuTempC,
        g_FeatureContext.cpuPackagePowerW,
        g_FeatureContext.gpuUsage, g_FeatureContext.gpuTempC,
        g_FeatureContext.gpuPowerW,
        g_FeatureContext.systemPowerEstimated ? "≈" : "",
        g_FeatureContext.estimatedSystemPowerW,
        g_FeatureContext.automaticSystemPowerSource[0]
            ? g_FeatureContext.automaticSystemPowerSource : "N/A",
        static_cast<int>(g_PowerRecoveryStatus.load(std::memory_order_acquire)));

    const bool copied = SetClipboardTextUtf8(summary);
    LogLine("Diagnostic summary copy: %s", copied ? "ok" : "failed");
    return copied;
}

static void UpdateFeatureContext(float fps, float cpuUsage, float ramUsed, float ramTotal)
{
    FeatureContext ctx;
    ctx.fps = fps;
    ctx.frametimeMs = fps > 0.0f ? 1000.0f / fps : 0.0f;

    ctx.cpuUsage = cpuUsage;
    ctx.cpuTempC = g_cpuTemp;
    ctx.hasCpuTemp = g_cpuTempAvailable && g_cpuTemp > 0.0f;

    ctx.gpuUsage = g_gpuUsage;
    ctx.gpuTempC = g_gpuTemp;
    ctx.hasGpuTemp = g_lhwmAvailable.load(std::memory_order_acquire) && g_gpuTemp > 0.0f;
    ctx.gpuPowerW = g_gpuPower;
    ctx.hasGpuPower = ValidLaptopPowerValue(g_gpuPower);
    ctx.discreteGpuPowerW = g_discreteGpuPower;
    ctx.hasDiscreteGpuPower = ValidLaptopPowerValue(g_discreteGpuPower);
    ctx.cpuPackagePowerW = g_cpuPower;
    ctx.hasCpuPackagePower = ValidLaptopPowerValue(g_cpuPower);
    ctx.cpuMemoryPowerW = g_cpuMemoryPower;
    ctx.hasCpuMemoryPower = ValidLaptopPowerValue(g_cpuMemoryPower);
    ctx.cpuPlatformPowerW = g_cpuPlatformPower;
    ctx.hasCpuPlatformPower = ValidLaptopPowerValue(g_cpuPlatformPower);
    {
        std::lock_guard<std::mutex> lock(g_lhwmStateMutex);
        ctx.gpuPowerIsDiscrete =
            g_lhwmGpuPowerPath.find("/gpu-nvidia/") != std::string::npos ||
            g_lhwmGpuPowerPath.find("/gpu-amd/") != std::string::npos;
        ctx.diskCount = static_cast<int>(g_lhwmDiskActivityPaths.size());
    }
    ctx.powerSensorSampleSequence =
        g_comparisonPowerSampleSequence.load(std::memory_order_acquire);
    const unsigned long long powerSampleTick =
        g_comparisonPowerSampleTick.load(std::memory_order_acquire);
    ctx.hasPowerSensorSample = powerSampleTick > 0;
    ctx.powerSensorSampleAgeMs = powerSampleTick > 0
        ? GetTickCount64() - powerSampleTick
        : 0;
    const unsigned long long hardwareSampleTick =
        g_asyncLhwmStatsSampleTick.load(std::memory_order_acquire);
    ctx.hasHardwareSensorSample = hardwareSampleTick > 0;
    ctx.hardwareSensorSampleAgeMs = hardwareSampleTick > 0
        ? GetTickCount64() - hardwareSampleTick
        : 0;
    const unsigned long long staleAfterMs =
        (std::max)(5000ULL, static_cast<unsigned long long>(g_Config.refreshMs) * 3ULL);
    ctx.hardwareSensorDataStale =
        ctx.hasHardwareSensorSample && ctx.hardwareSensorSampleAgeMs > staleAfterMs;

    ctx.vramUsedGb = g_vramUsed;
    ctx.vramTotalGb = g_vramTotal;
    ctx.ramUsedGb = ramUsed;
    ctx.ramTotalGb = ramTotal;
    ctx.cpuFanRpm = g_cpuFanRpm;
    ctx.gpuFanRpm = g_gpuFanRpm;
    ctx.systemFanRpm = g_systemFanRpm;
    ctx.diskActivityPercent = g_diskActivityPercent;
    ctx.networkUtilizationPercent = g_networkUtilizationPercent;

    static ULONGLONG s_lastForegroundTitlePoll = 0;
    static char s_cachedForegroundTitle[256] = "";
    const ULONGLONG titleNow = GetTickCount64();
    if (s_lastForegroundTitlePoll == 0 || titleNow - s_lastForegroundTitlePoll >= 250) {
        s_lastForegroundTitlePoll = titleNow;
        HWND fg = GetForegroundWindow();
        s_cachedForegroundTitle[0] = '\0';
        if (fg) {
            wchar_t titleW[256] = {};
            if (GetWindowTextW(fg, titleW, 256) > 0)
                WideToUtf8(titleW, s_cachedForegroundTitle, sizeof(s_cachedForegroundTitle));
        }
    }
    snprintf(ctx.foregroundWindowTitle, sizeof(ctx.foregroundWindowTitle), "%s",
             s_cachedForegroundTitle);
    snprintf(ctx.gameProcessName, sizeof(ctx.gameProcessName), "%s", g_targetProcessName);
    ctx.gameProcessId = g_targetPid.load(std::memory_order_relaxed);
    ctx.isInGame = g_ForegroundGameConfirmed;

    SYSTEM_POWER_STATUS power = {};
    if (GetSystemPowerStatus(&power)) {
        ctx.isOnAcPower = power.ACLineStatus == 1 || power.ACLineStatus == 255;
        ctx.acOnline = ctx.isOnAcPower;
        ctx.hasBatteryPercent =
            (power.BatteryFlag & 128) == 0 && power.BatteryLifePercent != 255;
        if (ctx.hasBatteryPercent)
            ctx.batteryPercent = std::clamp((int)power.BatteryLifePercent, 0, 100);
        ctx.batteryCharging = (power.BatteryFlag & 8) != 0;
    }
    ctx.isRogStrixG815LR = IsRogStrixG815LR();
    ctx.hasDisplayBrightness =
        QueryDisplayBrightnessPercent(ctx.acOnline, &ctx.displayBrightnessPercent);

    if (ValidLaptopPowerValue(g_acInputPower)) {
        ctx.acInputPowerW = g_acInputPower;
        ctx.hasAcInputPower = true;
        snprintf(ctx.acInputPowerSource, sizeof(ctx.acInputPowerSource), "%s",
                 g_acInputPowerSourceName.empty()
                     ? "输入功耗"
                     : g_acInputPowerSourceName.c_str());
        snprintf(ctx.powerSource, sizeof(ctx.powerSource), "%s",
                 g_acInputPowerSourceName.empty() ? "输入功耗" : g_acInputPowerSourceName.c_str());
    }
    if (ValidLaptopPowerValue(g_batteryChargePower)) {
        ctx.batteryChargePowerW = g_batteryChargePower;
        ctx.hasBatteryChargePower = true;
        if (ctx.powerSource[0] == '\0') snprintf(ctx.powerSource, sizeof(ctx.powerSource), "%s", "LHM");
    }
    if (ValidLaptopPowerValue(g_batteryDischargePower)) {
        ctx.batteryDischargePowerW = g_batteryDischargePower;
        ctx.hasBatteryDischargePower = true;
        if (ctx.powerSource[0] == '\0') snprintf(ctx.powerSource, sizeof(ctx.powerSource), "%s", "LHM");
    }
    if (ValidLaptopPowerValue(g_totalSystemPower)) {
        ctx.estimatedSystemPowerW = g_totalSystemPower;
        ctx.hasEstimatedSystemPower = true;
        snprintf(ctx.powerSource, sizeof(ctx.powerSource), "%s",
                 g_totalSystemPowerSourceName.rfind("AIDA64", 0) == 0
                     ? "AIDA64"
                     : "LibreHardwareMonitor");
        snprintf(ctx.automaticSystemPowerSource, sizeof(ctx.automaticSystemPowerSource),
                 "%s", g_totalSystemPowerSourceName.c_str());
    }
    ctx.rescanPowerSensors = RescanPowerSensors;
    GetLocalTime(&ctx.currentTime);

    ctx.overlayVisible = g_OvlVisible;
    ctx.gameppLayout = g_Config.layoutStyle == LAYOUT_GAMEPP;
    g_FeatureContext = ctx;
}

static void LogPowerModelSnapshot(const FeatureContext& context)
{
    static ULONGLONG s_lastLogTick = 0;
    static bool s_hasLastState = false;
    static bool s_lastAcOnline = true;
    static std::string s_lastSource;

    const ULONGLONG now = GetTickCount64();
    const std::string source = context.automaticSystemPowerSource[0]
        ? context.automaticSystemPowerSource
        : (context.powerFailureReason[0] ? context.powerFailureReason : "N/A");
    const bool stateChanged =
        !s_hasLastState ||
        s_lastAcOnline != context.acOnline ||
        s_lastSource != source;
    if (!stateChanged && s_lastLogTick != 0 && now - s_lastLogTick < 60000)
        return;

    s_hasLastState = true;
    s_lastAcOnline = context.acOnline;
    s_lastSource = source;
    s_lastLogTick = now;

    LogLine("Power model snapshot: ac=%d source=%s system=%s%.1fW input=%s%.1fW "
            "charge=%s%.1fW discharge=%s%.1fW cpu=%s%.1fW dgpu=%s%.1fW "
            "estimated=%d confidence=%d uncertainty=%.1fW",
            context.acOnline ? 1 : 0,
            source.c_str(),
            context.hasEstimatedSystemPower ? "" : "N/A/",
            context.estimatedSystemPowerW,
            context.hasAcInputPower ? "" : "N/A/",
            context.acInputPowerW,
            context.hasBatteryChargePower ? "" : "N/A/",
            context.batteryChargePowerW,
            context.hasBatteryDischargePower ? "" : "N/A/",
            context.batteryDischargePowerW,
            context.hasCpuPackagePower ? "" : "N/A/",
            context.cpuPackagePowerW,
            context.hasDiscreteGpuPower ? "" : "N/A/",
            context.discreteGpuPowerW,
            context.systemPowerEstimated ? 1 : 0,
            context.systemPowerEstimateConfidence,
            context.systemPowerEstimateUncertaintyW);
}

static void UpdateAutoFpsCandidate(DWORD pid, bool isDxgiEvent, bool isD3D9Event,
                                   bool isDxgKrnlOnlyEvent, double ts)
{
    if (pid == 0 || pid == GetCurrentProcessId())
        return;

    struct Candidate {
        DWORD pid = 0;
        int dxgi = 0;
        int d3d9 = 0;
        int dxgk = 0;
    };
    static Candidate s_candidates[96];
    static double s_startTs = 0.0;

    if (s_startTs <= 0.0)
        s_startTs = ts;

    Candidate* slot = nullptr;
    Candidate* empty = nullptr;
    for (auto& c : s_candidates) {
        if (c.pid == pid) {
            slot = &c;
            break;
        }
        if (c.pid == 0 && !empty)
            empty = &c;
    }
    if (!slot) {
        slot = empty ? empty : &s_candidates[0];
        *slot = {};
        slot->pid = pid;
    }

    if (isDxgiEvent) slot->dxgi++;
    if (isD3D9Event) slot->d3d9++;
    if (isDxgKrnlOnlyEvent) slot->dxgk++;

    const double elapsed = ts - s_startTs;
    if (elapsed < 1.0)
        return;

    DWORD bestPid = 0;
    int bestFrames = 0;
    for (const auto& c : s_candidates) {
        if (c.pid == 0)
            continue;

        int frames = 0;
        if (c.d3d9 > 0)
            frames = c.d3d9;
        else if (c.dxgi > 0)
            frames = c.dxgi;
        else if (c.dxgk > 0)
            frames = c.dxgk;

        if (frames > bestFrames) {
            bestFrames = frames;
            bestPid = c.pid;
        }
    }

    const float fps = elapsed > 0.0 ? (float)bestFrames / (float)elapsed : 0.0f;
    if (bestPid != 0 && fps >= 20.0f) {
        g_autoTargetPid.store(bestPid, std::memory_order_relaxed);
        g_autoGameFps.store(fps, std::memory_order_relaxed);
    } else {
        g_autoTargetPid.store(0, std::memory_order_relaxed);
        g_autoGameFps.store(0.0f, std::memory_order_relaxed);
    }

    for (auto& c : s_candidates)
        c = {};
    s_startTs = ts;
}

static void WINAPI EtwCallback(PEVENT_RECORD pEvent)
{
    if (!g_etwRunning.load(std::memory_order_relaxed)) return;

    DWORD pid = pEvent->EventHeader.ProcessId;
    bool isValidPresentEvent = false;
    bool isDxgiEvent = false;
    bool isD3D9Event = false;
    bool isDxgKrnlOnlyEvent = false;
    
    // Check for DXGI Present::Start (Event ID 42) - DirectX 10/11/12
    if (memcmp(&pEvent->EventHeader.ProviderId, &DXGI_PROVIDER, sizeof(GUID)) == 0) {
        if (pEvent->EventHeader.EventDescriptor.Id == 42) {
            isValidPresentEvent = true;
            isDxgiEvent = true;
        }
    }
    // Check for D3D9 Present events (Event ID 1 = Present::Start)
    else if (memcmp(&pEvent->EventHeader.ProviderId, &D3D9_PROVIDER, sizeof(GUID)) == 0) {
        if (pEvent->EventHeader.EventDescriptor.Id == 1) {
            isValidPresentEvent = true;
            isD3D9Event = true;
        }
    }
    // Check for DxgKrnl events - captures Vulkan, OpenGL, and all other graphics APIs at kernel level
    else if (memcmp(&pEvent->EventHeader.ProviderId, &DXGKRNL_PROVIDER, sizeof(GUID)) == 0) {
        USHORT eventId = pEvent->EventHeader.EventDescriptor.Id;
        // Present::Info, Flip::Info, or Blit::Info events indicate a frame present
        if (eventId == DXGKRNL_EVENT_PRESENT_INFO ||
            eventId == DXGKRNL_EVENT_FLIP_INFO ||
            eventId == DXGKRNL_EVENT_BLIT_INFO) {
            isValidPresentEvent = true;
            isDxgKrnlOnlyEvent = true;
        }
    }

    if (!isValidPresentEvent) return;

    double ts = (double)pEvent->EventHeader.TimeStamp.QuadPart / g_qpcFreq;
    UpdateAutoFpsCandidate(pid, isDxgiEvent, isD3D9Event, isDxgKrnlOnlyEvent, ts);

    DWORD target = g_targetPid.load(std::memory_order_relaxed);
    if (target == 0 || pid != target) return;

    // Simple 1-second accumulator (all on the ETW thread — no lock needed)
    static DWORD s_lastPid   = 0;
    static double s_startTs  = 0;
    static int   s_dxgiCount = 0;      // Count of DXGI events (DirectX 10/11/12)
    static int   s_d3d9Count = 0;      // Count of D3D9 events
    static int   s_dxgKrnlCount = 0;   // Count of DxgKrnl-only events (Vulkan/OpenGL)

    if (pid != s_lastPid) { 
        s_lastPid = pid; 
        s_dxgiCount = 0;
        s_d3d9Count = 0;
        s_dxgKrnlCount = 0;
        s_startTs = ts; 
        return; 
    }

    // Count events by source
    if (isDxgiEvent) s_dxgiCount++;
    if (isD3D9Event) s_d3d9Count++;
    if (isDxgKrnlOnlyEvent) s_dxgKrnlCount++;
    
    double elapsed = ts - s_startTs;
    if (elapsed >= 1.0) {
        // Prioritize DXGI/D3D9 (explicit game API calls) over DxgKrnl (kernel-level)
        // This filters out desktop apps like explorer.exe that only show up in DxgKrnl
        //
        // Priority:
        // 1. D3D9 events = DirectX 9 game
        // 2. DXGI events = DirectX 10/11/12 game  
        // 3. ONLY if no DXGI/D3D9 events, use DxgKrnl = Vulkan/OpenGL game
        //
        // Desktop apps (explorer, terminals, browsers) go through DWM which uses DxgKrnl,
        // but they don't call DXGI/D3D9 Present directly, so they get filtered out.
        
        int frameCount = 0;
        
        if (s_d3d9Count > 0) {
            // DirectX 9 game - use D3D9 count
            frameCount = s_d3d9Count;
        } else if (s_dxgiCount > 0) {
            // DirectX 10/11/12 game - use DXGI count
            frameCount = s_dxgiCount;
        } else if (s_dxgKrnlCount > 0) {
            // No DXGI/D3D9 events, only DxgKrnl
            // This could be Vulkan/OpenGL OR a desktop app through DWM
            // 
            // Filter: Only count as a game if FPS >= 20
            // Real games render at 20+ FPS, desktop apps typically < 20 FPS
            float potentialFps = (float)s_dxgKrnlCount / (float)elapsed;
            if (potentialFps >= 20.0f) {
                frameCount = s_dxgKrnlCount;
            }
            // If < 20 FPS, treat as desktop app (frameCount stays 0)
        }
        
        if (frameCount > 0) {
            g_gameFps.store((float)frameCount / (float)elapsed, std::memory_order_relaxed);
        } else {
            g_gameFps.store(0.0f, std::memory_order_relaxed);
        }
        
        s_dxgiCount = 0;
        s_d3d9Count = 0;
        s_dxgKrnlCount = 0;
        s_startTs = ts;
    }
}

static bool StartEtwSession()
{
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    g_qpcFreq = (double)freq.QuadPart;

    // Buffer for properties + session name
    struct { EVENT_TRACE_PROPERTIES p; char name[256]; } buf;

    // Stop any leftover session from a previous crash
    ZeroMemory(&buf, sizeof(buf));
    buf.p.Wnode.BufferSize   = sizeof(buf);
    buf.p.LoggerNameOffset   = offsetof(decltype(buf), name);
    ControlTraceA(0, ETW_SESSION_NAME, &buf.p, EVENT_TRACE_CONTROL_STOP);

    // Prepare fresh properties
    ZeroMemory(&buf, sizeof(buf));
    buf.p.Wnode.BufferSize    = sizeof(buf);
    buf.p.Wnode.Flags         = WNODE_FLAG_TRACED_GUID;
    buf.p.Wnode.ClientContext = 1;                        // QPC timestamps
    buf.p.LogFileMode         = EVENT_TRACE_REAL_TIME_MODE;
    buf.p.LoggerNameOffset    = offsetof(decltype(buf), name);

    ULONG rc = StartTraceA(&g_etwSession, ETW_SESSION_NAME, &buf.p);
    if (rc != ERROR_SUCCESS) return false;

    // Enable DXGI provider for DirectX 10/11/12 Present events
    rc = EnableTraceEx2(g_etwSession, &DXGI_PROVIDER,
                        EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                        TRACE_LEVEL_INFORMATION, 0, 0, 0, nullptr);
    if (rc != ERROR_SUCCESS) {
        ZeroMemory(&buf, sizeof(buf));
        buf.p.Wnode.BufferSize = sizeof(buf);
        buf.p.LoggerNameOffset = offsetof(decltype(buf), name);
        ControlTraceA(g_etwSession, nullptr, &buf.p, EVENT_TRACE_CONTROL_STOP);
        g_etwSession = 0;
        return false;
    }

    // Enable D3D9 provider for DirectX 9 games
    rc = EnableTraceEx2(g_etwSession, &D3D9_PROVIDER,
                        EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                        TRACE_LEVEL_INFORMATION, 0, 0, 0, nullptr);
    // D3D9 provider is optional - continue if it fails

    // Enable DxgKrnl provider for Vulkan, OpenGL, and all other graphics APIs
    // The Present keyword (0x8000000) captures Present, Flip, and Blit events at the kernel level
    rc = EnableTraceEx2(g_etwSession, &DXGKRNL_PROVIDER,
                        EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                        TRACE_LEVEL_INFORMATION, 
                        DXGKRNL_KEYWORD_PRESENT | DXGKRNL_KEYWORD_BASE, 
                        0, 0, nullptr);
    if (rc != ERROR_SUCCESS) {
        // DxgKrnl failed - continue anyway with just DXGI (DirectX will still work)
        // This might fail on older Windows versions or without proper permissions
    }

    EVENT_TRACE_LOGFILEA logFile = {};
    logFile.LoggerName          = const_cast<LPSTR>(ETW_SESSION_NAME);
    logFile.ProcessTraceMode    = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    logFile.EventRecordCallback = EtwCallback;

    g_etwTrace = OpenTraceA(&logFile);
    if (g_etwTrace == (TRACEHANDLE)INVALID_HANDLE_VALUE) {
        ZeroMemory(&buf, sizeof(buf));
        buf.p.Wnode.BufferSize = sizeof(buf);
        buf.p.LoggerNameOffset = offsetof(decltype(buf), name);
        ControlTraceA(g_etwSession, nullptr, &buf.p, EVENT_TRACE_CONTROL_STOP);
        g_etwSession = 0;
        return false;
    }

    g_etwRunning.store(true);
    g_etwThread = std::thread([]() {
        TRACEHANDLE h = g_etwTrace;
        ProcessTrace(&h, 1, nullptr, nullptr);
    });

    return true;
}

static void StopEtwSession()
{
    if (!g_etwRunning.load()) return;
    g_etwRunning.store(false);

    if (g_etwTrace != 0 && g_etwTrace != (TRACEHANDLE)INVALID_HANDLE_VALUE) {
        CloseTrace(g_etwTrace);
        g_etwTrace = 0;
    }
    if (g_etwThread.joinable())
        g_etwThread.join();

    struct { EVENT_TRACE_PROPERTIES p; char name[256]; } buf;
    ZeroMemory(&buf, sizeof(buf));
    buf.p.Wnode.BufferSize = sizeof(buf);
    buf.p.LoggerNameOffset = offsetof(decltype(buf), name);
    ControlTraceA(g_etwSession, ETW_SESSION_NAME, &buf.p, EVENT_TRACE_CONTROL_STOP);
    g_etwSession = 0;

    g_gameFps.store(0.0f);
    g_autoGameFps.store(0.0f);
    g_autoTargetPid.store(0);
}

// ═══════════════════════════════════════════════════════════════════════════
// CPU usage
// ═══════════════════════════════════════════════════════════════════════════
static float GetCpuUsage()
{
    static ULARGE_INTEGER sI = {}, sK = {}, sU = {};
    FILETIME fi, fk, fu;
    if (!GetSystemTimes(&fi, &fk, &fu)) return 0;

    ULARGE_INTEGER i,k,u;
    i.LowPart = fi.dwLowDateTime; i.HighPart = fi.dwHighDateTime;
    k.LowPart = fk.dwLowDateTime; k.HighPart = fk.dwHighDateTime;
    u.LowPart = fu.dwLowDateTime; u.HighPart = fu.dwHighDateTime;

    ULONGLONG di = i.QuadPart - sI.QuadPart;
    ULONGLONG dk = k.QuadPart - sK.QuadPart;
    ULONGLONG du = u.QuadPart - sU.QuadPart;
    sI = i; sK = k; sU = u;

    ULONGLONG total = dk + du;
    return total ? (1.0f - (float)di / (float)total) * 100.0f : 0.0f;
}

static float UpdateRollingAverageFps(float fps, DWORD targetPid)
{
    static constexpr size_t kSampleCount = 60;
    static float samples[kSampleCount] = {};
    static size_t count = 0;
    static size_t next = 0;
    static float sum = 0.0f;
    static DWORD trackedPid = 0;
    static ULONGLONG lastSampleTick = 0;

    if (targetPid == 0 || targetPid != trackedPid) {
        count = 0;
        next = 0;
        sum = 0.0f;
        trackedPid = targetPid;
        lastSampleTick = 0;
    }
    if (targetPid == 0)
        return 0.0f;

    const ULONGLONG now = GetTickCount64();
    if (fps > 0.0f && std::isfinite(fps) &&
        (lastSampleTick == 0 || now - lastSampleTick >= 1000)) {
        if (count == kSampleCount) {
            sum -= samples[next];
        } else {
            ++count;
        }
        samples[next] = fps;
        sum += fps;
        next = (next + 1) % kSampleCount;
        lastSampleTick = now;
    }
    return count > 0 ? sum / static_cast<float>(count) : 0.0f;
}

static ImVec4 FpsTierColor(float fps)
{
    const float good = g_Config.customFpsColors
        ? static_cast<float>(g_Config.fpsGoodThreshold) : 60.0f;
    const float warning = g_Config.customFpsColors
        ? static_cast<float>(g_Config.fpsWarningThreshold) : 30.0f;
    if (fps >= good)
        return ImVec4(.18f, .94f, .45f, 1.0f);
    if (fps >= warning)
        return ImVec4(1.0f, .85f, .15f, 1.0f);
    return ImVec4(1.0f, .25f, .25f, 1.0f);
}

// ═══════════════════════════════════════════════════════════════════════════
// Tray icon
// ═══════════════════════════════════════════════════════════════════════════
void AddTrayIcon()
{
    if (g_trayThread.joinable()) {
        if (g_trayHwnd.load(std::memory_order_acquire))
            return;
        g_trayThread.join();
    }
    if (g_trayReadyEvent) {
        CloseHandle(g_trayReadyEvent);
        g_trayReadyEvent = nullptr;
    }
    g_trayReadyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_trayOverlayVisible.store(g_OvlVisible, std::memory_order_relaxed);
    g_trayThread = std::thread(TrayThreadMain);
    if (g_trayReadyEvent)
        WaitForSingleObject(g_trayReadyEvent, 2000);
}

void RemoveTrayIcon()
{
    HWND tray = g_trayHwnd.load(std::memory_order_acquire);
    if (tray)
        PostMessageW(tray, WM_CLOSE, 0, 0);
    if (g_trayThread.joinable())
        g_trayThread.join();
    if (g_trayReadyEvent) {
        CloseHandle(g_trayReadyEvent);
        g_trayReadyEvent = nullptr;
    }
}

void UpdateTrayTooltip()
{
    if (HWND tray = g_trayHwnd.load(std::memory_order_acquire))
        PostMessageW(tray, WM_APP_TRAY_TOOLTIP, 0, 0);
}

static int ScalePx(int px)
{
    return (int)lroundf((float)px * g_dpiScale);
}

static UINT QueryDpiForWindowOrSystem(HWND hwnd)
{
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (user32 && hwnd) {
        typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
        auto getDpiForWindow = (GetDpiForWindowFn)GetProcAddress(user32, "GetDpiForWindow");
        if (getDpiForWindow) {
            UINT dpi = getDpiForWindow(hwnd);
            if (dpi > 0) return dpi;
        }
    }
    if (user32) {
        typedef UINT (WINAPI *GetDpiForSystemFn)(void);
        auto getDpiForSystem = (GetDpiForSystemFn)GetProcAddress(user32, "GetDpiForSystem");
        if (getDpiForSystem) {
            UINT dpi = getDpiForSystem();
            if (dpi > 0) return dpi;
        }
    }
    HDC dc = GetDC(nullptr);
    UINT dpi = dc ? (UINT)GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return dpi > 0 ? dpi : 96;
}

static void RefreshDpiScale(HWND hwnd)
{
    g_dpiScale = (float)QueryDpiForWindowOrSystem(hwnd) / 96.0f;
    if (g_dpiScale < 0.75f) g_dpiScale = 0.75f;
    if (g_dpiScale > 4.0f) g_dpiScale = 4.0f;
}

static void EnableProcessDpiAwareness()
{
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (user32) {
        typedef BOOL (WINAPI *SetProcessDpiAwarenessContextFn)(DPI_AWARENESS_CONTEXT);
        auto setCtx = (SetProcessDpiAwarenessContextFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (setCtx && setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
            return;

        typedef BOOL (WINAPI *SetProcessDPIAwareFn)(void);
        auto setAware = (SetProcessDPIAwareFn)GetProcAddress(user32, "SetProcessDPIAware");
        if (setAware) setAware();
    }
}

static void BuildUiFontAtlas(ImGuiIO& io)
{
    io.Fonts->Clear();

    ImFontGlyphRangesBuilder glyphBuilder;
    glyphBuilder.AddRanges(io.Fonts->GetGlyphRangesDefault());
    // The full CJK set creates a very large atlas and texture. The Simplified
    // Chinese common set covers this UI while substantially reducing startup
    // memory, font upload time and DPI-rebuild stalls.
    glyphBuilder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    glyphBuilder.AddChar((ImWchar)0x5C4F); // 屏
    glyphBuilder.AddChar((ImWchar)0x5E27); // 帧
    glyphBuilder.AddChar((ImWchar)0x9608); // 阈
    glyphBuilder.AddChar((ImWchar)0x9891); // 频
    glyphBuilder.AddChar((ImWchar)0x2122); // TRADE MARK SIGN
    glyphBuilder.AddChar((ImWchar)0x00A9); // COPYRIGHT SIGN
    glyphBuilder.AddChar((ImWchar)0x00AE); // REGISTERED SIGN
    static ImVector<ImWchar> s_imguiGlyphRanges;
    s_imguiGlyphRanges.clear();
    glyphBuilder.BuildRanges(&s_imguiGlyphRanges);

    const float fontSize = 17.0f * g_dpiScale;
    ImFont* font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", fontSize, nullptr,
                                                s_imguiGlyphRanges.Data);
    if (!font)
        font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\simhei.ttf", fontSize, nullptr,
                                            s_imguiGlyphRanges.Data);
    if (!font)
        font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", fontSize, nullptr,
                                            s_imguiGlyphRanges.Data);
    if (!font) {
        ImFontConfig fc;
        fc.SizePixels = 16.0f * g_dpiScale;
        io.Fonts->AddFontDefault(&fc);
    }
}

void ApplyStyle();

static void RebuildUiForDpi(HWND hwnd)
{
    RefreshDpiScale(hwnd);
    ApplyStyle();

    if (ImGui::GetCurrentContext() && g_pd3dDevice) {
        ImGuiIO& io = ImGui::GetIO();
        ImGui_ImplDX11_InvalidateDeviceObjects();
        BuildUiFontAtlas(io);
        ImGui_ImplDX11_CreateDeviceObjects();
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// ImGui style
// ═══════════════════════════════════════════════════════════════════════════
void ApplyStyle()
{
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 10; s.FrameRounding = 6; s.GrabRounding = 6; s.TabRounding = 6;
    s.WindowBorderSize = 1; s.FrameBorderSize = 0;
    s.WindowPadding = ImVec2(14, 10);
    s.FramePadding  = ImVec2(8, 5);
    s.ItemSpacing   = ImVec2(10, 8);
    s.ScaleAllSizes(g_dpiScale);

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]         = ImVec4(0.012f,0.020f,0.048f,1);
    c[ImGuiCol_Border]           = ImVec4(0.08f,0.55f,0.82f,0.55f);
    c[ImGuiCol_FrameBg]          = ImVec4(0.04f,0.07f,0.13f,1);
    c[ImGuiCol_FrameBgHovered]   = ImVec4(0.05f,0.15f,0.24f,1);
    c[ImGuiCol_FrameBgActive]    = ImVec4(0.08f,0.21f,0.32f,1);
    c[ImGuiCol_CheckMark]        = ImVec4(0.10f,0.92f,1.00f,1);
    c[ImGuiCol_SliderGrab]       = ImVec4(0.08f,0.76f,1.00f,1);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.58f,0.38f,1.00f,1);
    c[ImGuiCol_Button]           = ImVec4(0.04f,0.10f,0.18f,1);
    c[ImGuiCol_ButtonHovered]    = ImVec4(0.06f,0.21f,0.33f,1);
    c[ImGuiCol_ButtonActive]     = ImVec4(0.19f,0.12f,0.42f,1);
    c[ImGuiCol_Header]           = ImVec4(0.04f,0.15f,0.25f,1);
    c[ImGuiCol_HeaderHovered]    = ImVec4(0.05f,0.26f,0.40f,1);
    c[ImGuiCol_HeaderActive]     = ImVec4(0.24f,0.13f,0.50f,1);
    c[ImGuiCol_Separator]        = ImVec4(0.08f,0.48f,0.72f,0.75f);
}

// ═══════════════════════════════════════════════════════════════════════════
// DX11
// ═══════════════════════════════════════════════════════════════════════════
bool CreateDeviceD3D(HWND hWnd)
{
    if (!hWnd || !IsWindow(hWnd))
        return false;
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = 0;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_10_0;
    auto createDevice = [&](D3D_DRIVER_TYPE driverType) {
        return D3D11CreateDeviceAndSwapChain(
            nullptr, driverType, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &sd,
            &g_pSwapChain, &g_pd3dDevice, &got, &g_pd3dDeviceContext);
    };

    HRESULT hr = createDevice(D3D_DRIVER_TYPE_HARDWARE);
    if (FAILED(hr)) {
        CleanupDeviceD3D();
        hr = createDevice(D3D_DRIVER_TYPE_WARP);
        if (SUCCEEDED(hr))
            LogLine("DirectX hardware device unavailable; using WARP fallback");
    }
    g_LastD3DError = hr;
    if (FAILED(hr)) {
        CleanupDeviceD3D();
        return false;
    }
    if (!CreateRenderTarget()) {
        CleanupDeviceD3D();
        return false;
    }
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain)        { g_pSwapChain->Release();        g_pSwapChain        = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice)        { g_pd3dDevice->Release();        g_pd3dDevice        = nullptr; }
}

bool CreateRenderTarget()
{
    if (!g_pSwapChain || !g_pd3dDevice)
        return false;
    ID3D11Texture2D* buf = nullptr;
    HRESULT hr = g_pSwapChain->GetBuffer(
        0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&buf));
    if (FAILED(hr) || !buf) {
        g_LastD3DError = hr;
        return false;
    }
    hr = g_pd3dDevice->CreateRenderTargetView(buf, nullptr, &g_pRenderTargetView);
    buf->Release();
    g_LastD3DError = hr;
    return SUCCEEDED(hr) && g_pRenderTargetView != nullptr;
}

void CleanupRenderTarget()
{
    if (g_pRenderTargetView) { g_pRenderTargetView->Release(); g_pRenderTargetView = nullptr; }
}

// ═══════════════════════════════════════════════════════════════════════════
// Backend / mode helpers
// ═══════════════════════════════════════════════════════════════════════════
void ShutdownBackends()
{
    if (g_ImGuiDx11Initialized) {
        ImGui_ImplDX11_Shutdown();
        g_ImGuiDx11Initialized = false;
    }
    if (g_ImGuiWin32Initialized) {
        ImGui_ImplWin32_Shutdown();
        g_ImGuiWin32Initialized = false;
    }
    CleanupDeviceD3D();
}

bool InitBackends()
{
    if (!CreateDeviceD3D(g_hwnd))
        return false;
    if (!ImGui_ImplWin32_Init(g_hwnd)) {
        CleanupDeviceD3D();
        return false;
    }
    g_ImGuiWin32Initialized = true;
    if (!ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext)) {
        ImGui_ImplWin32_Shutdown();
        g_ImGuiWin32Initialized = false;
        CleanupDeviceD3D();
        return false;
    }
    g_ImGuiDx11Initialized = true;
    return true;
}

static bool IsD3DDeviceLoss(HRESULT hr)
{
    return hr == DXGI_ERROR_DEVICE_REMOVED ||
           hr == DXGI_ERROR_DEVICE_RESET ||
           hr == DXGI_ERROR_DEVICE_HUNG ||
           hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

static void RequestD3DRecovery(HRESULT hr, const char* reason)
{
    g_LastD3DError = hr;
    if (!g_D3DRecoveryPending) {
        LogLine("DirectX recovery requested: hr=0x%08lX reason=%s",
                (unsigned long)hr, reason ? reason : "unknown");
    }
    g_D3DRecoveryPending = true;
    g_D3DRecoveryRetryTick = 0;
}

static bool RecoverD3DDevice()
{
    if (!g_D3DRecoveryPending)
        return true;
    if (!g_hwnd || !IsWindow(g_hwnd))
        return false;

    const ULONGLONG now = GetTickCount64();
    if (g_D3DRecoveryRetryTick != 0 && now < g_D3DRecoveryRetryTick)
        return false;

    if (g_ImGuiDx11Initialized) {
        ImGui_ImplDX11_Shutdown();
        g_ImGuiDx11Initialized = false;
    }
    CleanupDeviceD3D();

    if (!CreateDeviceD3D(g_hwnd) ||
        !ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext)) {
        CleanupDeviceD3D();
        g_D3DRecoveryRetryTick = now + 1000;
        LogLine("DirectX recovery retry scheduled: hr=0x%08lX",
                (unsigned long)g_LastD3DError);
        return false;
    }

    g_ImGuiDx11Initialized = true;
    g_D3DRecoveryPending = false;
    g_D3DRecoveryRetryTick = 0;
    LogLine("DirectX recovery completed");
    return true;
}

// Toggle click-through mode on the overlay window
static void SetClickThrough(bool enable)
{
    LONG_PTR oldStyle = GetWindowLongPtr(g_hwnd, GWL_EXSTYLE);
    LONG_PTR style = oldStyle;
    if (enable) {
        style |= WS_EX_TRANSPARENT;
        style |= WS_EX_NOACTIVATE;
    } else {
        style &= ~WS_EX_TRANSPARENT;
        style &= ~WS_EX_NOACTIVATE;
    }
    if (style != oldStyle) {
        SetWindowLongPtr(g_hwnd, GWL_EXSTYLE, style);
        SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOOWNERZORDER | SWP_NOACTIVATE);
    }
}

static void SetLiveSettingsWindowLevel(bool settingsOpen)
{
    if (!g_hwnd || g_Mode != MODE_OVERLAY)
        return;

    if (settingsOpen &&
        (g_OverlayOwnerWindow || GetWindow(g_hwnd, GW_OWNER))) {
        SetWindowLongPtr(g_hwnd, GWLP_HWNDPARENT, 0);
        g_OverlayOwnerWindow = nullptr;
    }

    SetWindowPos(
        g_hwnd,
        settingsOpen ? HWND_NOTOPMOST : HWND_TOPMOST,
        0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER |
        SWP_SHOWWINDOW | (settingsOpen ? 0 : SWP_NOACTIVATE));

    if (settingsOpen) {
        ShowWindow(g_hwnd, SW_SHOW);
        SetForegroundWindow(g_hwnd);
    }
}

static bool RectEquals(const RECT& a, const RECT& b)
{
    return a.left == b.left && a.top == b.top &&
           a.right == b.right && a.bottom == b.bottom;
}

static bool GetMonitorRectForWindow(HWND hwnd, RECT* outRect)
{
    if (!hwnd || !outRect)
        return false;
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
    if (!monitor)
        return false;
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfo(monitor, &info))
        return false;
    *outRect = info.rcMonitor;
    return true;
}

static bool GetPrimaryMonitorRect(RECT* outRect)
{
    if (!outRect)
        return false;
    POINT pt = { 0, 0 };
    HMONITOR monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfo(monitor, &info))
        return false;
    *outRect = info.rcMonitor;
    return true;
}

struct PidWindowSearch {
    DWORD pid = 0;
    HWND hwnd = nullptr;
    LONG bestArea = 0;
};

static BOOL CALLBACK EnumPidWindowsProc(HWND hwnd, LPARAM param)
{
    auto* search = reinterpret_cast<PidWindowSearch*>(param);
    if (!search || !hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd))
        return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != search->pid)
        return TRUE;

    LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) || (exStyle & WS_EX_NOACTIVATE))
        return TRUE;

    RECT rc = {};
    if (!GetWindowRect(hwnd, &rc))
        return TRUE;
    const LONG w = rc.right - rc.left;
    const LONG h = rc.bottom - rc.top;
    if (w < 320 || h < 200)
        return TRUE;

    const LONG area = w * h;
    if (area > search->bestArea) {
        search->bestArea = area;
        search->hwnd = hwnd;
    }
    return TRUE;
}

static bool FindLargestVisibleWindowForPid(DWORD pid, HWND* outHwnd)
{
    if (outHwnd)
        *outHwnd = nullptr;
    if (pid == 0)
        return false;
    PidWindowSearch search = {};
    search.pid = pid;
    EnumWindows(EnumPidWindowsProc, reinterpret_cast<LPARAM>(&search));
    if (!search.hwnd)
        return false;
    if (outHwnd)
        *outHwnd = search.hwnd;
    return true;
}

static bool ResolveOverlayHostRect(bool gameMonitor, HWND foreground,
                                   DWORD targetPid, RECT* outRect)
{
    if (!outRect)
        return false;

    if (gameMonitor) {
        HWND gameWindow = nullptr;
        if (FindLargestVisibleWindowForPid(targetPid, &gameWindow) &&
            GetMonitorRectForWindow(gameWindow, outRect)) {
            return true;
        }
        if (foreground && foreground != g_hwnd &&
            GetMonitorRectForWindow(foreground, outRect)) {
            return true;
        }
        return GetPrimaryMonitorRect(outRect);
    }

    return SystemParametersInfo(SPI_GETWORKAREA, 0, outRect, 0) == TRUE;
}

static void DetachOverlayOwnerWindow(const char* reason)
{
    if (!g_hwnd || !IsWindow(g_hwnd))
        return;

    const HWND actualOwner = GetWindow(g_hwnd, GW_OWNER);
    if (!actualOwner && !g_OverlayOwnerWindow)
        return;

    SetLastError(ERROR_SUCCESS);
    SetWindowLongPtr(g_hwnd, GWLP_HWNDPARENT, 0);
    const DWORD err = GetLastError();
    if (err == ERROR_SUCCESS) {
        LogLine("Overlay owner detached: previous=%p reason=%s",
                actualOwner ? actualOwner : g_OverlayOwnerWindow,
                reason && reason[0] ? reason : "stability");
        g_OverlayOwnerWindow = nullptr;
    } else {
        LogLine("Overlay owner detach failed: owner=%p err=%lu reason=%s",
                actualOwner ? actualOwner : g_OverlayOwnerWindow,
                (unsigned long)err,
                reason && reason[0] ? reason : "stability");
    }
}

static void UpdateOverlayHostWindow(bool gameMonitor, HWND foreground,
                                    DWORD targetPid, bool force)
{
    if (!g_hwnd || g_Mode != MODE_OVERLAY || g_ShowLiveSettings)
        return;

    static ULONGLONG s_lastPlacementTick = 0;
    static RECT s_lastAppliedRect = {};
    static bool s_lastGameMonitor = false;

    const ULONGLONG now = GetTickCount64();
    if (!force && now - s_lastPlacementTick < (gameMonitor ? 16ULL : 1500ULL))
        return;
    s_lastPlacementTick = now;

    HWND gameWindow = nullptr;
    if (gameMonitor)
        FindLargestVisibleWindowForPid(targetPid, &gameWindow);

    RECT host = {};
    if (gameWindow && gameMonitor) {
        if (!GetMonitorRectForWindow(gameWindow, &host))
            gameWindow = nullptr;
    }
    if (!gameWindow &&
        !ResolveOverlayHostRect(gameMonitor, foreground, targetPid, &host))
        return;

    const bool changed =
        force || gameMonitor != s_lastGameMonitor ||
        !RectEquals(host, s_lastAppliedRect);
    const int width = host.right - host.left;
    const int height = host.bottom - host.top;
    if (width <= 0 || height <= 0)
        return;

    SetWindowPos(g_hwnd, HWND_TOPMOST, host.left, host.top, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);

    if (changed) {
        LogLine("Overlay host updated: gameMonitor=%d rect=%ld,%ld,%ld,%ld target=%lu targetWindow=%p owner=%p reason=%s",
                gameMonitor ? 1 : 0,
                host.left, host.top, host.right, host.bottom,
                (unsigned long)targetPid,
                gameWindow,
                GetWindow(g_hwnd, GW_OWNER),
                g_GameOverlayDisplayReason[0] ? g_GameOverlayDisplayReason : "N/A");
    }

    s_lastAppliedRect = host;
    s_lastGameMonitor = gameMonitor;
    g_OverlayHostRect = host;
    g_OverlayUsingGameMonitor = gameMonitor;
}

static void RestoreOverlayWindowAfterPowerResume()
{
    if (!g_hwnd || !IsWindow(g_hwnd) || g_Mode != MODE_OVERLAY)
        return;

    DetachOverlayOwnerWindow("power resume");

    LONG_PTR style = GetWindowLongPtr(g_hwnd, GWL_EXSTYLE);
    LONG_PTR restored = (style | WS_EX_LAYERED | WS_EX_TOOLWINDOW) &
                        ~static_cast<LONG_PTR>(WS_EX_APPWINDOW);
    if (!g_ShowLiveSettings)
        restored |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    if (restored != style)
        SetWindowLongPtr(g_hwnd, GWL_EXSTYLE, restored);

    SetWindowPos(g_hwnd,
                 g_ShowLiveSettings ? HWND_NOTOPMOST : HWND_TOPMOST,
                 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER |
                     SWP_FRAMECHANGED |
                     (g_ShowLiveSettings ? SWP_SHOWWINDOW : SWP_NOACTIVATE));

    const bool shouldShowOverlay =
        g_OvlVisible && (!g_FullscreenAutoHidden || g_ShowLiveSettings);
    if (!g_ShowLiveSettings && shouldShowOverlay) {
        const bool gameMonitor =
            g_GameOverlayDisplayActive ||
            g_DesktopVisibilityState == DESKTOP_VISIBILITY_GAME;
        UpdateOverlayHostWindow(
            gameMonitor, GetForegroundWindow(),
            g_targetPid.load(std::memory_order_relaxed), true);
    } else if (!shouldShowOverlay) {
        ShowWindow(g_hwnd, SW_HIDE);
    }
    LogLine("Power resume recovery: hwnd=%p toolWindow=1 owner=%p",
            g_hwnd, GetWindow(g_hwnd, GW_OWNER));
}

static bool RecoverOverlayHostWindow()
{
    if (g_Mode != MODE_OVERLAY)
        return true;
    if (g_hwnd && IsWindow(g_hwnd)) {
        g_OverlayHostRecoveryPending = false;
        g_OverlayHostRecoveryRetryTick = 0;
        return true;
    }

    const ULONGLONG now = GetTickCount64();
    if (g_OverlayHostRecoveryRetryTick != 0 &&
        now < g_OverlayHostRecoveryRetryTick) {
        return false;
    }

    LogLine("Overlay host recovery started: staleHwnd=%p", g_hwnd);
    ShutdownBackends();
    g_hwnd = nullptr;
    g_OverlayOwnerWindow = nullptr;

    RECT wa = {};
    if (!SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0))
        GetPrimaryMonitorRect(&wa);
    const int width = wa.right - wa.left;
    const int height = wa.bottom - wa.top;
    const DWORD exStyle =
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT |
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    g_hwnd = CreateWindowEx(
        exStyle, "FPSOverlay", "FPS Overlay", WS_POPUP,
        wa.left, wa.top, width, height,
        nullptr, nullptr, g_hInstance, nullptr);
    if (!g_hwnd) {
        LogLine("Overlay host recovery failed: CreateWindowEx err=%lu",
                (unsigned long)GetLastError());
        g_OverlayHostRecoveryRetryTick = now + 1000;
        return false;
    }

    SetLayeredWindowAttributes(g_hwnd, RGB(0, 0, 0), 255, LWA_ALPHA);
    MARGINS margins = { -1 };
    DwmExtendFrameIntoClientArea(g_hwnd, &margins);
    if (!InitBackends()) {
        LogLine("Overlay host recovery failed: DirectX initialization");
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
        g_OverlayHostRecoveryRetryTick = now + 1000;
        return false;
    }

    g_OverlayHostRecoveryPending = false;
    g_OverlayHostRecoveryRetryTick = 0;
    g_D3DRecoveryPending = false;
    if (!g_etwRunning.load(std::memory_order_acquire))
        g_etwAvailable = StartEtwSession();
    if (g_ShowLiveSettings) {
        SetClickThrough(false);
        SetLiveSettingsWindowLevel(true);
    } else {
        const bool shouldShowOverlay =
            g_OvlVisible && !g_FullscreenAutoHidden;
        ShowWindow(g_hwnd,
                   shouldShowOverlay ? SW_SHOWNOACTIVATE : SW_HIDE);
        RestoreOverlayWindowAfterPowerResume();
    }
    LogLine("Overlay host recovery completed: hwnd=%p", g_hwnd);
    return true;
}

void SwitchToOverlay()
{
    ReleaseHeaderIconTextures();
    ShutdownBackends();
    DestroyWindow(g_hwnd);

    RECT wa = {};
    if (!SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0))
        GetPrimaryMonitorRect(&wa);
    const int w = wa.right - wa.left;
    const int h = wa.bottom - wa.top;
    
    // Always start click-through - we toggle it when CTRL is held
    DWORD exStyle = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT |
                    WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    
    g_hwnd = CreateWindowEx(
        exStyle,
        "FPSOverlay", "FPS Overlay", WS_POPUP,
        wa.left, wa.top, w, h, nullptr, nullptr, g_hInstance, nullptr);

    g_Mode = MODE_OVERLAY;
    g_OvlVisible = true;
    if (!g_hwnd) {
        LogLine("SwitchToOverlay failed: CreateWindowEx err=%lu",
                (unsigned long)GetLastError());
        g_OverlayHostRecoveryPending = true;
        g_OverlayHostRecoveryRetryTick = 0;
        AddTrayIcon();
        return;
    }

    SetLayeredWindowAttributes(g_hwnd, RGB(0,0,0), 255, LWA_ALPHA);
    MARGINS m = { -1 }; DwmExtendFrameIntoClientArea(g_hwnd, &m);

    if (!InitBackends()) {
        LogLine("SwitchToOverlay failed: backend initialization");
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
        g_OverlayHostRecoveryPending = true;
        g_OverlayHostRecoveryRetryTick = 0;
        AddTrayIcon();
        return;
    }
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    AddTrayIcon();

    // Start ETW for real game FPS
    g_etwAvailable = StartEtwSession();

    g_OverlayHostRecoveryPending = false;
}

void SwitchToConfig()
{
    StopEtwSession();
    RemoveTrayIcon();
    ShutdownBackends();
    DestroyWindow(g_hwnd);

    const int cfgW = ScalePx(kConfigDlgOuterW);
    const int ch = ScalePx(kConfigDlgOuterH);
    int cx = (GetSystemMetrics(SM_CXSCREEN) - cfgW) / 2;
    int cy = (GetSystemMetrics(SM_CYSCREEN) - ch) / 2;
    g_hwnd = CreateWindowEx(0, "FPSOverlay", "FPS Overlay",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME,
        cx, cy, cfgW, ch, nullptr, nullptr, g_hInstance, nullptr);

    if (!g_hwnd || !InitBackends()) {
        LogLine("SwitchToConfig failed; returning to overlay recovery");
        if (g_hwnd)
            DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
        g_Mode = MODE_OVERLAY;
        g_OverlayHostRecoveryPending = true;
        g_OverlayHostRecoveryRetryTick = 0;
        AddTrayIcon();
        return;
    }
    ShowWindow(g_hwnd, SW_SHOW);
    g_Mode = MODE_CONFIG;
    g_OverlayHostRecoveryPending = false;
}

// ═══════════════════════════════════════════════════════════════════════════
// Rendering helpers
// ═══════════════════════════════════════════════════════════════════════════
static void Present(float r, float g, float b, float a)
{
    if (g_D3DRecoveryPending || !g_pd3dDeviceContext ||
        !g_pRenderTargetView || !g_pSwapChain) {
        return;
    }
    const bool traceSettingsFrame =
        g_SettingsFirstFramePending && g_SettingsTrace.active &&
        g_SettingsTrace.settingsBeginEndUs != 0;
    if (traceSettingsFrame)
        g_SettingsTrace.imguiRenderStartUs = PerfNowUs();
    ImGui::Render();
    if (traceSettingsFrame)
        g_SettingsTrace.imguiRenderEndUs = PerfNowUs();
    const float c[4] = { r, g, b, a };
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_pRenderTargetView, nullptr);
    g_pd3dDeviceContext->ClearRenderTargetView(g_pRenderTargetView, c);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    if (traceSettingsFrame) {
        g_SettingsTrace.drawSubmitEndUs = PerfNowUs();
        g_SettingsTrace.presentStartUs = g_SettingsTrace.drawSubmitEndUs;
    }
    // Disable VSync while dragging for instant response (reduces ~16ms latency per frame)
    const HRESULT presentResult = g_pSwapChain->Present(g_isDragging ? 0 : 1, 0);
    if (FAILED(presentResult)) {
        RequestD3DRecovery(
            presentResult,
            IsD3DDeviceLoss(presentResult) ? "device lost during Present" : "Present failed");
    }
    if (traceSettingsFrame) {
        g_SettingsTrace.presentEndUs = PerfNowUs();
        LogLine(
            "Settings open trace: source=%s total=%.3fms menuForeground=%.3fms "
            "popup=%.3fms popupReturnToQueue=%.3fms queueToCommand=%.3fms "
            "SetClickThrough=%.3fms settingsForeground=%.3fms command=%.3fms "
            "commandToNewFrame=%.3fms NewFrame=%.3fms newFrameToBegin=%.3fms "
            "Begin=%.3fms BeginToRender=%.3fms ImGuiRender=%.3fms "
            "drawSubmit=%.3fms Present=%.3fms",
            g_SettingsTrace.source,
            TraceMs(g_SettingsTrace.startUs, g_SettingsTrace.presentEndUs),
            TraceMs(g_SettingsTrace.foregroundMenuStartUs, g_SettingsTrace.foregroundMenuEndUs),
            TraceMs(g_SettingsTrace.popupStartUs, g_SettingsTrace.popupEndUs),
            TraceMs(g_SettingsTrace.popupEndUs, g_SettingsTrace.queuedUs),
            TraceMs(g_SettingsTrace.queuedUs, g_SettingsTrace.commandStartUs),
            TraceMs(g_SettingsTrace.clickThroughStartUs, g_SettingsTrace.clickThroughEndUs),
            TraceMs(g_SettingsTrace.foregroundSettingsStartUs, g_SettingsTrace.foregroundSettingsEndUs),
            TraceMs(g_SettingsTrace.commandStartUs, g_SettingsTrace.commandEndUs),
            TraceMs(g_SettingsTrace.commandEndUs, g_SettingsTrace.newFrameStartUs),
            TraceMs(g_SettingsTrace.newFrameStartUs, g_SettingsTrace.newFrameEndUs),
            TraceMs(g_SettingsTrace.newFrameEndUs, g_SettingsTrace.settingsBeginStartUs),
            TraceMs(g_SettingsTrace.settingsBeginStartUs, g_SettingsTrace.settingsBeginEndUs),
            TraceMs(g_SettingsTrace.settingsBeginEndUs, g_SettingsTrace.imguiRenderStartUs),
            TraceMs(g_SettingsTrace.imguiRenderStartUs, g_SettingsTrace.imguiRenderEndUs),
            TraceMs(g_SettingsTrace.imguiRenderEndUs, g_SettingsTrace.drawSubmitEndUs),
            TraceMs(g_SettingsTrace.presentStartUs, g_SettingsTrace.presentEndUs));
        LogLine("Live settings first frame: %llu ms",
                GetTickCount64() - g_SettingsRequestTick);
        LogLine("Live settings bounds: left=%ld top=%ld right=%ld bottom=%ld display=%dx%d",
                g_liveSettingsBounds.left, g_liveSettingsBounds.top,
                g_liveSettingsBounds.right, g_liveSettingsBounds.bottom,
                (int)ImGui::GetIO().DisplaySize.x, (int)ImGui::GetIO().DisplaySize.y);
        g_SettingsFirstFramePending = false;
        g_SettingsTrace.active = false;
    }
}

static ImVec4 ColorByLoad(float v, float warn = 70, float crit = 90)
{
    if (v > crit) return ImVec4(1,.3f,.3f,1);
    if (v > warn) return ImVec4(1,.85f,.15f,1);
    return ImVec4(.70f,.70f,.75f,1);
}

// Word-wrap tooltip text so it fits narrow settings windows
static void TooltipWrapped(const char* text)
{
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 260.f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

[[maybe_unused]] static void TooltipWrappedFmt(const char* fmt, ...)
{
    char buf[640];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    TooltipWrapped(buf);
}

static constexpr float kHdrBtnIconSz     = 11.f;
static constexpr float kHdrBtnPadX       = 6.f;
static constexpr float kHdrBtnPadY       = 2.f;
static constexpr float kHdrBtnGapIcon    = 4.f;
static constexpr float kHdrBtnStackGap   = 3.f;
static constexpr float kHdrBtnFontScale  = 0.78f;

static float CalcHeaderLinkButtonWidth(const char* label, float iconSz, float padX, float gapAfterIcon)
{
    ImGui::SetWindowFontScale(kHdrBtnFontScale);
    const float w = padX * 2.f + iconSz + gapAfterIcon + ImGui::CalcTextSize(label).x;
    ImGui::SetWindowFontScale(1.0f);
    return w;
}

static float CalcHeaderLinkButtonHeight(float iconSz)
{
    ImGui::SetWindowFontScale(kHdrBtnFontScale);
    const float textH = ImGui::CalcTextSize("Buy me a coffee").y;
    ImGui::SetWindowFontScale(1.0f);
    return (iconSz > textH ? iconSz : textH) + kHdrBtnPadY * 2.f;
}

static bool DrawHeaderLinkButton(const char* id, ImTextureID iconTex, float iconSz,
                                 const char* label, const char* url, float forcedW = 0.f)
{
    const float padX = kHdrBtnPadX;
    const float padY = kHdrBtnPadY;
    const float gapAfterIcon = kHdrBtnGapIcon;

    ImGui::SetWindowFontScale(kHdrBtnFontScale);
    const ImVec2 textSz = ImGui::CalcTextSize(label);
    ImGui::SetWindowFontScale(1.0f);

    const float btnW = forcedW > 0.f
        ? forcedW
        : (padX * 2.f + iconSz + gapAfterIcon + textSz.x);
    const ImVec2 btnSz(btnW,
                       (iconSz > textSz.y ? iconSz : textSz.y) + padY * 2.f);

    ImGui::PushID(id);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.f, 0.f));
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.13f, 0.14f, 0.17f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.17f, 0.18f, 0.22f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.11f, 0.12f, 0.15f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Border,        ImVec4(0.22f, 0.24f, 0.28f, 0.35f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);

    const bool pressed = ImGui::Button("##btn", btnSz);
    const ImRect bb(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float midY = (bb.Min.y + bb.Max.y) * 0.5f;

    if (iconTex) {
        const ImVec2 iconMin(bb.Min.x + padX, midY - iconSz * 0.5f);
        const ImVec2 iconMax(iconMin.x + iconSz, midY + iconSz * 0.5f);
        dl->AddImage(iconTex, iconMin, iconMax, ImVec2(0, 0), ImVec2(1, 1),
                     IM_COL32(170, 175, 185, 255));
    }

    ImFont* font = ImGui::GetFont();
    const float scaledSize = ImGui::GetFontSize() * kHdrBtnFontScale;
    dl->AddText(font, scaledSize,
                ImVec2(bb.Min.x + padX + iconSz + gapAfterIcon, midY - textSz.y * 0.5f),
                IM_COL32(165, 170, 180, 255), label);

    if (pressed)
        ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(4);
    ImGui::PopID();
    return pressed;
}

static float CalcHeaderLinkButtonsWidth()
{
    return 0.f;
    const float githubW = CalcHeaderLinkButtonWidth("View on GitHub", kHdrBtnIconSz, kHdrBtnPadX, kHdrBtnGapIcon);
    const float kofiW   = CalcHeaderLinkButtonWidth("Buy me a coffee", kHdrBtnIconSz, kHdrBtnPadX, kHdrBtnGapIcon);
    return githubW > kofiW ? githubW : kofiW;
}

[[maybe_unused]] static float CalcHeaderLinkButtonsHeight()
{
    return 0.f;
    return CalcHeaderLinkButtonHeight(kHdrBtnIconSz) * 2.f + kHdrBtnStackGap;
}

[[maybe_unused]] static void DrawHeaderExternalLinkButtonsAt(float x, float y)
{
    (void)x; (void)y;
    return;
    const float btnW = CalcHeaderLinkButtonsWidth();
    const float btnH = CalcHeaderLinkButtonHeight(kHdrBtnIconSz);

    ImGui::SetCursorPos(ImVec2(x, y));
    DrawHeaderLinkButton("github", (ImTextureID)g_texGitHub, kHdrBtnIconSz,
                         "View on GitHub", "https://github.com/aneeskhan47/fps-overlay", btnW);
    ImGui::SetCursorPos(ImVec2(x, y + btnH + kHdrBtnStackGap));
    DrawHeaderLinkButton("kofi", (ImTextureID)g_texKofi, kHdrBtnIconSz,
                         "Buy me a coffee", "https://ko-fi.com/aneeskhan47", btnW);
}

[[maybe_unused]] static void DrawDeveloperAttributionLine()
{
    ImGui::TextColored(ImVec4(.45f,.45f,.5f,1), "开发者：aneeskhan47 与");
    ImGui::SameLine(0, 0);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.55f,.75f,1.f,1));
    ImGui::Text("贡献者");
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            ShellExecuteA(nullptr, "open",
                "https://github.com/aneeskhan47/fps-overlay/graphs/contributors?all=1",
                nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::PopStyleColor();
}

static void MarkConfigDirty()
{
    g_ConfigDirty = true;
    g_ConfigDirtyTick = GetTickCount64();
}

static bool DrawWindowsStartupToggle()
{
    bool changed = false;
    bool requested = g_Config.startWithWindows;
    ImGui::BeginDisabled(!g_isAdmin);
    if (ImGui::Checkbox("随 Windows 登录自动启动", &requested)) {
        const bool applied = SetAutoLaunchTaskEnabled(requested);
        const bool actual = IsAutoLaunchTaskEnabled();
        if (applied && actual == requested) {
            g_Config.startWithWindows = requested;
            g_autoLaunchTaskReady = actual;
            snprintf(g_autoLaunchStatus, sizeof(g_autoLaunchStatus), "%s",
                     actual ? "已启用，用户登录后以最高权限启动" : "已关闭");
            MarkConfigDirty();
            changed = true;
        } else {
            g_Config.startWithWindows = actual;
            g_autoLaunchTaskReady = actual;
            snprintf(g_autoLaunchStatus, sizeof(g_autoLaunchStatus), "%s",
                     "更新失败，请确认计划任务服务和管理员权限");
            MessageBoxW(g_hwnd,
                        L"无法更新开机自启动计划任务。\n请确认 Windows 任务计划程序服务正在运行。",
                        L"FPS Overlay", MB_OK | MB_ICONWARNING | MB_TOPMOST);
        }
    }
    ImGui::EndDisabled();

    SettingsUi::Status("开机自启动：",
                       g_autoLaunchTaskReady ? "已启用" : "已关闭",
                       g_autoLaunchTaskReady);
    SettingsUi::Muted("%s", g_isAdmin ? g_autoLaunchStatus : "需要管理员权限才能修改");
    return changed;
}

static void QueueShowSettings()
{
    g_SettingsRequestTick = GetTickCount64();
    if (!g_SettingsTrace.active)
        StartSettingsTrace("command");
    g_SettingsTrace.queuedUs = PerfNowUs();
    g_Pending = CMD_SHOW_SETTINGS;
}

static void FlushConfigIfDirty(bool force = false)
{
    if (!g_ConfigDirty) return;
    const ULONGLONG now = GetTickCount64();
    if (!force && g_ShowLiveSettings)
        return;
    if (!force && now - g_ConfigDirtyTick < 750)
        return;
    if (SaveConfig(g_Config))
        g_ConfigDirty = false;
}

static void UpdateHotkeyCapture()
{
    if (g_listeningFor == 0) return;
    if (GetAsyncKeyState(VK_ESCAPE) & 1) {
        g_listeningFor = 0;
        return;
    }
    for (int vk = 1; vk < 256; vk++) {
        if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) continue;
        if (vk == VK_ESCAPE) continue;
        if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) continue;
        if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) continue;
        if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) continue;

        if (GetAsyncKeyState(vk) & 1) {
            if (g_listeningFor == 1) g_Config.toggleKey = vk;
            if (g_listeningFor == 2) g_Config.exitKey   = vk;
            if (g_listeningFor == 3) g_Config.layoutSwitchKey = vk;
            g_listeningFor = 0;
            MarkConfigDirty();
            break;
        }
    }
}

static void CycleOverlayLayout()
{
    const int previous = g_Config.layoutStyle;
    g_Config.layoutStyle = (g_Config.layoutStyle + 1) % (LAYOUT_GAMEPP + 1);
    MarkConfigDirty();
    LogLine("Layout hotkey: previous=%d current=%d key=%d",
            previous, g_Config.layoutStyle, g_Config.layoutSwitchKey);
}

enum SettingsPageId {
    SETTINGS_HOME = 0,
    SETTINGS_MONITOR,
    SETTINGS_OVERLAY,
    SETTINGS_DESKTOP,
    SETTINGS_CONTROL,
    SETTINGS_HARDWARE,
    SETTINGS_FEATURES,
};

static int g_settingsPage = SETTINGS_HOME;

static const char* SettingsPageTitle(int page)
{
    switch (page) {
    case SETTINGS_MONITOR:  return "监控项目";
    case SETTINGS_OVERLAY:  return "游戏内监控";
    case SETTINGS_DESKTOP:  return "桌面显示";
    case SETTINGS_CONTROL:  return "控制与启动";
    case SETTINGS_HARDWARE: return "硬件信息";
    case SETTINGS_FEATURES: return "提醒与功耗";
    default:                return "控制中心";
    }
}

static const char* SettingsPageDescription(int page)
{
    switch (page) {
    case SETTINGS_MONITOR:  return "管理 FPS、CPU、GPU、内存、存储和网络指标";
    case SETTINGS_OVERLAY:  return "调整覆盖层布局、位置、缩放、透明度和刷新频率";
    case SETTINGS_DESKTOP:  return "仅在桌面环境显示监测，前台应用占满屏幕时自动隐藏";
    case SETTINGS_CONTROL:  return "配置快捷键、Windows 自启动和程序启动行为";
    case SETTINGS_HARDWARE: return "查看传感器状态、选择 GPU 并导出诊断信息";
    case SETTINGS_FEATURES: return "配置温度、低帧率、自动显示和整机功耗功能";
    default:                return "实时状态、常用开关和硬件概览";
    }
}

static const char* SettingsPageCode(int page)
{
    switch (page) {
    case SETTINGS_MONITOR:  return "02";
    case SETTINGS_OVERLAY:  return "03";
    case SETTINGS_DESKTOP:  return "04";
    case SETTINGS_CONTROL:  return "05";
    case SETTINGS_HARDWARE: return "06";
    case SETTINGS_FEATURES: return "07";
    default:                return "01";
    }
}

static bool DrawSettingsNavItem(const char* label, const char* hint, int page)
{
    ImGui::PushID(page);
    const bool compact =
        ImGui::GetIO().DisplaySize.x < 820.0f * g_dpiScale;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = (compact ? 43.0f : 52.0f) * g_dpiScale;
    const bool pressed = ImGui::InvisibleButton("##nav", ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    const bool selected = g_settingsPage == page;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (selected || hovered) {
        dl->AddRectFilled(
            pos, ImVec2(pos.x + width, pos.y + height),
            selected ? IM_COL32(29, 52, 91, 245) : IM_COL32(23, 34, 61, 220),
            8.0f * g_dpiScale);
    }
    if (selected) {
        dl->AddRectFilled(
            pos, ImVec2(pos.x + 3.0f * g_dpiScale, pos.y + height),
            IM_COL32(77, 164, 255, 255), 3.0f * g_dpiScale);
        dl->AddRectFilled(
            ImVec2(pos.x, pos.y + height * 0.60f),
            ImVec2(pos.x + 3.0f * g_dpiScale, pos.y + height),
            IM_COL32(133, 102, 255, 255), 3.0f * g_dpiScale);
        dl->AddRect(
            pos, ImVec2(pos.x + width, pos.y + height),
            IM_COL32(75, 119, 184, 115), 8.0f * g_dpiScale);
    }

    const ImVec2 codeMin(pos.x + 9.0f * g_dpiScale,
                         pos.y + (compact ? 10.0f : 10.0f) * g_dpiScale);
    const ImVec2 codeMax(codeMin.x + (compact ? 22.0f : 25.0f) * g_dpiScale,
                         codeMin.y + (compact ? 22.0f : 25.0f) * g_dpiScale);
    dl->AddRectFilled(codeMin, codeMax,
                      selected ? IM_COL32(61, 126, 224, 185)
                               : IM_COL32(39, 51, 78, 225),
                      6.0f * g_dpiScale);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.72f,
                ImVec2(codeMin.x + 5.0f * g_dpiScale,
                       codeMin.y + 5.0f * g_dpiScale),
                selected ? IM_COL32(235, 245, 255, 255)
                         : IM_COL32(137, 151, 178, 255),
                SettingsPageCode(page));
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                ImVec2(pos.x + (compact ? 39.0f : 44.0f) * g_dpiScale,
                       pos.y + (compact ? 11.0f : 7.0f) * g_dpiScale),
                selected ? IM_COL32(235, 244, 255, 255)
                         : IM_COL32(190, 201, 222, 255),
                label);
    if (!compact) {
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.76f,
                    ImVec2(pos.x + 44.0f * g_dpiScale,
                           pos.y + 29.0f * g_dpiScale),
                    selected ? IM_COL32(129, 158, 199, 255)
                             : IM_COL32(104, 119, 146, 255),
                    hint);
    }

    if (pressed)
        g_settingsPage = page;
    ImGui::PopID();
    return pressed;
}

static const char* CurrentLayoutName()
{
    switch (g_Config.layoutStyle) {
    case LAYOUT_HORIZONTAL: return "水平紧凑";
    case LAYOUT_STEAM:      return "Steam 风格栏";
    case LAYOUT_GAMEPP:     return "游戏加加风格";
    default:                return "垂直排列";
    }
}

static void DrawSettingsStatusRail()
{
    const bool overlayActive = g_Mode == MODE_OVERLAY && g_OvlVisible;
    const bool sensorsReady =
        g_lhwmInitFinished.load(std::memory_order_acquire) && g_lhwmAvailable;
    const bool sensorsStale = g_FeatureContext.hardwareSensorDataStale;
    const bool powerReady = g_FeatureContext.hasEstimatedSystemPower;
    const PowerRecoveryStatus recoveryStatus =
        g_PowerRecoveryStatus.load(std::memory_order_acquire);
    const char* sensorState = sensorsReady ? "在线" : "初始化";
    const char* sensorHint = sensorsReady ? "数据可用" : "等待硬件层";
    if (recoveryStatus == PowerRecoveryStatus::Recovering) {
        sensorState = "正在恢复";
        sensorHint = "等待新传感器快照";
    } else if (recoveryStatus == PowerRecoveryStatus::Recovered) {
        sensorState = "监测已恢复";
        sensorHint = "硬件数据已刷新";
    } else if (recoveryStatus == PowerRecoveryStatus::Failed) {
        sensorState = "恢复超时";
        sensorHint = "请在托盘重新扫描";
    } else if (sensorsStale) {
        sensorState = "数据延迟";
        sensorHint = "可在托盘重新扫描";
    }

    char powerValue[32] = "N/A";
    if (powerReady) {
        snprintf(powerValue, sizeof(powerValue), "%s%.0f W",
                 g_FeatureContext.systemPowerEstimated ? "≈" : "",
                 g_FeatureContext.estimatedSystemPowerW);
    }

    const float available = ImGui::GetContentRegionAvail().x;
    const int columns = available > 840.0f * g_dpiScale ? 4 :
                        (available > 430.0f * g_dpiScale ? 2 : 1);
    if (ImGui::BeginTable("##status_rail", columns,
                          ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        SettingsUi::MetricTile("##rail_overlay", "OVERLAY",
                               overlayActive ? "运行中" :
                               (g_Mode == MODE_CONFIG ? "待启动" : "已隐藏"),
                               "实时状态",
                               overlayActive ? SettingsUi::Success()
                                             : SettingsUi::Warning());

        ImGui::TableNextColumn();
        SettingsUi::MetricTile("##rail_sensors", "SENSORS",
                               sensorState, sensorHint,
                               sensorsReady && !sensorsStale &&
                                       (recoveryStatus == PowerRecoveryStatus::Idle ||
                                        recoveryStatus == PowerRecoveryStatus::Recovered)
                                   ? SettingsUi::Success()
                                   : SettingsUi::Warning());

        ImGui::TableNextColumn();
        SettingsUi::MetricTile("##rail_power", "SYSTEM POWER",
                               powerValue,
                               powerReady ? "自动融合" : "暂无来源",
                               powerReady ? SettingsUi::Accent()
                                          : SettingsUi::Warning());

        ImGui::TableNextColumn();
        SettingsUi::MetricTile("##rail_layout", "LAYOUT",
                               CurrentLayoutName(),
                               "当前配置",
                               SettingsUi::Violet());
        ImGui::EndTable();
    }
    ImGui::Spacing();
}

static void DrawSettingsHome(bool& changed)
{
    DrawSettingsStatusRail();
    const std::string gpuNameSnapshot = GetGpuNameSnapshot();

    const bool wide = ImGui::GetContentRegionAvail().x > 720.0f * g_dpiScale;
    if (ImGui::BeginTable("##home_grid", wide ? 2 : 1,
                          ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        if (SettingsUi::BeginCard("##home_hardware", "实时遥测",
                                  "处理器与图形核心")) {
            ImGui::TextWrapped("CPU  %s", g_cpuName);
            ImGui::TextWrapped("GPU  %s", gpuNameSnapshot.c_str());
            ImGui::Spacing();
            SettingsUi::Muted("CPU 使用率");
            ImGui::ProgressBar(std::clamp(g_FeatureContext.cpuUsage / 100.0f, 0.0f, 1.0f),
                               ImVec2(-1.0f, 0.0f));
            SettingsUi::Muted("GPU 使用率");
            ImGui::ProgressBar(std::clamp(g_FeatureContext.gpuUsage / 100.0f, 0.0f, 1.0f),
                               ImVec2(-1.0f, 0.0f));
        }
        SettingsUi::EndCard();

        if (SettingsUi::BeginCard("##home_power", "整机功耗模型")) {
            if (g_FeatureContext.hasEstimatedSystemPower) {
                ImGui::SetWindowFontScale(1.35f);
                ImGui::TextColored(SettingsUi::Accent(), "%s%.0f W",
                                   g_FeatureContext.systemPowerEstimated ? "≈" : "",
                                   g_FeatureContext.estimatedSystemPowerW);
                ImGui::SetWindowFontScale(1.0f);
                ImGui::TextWrapped("%s",
                    g_FeatureContext.automaticSystemPowerSource[0]
                        ? g_FeatureContext.automaticSystemPowerSource
                        : "自动来源");
                ImGui::ProgressBar(
                    g_FeatureContext.systemPowerEstimateConfidence / 100.0f,
                    ImVec2(-1.0f, 0.0f), "模型置信度");
            } else {
                SettingsUi::Status("整机功耗：", "N/A", false);
                SettingsUi::Muted("打开“提醒与功耗”查看来源和传感器状态。");
            }
        }
        SettingsUi::EndCard();

        ImGui::TableNextColumn();
        if (SettingsUi::BeginCard("##home_quick", "快捷控制",
                                  "常用启动与显示选项")) {
            changed |= DrawWindowsStartupToggle();
            changed |= ImGui::Checkbox("启动后直接显示覆盖层", &g_Config.autoStart);
            changed |= ImGui::Checkbox("显示 FPS", &g_Config.showFPS);
            changed |= ImGui::Checkbox("显示下载速度", &g_Config.showNetworkDownload);
            changed |= ImGui::Checkbox("显示上传速度", &g_Config.showNetworkUpload);
        }
        SettingsUi::EndCard();

        if (SettingsUi::BeginCard("##home_profile", "当前显示配置",
                                  "覆盖层渲染参数")) {
            SettingsUi::Status("布局：", CurrentLayoutName(), true);
            ImGui::Text("缩放：%d%%", g_Config.overlayScale);
            ImGui::Text("刷新：%d ms", g_Config.refreshMs);
            ImGui::Text("温度：%s", g_Config.useFahrenheit ? "华氏度" : "摄氏度");
            if (ImGui::Button("打开游戏内监控设置", ImVec2(-1.0f, 0.0f)))
                g_settingsPage = SETTINGS_OVERLAY;
        }
        SettingsUi::EndCard();
        ImGui::EndTable();
    }
}

static void DrawSettingsMonitor(bool& changed)
{
    const bool wide = ImGui::GetContentRegionAvail().x > 720.0f * g_dpiScale;
    if (ImGui::BeginTable("##monitor_grid", wide ? 2 : 1,
                          ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        if (SettingsUi::BeginCard("##basic_metrics", "基础信息", "帧率、程序和时间")) {
            changed |= ImGui::Checkbox("FPS 计数器", &g_Config.showFPS);
            if (g_Config.showFPS) {
                changed |= ImGui::Checkbox("使用最近 60 秒平均 FPS", &g_Config.useAverageFPS);
                changed |= ImGui::Checkbox("自定义 FPS 颜色阈值", &g_Config.customFpsColors);
                ImGui::BeginDisabled(!g_Config.customFpsColors);
                ImGui::SetNextItemWidth(-1.0f);
                changed |= ImGui::SliderInt("红色上限##fps_color",
                                            &g_Config.fpsWarningThreshold,
                                            15, g_Config.fpsGoodThreshold - 1, "%d FPS");
                ImGui::SetNextItemWidth(-1.0f);
                changed |= ImGui::SliderInt("绿色起点##fps_color",
                                            &g_Config.fpsGoodThreshold,
                                            g_Config.fpsWarningThreshold + 1, 240, "%d FPS");
                ImGui::EndDisabled();
            }
            changed |= ImGui::Checkbox("进程名称", &g_Config.showProcessName);
            changed |= ImGui::Checkbox("本地时间", &g_Config.showTime);
            if (g_Config.showTime) {
                changed |= ImGui::Checkbox("显示秒数", &g_Config.timeShowSeconds);
                const char* timeFormats[] = { "24 小时制", "12 小时制" };
                ImGui::SetNextItemWidth(-1.0f);
                changed |= ImGui::Combo("时间格式", &g_Config.timeFormat, timeFormats, 2);
            }
        }
        SettingsUi::EndCard();

        ImGui::TableNextColumn();
        if (SettingsUi::BeginCard("##cpu_metrics", "处理器", "CPU 实时指标")) {
            changed |= ImGui::Checkbox("使用率##cpu", &g_Config.showCpuUsage);
            changed |= ImGui::Checkbox("温度##cpu", &g_Config.showCpuTemp);
            changed |= ImGui::Checkbox("功耗##cpu", &g_Config.showCpuPower);
            changed |= ImGui::Checkbox("风扇转速##cpu", &g_Config.showCpuFan);
            changed |= ImGui::Checkbox("核心频率##cpu", &g_Config.showCpuFreq);
        }
        SettingsUi::EndCard();

        ImGui::TableNextColumn();
        if (SettingsUi::BeginCard("##gpu_metrics", "图形处理器", "GPU 与散热指标")) {
            changed |= ImGui::Checkbox("使用率##gpu", &g_Config.showGpuUsage);
            changed |= ImGui::Checkbox("温度##gpu", &g_Config.showGpuTemp);
            changed |= ImGui::Checkbox("功耗##gpu", &g_Config.showGpuPower);
            changed |= ImGui::Checkbox("GPU 风扇", &g_Config.showGpuFan);
            changed |= ImGui::Checkbox("系统风扇", &g_Config.showSystemFan);
            changed |= ImGui::Checkbox("核心频率##gpu", &g_Config.showGpuCoreFreq);
            changed |= ImGui::Checkbox("显存温度", &g_Config.showGpuMemTemp);
        }
        SettingsUi::EndCard();

        ImGui::TableNextColumn();
        if (SettingsUi::BeginCard("##memory_metrics", "内存与存储", "容量、占用和温度")) {
            changed |= ImGui::Checkbox("显存百分比", &g_Config.showVRAMPercent);
            changed |= ImGui::Checkbox("显存已用 / 总量", &g_Config.showVRAMUsage);
            changed |= ImGui::Checkbox("内存百分比", &g_Config.showRAMPercent);
            changed |= ImGui::Checkbox("内存已用 / 总量", &g_Config.showRAMUsage);
            changed |= ImGui::Checkbox("硬盘温度", &g_Config.showDiskTemp);
            changed |= ImGui::Checkbox("内存条温度", &g_Config.showMemoryTemp);
        }
        SettingsUi::EndCard();

        ImGui::TableNextColumn();
        if (SettingsUi::BeginCard("##network_metrics", "网络", "下载与上传独立控制")) {
            changed |= ImGui::Checkbox("下载速度", &g_Config.showNetworkDownload);
            changed |= ImGui::Checkbox("上传速度", &g_Config.showNetworkUpload);
        }
        SettingsUi::EndCard();
        ImGui::EndTable();
    }
}

static void DrawSettingsOverlay(bool& changed)
{
    const bool wide = ImGui::GetContentRegionAvail().x > 720.0f * g_dpiScale;
    if (ImGui::BeginTable("##overlay_grid", wide ? 2 : 1,
                          ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        if (SettingsUi::BeginCard("##layout_style", "监控样式", "参考 GamePP 的集中式选择区")) {
            changed |= ImGui::RadioButton("垂直排列", &g_Config.layoutStyle, LAYOUT_VERTICAL);
            changed |= ImGui::RadioButton("水平紧凑", &g_Config.layoutStyle, LAYOUT_HORIZONTAL);
            changed |= ImGui::RadioButton("Steam 风格栏", &g_Config.layoutStyle, LAYOUT_STEAM);
            changed |= ImGui::RadioButton("游戏加加风格", &g_Config.layoutStyle, LAYOUT_GAMEPP);
        }
        SettingsUi::EndCard();

        ImGui::TableNextColumn();
        if (SettingsUi::BeginCard("##appearance", "显示效果")) {
            SettingsUi::Muted("覆盖层缩放");
            ImGui::SetNextItemWidth(-1.0f);
            changed |= ImGui::SliderInt("##overlay_scale", &g_Config.overlayScale,
                                        50, 200, "%d%%");
            SettingsUi::Muted("背景不透明度");
            ImGui::SetNextItemWidth(-1.0f);
            changed |= ImGui::SliderInt("##overlay_opacity", &g_Config.opacity,
                                        30, 100, "%d%%");
            SettingsUi::Muted("刷新时间");
            ImGui::SetNextItemWidth(-1.0f);
            changed |= ImGui::SliderInt("##overlay_refresh", &g_Config.refreshMs,
                                        250, 5000, "%d ms");
            changed |= ImGui::Checkbox("拖动时吸附屏幕边缘", &g_Config.monitorSnap);
        }
        SettingsUi::EndCard();

        ImGui::TableNextColumn();
        if (SettingsUi::BeginCard("##position", "位置与单位")) {
            int prevPos = g_Config.position;
            if (ImGui::BeginTable("##position_grid", 3, ImGuiTableFlags_SizingStretchSame)) {
                ImGui::TableNextColumn(); changed |= ImGui::RadioButton("左上", &g_Config.position, POS_TOP_LEFT);
                ImGui::TableNextColumn(); changed |= ImGui::RadioButton("上中", &g_Config.position, POS_TOP_CENTER);
                ImGui::TableNextColumn(); changed |= ImGui::RadioButton("右上", &g_Config.position, POS_TOP_RIGHT);
                ImGui::TableNextColumn(); changed |= ImGui::RadioButton("左下", &g_Config.position, POS_BOTTOM_LEFT);
                ImGui::TableNextColumn(); changed |= ImGui::RadioButton("下中", &g_Config.position, POS_BOTTOM_CENTER);
                ImGui::TableNextColumn(); changed |= ImGui::RadioButton("右下", &g_Config.position, POS_BOTTOM_RIGHT);
                ImGui::EndTable();
            }
            if (g_Config.position != prevPos) {
                g_Config.customX = -1.0f;
                g_Config.customY = -1.0f;
                g_overlayForceCornerSnap = true;
            }
            if (ImGui::Button("重置拖动位置", ImVec2(-1.0f, 0.0f))) {
                g_Config.customX = -1.0f;
                g_Config.customY = -1.0f;
                g_overlayForceCornerSnap = true;
                changed = true;
            }
            int tempUnit = g_Config.useFahrenheit ? 1 : 0;
            if (ImGui::RadioButton("摄氏度", &tempUnit, 0)) {
                g_Config.useFahrenheit = false;
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("华氏度", &tempUnit, 1)) {
                g_Config.useFahrenheit = true;
                changed = true;
            }
        }
        SettingsUi::EndCard();

        if (g_Config.layoutStyle == LAYOUT_GAMEPP) {
            ImGui::TableNextColumn();
            if (SettingsUi::BeginCard("##gamepp_options", "游戏加加布局",
                                      "保留原横向分段与指标顺序")) {
                const char* presets[] = { "480", "720", "1080", "2K", "4K" };
                SettingsUi::Muted("字体预设");
                for (int i = 0; i < 5; ++i) {
                    if (i > 0) ImGui::SameLine();
                    changed |= ImGui::RadioButton(presets[i], &g_Config.gameppFontPreset, i);
                }
            }
            SettingsUi::EndCard();
        }
        ImGui::EndTable();
    }
}

static void DrawSettingsControl(bool& changed)
{
    if (SettingsUi::BeginCard("##hotkeys", "快捷键", "按 ESC 可取消录制")) {
        ImGui::Text("显示 / 隐藏");
        ImGui::SameLine(160.0f * g_dpiScale);
        if (g_listeningFor == 1) {
            ImGui::TextColored(ImVec4(1,.8f,.2f,1), "请按任意键...");
            ImGui::SameLine();
            if (ImGui::SmallButton("取消##live1")) g_listeningFor = 0;
        } else {
            ImGui::Text("%s", GetKeyName(g_Config.toggleKey));
            ImGui::SameLine();
            if (ImGui::SmallButton("更改##live1")) g_listeningFor = 1;
        }
        ImGui::Text("退出程序");
        ImGui::SameLine(160.0f * g_dpiScale);
        if (g_listeningFor == 2) {
            ImGui::TextColored(ImVec4(1,.8f,.2f,1), "请按任意键...");
            ImGui::SameLine();
            if (ImGui::SmallButton("取消##live2")) g_listeningFor = 0;
        } else {
            ImGui::Text("%s", GetKeyName(g_Config.exitKey));
            ImGui::SameLine();
            if (ImGui::SmallButton("更改##live2")) g_listeningFor = 2;
        }
        ImGui::Text("切换布局");
        ImGui::SameLine(160.0f * g_dpiScale);
        if (g_listeningFor == 3) {
            ImGui::TextColored(ImVec4(1,.8f,.2f,1), "请按任意键...");
            ImGui::SameLine();
            if (ImGui::SmallButton("取消##live3")) g_listeningFor = 0;
        } else {
            ImGui::Text("%s", GetKeyName(g_Config.layoutSwitchKey));
            ImGui::SameLine();
            if (ImGui::SmallButton("更改##live3")) g_listeningFor = 3;
        }
    }
    SettingsUi::EndCard();

    if (SettingsUi::BeginCard("##startup", "启动行为")) {
        changed |= DrawWindowsStartupToggle();
        changed |= ImGui::Checkbox("启动后直接显示覆盖层", &g_Config.autoStart);
        SettingsUi::Muted("系统自启动与程序启动后的显示模式相互独立。");
    }
    SettingsUi::EndCard();
}

static void DrawSettingsDesktop(bool& changed)
{
    if (SettingsUi::BeginCard("##desktop_only", "仅桌面显示监测",
                              "桌面专用显示模式")) {
        changed |= ImGui::Checkbox("启用仅桌面显示", &g_Config.desktopOnlyMode);
        SettingsUi::MutedWrapped(
            "开启后，明确的普通应用最大化或全屏时临时隐藏 Overlay；"
            "检测到游戏时优先保持显示，返回桌面后按原来的手动显示状态恢复。");
        ImGui::Spacing();
        if (!g_Config.desktopOnlyMode) {
            SettingsUi::Status("当前状态：", "未启用", false);
        } else if (g_DesktopVisibilityState == DESKTOP_VISIBILITY_GAME) {
            SettingsUi::Status("当前状态：", "已识别游戏，监测保持显示", true);
        } else if (g_DesktopVisibilityState == DESKTOP_VISIBILITY_EVALUATING) {
            SettingsUi::Status("当前状态：", "正在识别前台程序，监测暂时保持显示", true);
        } else if (g_DesktopVisibilityState == DESKTOP_VISIBILITY_APP_HIDDEN) {
            SettingsUi::Status("当前状态：", "普通应用占满屏幕，监测已临时隐藏", false);
        } else {
            SettingsUi::Status("当前状态：", "桌面监测显示中", true);
        }
        if (g_DesktopForegroundExe[0])
            ImGui::TextWrapped("前台程序：%s", g_DesktopForegroundExe);
    }
    SettingsUi::EndCard();

    if (SettingsUi::BeginCard("##desktop_behavior", "显示规则",
                              "不改变现有监测项目和布局")) {
        SettingsUi::MutedWrapped(
            "自动隐藏与快捷键或托盘的手动显示/隐藏状态相互独立；"
            "打开设置面板时 Overlay 保持可见。");
        SettingsUi::MutedWrapped(
            "识别依据包含游戏安装路径与前台实时渲染。无法确定时优先保持显示，"
            "避免把正在启动或加载中的游戏误判为普通应用。");
        SettingsUi::MutedWrapped(
            "此模式只控制 Overlay 何时出现，不修改监控项目、功耗功能、"
            "位置、缩放或游戏加加横向布局。");
    }
    SettingsUi::EndCard();
}

static void DrawSettingsHardware(bool& changed)
{
    const bool sensorsReady =
        g_lhwmInitFinished.load(std::memory_order_acquire) &&
        g_lhwmAvailable.load(std::memory_order_acquire);
    std::string gpuNameSnapshot;
    std::vector<GpuInfo> gpuSnapshot;
    std::vector<std::pair<std::string, std::string>> cpuClockOptionsSnapshot;
    int selectedGpuSnapshot = -1;
    {
        std::lock_guard<std::mutex> lock(g_lhwmStateMutex);
        gpuNameSnapshot = g_gpuName;
        selectedGpuSnapshot = g_Config.selectedGpu;
        if (sensorsReady) {
            gpuSnapshot.assign(g_gpuList, g_gpuList + g_gpuCount);
            cpuClockOptionsSnapshot = g_cpuClockOpts;
        }
    }

    if (SettingsUi::BeginCard("##hardware_summary", "硬件信息")) {
        SettingsUi::Status("传感器服务：",
                           sensorsReady ? "正常" : "初始化中或不可用",
                           sensorsReady);
        ImGui::TextWrapped("CPU  %s", g_cpuName);
        ImGui::TextWrapped("GPU  %s", gpuNameSnapshot.c_str());
        if (!gpuSnapshot.empty()) {
            const char* preview =
                (selectedGpuSnapshot >= 0 &&
                 selectedGpuSnapshot < static_cast<int>(gpuSnapshot.size()))
                    ? gpuSnapshot[selectedGpuSnapshot].name : "选择 GPU...";
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo("监控 GPU", preview)) {
                for (int i = 0; i < static_cast<int>(gpuSnapshot.size()); ++i) {
                    const bool selected = selectedGpuSnapshot == i;
                    if (ImGui::Selectable(gpuSnapshot[i].name, selected)) {
                        SelectGpu(i);
                        changed = true;
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }
    }
    SettingsUi::EndCard();

    if (SettingsUi::BeginCard("##frequency_sources", "频率传感器",
                              "选择 CPU 与 GPU 核心时钟数据来源")) {
        if (!g_lhwmInitFinished.load(std::memory_order_acquire)) {
            SettingsUi::Muted("正在初始化 LibreHardwareMonitor...");
        } else if (!g_lhwmAvailable.load(std::memory_order_acquire)) {
            SettingsUi::Status("频率传感器：", "不可用", false);
        } else {
            changed |= ImGui::Checkbox("显示 CPU 频率", &g_Config.showCpuFreq);
            const char* cpuPreview = "选择 CPU 频率传感器";
            for (const auto& option : cpuClockOptionsSnapshot) {
                if (strcmp(g_Config.cpuFreqPath, option.second.c_str()) == 0) {
                    cpuPreview = option.first.c_str();
                    break;
                }
            }
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo("##cpu_frequency_source", cpuPreview)) {
                for (const auto& option : cpuClockOptionsSnapshot) {
                    const bool selected =
                        strcmp(g_Config.cpuFreqPath, option.second.c_str()) == 0;
                    if (ImGui::Selectable(option.first.c_str(), selected)) {
                        snprintf(g_Config.cpuFreqPath, sizeof(g_Config.cpuFreqPath),
                                 "%s", option.second.c_str());
                        changed = true;
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            changed |= ImGui::Checkbox("显示 GPU 核心频率", &g_Config.showGpuCoreFreq);
            if (!gpuSnapshot.empty() && selectedGpuSnapshot >= 0 &&
                selectedGpuSnapshot < static_cast<int>(gpuSnapshot.size())) {
                GpuInfo& gpu = gpuSnapshot[selectedGpuSnapshot];
                const char* gpuPreview = "选择 GPU 核心频率传感器";
                for (const auto& option : gpu.coreClockOpts) {
                    if (strcmp(g_Config.gpuCoreFreqPath, option.second.c_str()) == 0) {
                        gpuPreview = option.first.c_str();
                        break;
                    }
                }
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::BeginCombo("##gpu_frequency_source", gpuPreview)) {
                    for (const auto& option : gpu.coreClockOpts) {
                        const bool selected =
                            strcmp(g_Config.gpuCoreFreqPath, option.second.c_str()) == 0;
                        if (ImGui::Selectable(option.first.c_str(), selected)) {
                            snprintf(g_Config.gpuCoreFreqPath,
                                     sizeof(g_Config.gpuCoreFreqPath),
                                     "%s", option.second.c_str());
                            changed = true;
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            } else {
                SettingsUi::Muted("未检测到可选择的 GPU。");
            }
        }
    }
    SettingsUi::EndCard();

    if (SettingsUi::BeginCard("##hardware_tools", "诊断工具")) {
        if (ImGui::Button("复制诊断摘要", ImVec2(-1.0f, 0.0f)) &&
            !CopyDiagnosticSummaryToClipboard()) {
            MessageBoxA(g_hwnd, "复制诊断摘要失败。", "FPS Overlay",
                        MB_OK | MB_ICONERROR | MB_TOPMOST);
        }
        if (ImGui::Button("导出诊断包", ImVec2(-1.0f, 0.0f))) {
            char diagDir[MAX_PATH] = {};
            if (ExportDiagnosticsPackage(diagDir, sizeof(diagDir)))
                ShellExecuteA(nullptr, "open", diagDir, nullptr, nullptr, SW_SHOWNORMAL);
            else
                MessageBoxA(g_hwnd, "导出诊断包失败。", "FPS Overlay",
                            MB_OK | MB_ICONERROR | MB_TOPMOST);
        }
        if (ImGui::Button("手动安装 PawnIO", ImVec2(-1.0f, 0.0f))) {
            const bool ok = ExtractAndRunPawnIOSetup();
            MessageBoxW(g_hwnd,
                ok ? L"PawnIO 安装程序已运行完成。若硬件数据仍不可用，请手动重启电脑后再试。"
                   : L"PawnIO 安装程序运行失败。程序仍会继续运行，读不到的数据会显示 N/A。",
                L"FPS Overlay", MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONWARNING) | MB_TOPMOST);
        }
    }
    SettingsUi::EndCard();
}

static void DrawSettingsPageContent(bool& changed)
{
    switch (g_settingsPage) {
    case SETTINGS_MONITOR:  DrawSettingsMonitor(changed); break;
    case SETTINGS_OVERLAY:  DrawSettingsOverlay(changed); break;
    case SETTINGS_DESKTOP:  DrawSettingsDesktop(changed); break;
    case SETTINGS_CONTROL:  DrawSettingsControl(changed); break;
    case SETTINGS_HARDWARE: DrawSettingsHardware(changed); break;
    case SETTINGS_FEATURES: changed |= g_FeatureRegistry.DrawSettings(g_FeatureContext); break;
    default:                DrawSettingsHome(changed); break;
    }
}

static void DrawSettingsSidebar(bool liveSettings)
{
    const bool compact =
        ImGui::GetIO().DisplaySize.x < 820.0f * g_dpiScale;
    const float sidebarWidth = (compact ? 145.0f : 190.0f) * g_dpiScale;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.025f, 0.031f, 0.062f, 1.0f));
    ImGui::BeginChild("##settings_sidebar", ImVec2(sidebarWidth, 0.0f),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);

    const float brandX = ImGui::GetCursorPosX();
    const float brandY = ImGui::GetCursorPosY();
    const ImVec2 brandScreen = ImGui::GetCursorScreenPos();
    const float badgeSize = (compact ? 30.0f : 34.0f) * g_dpiScale;
    ImDrawList* sidebarDraw = ImGui::GetWindowDrawList();
    sidebarDraw->AddRectFilled(
        brandScreen,
        ImVec2(brandScreen.x + badgeSize, brandScreen.y + badgeSize),
        IM_COL32(47, 104, 210, 255), 9.0f * g_dpiScale);
    sidebarDraw->AddRectFilled(
        ImVec2(brandScreen.x + badgeSize * 0.52f, brandScreen.y),
        ImVec2(brandScreen.x + badgeSize, brandScreen.y + badgeSize),
        IM_COL32(102, 78, 222, 205), 9.0f * g_dpiScale,
        ImDrawFlags_RoundCornersRight);
    sidebarDraw->AddText(
        ImGui::GetFont(), ImGui::GetFontSize() * 0.72f,
        ImVec2(brandScreen.x + 6.0f * g_dpiScale,
               brandScreen.y + 9.0f * g_dpiScale),
        IM_COL32(245, 249, 255, 255), "FPS");

    ImGui::SetCursorPos(ImVec2(brandX + (compact ? 39.0f : 44.0f) * g_dpiScale,
                               brandY));
    ImGui::SetWindowFontScale(compact ? 1.0f : 1.10f);
    ImGui::TextColored(ImVec4(0.92f, 0.95f, 1.0f, 1.0f), "FPS OVERLAY");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::SetCursorPosX(brandX + (compact ? 39.0f : 44.0f) * g_dpiScale);
    ImGui::TextColored(SettingsUi::MutedColor(),
                       compact ? "CONTROL" : "PERFORMANCE LAB");
    ImGui::SetCursorPosY(brandY + (compact ? 38.0f : 43.0f) * g_dpiScale);
    ImGui::TextColored(SettingsUi::MutedColor(), "%s",
                       compact ? VER_SHORT_STRING : APP_VERSION);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (!liveSettings) {
        const bool sensorsBusy =
            !g_lhwmInitFinished.load(std::memory_order_acquire);
        ImGui::BeginDisabled(sensorsBusy);
        if (SettingsUi::PrimaryButton(
                sensorsBusy ? "传感器初始化中" : "启动 Overlay",
                ImVec2(-1.0f, 36.0f * g_dpiScale))) {
#if !defined(FPSOVERLAY_UI_QA)
            g_Pending = CMD_START_OVERLAY;
#endif
        }
        ImGui::EndDisabled();
        ImGui::Spacing();
    }

    DrawSettingsNavItem("控制中心", "状态与快捷操作", SETTINGS_HOME);
    DrawSettingsNavItem("监控项目", "指标显示开关", SETTINGS_MONITOR);
    DrawSettingsNavItem("游戏内监控", "布局与显示效果", SETTINGS_OVERLAY);
    DrawSettingsNavItem("桌面显示", "仅桌面显示监测", SETTINGS_DESKTOP);
    DrawSettingsNavItem("控制与启动", "快捷键和自启动", SETTINGS_CONTROL);
    DrawSettingsNavItem("硬件信息", "传感器与诊断", SETTINGS_HARDWARE);
    DrawSettingsNavItem("提醒与功耗", "告警和整机模型", SETTINGS_FEATURES);

    const float footerHeight = 110.0f * g_dpiScale;
    if (ImGui::GetContentRegionAvail().y > footerHeight)
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                             ImGui::GetContentRegionAvail().y - footerHeight);
    ImGui::Separator();
    const bool overlayActive = g_Mode == MODE_OVERLAY && g_OvlVisible;
    SettingsUi::Status("Overlay",
                       overlayActive ? "运行中" :
                       (g_Mode == MODE_CONFIG ? "尚未启动" : "已隐藏"),
                       overlayActive);
    if (liveSettings) {
        if (ImGui::Button("关闭设置", ImVec2(-1.0f, 0.0f))) {
            g_ShowLiveSettings = false;
            g_listeningFor = 0;
        }
    } else {
        SettingsUi::Muted("硬件监控控制台");
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

static void DrawSettingsDashboard(bool liveSettings, bool& changed)
{
    DrawSettingsSidebar(liveSettings);
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::BeginChild("##settings_content", ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::TextColored(SettingsUi::Violet(), "SYSTEM / %s",
                       SettingsPageCode(g_settingsPage));
    ImGui::SetWindowFontScale(1.42f);
    ImGui::TextColored(ImVec4(0.92f, 0.95f, 1.0f, 1.0f), "%s",
                       SettingsPageTitle(g_settingsPage));
    ImGui::SetWindowFontScale(1.0f);
    SettingsUi::MutedWrapped("%s", SettingsPageDescription(g_settingsPage));
    ImGui::Spacing();
    const ImVec2 railStart = ImGui::GetCursorScreenPos();
    const float railWidth = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddRectFilledMultiColor(
        railStart, ImVec2(railStart.x + railWidth, railStart.y + 2.0f * g_dpiScale),
        IM_COL32(77, 164, 255, 230),
        IM_COL32(133, 102, 255, 175),
        IM_COL32(133, 102, 255, 15),
        IM_COL32(77, 164, 255, 15));
    ImGui::Dummy(ImVec2(railWidth, 2.0f * g_dpiScale));
    ImGui::Spacing();
    DrawSettingsPageContent(changed);
    ImGui::EndChild();
}

static void DrawLiveSettingsPanel()
{
    if (!g_ShowLiveSettings) return;

    bool changed = false;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const ImVec2 minSize(760.0f * g_dpiScale, 520.0f * g_dpiScale);
    const ImVec2 maxSize((std::max)(minSize.x, display.x - 24.0f * g_dpiScale),
                         (std::max)(minSize.y, display.y - 24.0f * g_dpiScale));
    ImGui::SetNextWindowSizeConstraints(minSize, maxSize);
    ImGui::SetNextWindowSize(ImVec2(1080.0f * g_dpiScale, 720.0f * g_dpiScale),
                             ImGuiCond_FirstUseEver);
    if (g_SettingsFirstFramePending) {
        const ImVec2 center(display.x * 0.5f, display.y * 0.5f);
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    }

    SettingsUi::PushTheme();
    if (g_SettingsFirstFramePending)
        g_SettingsTrace.settingsBeginStartUs = PerfNowUs();
    const bool expanded =
        ImGui::Begin("FPS Overlay 设置中心", &g_ShowLiveSettings,
                     ImGuiWindowFlags_NoCollapse);
    if (g_SettingsFirstFramePending)
        g_SettingsTrace.settingsBeginEndUs = PerfNowUs();
    if (!expanded) {
        ImGui::End();
        SettingsUi::PopTheme();
        return;
    }

    DrawSettingsDashboard(true, changed);

    const ImVec2 actualPos = ImGui::GetWindowPos();
    const ImVec2 actualSize = ImGui::GetWindowSize();
    g_liveSettingsBounds = {
        (LONG)actualPos.x, (LONG)actualPos.y,
        (LONG)(actualPos.x + actualSize.x),
        (LONG)(actualPos.y + actualSize.y)
    };

    if (changed) MarkConfigDirty();
    ImGui::End();
    SettingsUi::PopTheme();
}

// ═══════════════════════════════════════════════════════════════════════════
// WinMain
// ═══════════════════════════════════════════════════════════════════════════
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR commandLine, int)
{
    struct InstanceMutexGuard {
        HANDLE handle = nullptr;
        ~InstanceMutexGuard() { if (handle) CloseHandle(handle); }
    } instanceMutex;

    g_hInstance = hInst;
#if defined(FPSOVERLAY_UI_QA)
    constexpr wchar_t kInstanceMutexName[] = L"Local\\FPSOverlay.UiQa.Singleton";
#else
    constexpr wchar_t kInstanceMutexName[] = L"Local\\FPSOverlay.Singleton";
#endif
    SetLastError(ERROR_SUCCESS);
    instanceMutex.handle = CreateMutexW(nullptr, TRUE, kInstanceMutexName);
    const DWORD instanceMutexError = GetLastError();
    if (instanceMutex.handle && instanceMutexError == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowA("FPSOverlay", nullptr))
            PostMessageW(existing, WM_APP_TRAY_COMMAND, IDM_SETTINGS, 0);
        return 0;
    }
    if (!instanceMutex.handle)
        LogLine("Single-instance mutex unavailable: err=%lu",
                (unsigned long)instanceMutexError);

    g_TaskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    const bool autoSettingsTrace =
        commandLine && strstr(commandLine, "--diagnose-settings") != nullptr;
    const bool autoTrayTrace =
        commandLine && strstr(commandLine, "--diagnose-tray") != nullptr;
    EnableProcessDpiAwareness();
    RefreshDpiScale(nullptr);

    // ── Load saved configuration ──
    LoadConfig(g_Config);
    g_trayDesktopOnlyMode.store(g_Config.desktopOnlyMode,
                                std::memory_order_relaxed);
    LogLine("App start: version=%s schema=%d autoStart=%d startWithWindows=%d layout=%d scale=%d",
            APP_VERSION, CONFIG_SCHEMA_VERSION,
            g_Config.autoStart ? 1 : 0,
            g_Config.startWithWindows ? 1 : 0,
            g_Config.layoutStyle, g_Config.overlayScale);
    g_FeatureRegistry.Init();

    // ── Show welcome message on first run ──
    MarkWelcomeShown();

    // ── Query hardware ──
    QueryCpuName();

    // ── Register window class with icon ──
    HICON hIcon = LoadIcon(hInst, MAKEINTRESOURCE(1));
    if (!hIcon) hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(wc); wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = hIcon;
    wc.hIconSm = hIcon;
    wc.lpszClassName = "FPSOverlay";
    RegisterClassEx(&wc);

    WNDCLASSEXA trayClass = {};
    trayClass.cbSize = sizeof(trayClass);
    trayClass.lpfnWndProc = TrayWndProc;
    trayClass.hInstance = hInst;
    trayClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    trayClass.lpszClassName = "FPSOverlayTray";
    RegisterClassExA(&trayClass);

    // ── Config window (freely resizable in both dimensions) ──
    const int cfgW = ScalePx(kConfigDlgOuterW);
    const int ch = ScalePx(kConfigDlgOuterH);
    int cx = (GetSystemMetrics(SM_CXSCREEN) - cfgW) / 2;
    int cy = (GetSystemMetrics(SM_CYSCREEN) - ch) / 2;
    g_hwnd = CreateWindowEx(0, wc.lpszClassName, "FPS Overlay",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME,
        cx, cy, cfgW, ch, nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;
    RefreshDpiScale(g_hwnd);

    // ── Check admin privileges (app should always run as admin via manifest) ──
    g_isAdmin = IsRunningAsAdmin();
    ReconcileAutoLaunchTask();

    if (!CreateDeviceD3D(g_hwnd)) {
        MessageBoxW(g_hwnd, L"DirectX 11 初始化失败。", L"FPS Overlay", MB_OK | MB_ICONERROR);
        CleanupDeviceD3D(); return 1;
    }

    // Get GPU name from DXGI adapter (fallback if LHWM doesn't provide it)
    QueryGpuName();

    // Local-only build: PawnIO is optional. Missing driver means some sensors show N/A.
    LogLine("PawnIO optional: installed=%d outdated=%d",
            IsPawnIOInstalled() ? 1 : 0,
            IsPawnIOInstalled() && IsPawnIOOutdatedVsBundled() ? 1 : 0);
    
    // LibreHardwareMonitor init can take several seconds. It runs on a joinable
    // worker so the UI stays responsive and shutdown can wait for it safely.
    g_lhwmAvailable.store(false, std::memory_order_relaxed);
    g_lhwmInitFinished.store(false, std::memory_order_relaxed);
#if defined(FPSOVERLAY_UI_QA)
    // UI-only verification builds run without elevation and must not enter the
    // managed hardware-monitor runtime. Production builds never define this.
    g_lhwmInitFinished.store(true, std::memory_order_relaxed);
#endif
    
    // Try WMI for CPU temperature as fallback (LHWM may enable CPU temp when init completes)
    g_cpuTempAvailable = InitWMI();
    if (g_cpuTempAvailable) {
        float testTemp = QueryCpuTemperature();
        g_cpuTempAvailable = (testTemp > 0.0f && testTemp < 150.0f);
    }
    WriteAsusWmiDiagnostics();

    // Show config window (unless auto-start is enabled)
    if (!g_Config.autoStart) {
        ShowWindow(g_hwnd, SW_SHOW);
        UpdateWindow(g_hwnd);
    }

    // ── ImGui context (lives for the whole app) ──
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    BuildUiFontAtlas(io);

    ApplyStyle();
    if (!ImGui_ImplWin32_Init(g_hwnd)) {
        ImGui::DestroyContext();
        CleanupDeviceD3D();
        return 1;
    }
    g_ImGuiWin32Initialized = true;
    if (!ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext)) {
        ImGui_ImplWin32_Shutdown();
        g_ImGuiWin32Initialized = false;
        ImGui::DestroyContext();
        CleanupDeviceD3D();
        return 1;
    }
    g_ImGuiDx11Initialized = true;
#if !defined(FPSOVERLAY_UI_QA)
    StartLhwmInitializationThread();
#endif
    // Header link buttons are disabled in this build, so avoid decoding and
    // uploading their hidden textures.

    // ── Timing ──
    using Clock = std::chrono::high_resolution_clock;
    auto lastCpuTime = Clock::now();
    auto lastGpuTime = lastCpuTime;
    float cpuUsage = 0;
    GetCpuUsage(); // seed

    // ── Auto-start overlay if enabled ──
    if (g_Config.autoStart) {
        g_Pending = CMD_START_OVERLAY;
    }

    // ── Main loop ──
    MSG msg = {};
    ULONGLONG autoSettingsTraceReadyTick = 0;
    bool autoSettingsTraceDone = false;
    ULONGLONG autoTrayTraceReadyTick = 0;
    bool autoTrayTraceDone = false;
    while (g_Running)
    {
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessage(&msg);
            if (msg.message == WM_QUIT) g_Running = false;
        }
        if (!g_Running) break;
        if (g_Mode == MODE_OVERLAY &&
            (g_OverlayHostRecoveryPending || !g_hwnd || !IsWindow(g_hwnd))) {
            if (!RecoverOverlayHostWindow()) {
                MsgWaitForMultipleObjectsEx(
                    0, nullptr, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                continue;
            }
        }
        if (g_PowerResumePending) {
            g_PowerResumePending = false;
            RestoreOverlayWindowAfterPowerResume();
            if (HWND tray = g_trayHwnd.load(std::memory_order_acquire))
                PostMessageW(tray, WM_APP_TRAY_RESTORE, 0, 0);
            DelayLhwmReadsAfterResume();
        }
        ResumeLhwmReadsWhenReady();
        UpdatePowerRecoveryStatus();
        ProcessLhwmRescanRequest();
        if (g_D3DRecoveryPending && !RecoverD3DDevice()) {
            MsgWaitForMultipleObjectsEx(
                0, nullptr, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            continue;
        }
        g_trayOverlayVisible.store(g_OvlVisible, std::memory_order_relaxed);
        g_trayDesktopOnlyMode.store(g_Config.desktopOnlyMode,
                                    std::memory_order_relaxed);

        // Process commands after the message pump so tray/menu actions take effect
        // in this same iteration instead of waiting behind another VSync frame.
        if (g_Pending == CMD_START_OVERLAY) {
            g_Pending = CMD_NONE;
            SwitchToOverlay();
            continue;
        }
        if (autoSettingsTrace && g_Mode == MODE_OVERLAY && !autoSettingsTraceDone) {
            if (autoSettingsTraceReadyTick == 0)
                autoSettingsTraceReadyTick = GetTickCount64();
            if (GetTickCount64() - autoSettingsTraceReadyTick >= 2000) {
                autoSettingsTraceDone = true;
                StartSettingsTrace("diagnostic");
                g_SettingsTrace.foregroundMenuStartUs = PerfNowUs();
                g_SettingsTrace.foregroundMenuEndUs = PerfNowUs();
                QueueShowSettings();
            }
        }
        if (autoTrayTrace && g_Mode == MODE_OVERLAY && !autoTrayTraceDone &&
            g_trayHwnd.load(std::memory_order_acquire)) {
            if (autoTrayTraceReadyTick == 0)
                autoTrayTraceReadyTick = GetTickCount64();
            if (GetTickCount64() - autoTrayTraceReadyTick >= 2000) {
                autoTrayTraceDone = true;
                HWND tray = g_trayHwnd.load(std::memory_order_acquire);
                PostMessageW(tray, WM_TRAYICON, 1, MAKELPARAM(WM_RBUTTONUP, 1));
                std::thread([tray]() {
                    Sleep(300);
                    PostMessageW(tray, WM_CANCELMODE, 0, 0);
                }).detach();
            }
        }
        if (g_Pending == CMD_SHOW_SETTINGS) {
            g_SettingsTrace.commandStartUs = PerfNowUs();
            g_Pending = CMD_NONE;
            if (g_Mode == MODE_OVERLAY) {
                g_ShowLiveSettings = true;
                g_OvlVisible = true;
                g_SettingsFirstFramePending = true;
                g_SettingsTrace.clickThroughStartUs = PerfNowUs();
                SetClickThrough(false);
                SetLiveSettingsWindowLevel(true);
                g_SettingsTrace.clickThroughEndUs = PerfNowUs();
                g_SettingsTrace.foregroundSettingsStartUs = PerfNowUs();
                // The tray popup has already made this topmost overlay the active
                // owner. Forcing the full-screen layered HWND to the foreground
                // again can synchronously stall in window manager / DWM work.
                // Once WS_EX_NOACTIVATE is removed, the settings panel activates
                // naturally on the user's first click.
                g_SettingsTrace.foregroundSettingsEndUs = PerfNowUs();
            } else {
                SwitchToConfig();
            }
            g_SettingsTrace.commandEndUs = PerfNowUs();
        }
        if (g_Pending == CMD_EXIT)          { g_Running = false; break; }

        FlushConfigIfDirty(false);

        static bool s_appliedLhwmCpuTempLift = false;
        if (g_lhwmInitFinished.load(std::memory_order_acquire) && !s_appliedLhwmCpuTempLift) {
            s_appliedLhwmCpuTempLift = true;
            if (g_lhwmAvailable && !g_lhwmCpuTempPath.empty())
                g_cpuTempAvailable = true;
        }

        // ══════════════════════════════════════════════════════════════
        // CONFIG MODE
        // ══════════════════════════════════════════════════════════════
        if (g_Mode == MODE_CONFIG)
        {
            // ── Hotkey listener (runs even during config rendering) ──
            if (g_listeningFor != 0) {
                // Check if ESC was pressed to cancel
                if (GetAsyncKeyState(VK_ESCAPE) & 1) {
                    g_listeningFor = 0;
                } else {
                    for (int vk = 1; vk < 256; vk++) {
                        // Skip mouse buttons and modifier-only keys we don't want
                        if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) continue;
                        if (vk == VK_ESCAPE) continue;  // handled above
                        if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) continue;
                        if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) continue;
                        if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) continue;  // Alt keys
                        
                        if (GetAsyncKeyState(vk) & 1) {
                            if (g_listeningFor == 1) g_Config.toggleKey = vk;
                            if (g_listeningFor == 2) g_Config.exitKey   = vk;
                            if (g_listeningFor == 3) g_Config.layoutSwitchKey = vk;
                            g_listeningFor = 0;
                            break;
                        }
                    }
                }
            }

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            bool dashboardChanged = false;
            SettingsUi::PushTheme();
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            ImGui::Begin("##cfg_dashboard", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoSavedSettings);
            ImGui::PopStyleVar();
            DrawSettingsDashboard(false, dashboardChanged);
            if (dashboardChanged)
                MarkConfigDirty();
            ImGui::End();
            SettingsUi::PopTheme();
            Present(0.012f, 0.018f, 0.040f, 1.0f);

#if 0
            SettingsUi::PushTheme();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::Begin("##cfg", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoSavedSettings);

            // ── Title ──
            const float headerY = ImGui::GetCursorPosY();
            ImGui::SetWindowFontScale(1.4f);
            ImGui::TextColored(ImVec4(.18f,.92f,1,1), "FPS Overlay");
            ImGui::SetWindowFontScale(1.0f);
            ImGui::SameLine(); ImGui::TextColored(ImVec4(.45f,.45f,.5f,1), " %s", APP_VERSION);

            const float titleRowBottom = ImGui::GetCursorPosY();
            const float headerButtonsW = CalcHeaderLinkButtonsWidth();
            const float headerButtonsH = CalcHeaderLinkButtonsHeight();
            const float headerButtonsX = ImGui::GetWindowContentRegionMax().x - headerButtonsW;
            DrawHeaderExternalLinkButtonsAt(headerButtonsX, headerY);

            const float headerButtonsBottom = headerY + headerButtonsH;
            float headerNextY = (titleRowBottom > headerButtonsBottom ? titleRowBottom : headerButtonsBottom) + 4.f;

            ImGui::SetCursorPosY(headerNextY);
            DrawDeveloperAttributionLine();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetStyle().ItemSpacing.y);

            ImGui::Separator();

            // ── DISPLAY ──
            SettingsUi::BeginCard("##cfg_display", "显示项目", "选择覆盖层需要呈现的实时指标");
            ImGui::Checkbox("  FPS 计数器（游戏）", &g_Config.showFPS);
            if (!g_isAdmin) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(.9f,.4f,.2f,1), "（需要管理员权限）");
            }
            ImGui::Checkbox("  CPU 使用率", &g_Config.showCpuUsage);
            ImGui::Checkbox("  CPU 温度", &g_Config.showCpuTemp);
            ImGui::Checkbox("  CPU 功耗 (W)", &g_Config.showCpuPower);
            {
                const bool lhwmBusy = !g_lhwmInitFinished.load(std::memory_order_acquire);
                const bool lhwmBad = !g_lhwmAvailable || g_lhwmCpuPowerPath.empty();
                if (lhwmBusy || lhwmBad) {
                    ImGui::SameLine();
                    ImGui::TextColored(lhwmBusy ? ImVec4(.55f,.55f,.58f,1) : ImVec4(.9f,.4f,.2f,1),
                                       lhwmBusy ? "（加载中...）" : "（不可用）");
                }
            }
            ImGui::Checkbox("  CPU 风扇转速 (RPM)", &g_Config.showCpuFan);
            {
                const bool lhwmBusy = !g_lhwmInitFinished.load(std::memory_order_acquire);
                const bool lhwmBad = !g_lhwmAvailable || g_lhwmCpuFanPath.empty();
                if (lhwmBusy || lhwmBad) {
                    ImGui::SameLine();
                    ImGui::TextColored(lhwmBusy ? ImVec4(.55f,.55f,.58f,1) : ImVec4(.9f,.4f,.2f,1),
                                       lhwmBusy ? "（加载中...）" : "（不可用）");
                }
            }
            ImGui::Checkbox("  GPU 使用率", &g_Config.showGpuUsage);
            ImGui::Checkbox("  GPU 温度", &g_Config.showGpuTemp);
            {
                const bool lhwmBusy = !g_lhwmInitFinished.load(std::memory_order_acquire);
                const bool lhwmBad = !g_lhwmAvailable || g_gpuCount == 0;
                if (lhwmBusy || lhwmBad) {
                    ImGui::SameLine();
                    ImGui::TextColored(lhwmBusy ? ImVec4(.55f,.55f,.58f,1) : ImVec4(.9f,.4f,.2f,1),
                                       lhwmBusy ? "（加载中...）" : "（不可用）");
                }
            }
            ImGui::Checkbox("  GPU 功耗 (W)", &g_Config.showGpuPower);
            {
                const bool lhwmBusy = !g_lhwmInitFinished.load(std::memory_order_acquire);
                const bool lhwmBad = !g_lhwmAvailable || g_gpuCount == 0 || g_lhwmGpuPowerPath.empty();
                if (lhwmBusy || lhwmBad) {
                    ImGui::SameLine();
                    ImGui::TextColored(lhwmBusy ? ImVec4(.55f,.55f,.58f,1) : ImVec4(.9f,.4f,.2f,1),
                                       lhwmBusy ? "（加载中...）" : "（不可用）");
                }
            }
            ImGui::Checkbox("  GPU 风扇转速 (RPM)", &g_Config.showGpuFan);
            {
                const bool lhwmBusy = !g_lhwmInitFinished.load(std::memory_order_acquire);
                const bool lhwmBad = !g_lhwmAvailable || g_gpuCount == 0 || g_lhwmGpuFanPath.empty();
                if (lhwmBusy || lhwmBad) {
                    ImGui::SameLine();
                    ImGui::TextColored(lhwmBusy ? ImVec4(.55f,.55f,.58f,1) : ImVec4(.9f,.4f,.2f,1),
                                       lhwmBusy ? "（加载中...）" : "（不可用）");
                }
            }
            ImGui::Checkbox("  系统风扇 / 第三风扇 (RPM)", &g_Config.showSystemFan);
            ImGui::Checkbox("  GPU 显存温度", &g_Config.showGpuMemTemp);
            ImGui::Checkbox("  硬盘温度", &g_Config.showDiskTemp);
            ImGui::Checkbox("  内存条温度", &g_Config.showMemoryTemp);
            ImGui::Checkbox("  显存百分比", &g_Config.showVRAMPercent);
            ImGui::Checkbox("  显存已用 / 总量", &g_Config.showVRAMUsage);
            if (!g_lhwmInitFinished.load(std::memory_order_acquire)) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(.55f,.55f,.58f,1), "（加载中...）");
            } else if (!g_lhwmAvailable || g_gpuCount == 0) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(.9f,.4f,.2f,1), "（不可用）");
            }
            ImGui::Checkbox("  内存百分比", &g_Config.showRAMPercent);
            ImGui::Checkbox("  内存已用 / 总量", &g_Config.showRAMUsage);
            ImGui::Checkbox("  网络下载速度", &g_Config.showNetworkDownload);
            ImGui::Checkbox("  网络上传速度", &g_Config.showNetworkUpload);
            ImGui::Checkbox("  显示进程名称", &g_Config.showProcessName);
            if (ImGui::IsItemHovered())
                TooltipWrapped("在覆盖层中显示当前追踪的游戏/进程名称（适用于所有布局）。");
            ImGui::Checkbox("  显示时间", &g_Config.showTime);
            if (ImGui::IsItemHovered())
                TooltipWrapped("在覆盖层中显示当前本地时间（适用于所有布局）。");
            if (g_Config.showTime) {
                ImGui::Indent(16.f);
                const char* timeFormats[] = { "24 小时制", "12 小时制 (AM/PM)" };
                ImGui::SetNextItemWidth(-1);
                ImGui::Combo("  时间格式", &g_Config.timeFormat, timeFormats, 2);
                ImGui::Checkbox("  显示秒数", &g_Config.timeShowSeconds);
                ImGui::Unindent(16.f);
            }
            SettingsUi::EndCard();

            // ── GPU SELECTION ──
            if (g_gpuCount > 0) {
                SettingsUi::BeginCard("##cfg_gpu_select", "GPU 选择", "选择界面主监控设备");
                
                // Build combo preview string
                const char* previewName = (g_Config.selectedGpu >= 0 && g_Config.selectedGpu < g_gpuCount) 
                    ? g_gpuList[g_Config.selectedGpu].name 
                    : "选择 GPU...";
                
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##gpuselect", previewName)) {
                    for (int i = 0; i < g_gpuCount; i++) {
                        bool isSelected = (g_Config.selectedGpu == i);
                        if (ImGui::Selectable(g_gpuList[i].name, isSelected)) {
                            SelectGpu(i);
                        }
                        if (isSelected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
                
                if (g_gpuCount > 1) {
                    SettingsUi::Muted("检测到多个 GPU，请选择要监控的设备");
                }
                SettingsUi::EndCard();
            }

            // ── FREQUENCY (sparklines; LHWM clock sensors) ──
            SettingsUi::BeginCard("##cfg_frequency", "频率传感器", "CPU 与 GPU 核心时钟来源");
            if (!g_lhwmInitFinished.load(std::memory_order_acquire)) {
                ImGui::TextColored(ImVec4(.55f,.55f,.58f,1), "正在初始化 LibreHardwareMonitor...");
            } else if (!g_lhwmAvailable) {
                ImGui::TextColored(ImVec4(.55f,.55f,.58f,1), "需要 LibreHardwareMonitor。");
            } else {
                ImGui::Checkbox("  显示 CPU 频率", &g_Config.showCpuFreq);
                if (g_Config.showCpuFreq) {
                    ImGui::Indent();
                    const char* cpuPrev = "（选择传感器）";
                    for (const auto& o : g_cpuClockOpts) {
                        if (strcmp(g_Config.cpuFreqPath, o.second.c_str()) == 0) {
                            cpuPrev = o.first.c_str();
                            break;
                        }
                    }
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::BeginCombo("##cpuclkcombo", cpuPrev)) {
                        for (const auto& o : g_cpuClockOpts) {
                            bool isSel = (strcmp(g_Config.cpuFreqPath, o.second.c_str()) == 0);
                            if (ImGui::Selectable(o.first.c_str(), isSel))
                                snprintf(g_Config.cpuFreqPath, sizeof(g_Config.cpuFreqPath), "%s", o.second.c_str());
                            if (isSel) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    if (g_cpuClockOpts.empty())
                        ImGui::TextColored(ImVec4(.85f,.45f,.35f,1), "  未找到 CPU 频率传感器。");
                    ImGui::Unindent();
                }

                ImGui::Checkbox("  显示 GPU 核心频率", &g_Config.showGpuCoreFreq);
                if (g_Config.showGpuCoreFreq && g_gpuCount > 0) {
                    ImGui::Indent();
                    GpuInfo& gg = g_gpuList[g_Config.selectedGpu];
                    const char* gpPrev = "（选择传感器）";
                    for (const auto& o : gg.coreClockOpts) {
                        if (strcmp(g_Config.gpuCoreFreqPath, o.second.c_str()) == 0) {
                            gpPrev = o.first.c_str();
                            break;
                        }
                    }
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::BeginCombo("##gpclkcombo", gpPrev)) {
                        for (const auto& o : gg.coreClockOpts) {
                            bool isSel = (strcmp(g_Config.gpuCoreFreqPath, o.second.c_str()) == 0);
                            if (ImGui::Selectable(o.first.c_str(), isSel))
                                snprintf(g_Config.gpuCoreFreqPath, sizeof(g_Config.gpuCoreFreqPath), "%s", o.second.c_str());
                            if (isSel) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    if (gg.coreClockOpts.empty())
                        ImGui::TextColored(ImVec4(.85f,.45f,.35f,1), "  此 GPU 未找到核心频率传感器。");
                    ImGui::Unindent();
                }
            }
            SettingsUi::EndCard();

            // ── POSITION ──
            SettingsUi::BeginCard("##cfg_position", "覆盖层位置");
            int prevPos = g_Config.position;
            ImGui::RadioButton("左上", &g_Config.position, POS_TOP_LEFT);
            ImGui::SameLine(0, 16);
            ImGui::RadioButton("上方居中", &g_Config.position, POS_TOP_CENTER);
            ImGui::SameLine(0, 16);
            ImGui::RadioButton("右上", &g_Config.position, POS_TOP_RIGHT);
            ImGui::RadioButton("左下", &g_Config.position, POS_BOTTOM_LEFT);
            ImGui::SameLine(0, 16);
            ImGui::RadioButton("下方居中", &g_Config.position, POS_BOTTOM_CENTER);
            ImGui::SameLine(0, 16);
            ImGui::RadioButton("右下", &g_Config.position, POS_BOTTOM_RIGHT);
            // Reset custom position when corner preset is changed
            if (g_Config.position != prevPos) {
                g_Config.customX = -1.0f;
                g_Config.customY = -1.0f;
            }
            ImGui::Spacing();
            SettingsUi::Muted("按住 CTRL 可拖动覆盖层或右键打开菜单");
            SettingsUi::EndCard();

            // ── LAYOUT ──
            SettingsUi::BeginCard("##cfg_layout", "布局与外观", "游戏加加横向布局内部结构保持不变");
            ImGui::RadioButton("  垂直排列（默认）", &g_Config.layoutStyle, LAYOUT_VERTICAL);
            ImGui::RadioButton("  水平紧凑", &g_Config.layoutStyle, LAYOUT_HORIZONTAL);
            ImGui::RadioButton("  Steam 风格栏", &g_Config.layoutStyle, LAYOUT_STEAM);
            ImGui::RadioButton("  游戏加加风格", &g_Config.layoutStyle, LAYOUT_GAMEPP);
            if (ImGui::IsItemHovered())
                TooltipWrapped(
                    "黑色栏位，带 Steam 风格的 FPS / CPU / GPU 标签。\n"
                    "显示内容与水平紧凑布局一致（温度、显存/内存详情、进程名）。\n"
                    "100% 大小时文字比例与垂直/水平布局一致。");
            if (g_Config.layoutStyle == LAYOUT_GAMEPP) {
                ImGui::Indent(16.f);
                const char* presets[] = { "480", "720", "1080", "2K", "4K" };
                ImGui::TextColored(ImVec4(.55f,.70f,.95f,1), "字体大小");
                for (int i = 0; i < 5; ++i) {
                    if (i > 0) ImGui::SameLine(0, 14.f);
                    ImGui::RadioButton(presets[i], &g_Config.gameppFontPreset, i);
                }
                ImGui::SliderInt("刷新时间 (ms)", &g_Config.refreshMs, 250, 5000, "%d");
                int snap = g_Config.monitorSnap ? 1 : 0;
                if (ImGui::RadioButton("吸附开启", &snap, 1)) g_Config.monitorSnap = true;
                ImGui::SameLine(0, 18.f);
                if (ImGui::RadioButton("吸附关闭", &snap, 0)) g_Config.monitorSnap = false;
                ImGui::Unindent(16.f);
            }
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(.55f,.70f,.95f,1), "覆盖层大小");
            ImGui::Spacing();
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderInt("##ovscale", &g_Config.overlayScale, 50, 200, "%d%%");
            if (ImGui::IsItemHovered())
                TooltipWrapped(
                    "调整垂直、水平和 Steam 风格布局的文字与间距。\n"
                    "在覆盖层上按住 CTRL 并拖动即可移动。");
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(.55f,.70f,.95f,1), "覆盖层不透明度");
            ImGui::Spacing();
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderInt("##opac", &g_Config.opacity, 30, 100, "%d%%");
            if (ImGui::IsItemHovered())
                TooltipWrapped("所有布局的背景透明度（默认 85%）。");
            SettingsUi::EndCard();
             
            // ── TEMPERATURE UNIT ──
            SettingsUi::BeginCard("##cfg_temperature", "温度单位");
            int tempUnit = g_Config.useFahrenheit ? 1 : 0;
            if (ImGui::RadioButton("摄氏度", &tempUnit, 0)) g_Config.useFahrenheit = false;
            ImGui::SameLine(0,24);
            if (ImGui::RadioButton("华氏度", &tempUnit, 1)) g_Config.useFahrenheit = true;
            SettingsUi::EndCard();

            // ── HOTKEYS ──
            SettingsUi::BeginCard("##cfg_hotkeys", "快捷键", "按 ESC 可取消录制");

            // Toggle key
            ImGui::Text("显示/隐藏：");
            ImGui::SameLine(120);
            if (g_listeningFor == 1) {
                ImGui::TextColored(ImVec4(1,.8f,.2f,1), "请按任意键...  ");
                ImGui::SameLine();
                if (ImGui::SmallButton("取消##1")) g_listeningFor = 0;
            } else {
                ImGui::Text("%-12s", GetKeyName(g_Config.toggleKey));
                ImGui::SameLine();
                if (ImGui::SmallButton("更改##1")) g_listeningFor = 1;
            }

            // Exit key
            ImGui::Text("退出：");
            ImGui::SameLine(120);
            if (g_listeningFor == 2) {
                ImGui::TextColored(ImVec4(1,.8f,.2f,1), "请按任意键...  ");
                ImGui::SameLine();
                if (ImGui::SmallButton("取消##2")) g_listeningFor = 0;
            } else {
                ImGui::Text("%-12s", GetKeyName(g_Config.exitKey));
                ImGui::SameLine();
                if (ImGui::SmallButton("更改##2")) g_listeningFor = 2;
            }

            ImGui::Text("切换布局：");
            ImGui::SameLine(120);
            if (g_listeningFor == 3) {
                ImGui::TextColored(ImVec4(1,.8f,.2f,1), "请按任意键...  ");
                ImGui::SameLine();
                if (ImGui::SmallButton("取消##3")) g_listeningFor = 0;
            } else {
                ImGui::Text("%-12s", GetKeyName(g_Config.layoutSwitchKey));
                ImGui::SameLine();
                if (ImGui::SmallButton("更改##3")) g_listeningFor = 3;
            }
            SettingsUi::EndCard();

            // ── STARTUP ──
            SettingsUi::BeginCard("##cfg_startup", "启动行为");
            DrawWindowsStartupToggle();
            ImGui::Checkbox("  启动时直接显示覆盖层", &g_Config.autoStart);
            if (ImGui::IsItemHovered())
                TooltipWrapped("下次跳过此设置窗口，直接启动覆盖层。");
            ImGui::Checkbox("  仅桌面显示", &g_Config.desktopOnlyMode);
            if (ImGui::IsItemHovered())
                TooltipWrapped("前台应用铺满屏幕时自动隐藏监测，退出全屏后自动恢复。");
            SettingsUi::Muted("开机自启动使用用户登录计划任务，并以最高权限运行。");
            SettingsUi::EndCard();

            // ── HARDWARE ──
            SettingsUi::BeginCard("##cfg_hardware", "硬件与诊断");
            ImGui::TextColored(ImVec4(.50f,.50f,.55f,1), "CPU:  %s", g_cpuName);
            ImGui::TextColored(ImVec4(.50f,.50f,.55f,1), "GPU:  %s", g_gpuName);
            if (ImGui::Button("复制诊断摘要##cfg_copy_diag")) {
                if (!CopyDiagnosticSummaryToClipboard())
                    MessageBoxA(g_hwnd, "复制诊断摘要失败。", "FPS Overlay",
                                MB_OK | MB_ICONERROR | MB_TOPMOST);
            }
            ImGui::SameLine();
            if (ImGui::Button("导出诊断包##cfg_diag")) {
                char diagDir[MAX_PATH] = {};
                if (ExportDiagnosticsPackage(diagDir, sizeof(diagDir))) {
                    ShellExecuteA(nullptr, "open", diagDir, nullptr, nullptr, SW_SHOWNORMAL);
                } else {
                    MessageBoxA(g_hwnd, "导出诊断包失败。", "FPS Overlay",
                                MB_OK | MB_ICONERROR | MB_TOPMOST);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("手动安装 PawnIO##cfg_pawnio")) {
                bool ok = ExtractAndRunPawnIOSetup();
                MessageBoxW(g_hwnd,
                    ok ? L"PawnIO 安装程序已运行完成。若硬件数据仍不可用，请手动重启电脑后再试。"
                       : L"PawnIO 安装程序运行失败。程序仍会继续运行，读不到的数据会显示 N/A。",
                    L"FPS Overlay", MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONWARNING) | MB_TOPMOST);
            }
            SettingsUi::EndCard();

            SettingsUi::Subsection("提醒与功耗", "温度、低帧率、自动显示和笔记本功耗功能");
            g_FeatureRegistry.DrawSettings(g_FeatureContext);

            // ── START BUTTON ──
            ImGui::Spacing(); ImGui::Spacing(); ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(.04f,.42f,.62f,1));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(.08f,.63f,.82f,1));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(.34f,.18f,.68f,1));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8);
            const bool lhwmBusy = !g_lhwmInitFinished.load(std::memory_order_acquire);
            const char* startBtnLabel = lhwmBusy ? "正在初始化 LibreHardwareMonitor..." : "启动覆盖层";
            ImGui::BeginDisabled(lhwmBusy);
            if (ImGui::Button(startBtnLabel, ImVec2(ImGui::GetContentRegionAvail().x, 42)))
                g_Pending = CMD_START_OVERLAY;
            ImGui::EndDisabled();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);

            ImGui::End();
            SettingsUi::PopTheme();
            Present(0.08f, 0.08f, 0.10f, 1);
#endif
        }

        // ══════════════════════════════════════════════════════════════
        // OVERLAY MODE
        // ══════════════════════════════════════════════════════════════
        else
        {
            // ── Hotkeys (user-configurable) ──
            if (g_ShowLiveSettings || g_listeningFor != 0) {
                UpdateHotkeyCapture();
            } else {
                if (GetAsyncKeyState(g_Config.toggleKey) & 1)
                    g_OvlVisible = !g_OvlVisible;
                if (GetAsyncKeyState(g_Config.layoutSwitchKey) & 1)
                    CycleOverlayLayout();
                if (GetAsyncKeyState(g_Config.exitKey) & 1)
                    { g_Running = false; break; }
            }

            // ── Update target PID (foreground window's process) ──
            DWORD currentPid = g_targetPid.load(std::memory_order_relaxed);
            static ULONGLONG s_lastForegroundPoll = 0;
            static DWORD s_pendingTargetPid = 0;
            static ULONGLONG s_pendingTargetSince = 0;
            static ULONGLONG s_lastTargetEvidenceTick = 0;
            const ULONGLONG foregroundNow = GetTickCount64();
            if (foregroundNow - s_lastForegroundPoll >= 250) {
                s_lastForegroundPoll = foregroundNow;
                const DWORD previousTargetPid = currentPid;
                const ForegroundSnapshot foreground = CaptureForegroundSnapshot();
                const DWORD fgPid = foreground.pid;
                bool foregroundBlocksGameTarget = false;
                const bool foregroundStrongGame =
                    foreground.likelyGameWindow && foreground.strongGame;
                const bool foregroundKnownDesktop = foreground.knownDesktop;
                bool gameDisplayActive = false;
                DWORD proposedPid = 0;
                bool proposedImmediate = false;
                const char* proposedReason = nullptr;
                char proposedExe[320] = {};
                if (foreground.likelyGameWindow) {
                    foregroundBlocksGameTarget =
                        foregroundKnownDesktop && !foregroundStrongGame &&
                        foreground.fullscreen;
                    if (foregroundStrongGame) {
                        proposedPid = fgPid;
                        proposedImmediate = true;
                        proposedReason = "foreground strong game";
                        snprintf(proposedExe, sizeof(proposedExe), "%s",
                                 foreground.exeName);
                    }
                }

                const DWORD autoPid = g_autoTargetPid.load(std::memory_order_relaxed);
                const float autoFps = g_autoGameFps.load(std::memory_order_relaxed);
                char autoExe[320] = {};
                bool autoStrongGame = false;
                if (!foregroundBlocksGameTarget && autoPid != 0 && autoFps >= 12.0f) {
                    if (autoPid == foreground.pid && foreground.likelyGameWindow) {
                        autoStrongGame = foregroundStrongGame;
                        if (autoStrongGame)
                            snprintf(autoExe, sizeof(autoExe), "%s", foreground.exeName);
                    } else {
                        autoStrongGame = IsStrongGameProcess(
                            autoPid, autoExe, sizeof(autoExe));
                    }
                }
                const bool sustainedUnknownForeground =
                    !foregroundBlocksGameTarget && !foregroundKnownDesktop &&
                    fgPid != 0 && autoPid == fgPid && autoFps >= 20.0f;
                if (proposedPid == 0 && (autoStrongGame || sustainedUnknownForeground)) {
                    proposedPid = autoPid;
                    proposedImmediate = autoStrongGame;
                    proposedReason = autoStrongGame
                        ? "active ETW game target"
                        : "sustained foreground ETW target";
                    if (autoExe[0])
                        snprintf(proposedExe, sizeof(proposedExe), "%s", autoExe);
                }

                if (proposedPid == 0 && !foregroundBlocksGameTarget &&
                    RecentGameStillUsable(foregroundNow, 120000)) {
                    const float directFps = g_gameFps.load(std::memory_order_relaxed);
                    if (autoPid == g_RecentGamePid || autoFps >= 12.0f || directFps >= 1.0f) {
                        proposedPid = g_RecentGamePid;
                        proposedReason = "recent game target hold";
                    }
                }

                const float directFps = g_gameFps.load(std::memory_order_relaxed);
                if (previousTargetPid != 0 && directFps >= 1.0f)
                    s_lastTargetEvidenceTick = foregroundNow;

                if (foregroundBlocksGameTarget) {
                    currentPid = 0;
                    s_pendingTargetPid = 0;
                    s_pendingTargetSince = 0;
                } else if (proposedPid == previousTargetPid && proposedPid != 0) {
                    currentPid = proposedPid;
                    s_lastTargetEvidenceTick = foregroundNow;
                    s_pendingTargetPid = 0;
                    s_pendingTargetSince = 0;
                } else if (previousTargetPid == 0 && proposedPid != 0 && proposedImmediate) {
                    currentPid = proposedPid;
                    s_lastTargetEvidenceTick = foregroundNow;
                    s_pendingTargetPid = 0;
                    s_pendingTargetSince = 0;
                } else if (proposedPid != 0 && proposedPid != previousTargetPid) {
                    if (s_pendingTargetPid != proposedPid) {
                        s_pendingTargetPid = proposedPid;
                        s_pendingTargetSince = foregroundNow;
                    }
                    const ULONGLONG requiredStableMs = previousTargetPid == 0 ? 1000 : 1500;
                    if (foregroundNow - s_pendingTargetSince >= requiredStableMs) {
                        currentPid = proposedPid;
                        s_lastTargetEvidenceTick = foregroundNow;
                        s_pendingTargetPid = 0;
                        s_pendingTargetSince = 0;
                    } else {
                        currentPid = previousTargetPid;
                    }
                } else if (previousTargetPid != 0 &&
                           IsProcessAlive(previousTargetPid) &&
                           s_lastTargetEvidenceTick != 0 &&
                           foregroundNow - s_lastTargetEvidenceTick < 3000) {
                    currentPid = previousTargetPid;
                } else {
                    currentPid = 0;
                    s_pendingTargetPid = 0;
                    s_pendingTargetSince = 0;
                }

                g_targetPid.store(currentPid, std::memory_order_relaxed);
                if (currentPid != 0 && currentPid == proposedPid) {
                    if (proposedExe[0])
                        RememberRecentGame(currentPid, proposedExe);
                    else
                        RefreshRecentGame(currentPid);
                    RememberGameOverlayDisplay(
                        currentPid, proposedReason ? proposedReason : "stable game target");
                    gameDisplayActive = true;
                } else if (currentPid != 0 &&
                           g_GameOverlayDisplayActive &&
                           g_GameOverlayDisplayPid == currentPid) {
                    gameDisplayActive = true;
                }
                if (foregroundBlocksGameTarget || !gameDisplayActive)
                    ClearGameOverlayDisplay();

                const bool wasAutoHidden = g_FullscreenAutoHidden;
                const DesktopVisibilityState previousState = g_DesktopVisibilityState;
                UpdateDesktopVisibilityDecision(foreground);
                if (wasAutoHidden != g_FullscreenAutoHidden ||
                    previousState != g_DesktopVisibilityState) {
                    LogLine("Desktop-only decision: pid=%lu exe=%s state=%d hidden=%s reason=%s fps=%.1f autoPid=%lu autoFps=%.1f recentGame=%lu target=%lu gameDisplay=%d",
                            (unsigned long)g_DesktopForegroundPid,
                            g_DesktopForegroundExe[0] ? g_DesktopForegroundExe : "N/A",
                            (int)g_DesktopVisibilityState,
                            g_FullscreenAutoHidden ? "yes" : "no",
                            g_DesktopVisibilityReason,
                            g_gameFps.load(std::memory_order_relaxed),
                            (unsigned long)autoPid,
                            autoFps,
                            (unsigned long)g_RecentGamePid,
                            (unsigned long)currentPid,
                            g_GameOverlayDisplayActive ? 1 : 0);
                }
            }
            
            // ── Reset FPS when target app changes or closes ──
            if (currentPid != 0 && currentPid != g_lastTargetPid) {
                g_gameFps.store(0.0f, std::memory_order_relaxed);
                g_lastTargetPid = currentPid;
                // Update process name
                GetProcessName(currentPid, g_targetProcessName, sizeof(g_targetProcessName));
                LogLine("Target process changed: pid=%lu name=%s",
                        (unsigned long)currentPid,
                        g_targetProcessName[0] ? g_targetProcessName : "N/A");
            }
            if (currentPid == 0 && g_lastTargetPid != 0) {
                LogLine("Target process cleared: previous pid=%lu",
                        (unsigned long)g_lastTargetPid);
                g_gameFps.store(0.0f, std::memory_order_relaxed);
                g_lastTargetPid = 0;
                g_targetProcessName[0] = '\0';
            }
            // Also check if the process is still alive. Do not do this every frame:
            // OpenProcess can become expensive while the live settings panel is active.
            static ULONGLONG s_lastTargetAliveCheck = 0;
            const ULONGLONG aliveNow = GetTickCount64();
            if (g_lastTargetPid != 0 && aliveNow - s_lastTargetAliveCheck >= 1000) {
                s_lastTargetAliveCheck = aliveNow;
                if (!IsProcessAlive(g_lastTargetPid)) {
                    g_gameFps.store(0.0f, std::memory_order_relaxed);
                    g_lastTargetPid = 0;
                    g_targetPid.store(0, std::memory_order_relaxed);
                    g_targetProcessName[0] = '\0';
                    ClearGameOverlayDisplay();
                }
            }

            if (g_ShowLiveSettings && !g_OvlVisible)
                g_OvlVisible = true;
            const bool effectiveOverlayVisible =
                g_OvlVisible && (!g_FullscreenAutoHidden || g_ShowLiveSettings);

            // ── Show/Hide window based on effective visibility ──
            static bool wasVisible = true;
            bool visibilityChanged = false;
            if (effectiveOverlayVisible != wasVisible) {
                ShowWindow(g_hwnd, effectiveOverlayVisible ? SW_SHOWNA : SW_HIDE);
                wasVisible = effectiveOverlayVisible;
                visibilityChanged = true;
            }

            if (!effectiveOverlayVisible && !g_ShowLiveSettings) {
                // Foreground recognition and ETW stay active above this point,
                // so a game can restore the overlay even after an app was hidden.
                MsgWaitForMultipleObjectsEx(0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                continue;
            }

            if (!g_ShowLiveSettings) {
                HWND placementFg = GetForegroundWindow();
                const DWORD placementTargetPid =
                    g_targetPid.load(std::memory_order_relaxed);
                const bool gameMonitor =
                    g_GameOverlayDisplayActive ||
                    g_DesktopVisibilityState == DESKTOP_VISIBILITY_GAME;
                UpdateOverlayHostWindow(gameMonitor, placementFg,
                                        placementTargetPid, visibilityChanged);
            }

            // ── Periodic metrics (once/sec) ──
            // Skip ALL stats reading while dragging for maximum smoothness
            static float cachedRamUsed = 0, cachedRamTotal = 1;
            auto now = Clock::now();

            if (!g_isDragging) {
                const bool powerComparisonRecording =
                    g_FeatureRegistry.IsPowerComparisonRecording();
                static bool wasPowerComparisonRecording = false;
                if (powerComparisonRecording && !wasPowerComparisonRecording) {
                    lastCpuTime = now - std::chrono::seconds(10);
                    lastGpuTime = now - std::chrono::seconds(10);
                    g_comparisonPowerSampleTick.store(
                        0, std::memory_order_release);
                    g_comparisonPowerSampleSequence.store(
                        0, std::memory_order_release);
                }
                wasPowerComparisonRecording = powerComparisonRecording;
                const float refreshSec = powerComparisonRecording
                    ? 0.25f
                    : (float)g_Config.refreshMs / 1000.0f;
                const bool pauseHeavySensors =
                    (g_ShowLiveSettings || g_listeningFor != 0) &&
                    !powerComparisonRecording;
                float cpuElapsed = std::chrono::duration<float>(now - lastCpuTime).count();
                if (cpuElapsed >= refreshSec) {
                    cpuUsage = GetCpuUsage();
                    // Poll CPU temp - prefer LHWM over WMI
                    if (!pauseHeavySensors && g_lhwmAvailable && !g_lhwmCpuTempPath.empty()) {
                        g_cpuTemp = g_lhwmCpuTemp;
                    } else if (!pauseHeavySensors && g_cpuTempAvailable) {
                        g_cpuTemp = QueryCpuTemperature();
                    }
                    lastCpuTime = now;
                }

                float gpuElapsed = std::chrono::duration<float>(now - lastGpuTime).count();
                if (gpuElapsed >= refreshSec) {
                    if (powerComparisonRecording) {
                        ScheduleComparisonPowerPoll();
                    } else {
                        // Poll LHWM first (covers AMD, Intel, NVIDIA)
                        if (!pauseHeavySensors && g_lhwmAvailable &&
                            !g_comparisonPowerPollInFlight.load(std::memory_order_acquire)) {
                            ScheduleAsyncLHWMStatsPoll();
                        }
                        if (!pauseHeavySensors) {
                            PollExternalPowerStats();
                            PollWindowsPowerCounters();
                        }
                    }
                    lastGpuTime = now;
                    
                }

                // ── RAM ──
                static ULONGLONG s_lastRamPoll = 0;
                const ULONGLONG ramNow = GetTickCount64();
                const int ramRefreshMs =
                    powerComparisonRecording ? 250 : (std::max)(250, g_Config.refreshMs);
                if (s_lastRamPoll == 0 || ramNow - s_lastRamPoll >=
                    (ULONGLONG)ramRefreshMs) {
                    s_lastRamPoll = ramNow;
                    MEMORYSTATUSEX mem = {}; mem.dwLength = sizeof(mem);
                    if (GlobalMemoryStatusEx(&mem)) {
                        cachedRamUsed  = (float)(mem.ullTotalPhys - mem.ullAvailPhys) / (1024.f*1024.f*1024.f);
                        cachedRamTotal = (float)(mem.ullTotalPhys)                    / (1024.f*1024.f*1024.f);
                    }
                }

                static ULONGLONG s_lastNetworkPoll = 0;
                const ULONGLONG networkNow = GetTickCount64();
                if ((g_Config.showNetworkDownload || g_Config.showNetworkUpload) &&
                    (s_lastNetworkPoll == 0 || networkNow - s_lastNetworkPoll >= 500)) {
                    s_lastNetworkPoll = networkNow;
                    UpdateNetworkRates();
                }

                static auto lastClkPoll = Clock::now();
                float clkDt = std::chrono::duration<float>(now - lastClkPoll).count();
                const float clkRefreshSec =
                    (std::max)(0.5f, (float)g_Config.refreshMs / 1000.0f);
                if (!powerComparisonRecording &&
                    !pauseHeavySensors && clkDt >= clkRefreshSec) {
                    lastClkPoll = now;
                    PollClockSensors();
                }
            }
            ApplyAsyncLHWMStatsSnapshot();
            ApplyComparisonPowerPollSnapshot();
            
            // Use cached values (updated when not dragging)
            float ramUsed = cachedRamUsed;
            float ramTotal = cachedRamTotal;

            // ── Game FPS (from ETW) ──
            float gameFps = g_gameFps.load(std::memory_order_relaxed);
            if (gameFps <= 0.0f) {
                const float autoFps = g_autoGameFps.load(std::memory_order_relaxed);
                const DWORD autoPid = g_autoTargetPid.load(std::memory_order_relaxed);
                if (autoFps > 0.0f && autoPid != 0) {
                    gameFps = autoFps;
                    static DWORD s_lastAutoNamePid = 0;
                    if (autoPid != s_lastAutoNamePid && g_targetProcessName[0] == '\0') {
                        GetProcessName(autoPid, g_targetProcessName, sizeof(g_targetProcessName));
                        s_lastAutoNamePid = autoPid;
                    }
                }
            }
            const float averageFps = UpdateRollingAverageFps(
                gameFps, g_targetPid.load(std::memory_order_relaxed));
            const float displayFps =
                g_Config.useAverageFPS && averageFps > 0.0f ? averageFps : gameFps;

            // ── Handle CTRL key for dragging / right-click menu ──
            // Only respond to CTRL when cursor is hovering over the overlay
            UpdateFeatureContext(gameFps, cpuUsage, ramUsed, ramTotal);
            g_FeatureRegistry.Update(g_FeatureContext);
            LogPowerModelSnapshot(g_FeatureContext);
            if (g_FeatureContext.hasOverlayVisibilityRequest) {
                g_OvlVisible = g_FeatureContext.requestOverlayVisible;
            }

            POINT cursorPt; GetCursorPos(&cursorPt);
            bool cursorOverOverlay = PtInRect(&g_overlayBounds, cursorPt);
            bool ctrlKeyDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            bool leftMouseDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            
            // Track interaction and drag state separately
            // - interactionActive: controls click-through and window flags
            // - g_isDragging: controls VSync (set by ImGui's drag detection for accuracy)
            static bool interactionActive = false;
            
            // Reset drag state when mouse released
            if (!leftMouseDown) {
                g_isDragging = false;
            }
            
            // Enter interaction mode: Ctrl held AND cursor over overlay
            if (ctrlKeyDown && cursorOverOverlay) {
                interactionActive = true;
            }
            // Exit interaction mode: Ctrl released, OR (cursor left overlay AND not actively interacting)
            else if (!ctrlKeyDown) {
                interactionActive = false;
                g_isDragging = false;
            }
            else if (!cursorOverOverlay && !leftMouseDown) {
                // Only exit if cursor left AND not holding mouse button
                interactionActive = false;
            }
            // While mouse is held, stay in interaction mode regardless of cursor position
            
            bool ctrlHeld = interactionActive;
            const bool cursorOverLiveSettings =
                g_ShowLiveSettings && PtInRect(&g_liveSettingsBounds, cursorPt);
            const bool wantsOverlayInput = ctrlHeld || cursorOverLiveSettings;
            
            // Manage click-through state
            // IMPORTANT: Don't re-enable click-through while mouse button is held
            // (this would interrupt an ongoing drag operation)
            static bool wasCtrlHeld = false;
            if (wantsOverlayInput && !wasCtrlHeld) {
                // Entering interaction mode - disable click-through
                SetClickThrough(false);
                wasCtrlHeld = true;
            } else if (!wantsOverlayInput && wasCtrlHeld && !leftMouseDown) {
                // Exiting interaction mode - re-enable click-through only if mouse is released
                SetClickThrough(true);
                wasCtrlHeld = false;
            }
            
            // Right-click context menu (when CTRL is held AND right-click happens IN interaction mode)
            // Track right mouse button state to detect fresh clicks (not clicks from elsewhere)
            static bool rightMouseWasDown = false;
            bool rightMouseDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
            bool rightMouseJustPressed = rightMouseDown && !rightMouseWasDown;
            rightMouseWasDown = rightMouseDown;
            
            // Only show menu if right-click happened while already in interaction mode
            if (ctrlHeld && rightMouseJustPressed) {
                HMENU m = CreatePopupMenu();
                AppendMenuW(m, MF_STRING, IDM_HIDE, L"隐藏覆盖层");
                AppendMenuW(m, MF_STRING, IDM_RESET_POS, L"重置位置");
                AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"设置");
                AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出");
                SetForegroundWindow(g_hwnd);
                int cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                         cursorPt.x, cursorPt.y, 0, g_hwnd, nullptr);
                DestroyMenu(m);
                switch (cmd) {
                    case IDM_HIDE:       g_OvlVisible = false;           break;
                    case IDM_RESET_POS:
                        g_Config.customX = -1.f;
                        g_Config.customY = -1.f;
                        g_overlayForceCornerSnap = true;
                        MarkConfigDirty();
                        break;
                    case IDM_SETTINGS: QueueShowSettings();           break;
                    case IDM_EXIT:     g_Pending = CMD_EXIT;           break;
                }
            }

            // ── ImGui frame ──
            if (g_SettingsFirstFramePending)
                g_SettingsTrace.newFrameStartUs = PerfNowUs();
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            if (g_SettingsFirstFramePending)
                g_SettingsTrace.newFrameEndUs = PerfNowUs();

            // Position: use custom if set, otherwise use corner preset
            // IMPORTANT: Don't set position during interaction mode - let ImGui handle dragging
            float margin = (g_Config.layoutStyle == LAYOUT_STEAM || g_Config.layoutStyle == LAYOUT_GAMEPP) ? 0.f : 16.f;
            static RECT work{};
            static ULONGLONG s_lastWorkAreaPoll = 0;
            static bool s_lastAnchorGameMonitor = false;
            const ULONGLONG workAreaNow = GetTickCount64();
            if (s_lastWorkAreaPoll == 0 ||
                s_lastAnchorGameMonitor != g_OverlayUsingGameMonitor ||
                workAreaNow - s_lastWorkAreaPoll >= 1000) {
                if (g_OverlayUsingGameMonitor) {
                    GetClientRect(g_hwnd, &work);
                } else {
                    SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0);
                    MapWindowPoints(HWND_DESKTOP, g_hwnd,
                                    reinterpret_cast<POINT*>(&work), 2);
                }
                s_lastWorkAreaPoll = workAreaNow;
                s_lastAnchorGameMonitor = g_OverlayUsingGameMonitor;
            }
            const float wx = (float)work.left;
            const float wy = (float)work.top;
            const float sw = (float)(work.right - work.left);
            const float sh = (float)(work.bottom - work.top);
            ImVec2 pos, pivot = {0, 0};
            bool hasCustomPos = (g_Config.customX >= 0 && g_Config.customY >= 0);
            
            if (hasCustomPos) {
                // User has dragged the overlay - use their position
                pos = ImVec2(g_Config.customX, g_Config.customY);
            } else {
                // Use corner preset (coordinates within primary monitor work area)
                switch (g_Config.position) {
                    default:
                    case POS_TOP_LEFT:
                        pos = ImVec2(wx + margin, wy + margin);
                        pivot = {0, 0};
                        break;
                    case POS_TOP_CENTER:
                        pos = ImVec2(wx + sw * 0.5f, wy + margin);
                        pivot = {0.5f, 0};
                        break;
                    case POS_TOP_RIGHT:
                        pos = ImVec2(wx + sw - margin, wy + margin);
                        pivot = {1, 0};
                        break;
                    case POS_BOTTOM_LEFT:
                        pos = ImVec2(wx + margin, wy + sh - margin);
                        pivot = {0, 1};
                        break;
                    case POS_BOTTOM_CENTER:
                        pos = ImVec2(wx + sw * 0.5f, wy + sh - margin);
                        pivot = {0.5f, 1};
                        break;
                    case POS_BOTTOM_RIGHT:
                        pos = ImVec2(wx + sw - margin, wy + sh - margin);
                        pivot = {1, 1};
                        break;
                }
            }
            
            // Only set position when NOT in interaction mode to avoid fighting with ImGui's drag
            // - When not interacting: set position (either corner preset or saved custom position)
            // - When interacting (ctrlHeld): let ImGui manage position freely for smooth dragging
            // - g_overlayForceCornerSnap: after "Reset Position" menu, snap to corner even if CTRL still held
            {
                bool forceSnap = g_overlayForceCornerSnap;
                if (forceSnap)
                    g_overlayForceCornerSnap = false;
                if (!ctrlHeld || forceSnap) {
                    ImGui::SetNextWindowPos(pos, hasCustomPos ? ImGuiCond_Once : ImGuiCond_Always, pivot);
                }
            }
            
            const float opacityPct = (float)g_Config.opacity;
            const float overlayBgAlpha = ctrlHeld ? 1.0f : (opacityPct / 100.f);
            ImGui::SetNextWindowBgAlpha(overlayBgAlpha);

            const float ovSc = g_Config.overlayScale / 100.f;

            // Window flags - allow dragging when CTRL is held
            ImGuiWindowFlags wf =
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
            
            if (!ctrlHeld) {
                wf |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs;
            }

            if (g_Config.layoutStyle == LAYOUT_STEAM || g_Config.layoutStyle == LAYOUT_GAMEPP) {
                const bool gamepp = g_Config.layoutStyle == LAYOUT_GAMEPP;
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, gamepp ? ImVec2(0.f, 0.f) : ImVec2(10.f * ovSc, 6.f * ovSc));
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, gamepp ? ImVec2(0.f, 0.f) : ImVec2(4.f * ovSc, 2.f * ovSc));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
                ImGui::PushStyleColor(ImGuiCol_WindowBg, gamepp ? ImVec4(0.f, 0.f, 0.f, 0.f) : ImVec4(0.f, 0.f, 0.f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.22f, 0.22f, 0.24f, 0.f));
            }

            ImGui::Begin("##ovl", nullptr, wf);
            
            // Update overlay bounds for hit-testing (used for context menu check)
            {
                ImVec2 wPos = ImGui::GetWindowPos();
                ImVec2 wSize = ImGui::GetWindowSize();
                g_overlayBounds.left   = (LONG)wPos.x;
                g_overlayBounds.top    = (LONG)wPos.y;
                g_overlayBounds.right  = (LONG)(wPos.x + wSize.x);
                g_overlayBounds.bottom = (LONG)(wPos.y + wSize.y);
            }
            
            // Save position when dragged and update drag state
            if (ctrlHeld) {
                ImVec2 winPos = ImGui::GetWindowPos();
                g_Config.customX = winPos.x;
                g_Config.customY = winPos.y;
                
                // Disable VSync as soon as user clicks in interaction mode
                // (don't wait for drag threshold - this prevents initial lag)
                if (leftMouseDown) {
                    g_isDragging = true;
                }
            }
            {
                static bool s_prevOverlayLeftDown = false;
                if (ctrlHeld && s_prevOverlayLeftDown && !leftMouseDown && g_Config.monitorSnap) {
                    ImVec2 winPos = ImGui::GetWindowPos();
                    ImVec2 winSize = ImGui::GetWindowSize();
                    const float snap = 32.f * g_dpiScale;
                    if (fabsf(winPos.x - wx) <= snap) winPos.x = wx;
                    if (fabsf((winPos.x + winSize.x) - (wx + sw)) <= snap) winPos.x = wx + sw - winSize.x;
                    if (fabsf(winPos.y - wy) <= snap) winPos.y = wy;
                    if (fabsf((winPos.y + winSize.y) - (wy + sh)) <= snap) winPos.y = wy + sh - winSize.y;
                    g_Config.customX = winPos.x;
                    g_Config.customY = winPos.y;
                    MarkConfigDirty();
                }
                s_prevOverlayLeftDown = ctrlHeld && leftMouseDown;
            }
            
            // ── Draw glowing border when CTRL is held ──
            if (ctrlHeld) {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 wMin = ImGui::GetWindowPos();
                ImVec2 wMax = ImVec2(wMin.x + ImGui::GetWindowSize().x, wMin.y + ImGui::GetWindowSize().y);
                
                // Animated glow effect (pulsing)
                float t = (float)fmod(ImGui::GetTime() * 2.0, 3.14159 * 2.0);
                float glow = 0.6f + 0.4f * sinf(t);
                
                // Draw multiple borders for glow effect (outer to inner)
                ImU32 glowColor1 = IM_COL32(80, 180, 255, (int)(40 * glow));
                ImU32 glowColor2 = IM_COL32(80, 180, 255, (int)(80 * glow));
                ImU32 glowColor3 = IM_COL32(100, 200, 255, (int)(160 * glow));
                ImU32 coreColor  = IM_COL32(120, 220, 255, (int)(255 * glow));
                
                dl->AddRect(ImVec2(wMin.x - 4, wMin.y - 4), ImVec2(wMax.x + 4, wMax.y + 4), glowColor1, 8.0f, 0, 3.0f);
                dl->AddRect(ImVec2(wMin.x - 2, wMin.y - 2), ImVec2(wMax.x + 2, wMax.y + 2), glowColor2, 6.0f, 0, 2.0f);
                dl->AddRect(ImVec2(wMin.x - 1, wMin.y - 1), ImVec2(wMax.x + 1, wMax.y + 1), glowColor3, 4.0f, 0, 1.5f);
                dl->AddRect(wMin, wMax, coreColor, 4.0f, 0, 1.0f);
            }

            // ═══════════════════════════════════════════════════════════
            // STEAM-LIKE BAR (same stats as horizontal: FPS, CPU+temp, GPU+temp, VRAM/RAM, process line)
            // ═══════════════════════════════════════════════════════════
            if (g_Config.layoutStyle == LAYOUT_GAMEPP) {
                const float gpSc = GameppPresetScale(g_Config.gameppFontPreset) * ovSc;
                ImGui::SetWindowFontScale(gpSc);
                const float rowH = 36.f * gpSc;
                const float gap = 2.f * gpSc;
                const float padX = 12.f * gpSc;
                const ImVec4 labelC(.62f, .64f, .68f, 1.f);
                const ImVec4 greenC(.20f, .90f, .22f, 1.f);
                const ImVec4 redC(.92f, .32f, .26f, 1.f);
                const ImVec4 yellowC(1.00f, .90f, .10f, 1.f);
                const ImVec4 blueC(.25f, .72f, 1.00f, 1.f);
                const ImU32 bgA = IM_COL32(0, 0, 0, (int)(235 * overlayBgAlpha));
                ImDrawList* dl = ImGui::GetWindowDrawList();

                struct GameppVal {
                    const char* text;
                    ImVec4 col;
                };
                auto calcSegWidth = [&](const char* label, const GameppVal* vals, int count) {
                    float w = padX * 2.f + ImGui::CalcTextSize(label).x + 9.f * gpSc;
                    for (int i = 0; i < count; ++i)
                        w += ImGui::CalcTextSize(vals[i].text).x + 10.f * gpSc;
                    return w + 2.f * gpSc;
                };

                auto drawSeg = [&](const char* id, const char* label, const GameppVal* vals, int count) {
                    if (count <= 0) return;
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    float width = calcSegWidth(label, vals, count);
                    ImGui::PushID(id);
                    ImGui::Dummy(ImVec2(width, rowH));
                    ImGui::PopID();
                    ImVec2 q(p.x + width, p.y + rowH);
                    dl->AddRectFilled(p, q, bgA, 0.f);
                    float x = p.x + padX;
                    const float y = p.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
                    ImFont* font = ImGui::GetFont();
                    const float fs = ImGui::GetFontSize();
                    dl->AddText(font, fs, ImVec2(x, y), ImColor(labelC), label);
                    x += ImGui::CalcTextSize(label).x + 9.f * gpSc;
                    for (int i = 0; i < count; ++i) {
                        dl->AddText(font, fs, ImVec2(x, y), ImColor(vals[i].col), vals[i].text);
                        x += ImGui::CalcTextSize(vals[i].text).x + 10.f * gpSc;
                    }
                    ImGui::SameLine(0, gap);
                };

                char fpsBuf[32] = "---";
                if (g_etwAvailable && displayFps > 0)
                    snprintf(fpsBuf, sizeof(fpsBuf), "%.0f", displayFps);
                if (g_Config.showFPS) {
                    const ImVec4 fpsColor = g_Config.customFpsColors && displayFps > 0.0f
                        ? FpsTierColor(displayFps) : greenC;
                    GameppVal vals[] = {{fpsBuf, fpsColor}};
                    drawSeg("##gamepp_fps", "FPS", vals, 1);
                }

                char cpuUsageBuf[32] = "";
                char cpuTempBuf[32] = "";
                char cpuFreqBuf[32] = "";
                char cpuPowerBuf[32] = "";
                char cpuFanBuf[32] = "";
                GameppVal cpuVals[5];
                int cpuN = 0;
                if (g_Config.showCpuUsage) {
                    snprintf(cpuUsageBuf, sizeof(cpuUsageBuf), "%.0f%%", cpuUsage);
                    cpuVals[cpuN++] = {cpuUsageBuf, ColorByLoad(cpuUsage, 65.f, 85.f)};
                }
                if (g_Config.showCpuTemp) {
                    if (g_cpuTempAvailable && g_cpuTemp > 0) {
                        float dispTemp = ToDisplayTemp(g_cpuTemp, g_Config.useFahrenheit);
                        snprintf(cpuTempBuf, sizeof(cpuTempBuf), "%.0f°%s", dispTemp, g_Config.useFahrenheit ? "F" : "C");
                    } else {
                        snprintf(cpuTempBuf, sizeof(cpuTempBuf), "---");
                    }
                    cpuVals[cpuN++] = {cpuTempBuf, yellowC};
                }
                if (g_Config.showCpuFreq) {
                    if (g_cpuClockMHz > 0)
                        snprintf(cpuFreqBuf, sizeof(cpuFreqBuf), "%.0fMHz", g_cpuClockMHz);
                    else
                        snprintf(cpuFreqBuf, sizeof(cpuFreqBuf), "---MHz");
                    cpuVals[cpuN++] = {cpuFreqBuf, blueC};
                }
                if (g_Config.showCpuPower) {
                    if (g_cpuPower > 0.f) snprintf(cpuPowerBuf, sizeof(cpuPowerBuf), "%.0fW", g_cpuPower);
                    else snprintf(cpuPowerBuf, sizeof(cpuPowerBuf), "---W");
                    cpuVals[cpuN++] = {cpuPowerBuf, ImVec4(.95f, .76f, .36f, 1.f)};
                }
                if (g_Config.showCpuFan) {
                    FormatFanValue(cpuFanBuf, sizeof(cpuFanBuf), g_cpuFanRpm, g_lhwmCpuFanIsPercent);
                    cpuVals[cpuN++] = {cpuFanBuf, ImVec4(.48f, .82f, .95f, 1.f)};
                }
                drawSeg("##gamepp_cpu", "CPU", cpuVals, cpuN);

                char gpuUsageBuf[32] = "";
                char gpuTempBuf[32] = "";
                char gpuMemTempBuf[32] = "";
                char gpuFreqBuf[32] = "";
                char gpuPowerBuf[32] = "";
                char gpuFanBuf[32] = "";
                GameppVal gpuVals[6];
                int gpuN = 0;
                if (g_lhwmAvailable && g_gpuCount > 0) {
                    if (g_Config.showGpuUsage) {
                        snprintf(gpuUsageBuf, sizeof(gpuUsageBuf), "%.0f%%", g_gpuUsage);
                        gpuVals[gpuN++] = {gpuUsageBuf, ColorByLoad(g_gpuUsage, 65.f, 85.f)};
                    }
                    if (g_Config.showGpuTemp) {
                        if (g_gpuTemp > 0) {
                            float dispTemp = ToDisplayTemp(g_gpuTemp, g_Config.useFahrenheit);
                            snprintf(gpuTempBuf, sizeof(gpuTempBuf), "%.0f°%s", dispTemp, g_Config.useFahrenheit ? "F" : "C");
                        } else {
                            snprintf(gpuTempBuf, sizeof(gpuTempBuf), "---");
                        }
                        gpuVals[gpuN++] = {gpuTempBuf, yellowC};
                    }
                    if (g_Config.showGpuMemTemp) {
                        FormatTempCompact(gpuMemTempBuf, sizeof(gpuMemTempBuf), g_gpuMemTemp, g_Config.useFahrenheit);
                        gpuVals[gpuN++] = {gpuMemTempBuf, ImVec4(1.00f, .62f, .35f, 1.f)};
                    }
                    if (g_Config.showGpuCoreFreq) {
                        if (g_gpuCoreClockMHz > 0)
                            snprintf(gpuFreqBuf, sizeof(gpuFreqBuf), "%.0fMHz", g_gpuCoreClockMHz);
                        else
                            snprintf(gpuFreqBuf, sizeof(gpuFreqBuf), "---MHz");
                        gpuVals[gpuN++] = {gpuFreqBuf, blueC};
                    }
                    if (g_Config.showGpuPower) {
                        if (g_gpuPower > 0.f) snprintf(gpuPowerBuf, sizeof(gpuPowerBuf), "%.0fW", g_gpuPower);
                        else snprintf(gpuPowerBuf, sizeof(gpuPowerBuf), "---W");
                        gpuVals[gpuN++] = {gpuPowerBuf, ImVec4(.95f, .76f, .36f, 1.f)};
                    }
                    if (g_Config.showGpuFan) {
                        FormatFanValue(gpuFanBuf, sizeof(gpuFanBuf), g_gpuFanRpm, g_lhwmGpuFanIsPercent);
                        gpuVals[gpuN++] = {gpuFanBuf, ImVec4(.48f, .82f, .95f, 1.f)};
                    }
                } else if (g_Config.showGpuUsage || g_Config.showGpuTemp) {
                    gpuVals[gpuN++] = {"N/A", labelC};
                }
                drawSeg("##gamepp_gpu", "GPU", gpuVals, gpuN);

                char systemFanBuf[32] = "";
                if (g_Config.showSystemFan) {
                    FormatFanValue(systemFanBuf, sizeof(systemFanBuf), g_systemFanRpm, false);
                    GameppVal vals[] = {{systemFanBuf, ImVec4(.48f, .82f, .95f, 1.f)}};
                    drawSeg("##gamepp_sysfan", "系统风扇", vals, 1);
                }

                char vramPctBuf[32] = "";
                char vramUsedBuf[48] = "";
                if (g_Config.showVRAMPercent || g_Config.showVRAMUsage) {
                    GameppVal vals[2];
                    int n = 0;
                    if (g_vramTotal > 0.f) {
                        float vramPct = (g_vramUsed / g_vramTotal) * 100.0f;
                        snprintf(vramPctBuf, sizeof(vramPctBuf), "%.0f%%", vramPct);
                        snprintf(vramUsedBuf, sizeof(vramUsedBuf), "%.1f/%.0fG", g_vramUsed, g_vramTotal);
                        if (g_Config.showVRAMPercent)
                            vals[n++] = {vramPctBuf, ColorByLoad(vramPct)};
                        if (g_Config.showVRAMUsage)
                            vals[n++] = {vramUsedBuf, labelC};
                    } else {
                        vals[n++] = {"---", labelC};
                    }
                    drawSeg("##gamepp_vram", "显存", vals, n);
                }

                char ramPctBuf[32] = "";
                char ramUsedBuf[48] = "";
                char memTempBuf[32] = "";
                if (g_Config.showRAMPercent || g_Config.showRAMUsage) {
                    float pct = ramTotal > 0.f ? (ramUsed / ramTotal) * 100.0f : 0.f;
                    snprintf(ramPctBuf, sizeof(ramPctBuf), "%.0f%%", pct);
                    snprintf(ramUsedBuf, sizeof(ramUsedBuf), "%.1f/%.0fG", ramUsed, ramTotal);
                    GameppVal vals[3];
                    int n = 0;
                    if (g_Config.showRAMPercent)
                        vals[n++] = {ramPctBuf, ColorByLoad(pct)};
                    if (g_Config.showRAMUsage)
                        vals[n++] = {ramUsedBuf, labelC};
                    if (g_Config.showMemoryTemp) {
                        FormatTempCompact(memTempBuf, sizeof(memTempBuf), g_memoryTemp, g_Config.useFahrenheit);
                        vals[n++] = {memTempBuf, yellowC};
                    }
                    drawSeg("##gamepp_ram", "内存", vals, n);
                }

                char networkRateBuf[48] = "";
                if (g_Config.showNetworkDownload) {
                    FormatNetworkRate(networkRateBuf, sizeof(networkRateBuf), g_networkDownloadBps);
                    GameppVal vals[] = {{networkRateBuf, blueC}};
                    drawSeg("##gamepp_net_down", "下载", vals, 1);
                }
                if (g_Config.showNetworkUpload) {
                    FormatNetworkRate(networkRateBuf, sizeof(networkRateBuf), g_networkUploadBps);
                    GameppVal vals[] = {{networkRateBuf, greenC}};
                    drawSeg("##gamepp_net_up", "上传", vals, 1);
                }

                char diskTempBuf[32] = "";
                if (g_Config.showDiskTemp) {
                    FormatTempCompact(diskTempBuf, sizeof(diskTempBuf), g_diskTemp, g_Config.useFahrenheit);
                    GameppVal vals[] = {{diskTempBuf, yellowC}};
                    drawSeg("##gamepp_disk", "SSD", vals, 1);
                }

                InlineOverlayMetric featureMetrics[8] = {};
                const size_t featureMetricCount =
                    g_FeatureRegistry.GetInlineOverlayMetrics(
                        g_FeatureContext, featureMetrics, IM_ARRAYSIZE(featureMetrics));
                for (size_t i = 0; i < featureMetricCount; ++i) {
                    ImVec4 valueColor = labelC;
                    switch (featureMetrics[i].tone) {
                    case InlineMetricTone::Good:    valueColor = greenC; break;
                    case InlineMetricTone::Warning: valueColor = yellowC; break;
                    case InlineMetricTone::Info:    valueColor = blueC; break;
                    case InlineMetricTone::Neutral: valueColor = labelC; break;
                    }
                    char metricId[48] = {};
                    snprintf(metricId, sizeof(metricId), "##gamepp_feature_%zu", i);
                    GameppVal vals[] = {{featureMetrics[i].value, valueColor}};
                    drawSeg(metricId, featureMetrics[i].label, vals, 1);
                }

                char timeBuf[32] = "";
                if (g_Config.showTime) {
                    FormatOverlayTime(g_Config, timeBuf, sizeof(timeBuf));
                    GameppVal vals[] = {{timeBuf, ImVec4(.55f, .78f, 1.f, 1.f)}};
                    drawSeg("##gamepp_time", "时间", vals, 1);
                }
                if (g_Config.showFPS && g_Config.showProcessName && g_targetProcessName[0]) {
                    GameppVal vals[] = {{g_targetProcessName, ImVec4(.55f, .78f, .55f, 1.f)}};
                    drawSeg("##gamepp_proc", "程序", vals, 1);
                }

                ImGui::SetWindowFontScale(1.0f);
            }
            else if (g_Config.layoutStyle == LAYOUT_STEAM) {
                const float ss = ovSc;
                const float hs = 4.f * ss;
                const float hsTight = 3.f * ss;

                const ImVec4 labFps(0.92f, 0.52f, 0.58f, 1.f);  // muted pink-red (Steam-style label)
                const ImVec4 labCpu(0.94f, 0.88f, 0.58f, 1.f);  // pale yellow
                const ImVec4 labGpu(0.52f, 0.90f, 0.70f, 1.f);  // mint green
                const ImVec4 sepC(0.55f, 0.55f, 0.58f, 1.f);

                const bool showProcLine = g_Config.showFPS && g_Config.showProcessName && g_targetProcessName[0];
                const bool showTimeLine = g_Config.showTime;
                const bool showMetaLine = showProcLine || showTimeLine;
                const float lineH = ImGui::GetTextLineHeightWithSpacing();

                ImGui::SetWindowFontScale(1.0f * ss);

                // Full-row hit target so CTRL+drag works on labels/values (not only padding)
                ImVec2 steamRowStart = ImGui::GetCursorPos();
                if (ctrlHeld) {
                    float rowH = lineH * (showMetaLine ? 2.45f : 1.55f);
                    float rowW = ImGui::GetContentRegionAvail().x;
                    if (rowW < 32.f)
                        rowW = ImGui::GetWindowWidth() - ImGui::GetCursorPos().x - ImGui::GetStyle().WindowPadding.x;
                    ImGui::InvisibleButton("##SteamBarDrag", ImVec2(rowW, rowH));
                    ImGuiWindow* wbar = ImGui::GetCurrentWindow();
                    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0))
                        ImGui::StartMouseMovingWindow(wbar);
                    ImGui::SetCursorPos(steamRowStart);
                }

                bool needSep = false;

                if (g_Config.showFPS) {
                    ImGui::TextColored(labFps, "FPS");
                    ImGui::SameLine(0, hsTight);
                    if (g_etwAvailable && displayFps > 0)
                        ImGui::TextColored(FpsTierColor(displayFps), "%.0f", displayFps);
                    else
                        ImGui::TextColored(ImVec4(.50f, .50f, .55f, 1.f), "---");
                    needSep = true;
                }

                const bool wantCpuHzSt  = g_Config.showCpuFreq;
                const bool wantCpuPwrSt = g_Config.showCpuPower && g_lhwmAvailable && !g_lhwmCpuPowerPath.empty();
                const bool wantCpuFanSt = g_Config.showCpuFan;
                if (g_Config.showCpuUsage || g_Config.showCpuTemp || wantCpuHzSt || wantCpuPwrSt || wantCpuFanSt) {
                    if (needSep) {
                        ImGui::SameLine(0, hs);
                        ImGui::TextColored(sepC, "|");
                        ImGui::SameLine(0, hs);
                    }
                    ImGui::TextColored(labCpu, "CPU");
                    ImGui::SameLine(0, hsTight);
                    bool anyCpuSteam = false;
                    if (g_Config.showCpuUsage) {
                        ImGui::TextColored(ColorByLoad(cpuUsage), "%.0f%%", cpuUsage);
                        anyCpuSteam = true;
                    }
                    if (g_Config.showCpuTemp) {
                        if (g_cpuTempAvailable && g_cpuTemp > 0) {
                            if (anyCpuSteam) ImGui::SameLine(0, 2.f * ss);
                            float dispTemp = ToDisplayTemp(g_cpuTemp, g_Config.useFahrenheit);
                            ImVec4 tc = g_cpuTemp > 85 ? ImVec4(1, .3f, .3f, 1)
                                      : g_cpuTemp > 70 ? ImVec4(1, .85f, .15f, 1)
                                                       : ImVec4(.70f, .70f, .75f, 1);
                            ImGui::TextColored(tc, "%s%.0f\xC2\xB0%s", anyCpuSteam ? " " : "", dispTemp,
                                               g_Config.useFahrenheit ? "F" : "C");
                            anyCpuSteam = true;
                        } else if (!g_Config.showCpuUsage) {
                            ImGui::TextColored(ImVec4(.50f, .50f, .55f, 1.f), "---");
                            anyCpuSteam = true;
                        }
                    }
                    if (wantCpuHzSt) {
                        if (anyCpuSteam) {
                            ImGui::SameLine(0, hsTight);
                            ImGui::TextColored(sepC, "|");
                            ImGui::SameLine(0, hsTight);
                        }
                        InlineFreqSparkMHz("##st_cpu", g_cpuSpark, g_cpuSparkN, g_cpuClockMHz,
                                           ImVec2(38.f * ss, 11.f * ss), 5.f * ss, ImVec4(.75f, .75f, .78f, 1.f));
                    }
                    if (wantCpuPwrSt && g_cpuPower > 0.f) {
                        if (anyCpuSteam) {
                            ImGui::SameLine(0, hsTight);
                            ImGui::TextColored(sepC, "|");
                            ImGui::SameLine(0, hsTight);
                        }
                        ImGui::TextColored(ImVec4(.85f, .78f, .55f, 1), "%.0fW", g_cpuPower);
                    }
                    if (wantCpuFanSt) {
                        if (anyCpuSteam) {
                            ImGui::SameLine(0, hsTight);
                            ImGui::TextColored(sepC, "|");
                            ImGui::SameLine(0, hsTight);
                        }
                        char fanBuf[32];
                        FormatFanValue(fanBuf, sizeof(fanBuf), g_cpuFanRpm, g_lhwmCpuFanIsPercent);
                        ImGui::TextColored(ImVec4(.60f, .80f, .90f, 1), "%s", fanBuf);
                    }
                    needSep = true;
                }

                const bool wantGpuHzSt  = g_Config.showGpuCoreFreq;
                const bool wantGpuPwrSt = g_Config.showGpuPower && g_lhwmAvailable && !g_lhwmGpuPowerPath.empty();
                const bool wantGpuFanSt = g_Config.showGpuFan;
                if (g_Config.showGpuUsage || g_Config.showGpuTemp || g_Config.showGpuMemTemp || wantGpuHzSt || wantGpuPwrSt || wantGpuFanSt) {
                    if (needSep) {
                        ImGui::SameLine(0, hs);
                        ImGui::TextColored(sepC, "|");
                        ImGui::SameLine(0, hs);
                    }
                    float dispGpuLoad = g_gpuUsage;
                    float dispGpuTemp = g_gpuTemp;
                    bool hasGpuData = (g_lhwmAvailable && g_gpuCount > 0) || g_gpuCoreClockMHz > 0.f || g_gpuFanRpm > 0.f || g_gpuMemTemp > 0.f;
                    ImGui::TextColored(labGpu, "GPU");
                    ImGui::SameLine(0, hsTight);
                    if (hasGpuData) {
                        bool anyGpuSteam = false;
                        if (g_Config.showGpuUsage) {
                            ImGui::TextColored(ColorByLoad(dispGpuLoad), "%.0f%%", dispGpuLoad);
                            anyGpuSteam = true;
                        }
                        if (g_Config.showGpuTemp) {
                            if (dispGpuTemp > 0) {
                                if (anyGpuSteam) ImGui::SameLine(0, 2.f * ss);
                                float dispTemp = ToDisplayTemp(dispGpuTemp, g_Config.useFahrenheit);
                                ImVec4 tc = dispGpuTemp > 85 ? ImVec4(1, .3f, .3f, 1)
                                          : dispGpuTemp > 70 ? ImVec4(1, .85f, .15f, 1)
                                                             : ImVec4(.70f, .70f, .75f, 1);
                                ImGui::TextColored(tc, "%s%.0f\xC2\xB0%s", anyGpuSteam ? " " : "", dispTemp,
                                                   g_Config.useFahrenheit ? "F" : "C");
                                anyGpuSteam = true;
                            } else if (!g_Config.showGpuUsage) {
                                ImGui::TextColored(ImVec4(.50f, .50f, .55f, 1.f), "---");
                                anyGpuSteam = true;
                            }
                        }
                        if (wantGpuHzSt) {
                            if (anyGpuSteam) {
                                ImGui::SameLine(0, hsTight);
                                ImGui::TextColored(sepC, "|");
                                ImGui::SameLine(0, hsTight);
                            }
                            InlineFreqSparkMHz("##st_gpu", g_gpuSpark, g_gpuSparkN, g_gpuCoreClockMHz,
                                               ImVec2(38.f * ss, 11.f * ss), 5.f * ss, ImVec4(.75f, .75f, .78f, 1.f));
                        }
                        if (wantGpuPwrSt && g_gpuPower > 0.f) {
                            if (anyGpuSteam) {
                                ImGui::SameLine(0, hsTight);
                                ImGui::TextColored(sepC, "|");
                                ImGui::SameLine(0, hsTight);
                            }
                            ImGui::TextColored(ImVec4(.85f, .78f, .55f, 1), "%.0fW", g_gpuPower);
                        }
                        if (g_Config.showGpuMemTemp) {
                            if (anyGpuSteam) {
                                ImGui::SameLine(0, hsTight);
                                ImGui::TextColored(sepC, "|");
                                ImGui::SameLine(0, hsTight);
                            }
                            char tempBuf[32];
                            FormatTempCompact(tempBuf, sizeof(tempBuf), g_gpuMemTemp, g_Config.useFahrenheit);
                            ImGui::TextColored(ImVec4(1.00f, .62f, .35f, 1), "显存温度 %s", tempBuf);
                            anyGpuSteam = true;
                        }
                        if (wantGpuFanSt) {
                            if (anyGpuSteam) {
                                ImGui::SameLine(0, hsTight);
                                ImGui::TextColored(sepC, "|");
                                ImGui::SameLine(0, hsTight);
                            }
                            char fanBuf[32];
                            FormatFanValue(fanBuf, sizeof(fanBuf), g_gpuFanRpm, g_lhwmGpuFanIsPercent);
                            ImGui::TextColored(ImVec4(.60f, .80f, .90f, 1), "%s", fanBuf);
                        }
                    } else {
                        ImGui::TextColored(ImVec4(.50f, .50f, .55f, 1.f), "N/A");
                    }
                    needSep = true;
                }

                if (g_Config.showSystemFan) {
                    if (needSep) {
                        ImGui::SameLine(0, hs);
                        ImGui::TextColored(sepC, "|");
                        ImGui::SameLine(0, hs);
                    }
                    char fanBuf[32];
                    FormatFanValue(fanBuf, sizeof(fanBuf), g_systemFanRpm, false);
                    ImGui::TextColored(ImVec4(.60f, .80f, .90f, 1), "系统风扇 %s", fanBuf);
                    needSep = true;
                }

                if (g_Config.showVRAMPercent || g_Config.showVRAMUsage) {
                    float dispVramUsed = g_vramUsed;
                    float dispVramTotal = g_vramTotal;
                    if (dispVramTotal > 0.f) {
                        if (needSep) {
                            ImGui::SameLine(0, hs);
                            ImGui::TextColored(sepC, "|");
                            ImGui::SameLine(0, hs);
                        }
                        float vramPct = (dispVramUsed / dispVramTotal) * 100.0f;
                        if (g_Config.showVRAMPercent && g_Config.showVRAMUsage)
                            ImGui::TextColored(ColorByLoad(vramPct), "显存 %.0f%% %.1f/%.0fG", vramPct, dispVramUsed, dispVramTotal);
                        else if (g_Config.showVRAMPercent)
                            ImGui::TextColored(ColorByLoad(vramPct), "显存 %.0f%%", vramPct);
                        else
                            ImGui::TextColored(ImVec4(.70f, .70f, .75f, 1), "显存 %.1f/%.0fG", dispVramUsed, dispVramTotal);
                        needSep = true;
                    }
                }

                if (g_Config.showRAMPercent || g_Config.showRAMUsage) {
                    if (needSep) {
                        ImGui::SameLine(0, hs);
                        ImGui::TextColored(sepC, "|");
                        ImGui::SameLine(0, hs);
                    }
                    float pct = (ramUsed / ramTotal) * 100;
                    if (g_Config.showRAMPercent && g_Config.showRAMUsage)
                        ImGui::TextColored(ColorByLoad(pct), "内存 %.0f%% %.1f/%.0fG", pct, ramUsed, ramTotal);
                    else if (g_Config.showRAMPercent)
                        ImGui::TextColored(ColorByLoad(pct), "内存 %.0f%%", pct);
                    else
                        ImGui::TextColored(ImVec4(.70f, .70f, .75f, 1), "内存 %.1f/%.0fG", ramUsed, ramTotal);
                    if (g_Config.showMemoryTemp) {
                        char tempBuf[32];
                        FormatTempCompact(tempBuf, sizeof(tempBuf), g_memoryTemp, g_Config.useFahrenheit);
                        ImGui::SameLine(0, hsTight);
                        ImGui::TextColored(ImVec4(.85f, .78f, .55f, 1), "%s", tempBuf);
                    }
                    needSep = true;
                }

                if (g_Config.showNetworkDownload || g_Config.showNetworkUpload) {
                    if (needSep) {
                        ImGui::SameLine(0, hs);
                        ImGui::TextColored(sepC, "|");
                        ImGui::SameLine(0, hs);
                    }
                    char rateBuf[48];
                    bool wroteNetwork = false;
                    if (g_Config.showNetworkDownload) {
                        FormatNetworkRate(rateBuf, sizeof(rateBuf), g_networkDownloadBps);
                        ImGui::TextColored(ImVec4(.48f, .78f, 1.f, 1.f), "下载 %s", rateBuf);
                        wroteNetwork = true;
                    }
                    if (g_Config.showNetworkUpload) {
                        if (wroteNetwork) ImGui::SameLine(0, hsTight);
                        FormatNetworkRate(rateBuf, sizeof(rateBuf), g_networkUploadBps);
                        ImGui::TextColored(ImVec4(.52f, .90f, .70f, 1.f), "上传 %s", rateBuf);
                    }
                    needSep = true;
                }

                if (g_Config.showDiskTemp) {
                    if (needSep) {
                        ImGui::SameLine(0, hs);
                        ImGui::TextColored(sepC, "|");
                        ImGui::SameLine(0, hs);
                    }
                    char tempBuf[32];
                    FormatTempCompact(tempBuf, sizeof(tempBuf), g_diskTemp, g_Config.useFahrenheit);
                    ImGui::TextColored(ImVec4(.85f, .78f, .55f, 1), "SSD %s", tempBuf);
                    needSep = true;
                }

                if (showMetaLine) {
                    char timeBuf[32] = {};
                    if (showTimeLine)
                        FormatOverlayTime(g_Config, timeBuf, sizeof(timeBuf));
                    ImGui::SetWindowFontScale(0.78f * ss);
                    if (showProcLine)
                        ImGui::TextColored(ImVec4(.42f, .52f, .42f, 1.f), "%s", g_targetProcessName);
                    if (showTimeLine) {
                        if (showProcLine) ImGui::SameLine(0, 10.f * ss);
                        ImGui::TextColored(ImVec4(.55f, .65f, .78f, 1.f), "%s", timeBuf);
                    }
                }

                ImGui::SetWindowFontScale(1.0f);
            }
            // ═══════════════════════════════════════════════════════════
            // HORIZONTAL COMPACT VIEW
            // ═══════════════════════════════════════════════════════════
            else if (g_Config.layoutStyle == LAYOUT_HORIZONTAL) {
                ImGui::SetWindowFontScale(ovSc);
                bool needSep = false;
                
                // FPS
                if (g_Config.showFPS) {
                    if (g_etwAvailable && displayFps > 0) {
                        ImGui::TextColored(FpsTierColor(displayFps),
                                           "FPS %.0f", displayFps);
                    } else {
                        ImGui::TextColored(ImVec4(.50f,.50f,.55f,1), "FPS ---");
                    }
                    needSep = true;
                }

                // CPU
                {
                    const bool wantCpuHzHz =
                        g_Config.showCpuFreq;
                    const bool wantCpuPwrHz =
                        g_Config.showCpuPower && g_lhwmAvailable && !g_lhwmCpuPowerPath.empty();
                    const bool wantCpuFanHz =
                        g_Config.showCpuFan;
                    if (g_Config.showCpuUsage || g_Config.showCpuTemp || wantCpuHzHz || wantCpuPwrHz || wantCpuFanHz) {
                        if (needSep) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(.35f, .35f, .40f, 1), " | ");
                            ImGui::SameLine();
                        }
                        const bool hasCpuTempVal = g_cpuTempAvailable && g_cpuTemp > 0;
                        if (g_Config.showCpuUsage)
                            ImGui::TextColored(ColorByLoad(cpuUsage), "CPU %.0f%%", cpuUsage);
                        else if (g_Config.showCpuTemp || wantCpuHzHz || wantCpuPwrHz || wantCpuFanHz)
                            ImGui::TextColored(ImVec4(.78f, .78f, .82f, 1), "CPU");

                        if (g_Config.showCpuTemp && hasCpuTempVal) {
                            float dispTemp = ToDisplayTemp(g_cpuTemp, g_Config.useFahrenheit);
                            ImVec4 tc = g_cpuTemp > 85 ? ImVec4(1, .3f, .3f, 1)
                                      : g_cpuTemp > 70 ? ImVec4(1, .85f, .15f, 1)
                                                       : ImVec4(.70f, .70f, .75f, 1);
                            if (g_Config.showCpuUsage) ImGui::SameLine(0, 2);
                            else ImGui::SameLine(0, 4);
                            ImGui::TextColored(tc, "%.0f\xC2\xB0%s", dispTemp,
                                               g_Config.useFahrenheit ? "F" : "C");
                        } else if (g_Config.showCpuTemp && !g_Config.showCpuUsage) {
                            ImGui::SameLine(0, 2);
                            ImGui::TextColored(ImVec4(.50f, .50f, .55f, 1), "---");
                        }

                        if (wantCpuHzHz) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(.35f, .35f, .40f, 1), " | ");
                            ImGui::SameLine();
                            InlineFreqSparkMHz("##hz_cpu", g_cpuSpark, g_cpuSparkN, g_cpuClockMHz,
                                               ImVec2(52.f * ovSc, 12.f * ovSc), 6.f * ovSc,
                                               ImVec4(.62f, .62f, .68f, 1.f));
                        }
                        if (wantCpuPwrHz && g_cpuPower > 0.f) {
                            ImGui::SameLine(0, 2);
                            ImGui::TextColored(ImVec4(.85f, .78f, .55f, 1), " %.0fW", g_cpuPower);
                        }
                        if (wantCpuFanHz) {
                            ImGui::SameLine(0, 2);
                            char fanBuf[32];
                            FormatFanValue(fanBuf, sizeof(fanBuf), g_cpuFanRpm, g_lhwmCpuFanIsPercent);
                            ImGui::TextColored(ImVec4(.60f, .80f, .90f, 1), " %s", fanBuf);
                        }
                        needSep = true;
                    }
                }

                // GPU stats via LHWM
                {
                    const bool wantGpuHzHz =
                        g_Config.showGpuCoreFreq;
                    const bool wantGpuPwrHz =
                        g_Config.showGpuPower && g_lhwmAvailable && !g_lhwmGpuPowerPath.empty();
                    const bool wantGpuFanHz =
                        g_Config.showGpuFan;
                    if (g_Config.showGpuUsage || g_Config.showGpuTemp || g_Config.showGpuMemTemp || wantGpuHzHz || wantGpuPwrHz || wantGpuFanHz) {
                        if (needSep) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(.35f, .35f, .40f, 1), " | ");
                            ImGui::SameLine();
                        }

                        float dispGpuLoad = g_gpuUsage;
                        float dispGpuTemp = g_gpuTemp;
                        bool hasGpuData = (g_lhwmAvailable && g_gpuCount > 0) || g_gpuCoreClockMHz > 0.f || g_gpuFanRpm > 0.f || g_gpuMemTemp > 0.f;

                        if (hasGpuData) {
                            if (g_Config.showGpuUsage)
                                ImGui::TextColored(ColorByLoad(dispGpuLoad), "GPU %.0f%%", dispGpuLoad);
                            else if (g_Config.showGpuTemp || wantGpuHzHz || wantGpuPwrHz || wantGpuFanHz)
                                ImGui::TextColored(ImVec4(.78f, .78f, .82f, 1), "GPU");

                            if (g_Config.showGpuTemp && dispGpuTemp > 0) {
                                float dispTemp = ToDisplayTemp(dispGpuTemp, g_Config.useFahrenheit);
                                ImVec4 tc = dispGpuTemp > 85 ? ImVec4(1, .3f, .3f, 1)
                                          : dispGpuTemp > 70 ? ImVec4(1, .85f, .15f, 1)
                                                             : ImVec4(.70f, .70f, .75f, 1);
                                if (g_Config.showGpuUsage) ImGui::SameLine(0, 2);
                                else ImGui::SameLine(0, 4);
                                ImGui::TextColored(tc, "%.0f\xC2\xB0%s", dispTemp,
                                                   g_Config.useFahrenheit ? "F" : "C");
                            } else if (g_Config.showGpuTemp && !g_Config.showGpuUsage) {
                                ImGui::SameLine(0, 2);
                                ImGui::TextColored(ImVec4(.50f, .50f, .55f, 1), "---");
                            }

                            if (wantGpuHzHz) {
                                ImGui::SameLine();
                                ImGui::TextColored(ImVec4(.35f, .35f, .40f, 1), " | ");
                                ImGui::SameLine();
                                InlineFreqSparkMHz("##hz_gpu", g_gpuSpark, g_gpuSparkN, g_gpuCoreClockMHz,
                                                   ImVec2(52.f * ovSc, 12.f * ovSc), 6.f * ovSc,
                                                   ImVec4(.62f, .62f, .68f, 1.f));
                            }
                            if (wantGpuPwrHz && g_gpuPower > 0.f) {
                                ImGui::SameLine(0, 2);
                                ImGui::TextColored(ImVec4(.85f, .78f, .55f, 1), " %.0fW", g_gpuPower);
                            }
                            if (g_Config.showGpuMemTemp) {
                                char tempBuf[32];
                                FormatTempCompact(tempBuf, sizeof(tempBuf), g_gpuMemTemp, g_Config.useFahrenheit);
                                ImGui::SameLine(0, 2);
                                ImGui::TextColored(ImVec4(1.00f, .62f, .35f, 1), " 显存温度 %s", tempBuf);
                            }
                            if (wantGpuFanHz) {
                                ImGui::SameLine(0, 2);
                                char fanBuf[32];
                                FormatFanValue(fanBuf, sizeof(fanBuf), g_gpuFanRpm, g_lhwmGpuFanIsPercent);
                                ImGui::TextColored(ImVec4(.60f, .80f, .90f, 1), " %s", fanBuf);
                            }
                        } else {
                            ImGui::TextColored(ImVec4(.50f, .50f, .55f, 1), "GPU N/A");
                        }
                        needSep = true;
                    }
                }

                // VRAM
                if (g_Config.showSystemFan) {
                    if (needSep) { ImGui::SameLine(); ImGui::TextColored(ImVec4(.35f,.35f,.40f,1), " | "); ImGui::SameLine(); }
                    char fanBuf[32];
                    FormatFanValue(fanBuf, sizeof(fanBuf), g_systemFanRpm, false);
                    ImGui::TextColored(ImVec4(.60f, .80f, .90f, 1), "系统风扇 %s", fanBuf);
                    needSep = true;
                }

                // VRAM
                if (g_Config.showVRAMPercent || g_Config.showVRAMUsage) {
                    float dispVramUsed = g_vramUsed;
                    float dispVramTotal = g_vramTotal;
                    if (dispVramTotal > 0) {
                        if (needSep) { ImGui::SameLine(); ImGui::TextColored(ImVec4(.35f,.35f,.40f,1), " | "); ImGui::SameLine(); }
                        float vramPct = (dispVramUsed / dispVramTotal) * 100.0f;
                        if (g_Config.showVRAMPercent && g_Config.showVRAMUsage)
                            ImGui::TextColored(ColorByLoad(vramPct), "显存 %.0f%% %.1f/%.0fG", vramPct, dispVramUsed, dispVramTotal);
                        else if (g_Config.showVRAMPercent)
                            ImGui::TextColored(ColorByLoad(vramPct), "显存 %.0f%%", vramPct);
                        else
                            ImGui::TextColored(ImVec4(.70f, .70f, .75f, 1), "显存 %.1f/%.0fG", dispVramUsed, dispVramTotal);
                        needSep = true;
                    }
                }
                
                // RAM
                if (g_Config.showRAMPercent || g_Config.showRAMUsage) {
                    if (needSep) { ImGui::SameLine(); ImGui::TextColored(ImVec4(.35f,.35f,.40f,1), " | "); ImGui::SameLine(); }
                    float pct = (ramUsed / ramTotal) * 100;
                    if (g_Config.showRAMPercent && g_Config.showRAMUsage)
                        ImGui::TextColored(ColorByLoad(pct), "内存 %.0f%% %.1f/%.0fG", pct, ramUsed, ramTotal);
                    else if (g_Config.showRAMPercent)
                        ImGui::TextColored(ColorByLoad(pct), "内存 %.0f%%", pct);
                    else
                        ImGui::TextColored(ImVec4(.70f, .70f, .75f, 1), "内存 %.1f/%.0fG", ramUsed, ramTotal);
                    if (g_Config.showMemoryTemp) {
                        char tempBuf[32];
                        FormatTempCompact(tempBuf, sizeof(tempBuf), g_memoryTemp, g_Config.useFahrenheit);
                        ImGui::SameLine(0, 2);
                        ImGui::TextColored(ImVec4(.85f, .78f, .55f, 1), " %s", tempBuf);
                    }
                    needSep = true;
                }

                if (g_Config.showNetworkDownload || g_Config.showNetworkUpload) {
                    if (needSep) { ImGui::SameLine(); ImGui::TextColored(ImVec4(.35f,.35f,.40f,1), " | "); ImGui::SameLine(); }
                    char rateBuf[48];
                    if (g_Config.showNetworkDownload) {
                        FormatNetworkRate(rateBuf, sizeof(rateBuf), g_networkDownloadBps);
                        ImGui::TextColored(ImVec4(.48f, .78f, 1.f, 1.f), "下载 %s", rateBuf);
                    }
                    if (g_Config.showNetworkUpload) {
                        if (g_Config.showNetworkDownload) ImGui::SameLine(0, 6.f * ovSc);
                        FormatNetworkRate(rateBuf, sizeof(rateBuf), g_networkUploadBps);
                        ImGui::TextColored(ImVec4(.52f, .90f, .70f, 1.f), "上传 %s", rateBuf);
                    }
                    needSep = true;
                }

                if (g_Config.showDiskTemp) {
                    if (needSep) { ImGui::SameLine(); ImGui::TextColored(ImVec4(.35f,.35f,.40f,1), " | "); ImGui::SameLine(); }
                    char tempBuf[32];
                    FormatTempCompact(tempBuf, sizeof(tempBuf), g_diskTemp, g_Config.useFahrenheit);
                    ImGui::TextColored(ImVec4(.85f, .78f, .55f, 1), "SSD %s", tempBuf);
                    needSep = true;
                }
                
                // Process name / time on second line (compact)
                if ((g_Config.showFPS && g_Config.showProcessName && g_targetProcessName[0]) || g_Config.showTime) {
                    char timeBuf[32] = {};
                    if (g_Config.showTime)
                        FormatOverlayTime(g_Config, timeBuf, sizeof(timeBuf));
                    ImGui::SetWindowFontScale(0.78f * ovSc);
                    if (g_Config.showFPS && g_Config.showProcessName && g_targetProcessName[0])
                        ImGui::TextColored(ImVec4(.42f,.52f,.42f,1), "%s", g_targetProcessName);
                    if (g_Config.showTime) {
                        if (g_Config.showFPS && g_Config.showProcessName && g_targetProcessName[0])
                            ImGui::SameLine(0, 10.f * ovSc);
                        ImGui::TextColored(ImVec4(.55f,.65f,.78f,1), "%s", timeBuf);
                    }
                    ImGui::SetWindowFontScale(ovSc);
                }
                ImGui::SetWindowFontScale(1.0f);
            }
            // ═══════════════════════════════════════════════════════════
            // VERTICAL VIEW (default)
            // ═══════════════════════════════════════════════════════════
            else {
                ImGui::SetWindowFontScale(ovSc);
                bool needSep = false;

                // FPS
                if (g_Config.showFPS) {
                    if (g_etwAvailable && displayFps > 0) {
                        ImGui::TextColored(FpsTierColor(displayFps),
                                           "FPS  %.0f", displayFps);
                    } else {
                        ImGui::TextColored(ImVec4(.50f,.50f,.55f,1), "FPS  ---");
                    }
                    // Show tracked process name
                    if (g_Config.showProcessName) {
                        if (g_targetProcessName[0]) {
                            ImGui::SetWindowFontScale(0.82f * ovSc);
                            ImGui::TextColored(ImVec4(.42f,.55f,.42f,1), "  %s", g_targetProcessName);
                            ImGui::SetWindowFontScale(ovSc);
                        } else {
                            ImGui::SetWindowFontScale(0.82f * ovSc);
                            ImGui::TextColored(ImVec4(.50f,.50f,.55f,1), "  （未检测到进程）");
                            ImGui::SetWindowFontScale(ovSc);
                        }
                    }
                    if (g_Config.showTime) {
                        char timeBuf[32];
                        FormatOverlayTime(g_Config, timeBuf, sizeof(timeBuf));
                        ImGui::SetWindowFontScale(0.82f * ovSc);
                        ImGui::TextColored(ImVec4(.55f,.65f,.78f,1), "  %s", timeBuf);
                        ImGui::SetWindowFontScale(ovSc);
                    }
                    needSep = true;
                } else if (g_Config.showTime) {
                    char timeBuf[32];
                    FormatOverlayTime(g_Config, timeBuf, sizeof(timeBuf));
                    ImGui::TextColored(ImVec4(.55f,.65f,.78f,1), "时间  %s", timeBuf);
                    needSep = true;
                }

                // CPU
                {
                    const bool wantCpuHzV =
                        g_Config.showCpuFreq;
                    const bool wantCpuPwrV =
                        g_Config.showCpuPower && g_lhwmAvailable && !g_lhwmCpuPowerPath.empty();
                    const bool wantCpuFanV =
                        g_Config.showCpuFan;
                    if (g_Config.showCpuUsage || g_Config.showCpuTemp || wantCpuHzV || wantCpuPwrV || wantCpuFanV) {
                        if (needSep) {
                            ImGui::Spacing();
                            ImGui::Separator();
                            ImGui::Spacing();
                        }
                        const bool hasCpuTempVal = g_cpuTempAvailable && g_cpuTemp > 0;
                        if (g_Config.showCpuUsage)
                            ImGui::TextColored(ColorByLoad(cpuUsage), "CPU  %.0f%%", cpuUsage);
                        if (g_Config.showCpuTemp && hasCpuTempVal) {
                            float dispTemp = ToDisplayTemp(g_cpuTemp, g_Config.useFahrenheit);
                            ImVec4 tc = g_cpuTemp > 85 ? ImVec4(1, .3f, .3f, 1)
                                      : g_cpuTemp > 70 ? ImVec4(1, .85f, .15f, 1)
                                                       : ImVec4(.70f, .70f, .75f, 1);
                            if (g_Config.showCpuUsage) ImGui::SameLine();
                            else ImGui::TextColored(ImVec4(.82f, .82f, .88f, 1), "CPU  ");
                            if (!g_Config.showCpuUsage) ImGui::SameLine(0, 0);
                            ImGui::TextColored(tc, "%.0f\xC2\xB0%s", dispTemp,
                                               g_Config.useFahrenheit ? "F" : "C");
                        } else if (g_Config.showCpuTemp && !g_Config.showCpuUsage) {
                            ImGui::TextColored(ImVec4(.50f, .50f, .55f, 1), "CPU  ---");
                        }

                        if (wantCpuPwrV && g_cpuPower > 0.f)
                            ImGui::TextColored(ImVec4(.85f, .78f, .55f, 1), "功耗  %.0f W", g_cpuPower);
                        if (wantCpuFanV) {
                            char fanBuf[32];
                            FormatFanValue(fanBuf, sizeof(fanBuf), g_cpuFanRpm, g_lhwmCpuFanIsPercent);
                            ImGui::TextColored(ImVec4(.60f, .80f, .90f, 1), "风扇  %s", fanBuf);
                        }
                        ImGui::SetWindowFontScale(0.82f * ovSc);
                        ImGui::TextColored(ImVec4(.42f, .42f, .48f, 1), "  %s", g_cpuName);
                        ImGui::SetWindowFontScale(ovSc);
                        if (wantCpuHzV) {
                            ImGui::Dummy(ImVec2(0, 3.f * ovSc));
                            ImGui::TextColored(ImVec4(.48f, .58f, .65f, 1), "CPU MHz");
                            ImGui::SameLine();
                            DrawMiniSpark("##vsp_cpu", g_cpuSpark, g_cpuSparkN, g_cpuClockMHz,
                                          ImVec2(130.f * ovSc, 24.f * ovSc));
                        }
                        needSep = true;
                    }
                }

                // GPU stats via LHWM
                {
                    const bool wantGpuHzV =
                        g_Config.showGpuCoreFreq;
                    const bool wantGpuPwrV =
                        g_Config.showGpuPower && g_lhwmAvailable && !g_lhwmGpuPowerPath.empty();
                    const bool wantGpuFanV =
                        g_Config.showGpuFan;
                    const bool gpuVertBlock = g_Config.showGpuUsage || g_Config.showGpuTemp || g_Config.showGpuMemTemp || wantGpuHzV ||
                                              ((g_Config.showVRAMPercent || g_Config.showVRAMUsage) && g_lhwmAvailable && g_gpuCount > 0) ||
                                              wantGpuPwrV || wantGpuFanV;
                    if (gpuVertBlock) {
                        if (needSep) {
                            ImGui::Spacing();
                            ImGui::Separator();
                            ImGui::Spacing();
                        }

                        float dispGpuLoad = g_gpuUsage;
                        float dispGpuTemp = g_gpuTemp;
                        float dispVramUsed = g_vramUsed;
                        float dispVramTotal = g_vramTotal;
                        bool hasGpuData = (g_lhwmAvailable && g_gpuCount > 0) || g_gpuCoreClockMHz > 0.f || g_gpuFanRpm > 0.f || g_gpuMemTemp > 0.f;

                        if (hasGpuData) {
                            if (g_Config.showGpuUsage)
                                ImGui::TextColored(ColorByLoad(dispGpuLoad), "GPU  %.0f%%", dispGpuLoad);
                            if (g_Config.showGpuTemp && dispGpuTemp > 0) {
                                float dispTemp = ToDisplayTemp(dispGpuTemp, g_Config.useFahrenheit);
                                ImVec4 tc = dispGpuTemp > 85 ? ImVec4(1, .3f, .3f, 1)
                                          : dispGpuTemp > 70 ? ImVec4(1, .85f, .15f, 1)
                                                             : ImVec4(.70f, .70f, .75f, 1);
                                if (g_Config.showGpuUsage) ImGui::SameLine();
                                else ImGui::TextColored(ImVec4(.82f, .82f, .88f, 1), "GPU  ");
                                if (!g_Config.showGpuUsage) ImGui::SameLine(0, 0);
                                ImGui::TextColored(tc, "%.0f\xC2\xB0%s", dispTemp,
                                                   g_Config.useFahrenheit ? "F" : "C");
                            } else if (g_Config.showGpuTemp && !g_Config.showGpuUsage) {
                                ImGui::TextColored(ImVec4(.50f, .50f, .55f, 1), "GPU  ---");
                            }
                            // VRAM usage
                            if ((g_Config.showVRAMPercent || g_Config.showVRAMUsage) && dispVramTotal > 0) {
                                float vramPct = (dispVramUsed / dispVramTotal) * 100.0f;
                                if (g_Config.showVRAMPercent)
                                    ImGui::TextColored(ColorByLoad(vramPct), "显存 %.0f%%", vramPct);
                                if (g_Config.showVRAMUsage) {
                                    if (g_Config.showVRAMPercent)
                                        ImGui::SameLine();
                                    ImGui::TextColored(ImVec4(.70f, .70f, .75f, 1),
                                                       "%s%.1f / %.0f GB",
                                                       g_Config.showVRAMPercent ? " " : "显存 ",
                                                       dispVramUsed, dispVramTotal);
                                }
                            }
                            if (wantGpuPwrV && g_gpuPower > 0.f)
                                ImGui::TextColored(ImVec4(.85f, .78f, .55f, 1), "功耗  %.0f W", g_gpuPower);
                            if (g_Config.showGpuMemTemp) {
                                char tempBuf[32];
                                FormatTempCompact(tempBuf, sizeof(tempBuf), g_gpuMemTemp, g_Config.useFahrenheit);
                                ImGui::TextColored(ImVec4(1.00f, .62f, .35f, 1), "显存温度  %s", tempBuf);
                            }
                            if (wantGpuFanV) {
                                char fanBuf[32];
                                FormatFanValue(fanBuf, sizeof(fanBuf), g_gpuFanRpm, g_lhwmGpuFanIsPercent);
                                ImGui::TextColored(ImVec4(.60f, .80f, .90f, 1), "风扇  %s", fanBuf);
                            }
                        } else {
                            ImGui::TextColored(ImVec4(.50f, .50f, .55f, 1), "GPU  N/A");
                        }
                        ImGui::SetWindowFontScale(0.82f * ovSc);
                        const std::string gpuNameSnapshot = GetGpuNameSnapshot();
                        ImGui::TextColored(ImVec4(.42f, .42f, .48f, 1),
                                           "  %s", gpuNameSnapshot.c_str());
                        ImGui::SetWindowFontScale(ovSc);
                        if (wantGpuHzV && hasGpuData) {
                            ImGui::Dummy(ImVec2(0, 3.f * ovSc));
                            ImGui::TextColored(ImVec4(.48f, .58f, .65f, 1), "GPU MHz");
                            ImGui::SameLine();
                            DrawMiniSpark("##vsp_gpu", g_gpuSpark, g_gpuSparkN, g_gpuCoreClockMHz,
                                          ImVec2(130.f * ovSc, 24.f * ovSc));
                        }
                        needSep = true;
                    }
                }

                if (g_Config.showSystemFan) {
                    if (needSep) { ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing(); }
                    char fanBuf[32];
                    FormatFanValue(fanBuf, sizeof(fanBuf), g_systemFanRpm, false);
                    ImGui::TextColored(ImVec4(.60f, .80f, .90f, 1), "系统风扇  %s", fanBuf);
                    needSep = true;
                }

                // RAM
                if (g_Config.showRAMPercent || g_Config.showRAMUsage) {
                    if (needSep) { ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing(); }
                    float pct = (ramUsed / ramTotal) * 100;
                    if (g_Config.showRAMPercent)
                        ImGui::TextColored(ColorByLoad(pct), "内存  %.0f%%", pct);
                    if (g_Config.showRAMUsage) {
                        if (g_Config.showRAMPercent)
                            ImGui::SameLine();
                        ImGui::TextColored(ImVec4(.70f,.70f,.75f,1),
                                           "%s%.1f / %.1f GB",
                                           g_Config.showRAMPercent ? " " : "内存  ",
                                           ramUsed, ramTotal);
                    }
                    if (g_Config.showMemoryTemp) {
                        char tempBuf[32];
                        FormatTempCompact(tempBuf, sizeof(tempBuf), g_memoryTemp, g_Config.useFahrenheit);
                        ImGui::TextColored(ImVec4(.85f, .78f, .55f, 1), "温度 %s", tempBuf);
                    }
                    needSep = true;
                }

                if (g_Config.showNetworkDownload || g_Config.showNetworkUpload) {
                    if (needSep) { ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing(); }
                    char rateBuf[48];
                    if (g_Config.showNetworkDownload) {
                        FormatNetworkRate(rateBuf, sizeof(rateBuf), g_networkDownloadBps);
                        ImGui::TextColored(ImVec4(.48f, .78f, 1.f, 1.f), "下载  %s", rateBuf);
                    }
                    if (g_Config.showNetworkUpload) {
                        FormatNetworkRate(rateBuf, sizeof(rateBuf), g_networkUploadBps);
                        ImGui::TextColored(ImVec4(.52f, .90f, .70f, 1.f), "上传  %s", rateBuf);
                    }
                    needSep = true;
                }

                if (g_Config.showDiskTemp) {
                    if (needSep) { ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing(); }
                    char tempBuf[32];
                    FormatTempCompact(tempBuf, sizeof(tempBuf), g_diskTemp, g_Config.useFahrenheit);
                    ImGui::TextColored(ImVec4(.85f, .78f, .55f, 1), "SSD  %s", tempBuf);
                    needSep = true;
                }
                ImGui::SetWindowFontScale(1.0f);
            }
            
            g_FeatureRegistry.DrawOverlay(g_FeatureContext);

            // ── Show helper text when CTRL is held ──
            if (ctrlHeld) {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                float helpSc = 0.85f * ovSc;
                ImGui::SetWindowFontScale(helpSc);
                ImGui::TextColored(ImVec4(0.5f, 0.75f, 1.0f, 1.0f), "拖动可移动 | 右键打开菜单");
                ImGui::SetWindowFontScale(1.0f);
            }

            ImGui::End();

            if (g_Config.layoutStyle == LAYOUT_STEAM || g_Config.layoutStyle == LAYOUT_GAMEPP) {
                ImGui::PopStyleColor(2);
                ImGui::PopStyleVar(4);
            }

            const bool liveSettingsWasOpen = g_ShowLiveSettings;
            DrawLiveSettingsPanel();
            if (liveSettingsWasOpen && !g_ShowLiveSettings) {
                FlushConfigIfDirty(true);
                g_isDragging = false;
                g_liveSettingsBounds = {0, 0, 0, 0};
                SetLiveSettingsWindowLevel(false);
            }
            if (liveSettingsWasOpen && !g_ShowLiveSettings && !ctrlHeld && !leftMouseDown)
                SetClickThrough(true);

            Present(0, 0, 0, 0);
            if (!g_ShowLiveSettings && !g_isDragging && g_listeningFor == 0)
                Sleep(12);
        }
    }

    // ═══ Cleanup ═══
    if (!SaveConfig(g_Config)) {
        Sleep(100);
        SaveConfig(g_Config);
    }
    g_FeatureRegistry.Shutdown();
    g_lhwmReadsPaused.store(true, std::memory_order_release);
    if (g_lhwmInitThread.joinable())
        g_lhwmInitThread.join();
    if (g_asyncLhwmPollThread.joinable())
        g_asyncLhwmPollThread.join();
    if (g_comparisonPowerPollThread.joinable())
        g_comparisonPowerPollThread.join();
    ShutdownWindowsPowerCounters();
    StopEtwSession();
    if (g_Mode == MODE_OVERLAY) RemoveTrayIcon();
    ReleaseHeaderIconTextures();
    ShutdownBackends();
    ImGui::DestroyContext();
    DestroyWindow(g_hwnd);
    UnregisterClass("FPSOverlay", g_hInstance);
    UnregisterClass("FPSOverlayTray", g_hInstance);
    ShutdownWMI();

    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// Window procedure
// ═══════════════════════════════════════════════════════════════════════════
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_CLOSE:
        if (g_Mode == MODE_CONFIG) g_Running = false;
        return 0;
    case WM_DESTROY:
        if (hWnd == g_hwnd && g_Mode == MODE_OVERLAY && g_Running)
            g_OverlayHostRecoveryPending = true;
        return 0;
    case WM_POWERBROADCAST:
        if (wParam == PBT_APMSUSPEND) {
            PauseLhwmReadsForSuspend();
            DetachOverlayOwnerWindow("power suspend");
        } else if (wParam == PBT_APMRESUMEAUTOMATIC ||
                   wParam == PBT_APMRESUMECRITICAL ||
                   wParam == PBT_APMRESUMESUSPEND) {
            g_PowerResumePending = true;
        }
        return 0;
    case WM_GETMINMAXINFO:
        if (g_Mode == MODE_CONFIG && lParam) {
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            mmi->ptMinTrackSize.x = ScalePx(kConfigDlgMinOuterW);
            mmi->ptMinTrackSize.y = ScalePx(kConfigDlgMinOuterH);
        }
        return 0;
    case WM_DPICHANGED:
        if (lParam) {
            RECT* suggested = (RECT*)lParam;
            SetWindowPos(hWnd, nullptr,
                         suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        RebuildUiForDpi(hWnd);
        return 0;
    case WM_SIZE:
        if (g_pd3dDevice && g_pSwapChain && !g_D3DRecoveryPending &&
            wParam != SIZE_MINIMIZED && LOWORD(lParam) != 0 && HIWORD(lParam) != 0) {
            CleanupRenderTarget();
            const HRESULT resizeResult = g_pSwapChain->ResizeBuffers(
                0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam),
                DXGI_FORMAT_UNKNOWN, 0);
            if (FAILED(resizeResult)) {
                RequestD3DRecovery(resizeResult, "ResizeBuffers failed");
            } else if (!CreateRenderTarget()) {
                RequestD3DRecovery(g_LastD3DError, "render target recreation failed");
            }
        }
        return 0;
    case WM_TRAYICON:
        if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
            StartSettingsTrace("tray");
            POINT pt; GetCursorPos(&pt);
            HMENU m = CreatePopupMenu();
            // Show/Hide toggle based on current visibility
            if (g_OvlVisible)
                AppendMenuW(m, MF_STRING, IDM_HIDE, L"隐藏覆盖层");
            else
                AppendMenuW(m, MF_STRING, IDM_SHOW, L"显示覆盖层");
            AppendMenuW(m,
                        MF_STRING | (g_Config.desktopOnlyMode ? MF_CHECKED
                                                             : MF_UNCHECKED),
                        IDM_DESKTOP_ONLY, L"仅桌面显示");
            AppendMenuW(m, MF_STRING, IDM_RESCAN_SENSORS, L"重新扫描传感器");
            AppendMenuW(m, MF_STRING, IDM_COPY_DIAGNOSTIC_SUMMARY, L"复制诊断摘要");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"设置");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出");
            g_SettingsTrace.foregroundMenuStartUs = PerfNowUs();
            g_SettingsTrace.foregroundMenuEndUs = PerfNowUs();
            g_SettingsTrace.popupStartUs = PerfNowUs();
            int cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                     pt.x, pt.y, 0, hWnd, nullptr);
            g_SettingsTrace.popupEndUs = PerfNowUs();
            DestroyMenu(m);
            PostMessageW(hWnd, WM_NULL, 0, 0);
            // Handle the command directly
            switch (cmd) {
                case IDM_SHOW:     g_OvlVisible = true;            break;
                case IDM_HIDE:     g_OvlVisible = false;           break;
                case IDM_DESKTOP_ONLY:
                    g_Config.desktopOnlyMode = !g_Config.desktopOnlyMode;
                    g_trayDesktopOnlyMode.store(g_Config.desktopOnlyMode,
                                                std::memory_order_relaxed);
                    SaveConfig(g_Config);
                    break;
                case IDM_RESCAN_SENSORS: RescanPowerSensors();       break;
                case IDM_COPY_DIAGNOSTIC_SUMMARY:
                    CopyDiagnosticSummaryToClipboard();
                    break;
                case IDM_SETTINGS: QueueShowSettings();           break;
                case IDM_EXIT:     g_Pending = CMD_EXIT;           break;
            }
        }
        return 0;
    case WM_CONTEXTMENU:
        // Right-click on overlay window itself - only show if Ctrl is held AND cursor is over overlay
        // This prevents showing our menu when user right-clicked elsewhere
        if (g_Mode == MODE_OVERLAY) {
            bool ctrlHeld = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            POINT pt; GetCursorPos(&pt);
            // Only show menu if Ctrl is held AND cursor is over the overlay
            if (ctrlHeld && PtInRect(&g_overlayBounds, pt)) {
                HMENU m = CreatePopupMenu();
                AppendMenuW(m, MF_STRING, IDM_HIDE, L"隐藏覆盖层");
                AppendMenuW(m, MF_STRING, IDM_RESET_POS, L"重置位置");
                AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"设置");
                AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出");
                SetForegroundWindow(hWnd);
                int cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                         pt.x, pt.y, 0, hWnd, nullptr);
                DestroyMenu(m);
                switch (cmd) {
                    case IDM_HIDE:       g_OvlVisible = false;           break;
                    case IDM_RESET_POS:
                        g_Config.customX = -1.f;
                        g_Config.customY = -1.f;
                        g_overlayForceCornerSnap = true;
                        MarkConfigDirty();
                        break;
                    case IDM_SETTINGS: QueueShowSettings();           break;
                    case IDM_EXIT:     g_Pending = CMD_EXIT;           break;
                }
            }
        }
        return 0;
    case WM_APP_TRAY_COMMAND:
        if ((UINT)wParam == IDM_SETTINGS)
            StartSettingsTrace("tray-command");
        switch ((UINT)wParam) {
            case IDM_SHOW:     g_OvlVisible = true;            break;
            case IDM_HIDE:     g_OvlVisible = false;           break;
            case IDM_DESKTOP_ONLY:
                g_Config.desktopOnlyMode = !g_Config.desktopOnlyMode;
                g_trayDesktopOnlyMode.store(g_Config.desktopOnlyMode,
                                            std::memory_order_relaxed);
                SaveConfig(g_Config);
                LogLine("Tray quick option: desktop-only mode %s",
                        g_Config.desktopOnlyMode ? "enabled" : "disabled");
                break;
            case IDM_RESCAN_SENSORS: RescanPowerSensors();         break;
            case IDM_COPY_DIAGNOSTIC_SUMMARY:
                CopyDiagnosticSummaryToClipboard();
                break;
            case IDM_SETTINGS: QueueShowSettings();            break;
            case IDM_EXIT:     g_Pending = CMD_EXIT;           break;
            case IDM_POWER_SHOW_SYSTEM:
                g_FeatureRegistry.ToggleLaptopPowerQuickOption(
                    LaptopPowerQuickOption::ShowSystemPower);
                SaveConfig(g_Config);
                break;
            case IDM_POWER_SHOW_KNOWN:
                g_FeatureRegistry.ToggleLaptopPowerQuickOption(
                    LaptopPowerQuickOption::ShowKnownComponentsPower);
                SaveConfig(g_Config);
                break;
            case IDM_POWER_SHOW_BATTERY:
                g_FeatureRegistry.ToggleLaptopPowerQuickOption(
                    LaptopPowerQuickOption::ShowBatteryDischargePower);
                SaveConfig(g_Config);
                break;
            case IDM_POWER_ALLOW_ESTIMATE:
                g_FeatureRegistry.ToggleLaptopPowerQuickOption(
                    LaptopPowerQuickOption::AllowEstimatedSystemPower);
                SaveConfig(g_Config);
                break;
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
            case IDM_SHOW:     g_OvlVisible = true;            break;
            case IDM_HIDE:     g_OvlVisible = false;           break;
            case IDM_DESKTOP_ONLY:
                g_Config.desktopOnlyMode = !g_Config.desktopOnlyMode;
                g_trayDesktopOnlyMode.store(g_Config.desktopOnlyMode,
                                            std::memory_order_relaxed);
                SaveConfig(g_Config);
                break;
            case IDM_RESCAN_SENSORS: RescanPowerSensors();         break;
            case IDM_COPY_DIAGNOSTIC_SUMMARY:
                CopyDiagnosticSummaryToClipboard();
                break;
            case IDM_SETTINGS: QueueShowSettings();           break;
            case IDM_EXIT:     g_Pending = CMD_EXIT;           break;
        }
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

static void InitializeTrayIconData(HWND owner)
{
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = owner;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(g_hInstance, MAKEINTRESOURCE(1));
    if (!g_nid.hIcon)
        g_nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    lstrcpyW(g_nid.szTip, L"FPS Overlay");
}

static bool PublishTrayIcon(bool replaceExisting)
{
    const HWND owner = g_nid.hWnd;
    if (!owner)
        return false;

    if (replaceExisting && g_trayIconPublished &&
        Shell_NotifyIconW(NIM_MODIFY, &g_nid)) {
        return true;
    }

    InitializeTrayIconData(owner);
    SetLastError(ERROR_SUCCESS);
    if (Shell_NotifyIconW(NIM_ADD, &g_nid)) {
        g_trayIconPublished = true;
        g_nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
        return true;
    }
    const DWORD addErrorHint = GetLastError();

    // A timed-out NIM_ADD can still reach Explorer. A modify probe avoids
    // deleting an icon that the shell accepted after the caller timed out.
    if (Shell_NotifyIconW(NIM_MODIFY, &g_nid)) {
        g_trayIconPublished = true;
        return true;
    }

    g_trayIconPublished = false;
    LogLine("Tray icon publish failed: replace=%d addErrorHint=%lu shell=%p",
            replaceExisting ? 1 : 0,
            (unsigned long)addErrorHint,
            FindWindowW(L"Shell_TrayWnd", nullptr));
    return false;
}

static void ScheduleTrayRestoreRetry(HWND hWnd)
{
    static constexpr UINT retryDelaysMs[] = {1000, 3000, 8000, 15000, 30000};
    if (!hWnd)
        return;
    const size_t delayIndex = (std::min)(
        static_cast<size_t>(g_trayRestoreRetryIndex),
        ARRAYSIZE(retryDelaysMs) - 1);
    const UINT delay = retryDelaysMs[delayIndex];
    if (g_trayRestoreRetryIndex != UINT_MAX)
        ++g_trayRestoreRetryIndex;
    SetTimer(hWnd, kTrayRestoreTimerId, delay, nullptr);
    LogLine("Tray icon restore retry scheduled: attempt=%u delay=%u ms",
            g_trayRestoreRetryIndex, delay);
}

static void RestoreTrayIcon(HWND hWnd, bool explorerRestarted)
{
    const ULONGLONG now = GetTickCount64();
    if (!explorerRestarted && g_trayIconPublished &&
        g_trayRestoreRetryIndex == 0 && g_trayLastRestoreTick != 0 &&
        now - g_trayLastRestoreTick < 500) {
        return;
    }
    KillTimer(hWnd, kTrayRestoreTimerId);
    if (explorerRestarted)
        g_trayIconPublished = false;
    if (PublishTrayIcon(!explorerRestarted)) {
        g_trayRestoreRetryIndex = 0;
        g_trayLastRestoreTick = now;
        LogLine("Tray icon restored: explorerRestarted=%d",
                explorerRestarted ? 1 : 0);
    } else {
        ScheduleTrayRestoreRetry(hWnd);
    }
}

LRESULT CALLBACK TrayWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    static LONGLONG menuCallbackUs = 0;
    static LONGLONG menuVisibleUs = 0;
    static DWORD menuQueueDelayMs = 0;

    if (g_TaskbarCreatedMessage != 0 &&
        msg == g_TaskbarCreatedMessage) {
        g_trayRestoreRetryIndex = 0;
        RestoreTrayIcon(hWnd, true);
        return 0;
    }

    switch (msg) {
    case WM_TRAYICON:
        if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
            const DWORD nowMs = GetTickCount();
            const DWORD postedMs = (DWORD)GetMessageTime();
            menuQueueDelayMs = nowMs - postedMs;
            menuCallbackUs = PerfNowUs();
            menuVisibleUs = 0;

            POINT pt{};
            GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            if (g_trayOverlayVisible.load(std::memory_order_relaxed))
                AppendMenuW(menu, MF_STRING, IDM_HIDE, L"隐藏覆盖层");
            else
                AppendMenuW(menu, MF_STRING, IDM_SHOW, L"显示覆盖层");
            AppendMenuW(
                menu,
                MF_STRING |
                    (g_trayDesktopOnlyMode.load(std::memory_order_relaxed)
                         ? MF_CHECKED : MF_UNCHECKED),
                IDM_DESKTOP_ONLY, L"仅桌面显示");
            AppendMenuW(menu, MF_STRING, IDM_RESCAN_SENSORS, L"重新扫描传感器");
            AppendMenuW(menu, MF_STRING, IDM_COPY_DIAGNOSTIC_SUMMARY, L"复制诊断摘要");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            HMENU powerMenu = CreatePopupMenu();
            AppendMenuW(powerMenu,
                        MF_STRING | (g_trayShowSystemPower.load(std::memory_order_relaxed) ? MF_CHECKED : MF_UNCHECKED),
                        IDM_POWER_SHOW_SYSTEM, L"显示整机功耗");
            AppendMenuW(powerMenu,
                        MF_STRING | (g_trayShowKnownPower.load(std::memory_order_relaxed) ? MF_CHECKED : MF_UNCHECKED),
                        IDM_POWER_SHOW_KNOWN, L"显示已知组件功耗");
            AppendMenuW(powerMenu,
                        MF_STRING | (g_trayShowBatteryPower.load(std::memory_order_relaxed) ? MF_CHECKED : MF_UNCHECKED),
                        IDM_POWER_SHOW_BATTERY, L"显示电池放电功耗");
            AppendMenuW(powerMenu,
                        MF_STRING | (g_trayAllowEstimatedPower.load(std::memory_order_relaxed) ? MF_CHECKED : MF_UNCHECKED),
                        IDM_POWER_ALLOW_ESTIMATE, L"允许估算整机功耗");
            AppendMenuW(menu, MF_POPUP, (UINT_PTR)powerMenu, L"功耗显示");
            AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"设置");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出");

            // A tray popup must have a foreground owner so Windows can dismiss
            // it when the user clicks elsewhere without choosing a command.
            SetForegroundWindow(hWnd);
            const LONGLONG trackCallUs = PerfNowUs();
            const int cmd = TrackPopupMenu(
                menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                pt.x, pt.y, 0, hWnd, nullptr);
            const LONGLONG menuReturnUs = PerfNowUs();
            DestroyMenu(menu);
            PostMessageW(hWnd, WM_NULL, 0, 0);

            LogLine(
                "Tray menu trace: queue=%lu ms prepare=%.3f ms callbackToVisible=%.3f ms "
                "visibleToSelection=%.3f ms command=%d",
                (unsigned long)menuQueueDelayMs,
                TraceMs(menuCallbackUs, trackCallUs),
                TraceMs(menuCallbackUs, menuVisibleUs),
                TraceMs(menuVisibleUs, menuReturnUs),
                cmd);
            if (cmd != 0 && g_hwnd)
                PostMessageW(g_hwnd, WM_APP_TRAY_COMMAND, (WPARAM)cmd, 0);
        }
        return 0;
    case WM_INITMENUPOPUP:
        if (menuCallbackUs != 0 && menuVisibleUs == 0)
            menuVisibleUs = PerfNowUs();
        return 0;
    case WM_APP_TRAY_TOOLTIP:
        lstrcpyW(g_nid.szTip, L"FPS Overlay");
        if (!g_trayIconPublished || !Shell_NotifyIconW(NIM_MODIFY, &g_nid))
            RestoreTrayIcon(hWnd, false);
        return 0;
    case WM_APP_TRAY_RECOVERY_STATUS:
        {
            const PowerRecoveryStatus status =
                static_cast<PowerRecoveryStatus>(wParam);
            if (status == PowerRecoveryStatus::Idle)
                return 0;
            const UINT originalFlags = g_nid.uFlags;
            g_nid.uFlags = NIF_INFO;
            lstrcpynW(g_nid.szInfoTitle, L"FPS Overlay", ARRAYSIZE(g_nid.szInfoTitle));
            if (status == PowerRecoveryStatus::Recovering) {
                lstrcpynW(g_nid.szInfo, L"正在恢复监测，等待显卡和传感器稳定。",
                          ARRAYSIZE(g_nid.szInfo));
                g_nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
            } else if (status == PowerRecoveryStatus::Recovered) {
                lstrcpynW(g_nid.szInfo, L"监测已恢复，硬件数据已重新刷新。",
                          ARRAYSIZE(g_nid.szInfo));
                g_nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
            } else {
                lstrcpynW(g_nid.szInfo, L"监测恢复超时，请在托盘选择“重新扫描传感器”。",
                          ARRAYSIZE(g_nid.szInfo));
                g_nid.dwInfoFlags = NIIF_WARNING | NIIF_NOSOUND;
            }
            Shell_NotifyIconW(NIM_MODIFY, &g_nid);
            g_nid.uFlags = originalFlags;
        }
        return 0;
    case WM_APP_TRAY_RESTORE:
        g_trayRestoreRetryIndex = 0;
        RestoreTrayIcon(hWnd, false);
        return 0;
    case WM_TIMER:
        if (wParam == kTrayRestoreTimerId) {
            KillTimer(hWnd, kTrayRestoreTimerId);
            if (PublishTrayIcon(false)) {
                g_trayRestoreRetryIndex = 0;
                g_trayLastRestoreTick = GetTickCount64();
                LogLine("Tray icon restored by retry");
            } else {
                ScheduleTrayRestoreRetry(hWnd);
            }
            return 0;
        }
        break;
    case WM_POWERBROADCAST:
        if (wParam == PBT_APMRESUMEAUTOMATIC ||
            wParam == PBT_APMRESUMECRITICAL ||
            wParam == PBT_APMRESUMESUSPEND) {
            PostMessageW(hWnd, WM_APP_TRAY_RESTORE, 0, 0);
        }
        return TRUE;
    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hWnd, kTrayRestoreTimerId);
        g_trayIconPublished = false;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

static void TrayThreadMain()
{
    HWND tray = CreateWindowExA(
        WS_EX_TOOLWINDOW,
        "FPSOverlayTray", "FPS Overlay Tray Owner", WS_POPUP,
        -32000, -32000, 1, 1,
        nullptr, nullptr, g_hInstance, nullptr);
    g_trayHwnd.store(tray, std::memory_order_release);

    if (tray) {
        if (g_TaskbarCreatedMessage != 0) {
            ChangeWindowMessageFilterEx(
                tray, g_TaskbarCreatedMessage, MSGFLT_ALLOW, nullptr);
        }
        InitializeTrayIconData(tray);
        if (!PublishTrayIcon(false))
            ScheduleTrayRestoreRetry(tray);
    }

    if (g_trayReadyEvent)
        SetEvent(g_trayReadyEvent);

    if (tray) {
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayIconPublished = false;
    }
    g_trayHwnd.store(nullptr, std::memory_order_release);
}
