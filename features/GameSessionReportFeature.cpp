#include "GameSessionReportFeature.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cwchar>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <locale>
#include <mutex>
#include <numeric>
#include <shellapi.h>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "imgui.h"
#include "SettingsUi.h"

namespace {

constexpr ULONGLONG kTargetStableMs = 2000;
constexpr ULONGLONG kTargetMissingGraceMs = 5000;
constexpr ULONGLONG kLongUpdateGapMs = 10000;
constexpr ULONGLONG kSampleIntervalMs = 1000;
constexpr double kAutoOpenDurationSeconds = 30.0;
constexpr size_t kMaxSamples = 8u * 60u * 60u;
constexpr size_t kMaxHistoryEntries = 100;

enum class MetricId {
    Fps,
    CpuUsage,
    CpuTemperature,
    CpuPower,
    GpuUsage,
    GpuTemperature,
    GpuPower,
    RamUsagePercent,
    RamUsedGb,
    VramUsagePercent,
    VramUsedGb,
    SystemPower,
    Count
};

struct MetricStatistics {
    double average = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
    size_t validSamples = 0;
};

struct HardwareInfo {
    std::string windowsVersion = "N/A";
    std::string cpuName = "N/A";
    std::string gpuName = "N/A";
    std::string displayMode = "N/A";
    double totalMemoryGb = 0.0;
};

struct SessionData {
    DWORD processId = 0;
    std::string processName;
    std::string gameName;
    SYSTEMTIME startLocal = {};
    SYSTEMTIME endLocal = {};
    ULONGLONG startTick = 0;
    ULONGLONG endTick = 0;
    ULONGLONG pausedMs = 0;
    ULONGLONG lastTargetTick = 0;
    SYSTEMTIME lastTargetLocal = {};
    ULONGLONG lastSampleTick = 0;
    ULONGLONG missingSinceTick = 0;
    ULONGLONG lastProcessCheckTick = 0;
    double durationSeconds = 0.0;
    double validSampleDurationSeconds = 0.0;
    HardwareInfo hardware;
    std::vector<GameSessionSample> samples;
    MetricStatistics statistics[static_cast<size_t>(MetricId::Count)] = {};
    double onePercentLowFps = 0.0;
    bool hasOnePercentLowFps = false;
    double energyWh = 0.0;
    bool energyIncludesEstimate = false;
    double powerCoveragePercent = 0.0;
    bool sampleLimitReached = false;
    std::filesystem::path csvPath;
};

struct CsvJob {
    std::shared_ptr<const SessionData> session;
};

struct HistoryEntry {
    std::filesystem::path path;
    std::filesystem::file_time_type modifiedTime = {};
    uintmax_t sizeBytes = 0;
    std::string gameName;
    std::string processName;
    std::string startTime;
    std::string duration;
    std::string averageFps;
};

struct WindowTitleSearch {
    DWORD processId = 0;
    std::wstring title;
};

bool IsFinite(float value)
{
    return std::isfinite(static_cast<double>(value));
}

float ClampUsage(float value)
{
    return std::clamp(value, 0.0f, 100.0f);
}

bool ValidTemperature(float value)
{
    return IsFinite(value) && value >= 1.0f && value <= 150.0f;
}

bool ValidPower(float value)
{
    return IsFinite(value) && value > 0.0f;
}

std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty())
        return {};
    const int required = WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
                                              static_cast<int>(value.size()),
                                              nullptr, 0, nullptr, nullptr);
    if (required <= 0)
        return {};
    std::string result(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
                        static_cast<int>(value.size()), result.data(), required,
                        nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string& value)
{
    if (value.empty())
        return {};
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                             value.c_str(),
                                             static_cast<int>(value.size()),
                                             nullptr, 0);
    if (required <= 0)
        return {};
    std::wstring result(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(),
                        static_cast<int>(value.size()), result.data(), required);
    return result;
}

std::filesystem::path ExecutableDirectory()
{
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
        return std::filesystem::current_path();
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path ReportDirectory()
{
    return ExecutableDirectory() / L"GameSessionReports";
}

std::string ReadRegistryString(HKEY root, const wchar_t* subKey,
                               const wchar_t* valueName)
{
    wchar_t buffer[512] = {};
    DWORD bytes = sizeof(buffer);
    const LONG result = RegGetValueW(root, subKey, valueName,
                                     RRF_RT_REG_SZ, nullptr, buffer, &bytes);
    if (result != ERROR_SUCCESS)
        return {};
    return WideToUtf8(buffer);
}

HardwareInfo QueryStaticHardwareInfo()
{
    HardwareInfo info;

    const std::string productName = ReadRegistryString(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"ProductName");
    const std::string displayVersion = ReadRegistryString(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"DisplayVersion");
    const std::string build = ReadRegistryString(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuildNumber");
    if (!productName.empty()) {
        info.windowsVersion = productName;
        if (!displayVersion.empty())
            info.windowsVersion += " " + displayVersion;
        if (!build.empty())
            info.windowsVersion += " (" + build + ")";
    }

    const std::string cpuName = ReadRegistryString(
        HKEY_LOCAL_MACHINE,
        L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        L"ProcessorNameString");
    if (!cpuName.empty())
        info.cpuName = cpuName;

    DISPLAY_DEVICEW displayDevice = {};
    displayDevice.cb = sizeof(displayDevice);
    for (DWORD index = 0; EnumDisplayDevicesW(nullptr, index, &displayDevice, 0); ++index) {
        const bool active = (displayDevice.StateFlags & DISPLAY_DEVICE_ACTIVE) != 0;
        const bool mirroring = (displayDevice.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) != 0;
        if (active && !mirroring && displayDevice.DeviceString[0] != L'\0') {
            info.gpuName = WideToUtf8(displayDevice.DeviceString);
            break;
        }
        displayDevice = {};
        displayDevice.cb = sizeof(displayDevice);
    }

    MEMORYSTATUSEX memory = {};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        info.totalMemoryGb = static_cast<double>(memory.ullTotalPhys) /
                             (1024.0 * 1024.0 * 1024.0);
    }

    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode)) {
        char display[96] = {};
        snprintf(display, sizeof(display), "%lu x %lu @ %lu Hz",
                 mode.dmPelsWidth, mode.dmPelsHeight, mode.dmDisplayFrequency);
        info.displayMode = display;
    }
    return info;
}

BOOL CALLBACK FindWindowTitleCallback(HWND hwnd, LPARAM parameter)
{
    auto* search = reinterpret_cast<WindowTitleSearch*>(parameter);
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId != search->processId || !IsWindowVisible(hwnd))
        return TRUE;

    wchar_t title[512] = {};
    if (GetWindowTextW(hwnd, title, static_cast<int>(std::size(title))) <= 0)
        return TRUE;
    search->title = title;
    return FALSE;
}

std::string FindWindowTitle(DWORD processId)
{
    WindowTitleSearch search;
    search.processId = processId;
    EnumWindows(FindWindowTitleCallback, reinterpret_cast<LPARAM>(&search));
    return WideToUtf8(search.title);
}

std::string QueryProcessFilename(DWORD processId)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process)
        return {};
    std::wstring path(32768, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    std::string result;
    if (QueryFullProcessImageNameW(process, 0, path.data(), &length)) {
        path.resize(length);
        result = WideToUtf8(std::filesystem::path(path).filename().wstring());
    }
    CloseHandle(process);
    return result;
}

bool ProcessDefinitelyExited(DWORD processId)
{
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
    if (!process)
        return GetLastError() == ERROR_INVALID_PARAMETER;
    const DWORD waitResult = WaitForSingleObject(process, 0);
    CloseHandle(process);
    return waitResult == WAIT_OBJECT_0;
}

SYSTEMTIME CurrentLocalTime()
{
    SYSTEMTIME time = {};
    GetLocalTime(&time);
    return time;
}

bool HasTimeValue(const SYSTEMTIME& time)
{
    return time.wYear != 0;
}

std::string FormatLocalTime(const SYSTEMTIME& time)
{
    if (!HasTimeValue(time))
        return "N/A";
    char buffer[64] = {};
    snprintf(buffer, sizeof(buffer), "%04u-%02u-%02u %02u:%02u:%02u",
             time.wYear, time.wMonth, time.wDay,
             time.wHour, time.wMinute, time.wSecond);
    return buffer;
}

std::string FormatDuration(double seconds)
{
    const unsigned long long total = static_cast<unsigned long long>((std::max)(0.0, seconds));
    const unsigned long long hours = total / 3600ULL;
    const unsigned long long minutes = (total % 3600ULL) / 60ULL;
    const unsigned long long remainingSeconds = total % 60ULL;
    char buffer[96] = {};
    if (hours > 0) {
        snprintf(buffer, sizeof(buffer), "%llu 小时 %llu 分钟 %llu 秒",
                 hours, minutes, remainingSeconds);
    } else if (minutes > 0) {
        snprintf(buffer, sizeof(buffer), "%llu 分钟 %llu 秒",
                 minutes, remainingSeconds);
    } else {
        snprintf(buffer, sizeof(buffer), "%llu 秒", remainingSeconds);
    }
    return buffer;
}

std::string FormatDurationShort(double seconds)
{
    const unsigned long long total = static_cast<unsigned long long>((std::max)(0.0, seconds));
    const unsigned long long hours = total / 3600ULL;
    const unsigned long long minutes = (total % 3600ULL) / 60ULL;
    const unsigned long long remainingSeconds = total % 60ULL;
    char buffer[48] = {};
    if (hours > 0)
        snprintf(buffer, sizeof(buffer), "%02llu:%02llu:%02llu", hours, minutes, remainingSeconds);
    else
        snprintf(buffer, sizeof(buffer), "%02llu:%02llu", minutes, remainingSeconds);
    return buffer;
}

double SessionElapsedSeconds(const SessionData& session, ULONGLONG tick)
{
    if (tick <= session.startTick)
        return 0.0;
    const ULONGLONG rawElapsed = tick - session.startTick;
    const ULONGLONG elapsed = rawElapsed > session.pausedMs
        ? rawElapsed - session.pausedMs : 0;
    return static_cast<double>(elapsed) / 1000.0;
}

std::pair<float, bool> MetricValue(const GameSessionSample& sample, MetricId metric)
{
    switch (metric) {
    case MetricId::Fps:                 return {sample.fps, sample.fpsValid};
    case MetricId::CpuUsage:            return {sample.cpuUsage, sample.cpuUsageValid};
    case MetricId::CpuTemperature:      return {sample.cpuTemperature, sample.cpuTemperatureValid};
    case MetricId::CpuPower:            return {sample.cpuPackagePower, sample.cpuPackagePowerValid};
    case MetricId::GpuUsage:            return {sample.gpuUsage, sample.gpuUsageValid};
    case MetricId::GpuTemperature:      return {sample.gpuTemperature, sample.gpuTemperatureValid};
    case MetricId::GpuPower:            return {sample.gpuPower, sample.gpuPowerValid};
    case MetricId::RamUsagePercent:     return {sample.ramUsagePercent, sample.ramUsagePercentValid};
    case MetricId::RamUsedGb:           return {sample.ramUsedGb, sample.ramUsedGbValid};
    case MetricId::VramUsagePercent:    return {sample.vramUsagePercent, sample.vramUsagePercentValid};
    case MetricId::VramUsedGb:          return {sample.vramUsedGb, sample.vramUsedGbValid};
    case MetricId::SystemPower:         return {sample.systemPower, sample.systemPowerValid};
    default:                            return {0.0f, false};
    }
}

MetricStatistics CalculateStatistics(const std::vector<GameSessionSample>& samples,
                                      MetricId metric)
{
    MetricStatistics result;
    double total = 0.0;
    for (const GameSessionSample& sample : samples) {
        const auto [value, valid] = MetricValue(sample, metric);
        if (!valid || !IsFinite(value))
            continue;
        if (result.validSamples == 0) {
            result.minimum = value;
            result.maximum = value;
        } else {
            result.minimum = (std::min)(result.minimum, static_cast<double>(value));
            result.maximum = (std::max)(result.maximum, static_cast<double>(value));
        }
        total += value;
        ++result.validSamples;
    }
    if (result.validSamples > 0)
        result.average = total / static_cast<double>(result.validSamples);
    return result;
}

double CalculateOnePercentLow(const std::vector<GameSessionSample>& samples,
                              bool& valid)
{
    std::vector<float> values;
    values.reserve(samples.size());
    for (const GameSessionSample& sample : samples) {
        if (sample.fpsValid && IsFinite(sample.fps) && sample.fps > 0.0f)
            values.push_back(sample.fps);
    }
    if (values.empty()) {
        valid = false;
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const size_t lowCount = (std::max)(
        static_cast<size_t>(1),
        static_cast<size_t>(std::ceil(static_cast<double>(values.size()) * 0.01)));
    const double sum = std::accumulate(values.begin(), values.begin() + lowCount, 0.0);
    valid = true;
    return sum / static_cast<double>(lowCount);
}

bool SampleHasAnyValidValue(const GameSessionSample& sample)
{
    return sample.fpsValid || sample.cpuUsageValid || sample.cpuTemperatureValid ||
           sample.cpuPackagePowerValid || sample.gpuUsageValid ||
           sample.gpuTemperatureValid || sample.gpuPowerValid ||
           sample.ramUsagePercentValid || sample.ramUsedGbValid ||
           sample.vramUsagePercentValid || sample.vramUsedGbValid ||
           sample.systemPowerValid;
}

void CalculateSessionStatistics(SessionData& session)
{
    for (size_t index = 0; index < static_cast<size_t>(MetricId::Count); ++index) {
        session.statistics[index] = CalculateStatistics(
            session.samples, static_cast<MetricId>(index));
    }
    session.onePercentLowFps = CalculateOnePercentLow(
        session.samples, session.hasOnePercentLowFps);

    size_t validPowerSamples = 0;
    bool havePreviousPower = false;
    const GameSessionSample* previousPower = nullptr;
    for (const GameSessionSample& sample : session.samples) {
        if (!sample.systemPowerValid)
            continue;
        ++validPowerSamples;
        if (havePreviousPower && previousPower) {
            const double elapsedSeconds = sample.elapsedSeconds - previousPower->elapsedSeconds;
            if (elapsedSeconds > 0.0 && elapsedSeconds <= 3.0) {
                session.energyWh +=
                    (static_cast<double>(previousPower->systemPower) + sample.systemPower) *
                    0.5 * elapsedSeconds / 3600.0;
                if (previousPower->systemPowerEstimated || sample.systemPowerEstimated)
                    session.energyIncludesEstimate = true;
            }
        }
        previousPower = &sample;
        havePreviousPower = true;
    }
    if (!session.samples.empty()) {
        session.powerCoveragePercent =
            static_cast<double>(validPowerSamples) * 100.0 /
            static_cast<double>(session.samples.size());
    }

    bool foundFirst = false;
    double firstElapsed = 0.0;
    double lastElapsed = 0.0;
    for (const GameSessionSample& sample : session.samples) {
        if (!SampleHasAnyValidValue(sample))
            continue;
        if (!foundFirst) {
            firstElapsed = sample.elapsedSeconds;
            foundFirst = true;
        }
        lastElapsed = sample.elapsedSeconds;
    }
    if (foundFirst)
        session.validSampleDurationSeconds = (std::max)(0.0, lastElapsed - firstElapsed);
}

#if defined(FPSOVERLAY_UI_QA)
bool RunStatisticsSelfTest()
{
    SessionData session;
    session.samples.reserve(200);
    for (int index = 1; index <= 200; ++index) {
        GameSessionSample sample;
        sample.elapsedSeconds = static_cast<double>(index);
        sample.fps = static_cast<float>(index);
        sample.fpsValid = true;
        if (index == 2 || index == 3) {
            sample.cpuTemperature = index == 2 ? 50.0f : 70.0f;
            sample.cpuTemperatureValid = true;
        }
        if (index <= 2) {
            sample.systemPower = index == 1 ? 100.0f : 200.0f;
            sample.systemPowerValid = true;
            sample.systemPowerEstimated = index == 2;
        }
        session.samples.push_back(sample);
    }
    CalculateSessionStatistics(session);

    const MetricStatistics& fps =
        session.statistics[static_cast<size_t>(MetricId::Fps)];
    const MetricStatistics& temperature =
        session.statistics[static_cast<size_t>(MetricId::CpuTemperature)];
    const double expectedEnergyWh = 150.0 / 3600.0;
    return fps.validSamples == 200 && std::fabs(fps.average - 100.5) < 0.0001 &&
           std::fabs(fps.minimum - 1.0) < 0.0001 &&
           std::fabs(fps.maximum - 200.0) < 0.0001 &&
           session.hasOnePercentLowFps &&
           std::fabs(session.onePercentLowFps - 1.5) < 0.0001 &&
           temperature.validSamples == 2 &&
           std::fabs(temperature.average - 60.0) < 0.0001 &&
           std::fabs(temperature.minimum - 50.0) < 0.0001 &&
           std::fabs(temperature.maximum - 70.0) < 0.0001 &&
           std::fabs(session.energyWh - expectedEnergyWh) < 0.000001 &&
           session.energyIncludesEstimate;
}
#endif

std::string CsvEscape(const std::string& value)
{
    if (value.find_first_of(",\"\r\n") == std::string::npos)
        return value;
    std::string escaped = "\"";
    for (const char character : value) {
        if (character == '\"')
            escaped += "\"\"";
        else
            escaped += character;
    }
    escaped += '\"';
    return escaped;
}

std::string NumberOrNa(bool valid, double value, int precision = 2)
{
    if (!valid || !std::isfinite(value))
        return "N/A";
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::fixed << std::setprecision(precision) << value;
    return output.str();
}

const char* MetricName(MetricId metric)
{
    switch (metric) {
    case MetricId::Fps:              return "FPS";
    case MetricId::CpuUsage:         return "CPU 占用 (%)";
    case MetricId::CpuTemperature:   return "CPU 温度 (°C)";
    case MetricId::CpuPower:         return "CPU 功耗 (W)";
    case MetricId::GpuUsage:         return "GPU 占用 (%)";
    case MetricId::GpuTemperature:   return "GPU 温度 (°C)";
    case MetricId::GpuPower:         return "GPU 功耗 (W)";
    case MetricId::RamUsagePercent:  return "内存占用 (%)";
    case MetricId::RamUsedGb:        return "内存已用 (GB)";
    case MetricId::VramUsagePercent: return "显存占用 (%)";
    case MetricId::VramUsedGb:       return "显存已用 (GB)";
    case MetricId::SystemPower:      return "整机功耗 (W)";
    default:                         return "N/A";
    }
}

std::string BuildCsv(const SessionData& session)
{
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "游戏会话概要\r\n";
    output << "字段,值\r\n";
    output << "游戏名称," << CsvEscape(session.gameName) << "\r\n";
    output << "游戏进程," << CsvEscape(session.processName) << "\r\n";
    output << "游戏 PID," << session.processId << "\r\n";
    output << "开始时间," << CsvEscape(FormatLocalTime(session.startLocal)) << "\r\n";
    output << "结束时间," << CsvEscape(FormatLocalTime(session.endLocal)) << "\r\n";
    output << "游戏时长," << CsvEscape(FormatDuration(session.durationSeconds)) << "\r\n";
    output << "有效采样时长(秒)," << std::fixed << std::setprecision(1)
           << session.validSampleDurationSeconds << "\r\n";
    output << "功耗数据覆盖率(%)," << std::fixed << std::setprecision(1)
           << session.powerCoveragePercent << "\r\n";
    output << (session.energyIncludesEstimate ? "估算用电量(kWh)," : "实际用电量(kWh),")
           << std::fixed << std::setprecision(6) << session.energyWh / 1000.0 << "\r\n";
    output << "样本上限保护," << (session.sampleLimitReached ? "已触发" : "未触发") << "\r\n\r\n";

    output << "硬件信息\r\n";
    output << "项目,值\r\n";
    output << "Windows," << CsvEscape(session.hardware.windowsVersion) << "\r\n";
    output << "CPU," << CsvEscape(session.hardware.cpuName) << "\r\n";
    output << "GPU," << CsvEscape(session.hardware.gpuName) << "\r\n";
    output << "总内存(GB)," << NumberOrNa(session.hardware.totalMemoryGb > 0.0,
                                            session.hardware.totalMemoryGb, 2) << "\r\n";
    output << "显示器," << CsvEscape(session.hardware.displayMode) << "\r\n\r\n";

    output << "指标统计\r\n";
    output << "指标,平均值,最低值,最高值,有效样本数\r\n";
    for (size_t index = 0; index < static_cast<size_t>(MetricId::Count); ++index) {
        const MetricId metric = static_cast<MetricId>(index);
        const MetricStatistics& stats = session.statistics[index];
        const bool valid = stats.validSamples > 0;
        output << CsvEscape(MetricName(metric)) << ','
               << NumberOrNa(valid, stats.average) << ','
               << NumberOrNa(valid, stats.minimum) << ','
               << NumberOrNa(valid, stats.maximum) << ','
               << stats.validSamples << "\r\n";
    }
    output << "FPS 1% Low," << NumberOrNa(session.hasOnePercentLowFps,
                                            session.onePercentLowFps) << ",,,\r\n\r\n";

    output << "逐秒样本\r\n";
    output << "经过时间(秒),FPS,CPU占用(%),CPU温度(°C),CPU功耗(W),"
              "GPU占用(%),GPU温度(°C),GPU功耗(W),内存占用(%),内存已用(GB),"
              "显存占用(%),显存已用(GB),整机功耗(W),整机功耗是否估算,功耗置信度(%)\r\n";
    for (const GameSessionSample& sample : session.samples) {
        output << std::fixed << std::setprecision(3) << sample.elapsedSeconds << ','
               << NumberOrNa(sample.fpsValid, sample.fps) << ','
               << NumberOrNa(sample.cpuUsageValid, sample.cpuUsage) << ','
               << NumberOrNa(sample.cpuTemperatureValid, sample.cpuTemperature) << ','
               << NumberOrNa(sample.cpuPackagePowerValid, sample.cpuPackagePower) << ','
               << NumberOrNa(sample.gpuUsageValid, sample.gpuUsage) << ','
               << NumberOrNa(sample.gpuTemperatureValid, sample.gpuTemperature) << ','
               << NumberOrNa(sample.gpuPowerValid, sample.gpuPower) << ','
               << NumberOrNa(sample.ramUsagePercentValid, sample.ramUsagePercent) << ','
               << NumberOrNa(sample.ramUsedGbValid, sample.ramUsedGb) << ','
               << NumberOrNa(sample.vramUsagePercentValid, sample.vramUsagePercent) << ','
               << NumberOrNa(sample.vramUsedGbValid, sample.vramUsedGb) << ','
               << NumberOrNa(sample.systemPowerValid, sample.systemPower) << ','
               << (sample.systemPowerValid ? (sample.systemPowerEstimated ? "1" : "0") : "N/A") << ','
               << (sample.systemPowerValid ? std::to_string(sample.powerConfidence) : "N/A")
               << "\r\n";
    }
    return output.str();
}

std::wstring SanitizeFilename(const std::string& processName)
{
    std::wstring value = Utf8ToWide(processName);
    if (value.empty())
        value = L"game";
    for (wchar_t& character : value) {
        if (character < 32 || wcschr(L"<>:\"/\\|?*", character))
            character = L'_';
    }
    while (!value.empty() && (value.back() == L'.' || value.back() == L' '))
        value.pop_back();
    if (value.empty())
        value = L"game";
    if (value.size() > 96)
        value.resize(96);
    return value;
}

std::filesystem::path BuildCsvPath(const SessionData& session)
{
    wchar_t timestamp[32] = {};
    swprintf_s(timestamp, L"%04u%02u%02u-%02u%02u%02u",
               session.startLocal.wYear, session.startLocal.wMonth,
               session.startLocal.wDay, session.startLocal.wHour,
               session.startLocal.wMinute, session.startLocal.wSecond);
    return ReportDirectory() /
        (std::wstring(timestamp) + L"_" + SanitizeFilename(session.processName) + L".csv");
}

bool WriteCsvSafely(const SessionData& session, std::string& error)
{
    std::error_code fileError;
    std::filesystem::create_directories(session.csvPath.parent_path(), fileError);
    if (fileError) {
        error = "无法创建报告目录：" + fileError.message();
        return false;
    }

    std::filesystem::path temporary = session.csvPath;
    temporary += L".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            error = "无法创建临时报告文件";
            return false;
        }
        static constexpr unsigned char bom[] = {0xEF, 0xBB, 0xBF};
        file.write(reinterpret_cast<const char*>(bom), sizeof(bom));
        const std::string csv = BuildCsv(session);
        file.write(csv.data(), static_cast<std::streamsize>(csv.size()));
        file.flush();
        if (!file) {
            error = "写入报告文件失败";
            file.close();
            std::filesystem::remove(temporary, fileError);
            return false;
        }
    }

    if (!MoveFileExW(temporary.c_str(), session.csvPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        error = "提交报告文件失败，Windows 错误码 " + std::to_string(code);
        std::filesystem::remove(temporary, fileError);
        return false;
    }
    return true;
}

void NormalizeCsvLine(std::string& line)
{
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    if (line.size() >= 3 &&
        static_cast<unsigned char>(line[0]) == 0xEF &&
        static_cast<unsigned char>(line[1]) == 0xBB &&
        static_cast<unsigned char>(line[2]) == 0xBF) {
        line.erase(0, 3);
    }
}

std::vector<std::string> ParseCsvRow(const std::string& line)
{
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;
    for (size_t index = 0; index < line.size(); ++index) {
        const char character = line[index];
        if (quoted) {
            if (character == '"') {
                if (index + 1 < line.size() && line[index + 1] == '"') {
                    field.push_back('"');
                    ++index;
                } else {
                    quoted = false;
                }
            } else {
                field.push_back(character);
            }
        } else if (character == '"') {
            quoted = true;
        } else if (character == ',') {
            fields.push_back(std::move(field));
            field.clear();
        } else {
            field.push_back(character);
        }
    }
    fields.push_back(std::move(field));
    return fields;
}

bool ParseNumber(const std::string& text, double& value)
{
    if (text.empty() || text == "N/A")
        return false;
    char* end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || !std::isfinite(parsed))
        return false;
    while (*end == ' ' || *end == '\t')
        ++end;
    if (*end != '\0')
        return false;
    value = parsed;
    return true;
}

bool ParseUnsignedNumber(const std::string& text, unsigned long& value)
{
    if (text.empty() || text == "N/A")
        return false;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
    if (end == text.c_str())
        return false;
    while (*end == ' ' || *end == '\t')
        ++end;
    if (*end != '\0')
        return false;
    value = parsed;
    return true;
}

bool ParseLocalTimeText(const std::string& text, SYSTEMTIME& value)
{
    unsigned int year = 0;
    unsigned int month = 0;
    unsigned int day = 0;
    unsigned int hour = 0;
    unsigned int minute = 0;
    unsigned int second = 0;
    if (sscanf_s(text.c_str(), "%u-%u-%u %u:%u:%u",
                 &year, &month, &day, &hour, &minute, &second) != 6 ||
        year < 2000 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour > 23 || minute > 59 || second > 59) {
        return false;
    }
    value = {};
    value.wYear = static_cast<WORD>(year);
    value.wMonth = static_cast<WORD>(month);
    value.wDay = static_cast<WORD>(day);
    value.wHour = static_cast<WORD>(hour);
    value.wMinute = static_cast<WORD>(minute);
    value.wSecond = static_cast<WORD>(second);
    return true;
}

bool ReadHistoryEntry(const std::filesystem::path& path,
                      HistoryEntry& entry, std::string& error)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "无法读取历史报告";
        return false;
    }

    entry.path = path;
    entry.gameName = WideToUtf8(path.stem().wstring());
    entry.processName = "N/A";
    entry.startTime = "N/A";
    entry.duration = "N/A";
    entry.averageFps = "N/A";
    bool inStatistics = false;
    std::string line;
    size_t inspectedLines = 0;
    while (inspectedLines++ < 96 && std::getline(file, line)) {
        NormalizeCsvLine(line);
        if (line == "指标统计") {
            inStatistics = true;
            continue;
        }
        if (line == "逐秒样本")
            break;
        const std::vector<std::string> fields = ParseCsvRow(line);
        if (fields.size() < 2)
            continue;
        if (fields[0] == "游戏名称")
            entry.gameName = fields[1];
        else if (fields[0] == "游戏进程")
            entry.processName = fields[1];
        else if (fields[0] == "开始时间")
            entry.startTime = fields[1];
        else if (fields[0] == "游戏时长")
            entry.duration = fields[1];
        else if (inStatistics && fields[0] == "FPS")
            entry.averageFps = fields[1];
    }
    return true;
}

std::vector<HistoryEntry> ScanHistoryEntries(std::string& error)
{
    struct Candidate {
        std::filesystem::path path;
        std::filesystem::file_time_type modifiedTime = {};
        uintmax_t sizeBytes = 0;
    };

    std::vector<Candidate> candidates;
    std::error_code fileError;
    const std::filesystem::path directory = ReportDirectory();
    if (!std::filesystem::exists(directory, fileError))
        return {};
    if (fileError) {
        error = "无法访问历史报告目录：" + fileError.message();
        return {};
    }

    for (std::filesystem::directory_iterator iterator(directory, fileError), end;
         !fileError && iterator != end; iterator.increment(fileError)) {
        const std::filesystem::directory_entry& item = *iterator;
        std::error_code itemError;
        if (!item.is_regular_file(itemError) || itemError)
            continue;
        const std::wstring extension = item.path().extension().wstring();
        if (_wcsicmp(extension.c_str(), L".csv") != 0)
            continue;
        Candidate candidate;
        candidate.path = item.path();
        candidate.modifiedTime = item.last_write_time(itemError);
        if (itemError)
            candidate.modifiedTime = {};
        itemError.clear();
        candidate.sizeBytes = item.file_size(itemError);
        if (itemError)
            candidate.sizeBytes = 0;
        candidates.push_back(std::move(candidate));
    }
    if (fileError) {
        error = "扫描历史报告失败：" + fileError.message();
        return {};
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& left, const Candidate& right) {
                  return left.modifiedTime > right.modifiedTime;
              });
    if (candidates.size() > kMaxHistoryEntries)
        candidates.resize(kMaxHistoryEntries);

    std::vector<HistoryEntry> entries;
    entries.reserve(candidates.size());
    size_t invalidFiles = 0;
    for (const Candidate& candidate : candidates) {
        HistoryEntry entry;
        std::string entryError;
        if (!ReadHistoryEntry(candidate.path, entry, entryError)) {
            ++invalidFiles;
            continue;
        }
        entry.modifiedTime = candidate.modifiedTime;
        entry.sizeBytes = candidate.sizeBytes;
        entries.push_back(std::move(entry));
    }
    if (entries.empty() && invalidFiles > 0)
        error = "历史报告文件无法读取";
    return entries;
}

std::shared_ptr<SessionData> LoadSessionCsv(const std::filesystem::path& path,
                                            std::string& error)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "无法打开所选历史报告";
        return {};
    }

    enum class Section { Summary, Hardware, Statistics, Samples };
    Section section = Section::Summary;
    auto session = std::make_shared<SessionData>();
    session->csvPath = path;
    std::string line;
    while (std::getline(file, line)) {
        NormalizeCsvLine(line);
        if (line == "硬件信息") {
            section = Section::Hardware;
            continue;
        }
        if (line == "指标统计") {
            section = Section::Statistics;
            continue;
        }
        if (line == "逐秒样本") {
            section = Section::Samples;
            continue;
        }

        const std::vector<std::string> fields = ParseCsvRow(line);
        if (fields.size() < 2)
            continue;
        if (section == Section::Summary) {
            if (fields[0] == "游戏名称")
                session->gameName = fields[1];
            else if (fields[0] == "游戏进程")
                session->processName = fields[1];
            else if (fields[0] == "游戏 PID") {
                unsigned long processId = 0;
                if (ParseUnsignedNumber(fields[1], processId))
                    session->processId = static_cast<DWORD>(processId);
            } else if (fields[0] == "开始时间") {
                ParseLocalTimeText(fields[1], session->startLocal);
            } else if (fields[0] == "结束时间") {
                ParseLocalTimeText(fields[1], session->endLocal);
            } else if (fields[0] == "样本上限保护") {
                session->sampleLimitReached = fields[1] == "已触发";
            }
            continue;
        }
        if (section == Section::Hardware) {
            if (fields[0] == "Windows")
                session->hardware.windowsVersion = fields[1];
            else if (fields[0] == "CPU")
                session->hardware.cpuName = fields[1];
            else if (fields[0] == "GPU")
                session->hardware.gpuName = fields[1];
            else if (fields[0] == "总内存(GB)") {
                double totalMemory = 0.0;
                if (ParseNumber(fields[1], totalMemory) && totalMemory > 0.0)
                    session->hardware.totalMemoryGb = totalMemory;
            } else if (fields[0] == "显示器") {
                session->hardware.displayMode = fields[1];
            }
            continue;
        }
        if (section != Section::Samples || fields[0] == "经过时间(秒)" ||
            fields.size() < 15 || session->samples.size() >= kMaxSamples) {
            continue;
        }

        double parsed = 0.0;
        if (!ParseNumber(fields[0], parsed) || parsed < 0.0)
            continue;
        GameSessionSample sample;
        sample.elapsedSeconds = parsed;
        auto readFloat = [&fields](size_t index, float& output) {
            double value = 0.0;
            if (index >= fields.size() || !ParseNumber(fields[index], value))
                return false;
            output = static_cast<float>(value);
            return IsFinite(output);
        };
        sample.fpsValid = readFloat(1, sample.fps) && sample.fps > 0.0f;
        sample.cpuUsageValid = readFloat(2, sample.cpuUsage) &&
                               sample.cpuUsage >= 0.0f && sample.cpuUsage <= 100.0f;
        sample.cpuTemperatureValid = readFloat(3, sample.cpuTemperature) &&
                                     ValidTemperature(sample.cpuTemperature);
        sample.cpuPackagePowerValid = readFloat(4, sample.cpuPackagePower) &&
                                      ValidPower(sample.cpuPackagePower);
        sample.gpuUsageValid = readFloat(5, sample.gpuUsage) &&
                               sample.gpuUsage >= 0.0f && sample.gpuUsage <= 100.0f;
        sample.gpuTemperatureValid = readFloat(6, sample.gpuTemperature) &&
                                     ValidTemperature(sample.gpuTemperature);
        sample.gpuPowerValid = readFloat(7, sample.gpuPower) &&
                               ValidPower(sample.gpuPower);
        sample.ramUsagePercentValid = readFloat(8, sample.ramUsagePercent) &&
                                      sample.ramUsagePercent >= 0.0f &&
                                      sample.ramUsagePercent <= 100.0f;
        sample.ramUsedGbValid = readFloat(9, sample.ramUsedGb) && sample.ramUsedGb >= 0.0f;
        sample.vramUsagePercentValid = readFloat(10, sample.vramUsagePercent) &&
                                       sample.vramUsagePercent >= 0.0f &&
                                       sample.vramUsagePercent <= 100.0f;
        sample.vramUsedGbValid = readFloat(11, sample.vramUsedGb) && sample.vramUsedGb >= 0.0f;
        sample.systemPowerValid = readFloat(12, sample.systemPower) &&
                                  ValidPower(sample.systemPower);
        sample.systemPowerEstimated = sample.systemPowerValid && fields[13] == "1";
        unsigned long confidence = 0;
        if (sample.systemPowerValid && ParseUnsignedNumber(fields[14], confidence))
            sample.powerConfidence = std::clamp(static_cast<int>(confidence), 0, 100);
        session->samples.push_back(sample);
    }

    if (session->samples.empty()) {
        error = "历史报告没有可用的逐秒样本";
        return {};
    }
    if (session->processName.empty())
        session->processName = WideToUtf8(path.stem().wstring());
    if (session->gameName.empty())
        session->gameName = session->processName;
    session->durationSeconds = session->samples.back().elapsedSeconds;
    CalculateSessionStatistics(*session);
    return session;
}

const MetricStatistics& Stats(const SessionData& session, MetricId metric)
{
    return session.statistics[static_cast<size_t>(metric)];
}

std::string FormatMetricAverage(const MetricStatistics& stats,
                                const char* suffix, int precision = 1)
{
    if (stats.validSamples == 0)
        return "N/A";
    char buffer[64] = {};
    snprintf(buffer, sizeof(buffer), precision == 0 ? "%.0f%s" : "%.1f%s",
             stats.average, suffix);
    return buffer;
}

void DrawSummaryTile(const char* id, const char* label, const std::string& value,
                     const std::string& detail, const ImVec4& color,
                     const MetricStatistics& primaryStats, const char* suffix,
                     const MetricStatistics* secondaryStats = nullptr,
                     const char* secondaryLabel = nullptr,
                     const char* secondarySuffix = "")
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, SettingsUi::Surface2());
    ImGui::PushStyleColor(ImGuiCol_Border, SettingsUi::Hairline());
    ImGui::BeginChild(id, ImVec2(0.0f, 96.0f),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 tileStart = ImGui::GetWindowPos();
    const ImVec2 tileSize = ImGui::GetWindowSize();
    ImGui::GetWindowDrawList()->AddRectFilled(
        tileStart, ImVec2(tileStart.x + 2.0f, tileStart.y + tileSize.y),
        ImGui::ColorConvertFloat4ToU32(color), 1.0f);
    SettingsUi::Muted("%s", label);
    ImGui::TextColored(color, "%s", value.c_str());
    if (!detail.empty())
        SettingsUi::Muted("%s", detail.c_str());
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(label);
        if (primaryStats.validSamples > 0) {
            ImGui::Text("平均  %.1f%s", primaryStats.average, suffix);
            ImGui::Text("最低  %.1f%s", primaryStats.minimum, suffix);
            ImGui::Text("最高  %.1f%s", primaryStats.maximum, suffix);
            ImGui::Text("有效样本  %zu", primaryStats.validSamples);
        } else {
            ImGui::TextUnformatted("N/A");
        }
        if (secondaryStats && secondaryLabel) {
            ImGui::Separator();
            ImGui::TextUnformatted(secondaryLabel);
            if (secondaryStats->validSamples > 0) {
                ImGui::Text("平均  %.1f%s", secondaryStats->average, secondarySuffix);
                ImGui::Text("最低  %.1f%s", secondaryStats->minimum, secondarySuffix);
                ImGui::Text("最高  %.1f%s", secondaryStats->maximum, secondarySuffix);
                ImGui::Text("有效样本  %zu", secondaryStats->validSamples);
            } else {
                ImGui::TextUnformatted("N/A");
            }
        }
        ImGui::EndTooltip();
    }
}

void DrawDashedHorizontal(ImDrawList* drawList, float x1, float x2, float y,
                          ImU32 color, float dashLength = 6.0f,
                          float gapLength = 5.0f)
{
    for (float x = x1; x < x2; x += dashLength + gapLength)
        drawList->AddLine(ImVec2(x, y), ImVec2((std::min)(x + dashLength, x2), y), color, 1.0f);
}

void DrawSessionPlot(const SessionData& session, MetricId metric)
{
    const MetricStatistics& stats = Stats(session, metric);
    if (stats.validSamples == 0) {
        SettingsUi::MutedWrapped("本局没有可用的%s数据。传感器缺失时不会用 0 代替。",
                                 MetricName(metric));
        return;
    }

    const float height = 280.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size((std::max)(200.0f, ImGui::GetContentRegionAvail().x), height);
    ImGui::InvisibleButton("##game_session_plot", size);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 boundsMin = origin;
    const ImVec2 boundsMax(origin.x + size.x, origin.y + size.y);
    drawList->AddRectFilled(boundsMin, boundsMax, IM_COL32(16, 20, 27, 245), 6.0f);
    drawList->AddRect(boundsMin, boundsMax, IM_COL32(48, 58, 73, 230), 6.0f);

    constexpr float leftPadding = 48.0f;
    constexpr float rightPadding = 12.0f;
    constexpr float topPadding = 18.0f;
    constexpr float bottomPadding = 28.0f;
    const float plotLeft = boundsMin.x + leftPadding;
    const float plotRight = boundsMax.x - rightPadding;
    const float plotTop = boundsMin.y + topPadding;
    const float plotBottom = boundsMax.y - bottomPadding;
    const float plotWidth = (std::max)(1.0f, plotRight - plotLeft);
    const float plotHeight = (std::max)(1.0f, plotBottom - plotTop);

    double yMin = stats.minimum;
    double yMax = stats.maximum;
    const double padding = (std::max)(1.0, (yMax - yMin) * 0.10);
    yMin -= padding;
    yMax += padding;
    if (metric == MetricId::CpuUsage || metric == MetricId::GpuUsage ||
        metric == MetricId::RamUsagePercent || metric == MetricId::VramUsagePercent) {
        yMin = (std::max)(0.0, yMin);
        yMax = (std::min)(100.0, (std::max)(yMin + 1.0, yMax));
    }
    const double xMax = (std::max)(1.0, session.durationSeconds);

    const auto mapX = [&](double elapsed) {
        return plotLeft + static_cast<float>(std::clamp(elapsed / xMax, 0.0, 1.0)) * plotWidth;
    };
    const auto mapY = [&](double value) {
        const double normalized = std::clamp((value - yMin) / (yMax - yMin), 0.0, 1.0);
        return plotBottom - static_cast<float>(normalized) * plotHeight;
    };

    for (int line = 0; line <= 4; ++line) {
        const float fraction = static_cast<float>(line) / 4.0f;
        const float y = plotTop + fraction * plotHeight;
        drawList->AddLine(ImVec2(plotLeft, y), ImVec2(plotRight, y),
                          IM_COL32(63, 73, 88, 105), 1.0f);
        const double value = yMax - static_cast<double>(fraction) * (yMax - yMin);
        char label[32] = {};
        snprintf(label, sizeof(label), "%.0f", value);
        drawList->AddText(ImVec2(boundsMin.x + 7.0f, y - 7.0f),
                          IM_COL32(132, 146, 174, 220), label);
    }

    DrawDashedHorizontal(drawList, plotLeft, plotRight, mapY(stats.average),
                         IM_COL32(86, 184, 255, 230));
    DrawDashedHorizontal(drawList, plotLeft, plotRight, mapY(stats.minimum),
                         IM_COL32(255, 174, 79, 180), 3.0f, 6.0f);
    DrawDashedHorizontal(drawList, plotLeft, plotRight, mapY(stats.maximum),
                         IM_COL32(118, 225, 179, 180), 3.0f, 6.0f);

    bool havePrevious = false;
    ImVec2 previousPoint;
    double previousElapsed = 0.0;
    for (const GameSessionSample& sample : session.samples) {
        const auto [value, valid] = MetricValue(sample, metric);
        if (!valid) {
            havePrevious = false;
            continue;
        }
        const ImVec2 point(mapX(sample.elapsedSeconds), mapY(value));
        if (havePrevious && sample.elapsedSeconds - previousElapsed <= 3.0)
            drawList->AddLine(previousPoint, point, IM_COL32(104, 155, 255, 245), 2.0f);
        previousPoint = point;
        previousElapsed = sample.elapsedSeconds;
        havePrevious = true;
    }

    drawList->AddText(ImVec2(plotLeft, plotBottom + 7.0f),
                      IM_COL32(132, 146, 174, 220), "00:00");
    const std::string endTime = FormatDurationShort(session.durationSeconds);
    const ImVec2 endSize = ImGui::CalcTextSize(endTime.c_str());
    drawList->AddText(ImVec2(plotRight - endSize.x, plotBottom + 7.0f),
                      IM_COL32(132, 146, 174, 220), endTime.c_str());

    if (ImGui::IsItemHovered()) {
        const float mouseX = std::clamp(ImGui::GetMousePos().x, plotLeft, plotRight);
        const double targetElapsed = (mouseX - plotLeft) / plotWidth * xMax;
        const GameSessionSample* nearest = nullptr;
        double nearestDistance = (std::numeric_limits<double>::max)();
        for (const GameSessionSample& sample : session.samples) {
            const auto [value, valid] = MetricValue(sample, metric);
            (void)value;
            if (!valid)
                continue;
            const double distance = std::fabs(sample.elapsedSeconds - targetElapsed);
            if (distance < nearestDistance) {
                nearest = &sample;
                nearestDistance = distance;
            }
        }
        if (nearest) {
            const auto [value, valid] = MetricValue(*nearest, metric);
            if (valid) {
                const float x = mapX(nearest->elapsedSeconds);
                const float y = mapY(value);
                drawList->AddLine(ImVec2(x, plotTop), ImVec2(x, plotBottom),
                                  IM_COL32(170, 190, 224, 155), 1.0f);
                drawList->AddCircleFilled(ImVec2(x, y), 4.0f,
                                          IM_COL32(135, 102, 255, 255));
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(FormatDurationShort(nearest->elapsedSeconds).c_str());
                ImGui::Text("%s  %.2f", MetricName(metric), value);
                ImGui::EndTooltip();
            }
        }
    }

    SettingsUi::Muted("参考线：平均 %.1f · 最低 %.1f · 最高 %.1f · 有效样本 %zu",
                      stats.average, stats.minimum, stats.maximum, stats.validSamples);
}

} // namespace

struct GameSessionReportFeature::Impl {
    bool enabled = true;
    bool autoOpen = true;
    bool saveCsv = true;
    bool initialized = false;
    bool shutdown = false;
    std::atomic<bool> openRequested{false};

    DWORD candidatePid = 0;
    ULONGLONG candidateSinceTick = 0;
    SYSTEMTIME candidateStartLocal = {};
    std::string candidateProcessName;
    std::string candidateGameName;
    ULONGLONG lastUpdateTick = 0;

    std::unique_ptr<SessionData> activeSession;
    std::shared_ptr<const SessionData> lastCompletedSession;
    int selectedPlotMetric = 0;

    std::mutex hardwareMutex;
    HardwareInfo hardwareInfo;
    std::thread hardwareThread;
    bool hardwareStarted = false;

    std::mutex writerMutex;
    std::condition_variable writerCondition;
    std::deque<CsvJob> writerQueue;
    std::thread writerThread;
    bool writerStarted = false;
    bool stopWriter = false;
    std::mutex writerStatusMutex;
    std::string lastWriterError;
    std::filesystem::path lastWrittenPath;

    mutable std::mutex historyMutex;
    std::condition_variable historyCondition;
    std::vector<HistoryEntry> historyEntries;
    std::shared_ptr<const SessionData> selectedHistorySession;
    std::filesystem::path selectedHistoryPath;
    std::filesystem::path pendingHistoryLoad;
    std::thread historyThread;
    bool historyStarted = false;
    bool stopHistory = false;
    bool historyRefreshRequested = false;
    bool historyBusy = false;
    uint64_t historyGeneration = 0;
    std::string historyError;

    void StartHardwareQuery()
    {
        if (!initialized || hardwareStarted)
            return;
        hardwareStarted = true;
        try {
            hardwareThread = std::thread([this]() {
                try {
                    HardwareInfo queried = QueryStaticHardwareInfo();
                    std::lock_guard<std::mutex> lock(hardwareMutex);
                    hardwareInfo = std::move(queried);
                } catch (...) {
                    OutputDebugStringA("GameSessionReportFeature: hardware query failed.\n");
                }
            });
        } catch (...) {
            OutputDebugStringA("GameSessionReportFeature: hardware worker could not start.\n");
        }
    }

    HardwareInfo HardwareSnapshot()
    {
        std::lock_guard<std::mutex> lock(hardwareMutex);
        return hardwareInfo;
    }

    void HistoryWorkerLoop()
    {
        for (;;) {
            bool refresh = false;
            bool selectedLoaded = false;
            uint64_t requestGeneration = 0;
            std::filesystem::path requestedLoad;
            std::filesystem::path selectedPathSnapshot;
            {
                std::unique_lock<std::mutex> lock(historyMutex);
                historyCondition.wait(lock, [this]() {
                    return stopHistory || historyRefreshRequested ||
                           !pendingHistoryLoad.empty();
                });
                if (stopHistory)
                    break;
                refresh = historyRefreshRequested;
                historyRefreshRequested = false;
                requestedLoad = std::move(pendingHistoryLoad);
                pendingHistoryLoad.clear();
                selectedPathSnapshot = selectedHistoryPath;
                selectedLoaded = selectedHistorySession != nullptr;
                requestGeneration = historyGeneration;
                historyBusy = true;
            }

            std::vector<HistoryEntry> scannedEntries;
            std::string scanError;
            bool clearSelection = false;
            if (refresh) {
                scannedEntries = ScanHistoryEntries(scanError);
                const bool selectionStillExists =
                    !selectedPathSnapshot.empty() &&
                    std::any_of(scannedEntries.begin(), scannedEntries.end(),
                                [&selectedPathSnapshot](const HistoryEntry& entry) {
                                    return entry.path == selectedPathSnapshot;
                                });
                if (!selectionStillExists) {
                    clearSelection = true;
                    if (requestedLoad.empty() && !scannedEntries.empty())
                        requestedLoad = scannedEntries.front().path;
                } else if (!selectedLoaded && requestedLoad.empty()) {
                    requestedLoad = selectedPathSnapshot;
                }
            }

            std::shared_ptr<SessionData> loadedSession;
            std::string loadError;
            if (!requestedLoad.empty())
                loadedSession = LoadSessionCsv(requestedLoad, loadError);

            {
                std::lock_guard<std::mutex> lock(historyMutex);
                if (refresh)
                    historyEntries = std::move(scannedEntries);
                if (requestGeneration == historyGeneration) {
                    if (clearSelection && !loadedSession) {
                        selectedHistorySession.reset();
                        selectedHistoryPath.clear();
                    }
                    if (loadedSession) {
                        selectedHistorySession = std::move(loadedSession);
                        selectedHistoryPath = requestedLoad;
                    }
                    historyError = !loadError.empty() ? loadError : scanError;
                } else if (!scanError.empty()) {
                    historyError = scanError;
                }
                historyBusy = historyRefreshRequested || !pendingHistoryLoad.empty();
            }
        }
    }

    void StartHistoryWorker()
    {
        if (historyStarted)
            return;
        {
            std::lock_guard<std::mutex> lock(historyMutex);
            stopHistory = false;
            historyRefreshRequested = true;
            historyBusy = true;
            historyError.clear();
        }
        try {
            historyThread = std::thread([this]() { HistoryWorkerLoop(); });
            historyStarted = true;
        } catch (const std::exception& exception) {
            std::lock_guard<std::mutex> lock(historyMutex);
            historyBusy = false;
            historyError = std::string("无法启动历史记录线程：") + exception.what();
        } catch (...) {
            std::lock_guard<std::mutex> lock(historyMutex);
            historyBusy = false;
            historyError = "无法启动历史记录线程";
        }
    }

    void QueueHistoryRefresh()
    {
        std::lock_guard<std::mutex> lock(historyMutex);
        if (!historyStarted || stopHistory)
            return;
        historyRefreshRequested = true;
        historyBusy = true;
        historyError.clear();
        historyCondition.notify_one();
    }

    void QueueHistoryLoad(const std::filesystem::path& path)
    {
        if (path.empty())
            return;
        std::lock_guard<std::mutex> lock(historyMutex);
        if (!historyStarted || stopHistory)
            return;
        ++historyGeneration;
        pendingHistoryLoad = path;
        historyBusy = true;
        historyError.clear();
        historyCondition.notify_one();
    }

    void SelectCurrentSession()
    {
        std::lock_guard<std::mutex> lock(historyMutex);
        selectedHistorySession.reset();
        selectedHistoryPath.clear();
        pendingHistoryLoad.clear();
        ++historyGeneration;
        historyError.clear();
    }

    bool HasHistory() const
    {
        std::lock_guard<std::mutex> lock(historyMutex);
        return !historyEntries.empty() || selectedHistorySession != nullptr;
    }

    void ResetCandidate()
    {
        candidatePid = 0;
        candidateSinceTick = 0;
        candidateStartLocal = {};
        candidateProcessName.clear();
        candidateGameName.clear();
    }

    void StartCandidate(const FeatureContext& context, ULONGLONG now)
    {
        candidatePid = context.gameProcessId;
        candidateSinceTick = now;
        candidateStartLocal = HasTimeValue(context.currentTime)
            ? context.currentTime : CurrentLocalTime();
        candidateProcessName = context.gameProcessName;
        candidateGameName = context.isInGame ? context.foregroundWindowTitle : "";
    }

    void RefreshCandidateIdentity(const FeatureContext& context)
    {
        if (context.gameProcessName[0])
            candidateProcessName = context.gameProcessName;
        if (context.isInGame && context.foregroundWindowTitle[0])
            candidateGameName = context.foregroundWindowTitle;
    }

    void BeginSession(const FeatureContext& context, ULONGLONG now)
    {
        auto session = std::make_unique<SessionData>();
        session->processId = candidatePid;
        session->processName = QueryProcessFilename(candidatePid);
        if (session->processName.empty())
            session->processName = candidateProcessName.empty() ? "unknown.exe" : candidateProcessName;
        session->gameName = candidateGameName;
        if (session->gameName.empty())
            session->gameName = FindWindowTitle(candidatePid);
        if (session->gameName.empty())
            session->gameName = session->processName;
        session->startLocal = candidateStartLocal;
        session->startTick = candidateSinceTick;
        session->lastTargetTick = now;
        session->lastTargetLocal = HasTimeValue(context.currentTime)
            ? context.currentTime : CurrentLocalTime();
        session->hardware = HardwareSnapshot();
        if (context.cpuName[0])
            session->hardware.cpuName = context.cpuName;
        if (context.gpuName[0])
            session->hardware.gpuName = context.gpuName;
        if (context.ramTotalGb > 0.0f)
            session->hardware.totalMemoryGb = context.ramTotalGb;
        session->samples.reserve((std::min)(kMaxSamples, static_cast<size_t>(3600)));
        activeSession = std::move(session);
        ResetCandidate();
    }

    GameSessionSample MakeSample(const FeatureContext& context, ULONGLONG now) const
    {
        GameSessionSample sample;
        sample.elapsedSeconds = SessionElapsedSeconds(*activeSession, now);
        sample.fpsValid = sample.elapsedSeconds >= 2.0 &&
                          IsFinite(context.fps) && context.fps > 0.0f;
        if (sample.fpsValid)
            sample.fps = context.fps;

        sample.cpuUsageValid = IsFinite(context.cpuUsage);
        if (sample.cpuUsageValid)
            sample.cpuUsage = ClampUsage(context.cpuUsage);

        const bool hardwareFresh = !context.hardwareSensorDataStale;
        sample.cpuTemperatureValid = hardwareFresh && context.hasCpuTemp &&
                                     ValidTemperature(context.cpuTempC);
        if (sample.cpuTemperatureValid)
            sample.cpuTemperature = context.cpuTempC;
        sample.cpuPackagePowerValid = hardwareFresh && context.hasCpuPackagePower &&
                                      ValidPower(context.cpuPackagePowerW);
        if (sample.cpuPackagePowerValid)
            sample.cpuPackagePower = context.cpuPackagePowerW;

        sample.gpuUsageValid = hardwareFresh && context.hasGpuUsage &&
                               IsFinite(context.gpuUsage);
        if (sample.gpuUsageValid)
            sample.gpuUsage = ClampUsage(context.gpuUsage);
        sample.gpuTemperatureValid = hardwareFresh && context.hasGpuTemp &&
                                     ValidTemperature(context.gpuTempC);
        if (sample.gpuTemperatureValid)
            sample.gpuTemperature = context.gpuTempC;
        sample.gpuPowerValid = hardwareFresh && context.hasGpuPower &&
                               ValidPower(context.gpuPowerW);
        if (sample.gpuPowerValid)
            sample.gpuPower = context.gpuPowerW;

        sample.ramUsedGbValid = IsFinite(context.ramUsedGb) && context.ramUsedGb >= 0.0f &&
                                IsFinite(context.ramTotalGb) && context.ramTotalGb > 0.0f;
        if (sample.ramUsedGbValid) {
            sample.ramUsedGb = context.ramUsedGb;
            sample.ramUsagePercent = ClampUsage(context.ramUsedGb / context.ramTotalGb * 100.0f);
            sample.ramUsagePercentValid = true;
        }
        sample.vramUsedGbValid = hardwareFresh && IsFinite(context.vramUsedGb) &&
                                 context.vramUsedGb >= 0.0f &&
                                 IsFinite(context.vramTotalGb) && context.vramTotalGb > 0.0f;
        if (sample.vramUsedGbValid) {
            sample.vramUsedGb = context.vramUsedGb;
            sample.vramUsagePercent = ClampUsage(context.vramUsedGb / context.vramTotalGb * 100.0f);
            sample.vramUsagePercentValid = true;
        }

        const bool estimatedPowerInputsFresh =
            !context.systemPowerEstimated || hardwareFresh;
        sample.systemPowerValid = estimatedPowerInputsFresh &&
                                  context.hasEstimatedSystemPower &&
                                  ValidPower(context.estimatedSystemPowerW);
        if (sample.systemPowerValid) {
            sample.systemPower = context.estimatedSystemPowerW;
            sample.systemPowerEstimated = context.systemPowerEstimated;
            sample.powerConfidence = std::clamp(context.systemPowerEstimateConfidence, 0, 100);
        }
        return sample;
    }

    void Sample(const FeatureContext& context, ULONGLONG now)
    {
        if (!activeSession || now - activeSession->lastSampleTick < kSampleIntervalMs)
            return;
        activeSession->lastSampleTick = now;
        if (activeSession->samples.size() >= kMaxSamples) {
            activeSession->sampleLimitReached = true;
            return;
        }
        activeSession->samples.push_back(MakeSample(context, now));
    }

    bool EnsureWriterStartedLocked()
    {
        if (writerStarted)
            return true;
        try {
            writerThread = std::thread([this]() {
                for (;;) {
                    CsvJob job;
                    {
                        std::unique_lock<std::mutex> lock(writerMutex);
                        writerCondition.wait(lock, [this]() {
                            return stopWriter || !writerQueue.empty();
                        });
                        if (writerQueue.empty() && stopWriter)
                            break;
                        job = std::move(writerQueue.front());
                        writerQueue.pop_front();
                    }

                    std::string error;
                    bool success = false;
                    try {
                        success = job.session && WriteCsvSafely(*job.session, error);
                    } catch (const std::exception& exception) {
                        error = std::string("保存报告异常：") + exception.what();
                    } catch (...) {
                        error = "保存报告时发生未知异常";
                    }
                    {
                        std::lock_guard<std::mutex> statusLock(writerStatusMutex);
                        if (success) {
                            lastWrittenPath = job.session->csvPath;
                            lastWriterError.clear();
                        } else {
                            lastWriterError = error.empty() ? "保存报告失败" : error;
                        }
                    }
                    if (success)
                        QueueHistoryRefresh();
                }
            });
            writerStarted = true;
            return true;
        } catch (const std::exception& exception) {
            std::lock_guard<std::mutex> statusLock(writerStatusMutex);
            lastWriterError = std::string("无法启动报告写入线程：") + exception.what();
        } catch (...) {
            std::lock_guard<std::mutex> statusLock(writerStatusMutex);
            lastWriterError = "无法启动报告写入线程";
        }
        return false;
    }

    void QueueCsv(const std::shared_ptr<const SessionData>& session)
    {
        if (!session)
            return;
        std::lock_guard<std::mutex> lock(writerMutex);
        if (!EnsureWriterStartedLocked())
            return;
        if (writerQueue.size() >= 4)
            writerQueue.pop_front();
        writerQueue.push_back(CsvJob{session});
        writerCondition.notify_one();
    }

    void FinalizeActive(ULONGLONG endTick, const SYSTEMTIME& endLocal,
                        bool allowOpenRequest)
    {
        if (!activeSession)
            return;
        SessionData& session = *activeSession;
        session.endTick = (std::max)(endTick, session.startTick);
        session.endLocal = HasTimeValue(endLocal) ? endLocal : CurrentLocalTime();
        session.durationSeconds = SessionElapsedSeconds(session, session.endTick);

        const HardwareInfo latestHardware = HardwareSnapshot();
        if (session.hardware.windowsVersion == "N/A")
            session.hardware.windowsVersion = latestHardware.windowsVersion;
        if (session.hardware.cpuName == "N/A")
            session.hardware.cpuName = latestHardware.cpuName;
        if (session.hardware.gpuName == "N/A")
            session.hardware.gpuName = latestHardware.gpuName;
        if (session.hardware.displayMode == "N/A")
            session.hardware.displayMode = latestHardware.displayMode;
        if (session.hardware.totalMemoryGb <= 0.0)
            session.hardware.totalMemoryGb = latestHardware.totalMemoryGb;

        CalculateSessionStatistics(session);
        session.csvPath = BuildCsvPath(session);
        auto completed = std::shared_ptr<SessionData>(std::move(activeSession));
        lastCompletedSession = completed;
        SelectCurrentSession();
        if (saveCsv && !completed->samples.empty())
            QueueCsv(completed);
        if (allowOpenRequest && autoOpen &&
            completed->validSampleDurationSeconds >= kAutoOpenDurationSeconds) {
            openRequested.store(true, std::memory_order_release);
        }
    }

    void DiscardActive()
    {
        activeSession.reset();
        ResetCandidate();
        openRequested.store(false, std::memory_order_release);
    }

    void Update(FeatureContext& context)
    {
        const ULONGLONG now = GetTickCount64();
        const bool targetEligible = !context.gameProcessExcludedFromSession &&
                                    context.gameProcessId != 0 &&
                                    (context.isInGame ||
                                     (IsFinite(context.fps) && context.fps > 0.0f));
        const bool longGap = lastUpdateTick != 0 && now - lastUpdateTick > kLongUpdateGapMs;
        lastUpdateTick = now;

        if (!enabled) {
            DiscardActive();
            return;
        }

        if (longGap) {
            ResetCandidate();
            if (activeSession && targetEligible &&
                context.gameProcessId == activeSession->processId) {
                const ULONGLONG gap = now - activeSession->lastTargetTick;
                activeSession->pausedMs += gap;
                activeSession->lastSampleTick = now;
                activeSession->missingSinceTick = 0;
            }
        }

        if (activeSession) {
            if (targetEligible && context.gameProcessId == activeSession->processId) {
                activeSession->missingSinceTick = 0;
                activeSession->lastTargetTick = now;
                activeSession->lastTargetLocal = HasTimeValue(context.currentTime)
                    ? context.currentTime : CurrentLocalTime();
                Sample(context, now);
                return;
            }

            if (targetEligible && context.gameProcessId != activeSession->processId) {
                const ULONGLONG endTick = activeSession->lastTargetTick;
                const SYSTEMTIME endLocal = activeSession->lastTargetLocal;
                // A direct game-to-game switch must not replace the in-game overlay
                // with the completed report page for the previous process.
                FinalizeActive(endTick, endLocal, false);
                StartCandidate(context, now);
                return;
            }

            if (activeSession->missingSinceTick == 0)
                activeSession->missingSinceTick = now;
            if (now - activeSession->lastProcessCheckTick >= 1000) {
                activeSession->lastProcessCheckTick = now;
                if (ProcessDefinitelyExited(activeSession->processId)) {
                    const ULONGLONG endTick = activeSession->lastTargetTick;
                    const SYSTEMTIME endLocal = activeSession->lastTargetLocal;
                    FinalizeActive(endTick, endLocal, true);
                    return;
                }
            }
            if (now - activeSession->missingSinceTick >= kTargetMissingGraceMs) {
                const ULONGLONG endTick = activeSession->lastTargetTick;
                const SYSTEMTIME endLocal = activeSession->lastTargetLocal;
                FinalizeActive(endTick, endLocal, true);
            }
            return;
        }

        if (!targetEligible) {
            ResetCandidate();
            return;
        }
        if (candidatePid != context.gameProcessId) {
            StartCandidate(context, now);
            return;
        }
        RefreshCandidateIdentity(context);
        if (now - candidateSinceTick >= kTargetStableMs) {
            BeginSession(context, now);
            Sample(context, now);
        }
    }

    void StopThreads()
    {
        if (hardwareThread.joinable())
            hardwareThread.join();
        {
            std::lock_guard<std::mutex> lock(writerMutex);
            stopWriter = true;
            writerCondition.notify_all();
        }
        if (writerThread.joinable())
            writerThread.join();
        {
            std::lock_guard<std::mutex> lock(historyMutex);
            stopHistory = true;
            historyCondition.notify_all();
        }
        if (historyThread.joinable())
            historyThread.join();
        historyStarted = false;
    }

#if defined(FPSOVERLAY_UI_QA)
    bool RunLifecycleSelfTest()
    {
        Impl test;
        test.saveCsv = false;
        test.autoOpen = false;
        FeatureContext context;
        context.gameProcessId = GetCurrentProcessId();
        context.isInGame = true;
        context.fps = 60.0f;
        context.cpuUsage = 25.0f;
        context.ramUsedGb = 8.0f;
        context.ramTotalGb = 32.0f;
        context.currentTime = CurrentLocalTime();
        snprintf(context.gameProcessName, sizeof(context.gameProcessName),
                 "%s", "qa-game.exe");
        snprintf(context.foregroundWindowTitle, sizeof(context.foregroundWindowTitle),
                 "%s", "QA Game");

        FeatureContext excludedContext = context;
        excludedContext.gameProcessExcludedFromSession = true;
        test.Update(excludedContext);
        if (test.candidatePid != 0 || test.activeSession)
            return false;

        test.Update(context);
        if (test.candidateSinceTick < kTargetStableMs)
            return false;
        test.candidateSinceTick -= kTargetStableMs;
        test.Update(context);
        if (!test.activeSession || test.activeSession->samples.empty())
            return false;

        FeatureContext stalePowerContext = context;
        stalePowerContext.hasEstimatedSystemPower = true;
        stalePowerContext.estimatedSystemPowerW = 100.0f;
        stalePowerContext.systemPowerEstimated = true;
        stalePowerContext.hardwareSensorDataStale = true;
        if (test.MakeSample(stalePowerContext, GetTickCount64()).systemPowerValid)
            return false;
        stalePowerContext.systemPowerEstimated = false;
        if (!test.MakeSample(stalePowerContext, GetTickCount64()).systemPowerValid)
            return false;

        FeatureContext missingContext = context;
        missingContext.gameProcessId = 0;
        missingContext.isInGame = false;
        missingContext.fps = 0.0f;
        test.Update(missingContext);
        if (!test.activeSession)
            return false;
        test.Update(context);

        context.gameProcessId += 1;
        test.Update(context);
        if (test.activeSession || !test.lastCompletedSession ||
            test.candidatePid != context.gameProcessId ||
            test.lastCompletedSession->samples.empty() ||
            test.openRequested.load(std::memory_order_acquire)) {
            return false;
        }
        const std::shared_ptr<const SessionData> previousReport = test.lastCompletedSession;
        test.candidateSinceTick -= kTargetStableMs;
        test.Update(context);
        if (!test.activeSession || test.lastCompletedSession != previousReport)
            return false;
        test.enabled = false;
        test.Update(context);
        if (test.activeSession || test.lastCompletedSession != previousReport)
            return false;

        Impl autoOpenTest;
        autoOpenTest.saveCsv = false;
        autoOpenTest.activeSession = std::make_unique<SessionData>();
        autoOpenTest.activeSession->processId = GetCurrentProcessId();
        autoOpenTest.activeSession->processName = "qa-game.exe";
        autoOpenTest.activeSession->gameName = "QA Game";
        autoOpenTest.activeSession->startLocal = CurrentLocalTime();
        autoOpenTest.activeSession->startTick = GetTickCount64();
        GameSessionSample first;
        first.elapsedSeconds = 2.0;
        first.fps = 60.0f;
        first.fpsValid = true;
        GameSessionSample last = first;
        last.elapsedSeconds = 32.0;
        autoOpenTest.activeSession->samples.push_back(first);
        autoOpenTest.activeSession->samples.push_back(last);
        autoOpenTest.FinalizeActive(autoOpenTest.activeSession->startTick + 32000,
                                    CurrentLocalTime(), true);
        if (!autoOpenTest.openRequested.load(std::memory_order_acquire))
            return false;

        Impl gameSwitchTest;
        gameSwitchTest.saveCsv = false;
        gameSwitchTest.autoOpen = true;
        gameSwitchTest.activeSession = std::make_unique<SessionData>();
        const ULONGLONG switchNow = GetTickCount64();
        gameSwitchTest.activeSession->processId = GetCurrentProcessId();
        gameSwitchTest.activeSession->processName = "qa-old-game.exe";
        gameSwitchTest.activeSession->gameName = "QA Old Game";
        gameSwitchTest.activeSession->startLocal = CurrentLocalTime();
        gameSwitchTest.activeSession->startTick = switchNow - 32000;
        gameSwitchTest.activeSession->lastTargetTick = switchNow;
        gameSwitchTest.activeSession->lastTargetLocal = CurrentLocalTime();
        gameSwitchTest.activeSession->samples.push_back(first);
        gameSwitchTest.activeSession->samples.push_back(last);
        FeatureContext nextGame = context;
        nextGame.gameProcessId += 1;
        gameSwitchTest.Update(nextGame);
        return gameSwitchTest.lastCompletedSession &&
               gameSwitchTest.candidatePid == nextGame.gameProcessId &&
               !gameSwitchTest.openRequested.load(std::memory_order_acquire);
    }

    bool RunPersistenceSelfTest()
    {
        Impl test;
        auto session = std::make_shared<SessionData>();
        session->processId = GetCurrentProcessId();
        session->processName = "测试游戏.exe";
        session->gameName = "中文路径 QA";
        session->startLocal = CurrentLocalTime();
        session->endLocal = session->startLocal;
        session->durationSeconds = 1.0;
        GameSessionSample sample;
        sample.elapsedSeconds = 1.0;
        sample.fps = 60.0f;
        sample.fpsValid = true;
        session->samples.push_back(sample);
        CalculateSessionStatistics(*session);
        session->csvPath = ReportDirectory() /
            (L"QA_中文路径_" + std::to_wstring(GetCurrentProcessId()) + L".csv");
        test.QueueCsv(session);
        test.StopThreads();

        unsigned char bom[3] = {};
        std::ifstream file(session->csvPath, std::ios::binary);
        file.read(reinterpret_cast<char*>(bom), sizeof(bom));
        const bool validBom = file.gcount() == static_cast<std::streamsize>(sizeof(bom)) &&
                              bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF;
        file.close();
        std::string loadError;
        const std::shared_ptr<SessionData> loaded =
            LoadSessionCsv(session->csvPath, loadError);
        HistoryEntry historyEntry;
        std::string entryReadError;
        const bool historyEntryValid =
            ReadHistoryEntry(session->csvPath, historyEntry, entryReadError);
        std::error_code removeError;
        std::filesystem::remove(session->csvPath, removeError);
        return validBom && test.lastWriterError.empty() && loaded &&
               loaded->gameName == session->gameName &&
               loaded->processName == session->processName &&
               loaded->samples.size() == session->samples.size() &&
               historyEntryValid && historyEntry.gameName == session->gameName &&
               historyEntry.processName == session->processName &&
               loadError.empty() && entryReadError.empty() && !removeError;
    }
#endif
};

GameSessionReportFeature::GameSessionReportFeature()
    : impl_(std::make_unique<Impl>())
{
}

GameSessionReportFeature::~GameSessionReportFeature()
{
    Shutdown();
}

const char* GameSessionReportFeature::name() const
{
    return "游戏会话报告";
}

const char* GameSessionReportFeature::configKey() const
{
    return "feature.game_session_report";
}

bool GameSessionReportFeature::enabled() const
{
    return impl_->enabled;
}

void GameSessionReportFeature::setEnabled(bool value)
{
    if (impl_->enabled == value)
        return;
    impl_->enabled = value;
    if (!value)
        impl_->DiscardActive();
    else
        impl_->StartHardwareQuery();
}

void GameSessionReportFeature::SetAutoOpen(bool value)
{
    impl_->autoOpen = value;
}

bool GameSessionReportFeature::AutoOpen() const
{
    return impl_->autoOpen;
}

void GameSessionReportFeature::SetSaveCsv(bool value)
{
    impl_->saveCsv = value;
}

bool GameSessionReportFeature::SaveCsv() const
{
    return impl_->saveCsv;
}

void GameSessionReportFeature::Init()
{
#if defined(FPSOVERLAY_UI_QA)
    if (!RunStatisticsSelfTest() || !impl_->RunLifecycleSelfTest() ||
        !impl_->RunPersistenceSelfTest())
        std::terminate();
#endif
    impl_->initialized = true;
    impl_->StartHistoryWorker();
    if (impl_->enabled)
        impl_->StartHardwareQuery();
}

void GameSessionReportFeature::Update(FeatureContext& context)
{
    impl_->Update(context);
}

bool GameSessionReportFeature::DrawSettings(FeatureContext&)
{
    bool changed = false;
    bool featureEnabled = impl_->enabled;
    if (ImGui::Checkbox("启用游戏性能报告", &featureEnabled)) {
        setEnabled(featureEnabled);
        changed = true;
    }
    ImGui::BeginDisabled(!impl_->enabled);
    if (ImGui::Checkbox("游戏结束后自动打开报告", &impl_->autoOpen))
        changed = true;
    if (ImGui::Checkbox("自动保存 CSV", &impl_->saveCsv))
        changed = true;
    ImGui::EndDisabled();

    if (impl_->activeSession) {
        SettingsUi::Status("当前状态：", "正在记录游戏会话", true);
        SettingsUi::Muted("目标：%s · 已采样 %zu 秒",
                          impl_->activeSession->processName.c_str(),
                          impl_->activeSession->samples.size());
    } else if (impl_->lastCompletedSession) {
        SettingsUi::Status("最近报告：",
                           impl_->lastCompletedSession->processName.c_str(), true);
    } else if (impl_->HasHistory()) {
        SettingsUi::Status("历史报告：", "已找到本地记录", true);
    } else {
        SettingsUi::Status("最近报告：", "暂无记录", false);
    }
    if (ImGui::Button("查看最近一次游戏报告", ImVec2(-1.0f, 0.0f)))
        impl_->openRequested.store(true, std::memory_order_release);
    SettingsUi::MutedWrapped("每秒从现有监控缓存采样一次；不会主动查询传感器，也不会改变 Overlay 布局。关闭功能会丢弃尚未结束的会话。");
    return changed;
}

bool GameSessionReportFeature::DrawReportPage(FeatureContext&)
{
    bool changed = false;
    if (SettingsUi::BeginCard("##game_report_options", "报告设置",
                               "本地记录策略")) {
        bool featureEnabled = impl_->enabled;
        const int optionColumns = ImGui::GetContentRegionAvail().x > 720.0f ? 3 : 1;
        if (ImGui::BeginTable("##game_report_options_grid", optionColumns,
                              ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextColumn();
            if (ImGui::Checkbox("启用会话报告##report_page", &featureEnabled)) {
                setEnabled(featureEnabled);
                changed = true;
            }
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!impl_->enabled);
            if (ImGui::Checkbox("结束后自动打开##report_page", &impl_->autoOpen))
                changed = true;
            ImGui::EndDisabled();
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!impl_->enabled);
            if (ImGui::Checkbox("自动保存 CSV##report_page", &impl_->saveCsv))
                changed = true;
            ImGui::EndDisabled();
            ImGui::EndTable();
        }
        if (impl_->activeSession)
            SettingsUi::Status("记录状态：", "游戏会话进行中", true);
        else
            SettingsUi::Status("记录状态：", "等待稳定游戏目标", false);
    }
    SettingsUi::EndCard();

    std::vector<HistoryEntry> historyEntries;
    std::shared_ptr<const SessionData> historyReport;
    std::filesystem::path selectedHistoryPath;
    std::string historyError;
    bool historyBusy = false;
    {
        std::lock_guard<std::mutex> lock(impl_->historyMutex);
        historyEntries = impl_->historyEntries;
        historyReport = impl_->selectedHistorySession;
        selectedHistoryPath = impl_->selectedHistoryPath;
        historyError = impl_->historyError;
        historyBusy = impl_->historyBusy;
    }

    char historySubtitle[64] = {};
    snprintf(historySubtitle, sizeof(historySubtitle), "最近 100 份本地 CSV · 已找到 %zu 份",
             historyEntries.size());
    if (SettingsUi::BeginCard("##game_report_history", "历史记录",
                               historySubtitle)) {
        ImGui::BeginDisabled(historyBusy);
        if (ImGui::Button("刷新记录"))
            impl_->QueueHistoryRefresh();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("打开记录文件夹##history")) {
            const std::filesystem::path folder = ReportDirectory();
            std::error_code directoryError;
            std::filesystem::create_directories(folder, directoryError);
            ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr,
                          SW_SHOWNORMAL);
        }
        if (impl_->lastCompletedSession && !selectedHistoryPath.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("查看本次最近报告"))
                impl_->SelectCurrentSession();
        }

        if (historyBusy)
            SettingsUi::Muted("正在读取历史记录…");
        if (!historyError.empty())
            ImGui::TextColored(SettingsUi::Warning(), "%s", historyError.c_str());

        if (historyEntries.empty()) {
            SettingsUi::MutedWrapped("暂无已保存的游戏报告。新会话结束并成功保存 CSV 后会自动出现在这里。");
        } else if (ImGui::BeginTable(
                       "##game_report_history_table", 5,
                       ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                           ImGuiTableFlags_SizingStretchProp |
                           ImGuiTableFlags_ScrollY,
                       ImVec2(0.0f, 174.0f))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("游戏", ImGuiTableColumnFlags_WidthStretch, 2.2f);
            ImGui::TableSetupColumn("开始时间", ImGuiTableColumnFlags_WidthStretch, 1.7f);
            ImGui::TableSetupColumn("时长", ImGuiTableColumnFlags_WidthStretch, 1.2f);
            ImGui::TableSetupColumn("平均 FPS", ImGuiTableColumnFlags_WidthStretch, 0.9f);
            ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, 72.0f);
            ImGui::TableHeadersRow();
            for (const HistoryEntry& entry : historyEntries) {
                const std::string pathId = WideToUtf8(entry.path.wstring());
                const bool selected = entry.path == selectedHistoryPath &&
                                      historyReport != nullptr;
                ImGui::PushID(pathId.c_str());
                ImGui::TableNextRow();
                if (selected) {
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                           IM_COL32(31, 61, 94, 205));
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(entry.gameName.c_str());
                SettingsUi::Muted("%s", entry.processName.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(entry.startTime.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(entry.duration.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(entry.averageFps.c_str());
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(selected || historyBusy);
                if (ImGui::Button(selected ? "查看中" : "查看"))
                    impl_->QueueHistoryLoad(entry.path);
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    SettingsUi::EndCard();

    const std::shared_ptr<const SessionData> report =
        historyReport ? historyReport : impl_->lastCompletedSession;
    if (!report) {
        if (SettingsUi::BeginCard("##game_report_empty", "暂无游戏报告")) {
            SettingsUi::MutedWrapped(historyBusy
                ? "正在后台读取最新的历史报告。"
                : "识别到稳定游戏目标 2 秒后开始记录。游戏结束或目标连续丢失 5 秒后生成报告；不足 30 秒的会话不会自动弹出。");
        }
        SettingsUi::EndCard();
        return changed;
    }

    if (SettingsUi::BeginCard("##game_report_summary", report->gameName.c_str(),
                               report->processName.c_str())) {
        const char* energyLabel = report->energyIncludesEstimate ? "估算用电量" : "实际用电量";
        const int summaryColumns = ImGui::GetContentRegionAvail().x > 620.0f ? 2 : 1;
        if (ImGui::BeginTable("##game_report_session_summary", summaryColumns,
                              ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextColumn();
            SettingsUi::Muted("会话时间");
            ImGui::Text("开始  %s", FormatLocalTime(report->startLocal).c_str());
            ImGui::Text("结束  %s", FormatLocalTime(report->endLocal).c_str());
            ImGui::TextColored(SettingsUi::Accent(), "时长  %s",
                               FormatDuration(report->durationSeconds).c_str());

            ImGui::TableNextColumn();
            SettingsUi::Muted("能耗记录");
            if (Stats(*report, MetricId::SystemPower).validSamples > 0) {
                ImGui::TextColored(
                    report->energyIncludesEstimate ? SettingsUi::Warning() : SettingsUi::Success(),
                    "%s  %.6f kWh", energyLabel, report->energyWh / 1000.0);
                ImGui::Text("功耗覆盖率  %.1f%%", report->powerCoveragePercent);
                ImGui::ProgressBar(
                    std::clamp(static_cast<float>(report->powerCoveragePercent / 100.0),
                               0.0f, 1.0f),
                    ImVec2(-1.0f, 6.0f), "");
            } else {
                ImGui::Text("%s  N/A", energyLabel);
                SettingsUi::Muted("功耗覆盖率 0%%");
            }
            ImGui::EndTable();
        }
        if (report->powerCoveragePercent < 80.0) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(SettingsUi::Warning(),
                               "功耗数据覆盖率偏低，用电量只代表有有效功耗样本的时间段。");
            ImGui::PopTextWrapPos();
        }
        if (report->sampleLimitReached) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(SettingsUi::Warning(),
                               "本局超过 8 小时样本上限，曲线只保留前 28,800 个逐秒样本。");
            ImGui::PopTextWrapPos();
        }
        const std::string csvPath = WideToUtf8(report->csvPath.wstring());
        const std::string csvFileName = WideToUtf8(report->csvPath.filename().wstring());
        SettingsUi::Muted("记录文件");
        ImGui::SameLine();
        ImGui::TextUnformatted(csvFileName.empty() ? "尚未保存" : csvFileName.c_str());
        if (!csvPath.empty() && ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextWrapped("%s", csvPath.c_str());
            ImGui::EndTooltip();
        }
        if (ImGui::Button("打开记录文件夹")) {
            const std::filesystem::path folder = report->csvPath.parent_path();
            ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        std::lock_guard<std::mutex> statusLock(impl_->writerStatusMutex);
        if (!impl_->lastWriterError.empty()) {
            ImGui::TextColored(SettingsUi::Warning(), "CSV 保存失败：%s",
                               impl_->lastWriterError.c_str());
        }
    }
    SettingsUi::EndCard();

    const MetricStatistics& fps = Stats(*report, MetricId::Fps);
    const MetricStatistics& cpuUsage = Stats(*report, MetricId::CpuUsage);
    const MetricStatistics& cpuTemperature = Stats(*report, MetricId::CpuTemperature);
    const MetricStatistics& gpuUsage = Stats(*report, MetricId::GpuUsage);
    const MetricStatistics& gpuTemperature = Stats(*report, MetricId::GpuTemperature);
    const MetricStatistics& ramUsage = Stats(*report, MetricId::RamUsagePercent);
    const MetricStatistics& systemPower = Stats(*report, MetricId::SystemPower);
    const float tileAreaWidth = ImGui::GetContentRegionAvail().x;
    const int tileColumns = tileAreaWidth > 820.0f ? 5 :
                            (tileAreaWidth > 520.0f ? 3 :
                             (tileAreaWidth > 320.0f ? 2 : 1));
    if (ImGui::BeginTable("##game_report_tiles", tileColumns,
                          ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        const std::string fpsDetail = report->hasOnePercentLowFps
            ? "1% Low " + NumberOrNa(true, report->onePercentLowFps, 1)
            : "1% Low N/A";
        DrawSummaryTile("##report_fps", "平均 FPS",
                        FormatMetricAverage(fps, "", 1), fpsDetail,
                        SettingsUi::Accent(), fps, " FPS");
        ImGui::TableNextColumn();
        DrawSummaryTile("##report_cpu", "CPU",
                        FormatMetricAverage(cpuUsage, "%", 1),
                        "平均温度 " + FormatMetricAverage(cpuTemperature, "°C", 1),
                        SettingsUi::Success(), cpuUsage, "%",
                        &cpuTemperature, "CPU 温度", "°C");
        ImGui::TableNextColumn();
        DrawSummaryTile("##report_gpu", "GPU",
                        FormatMetricAverage(gpuUsage, "%", 1),
                        "平均温度 " + FormatMetricAverage(gpuTemperature, "°C", 1),
                        SettingsUi::Violet(), gpuUsage, "%",
                        &gpuTemperature, "GPU 温度", "°C");
        ImGui::TableNextColumn();
        DrawSummaryTile("##report_ram", "内存平均占用",
                        FormatMetricAverage(ramUsage, "%", 1), "",
                        ImVec4(0.33f, 0.82f, 0.83f, 1.0f), ramUsage, "%");
        ImGui::TableNextColumn();
        DrawSummaryTile("##report_power", "平均整机功耗",
                        FormatMetricAverage(systemPower, " W", 1),
                        report->energyIncludesEstimate ? "软件估算" : "真实传感器",
                        SettingsUi::Warning(), systemPower, " W");
        ImGui::EndTable();
    }

    if (SettingsUi::BeginCard("##game_report_curve", "完整会话曲线",
                               "平均 / 最低 / 最高参考线")) {
        static constexpr MetricId plotMetrics[] = {
            MetricId::Fps, MetricId::CpuUsage, MetricId::CpuTemperature,
            MetricId::CpuPower, MetricId::GpuUsage, MetricId::GpuTemperature,
            MetricId::GpuPower, MetricId::RamUsagePercent,
            MetricId::VramUsagePercent, MetricId::SystemPower
        };
        impl_->selectedPlotMetric = std::clamp(
            impl_->selectedPlotMetric, 0, static_cast<int>(std::size(plotMetrics)) - 1);
        if (ImGui::BeginCombo("曲线指标", MetricName(plotMetrics[impl_->selectedPlotMetric]))) {
            for (int index = 0; index < static_cast<int>(std::size(plotMetrics)); ++index) {
                const bool selected = index == impl_->selectedPlotMetric;
                if (ImGui::Selectable(MetricName(plotMetrics[index]), selected))
                    impl_->selectedPlotMetric = index;
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        DrawSessionPlot(*report, plotMetrics[impl_->selectedPlotMetric]);
    }
    SettingsUi::EndCard();

    if (SettingsUi::BeginCard("##game_report_hardware", "本局硬件环境",
                               "报告生成时的设备快照")) {
        if (ImGui::BeginTable("##game_report_hardware_table", 2,
                              ImGuiTableFlags_BordersInnerH |
                              ImGuiTableFlags_SizingStretchProp)) {
            auto row = [](const char* label, const std::string& value) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                SettingsUi::Muted("%s", label);
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", value.empty() ? "N/A" : value.c_str());
            };
            row("Windows", report->hardware.windowsVersion);
            row("CPU", report->hardware.cpuName);
            row("GPU", report->hardware.gpuName);
            row("总内存", NumberOrNa(report->hardware.totalMemoryGb > 0.0,
                                      report->hardware.totalMemoryGb, 2) + " GB");
            row("显示器", report->hardware.displayMode);
            ImGui::EndTable();
        }
    }
    SettingsUi::EndCard();
    return changed;
}

bool GameSessionReportFeature::ConsumeOpenRequest()
{
    return impl_->openRequested.exchange(false, std::memory_order_acq_rel);
}

bool GameSessionReportFeature::HasCompletedSession() const
{
    return impl_->lastCompletedSession != nullptr || impl_->HasHistory();
}

bool GameSessionReportFeature::NeedsSensorPolling() const
{
    return impl_->enabled &&
           (impl_->candidatePid != 0 || impl_->activeSession != nullptr);
}

void GameSessionReportFeature::Shutdown()
{
    if (!impl_ || impl_->shutdown)
        return;
    impl_->shutdown = true;
    if (impl_->activeSession) {
        const ULONGLONG now = GetTickCount64();
        impl_->FinalizeActive(now, CurrentLocalTime(), false);
    }
    impl_->StopThreads();
}
