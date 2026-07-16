#include "LaptopPowerFeature.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>

#include <batclass.h>
#include <setupapi.h>
#include <shellapi.h>

#include "imgui.h"
#include "SettingsUi.h"

#pragma comment(lib, "setupapi.lib")

namespace {

static const GUID kBatteryDeviceGuid =
    { 0x72631e54, 0x78a4, 0x11d0, { 0xbc, 0xf7, 0x00, 0xaa, 0x00, 0xb7, 0xb3, 0x2a } };

static void SafeCatSource(char* dest, size_t cap, const char* source)
{
    if (!dest || cap == 0 || !source || !source[0]) return;
    if (dest[0] != '\0')
        strncat_s(dest, cap, " / ", _TRUNCATE);
    strncat_s(dest, cap, source, _TRUNCATE);
}

static const char* PowerText(float value, bool has, char* buf, size_t cap)
{
    if (!has || value <= 0.0f) {
        snprintf(buf, cap, "N/A");
    } else {
        snprintf(buf, cap, "%.0fW", value);
    }
    return buf;
}

static std::string CsvText(const char* value)
{
    std::string out = "\"";
    for (const char* p = value ? value : ""; *p; ++p) {
        if (*p == '"')
            out += "\"\"";
        else
            out += *p;
    }
    out += '"';
    return out;
}

static bool WideToUtf8Text(const std::wstring& value,
                           char* output,
                           size_t capacity)
{
    if (!output || capacity == 0)
        return false;
    output[0] = '\0';
    if (value.empty())
        return true;
    const int required = WideCharToMultiByte(
        CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (required <= 0 || static_cast<size_t>(required) > capacity)
        return false;
    return WideCharToMultiByte(
        CP_UTF8, 0, value.c_str(), -1, output,
        static_cast<int>(capacity), nullptr, nullptr) > 0;
}

static float EstimateFanPower(float rpm, float maxPowerW)
{
    if (!std::isfinite(rpm) || rpm <= 0.0f)
        return 0.0f;
    const float ratio = std::clamp(rpm / 6500.0f, 0.0f, 1.0f);
    return 0.15f + (maxPowerW - 0.15f) * std::pow(ratio, 2.6f);
}

struct FusionEstimate {
    bool valid = false;
    float totalW = 0.0f;
    float cpuSocW = 0.0f;
    float gpuW = 0.0f;
    float memoryW = 0.0f;
    float screenW = 0.0f;
    float fanW = 0.0f;
    float storageW = 0.0f;
    float networkW = 0.0f;
    float boardW = 0.0f;
    float conversionLossW = 0.0f;
    int confidence = 0;
};

// G815LR AC outlet calibration from time-aligned software-estimate / wall-meter
// measurements collected on 2026-06-23. The second session uses a continuous
// 250ms software timeline, sensor-snapshot age metadata and a measured 1.5s
// alignment correction. Stable low/high clusters are fitted together; loading,
// scene changes and shutdown transitions are excluded from calibration.
constexpr float kG815OutletScale = 1.0218875f;
constexpr float kG815OutletOffsetW = 25.8991f;
constexpr float kG815OutletStableMaeW = 5.0f;

static float ApplyG815OutletCalibration(float internalEstimateW)
{
    return internalEstimateW * kG815OutletScale + kG815OutletOffsetW;
}

static FusionEstimate BuildFusionEstimate(const FeatureContext& context)
{
    FusionEstimate out;
    const bool g815 = context.isRogStrixG815LR;

    float cpuPackage = context.hasCpuPackagePower
        ? context.cpuPackagePowerW
        : 0.0f;
    if (cpuPackage <= 0.0f && context.cpuUsage > 0.0f)
        cpuPackage = 4.0f + context.cpuUsage * 1.35f;

    bool platformDomainUsed = false;
    out.cpuSocW = cpuPackage;
    if (context.hasCpuPlatformPower && context.cpuPlatformPowerW >= cpuPackage * 0.85f) {
        out.cpuSocW = (std::max)(cpuPackage, context.cpuPlatformPowerW);
        platformDomainUsed = true;
    }

    const float ramLoad = context.ramTotalGb > 0.0f
        ? std::clamp(context.ramUsedGb / context.ramTotalGb, 0.0f, 1.0f)
        : 0.5f;
    if (!platformDomainUsed) {
        out.memoryW = context.hasCpuMemoryPower
            ? context.cpuMemoryPowerW
            : (g815 ? 1.25f + 2.35f * std::pow(ramLoad, 1.2f)
                    : 1.5f + context.ramTotalGb * 0.06f + ramLoad);
    }

    if (context.hasDiscreteGpuPower) {
        out.gpuW = context.discreteGpuPowerW;
    } else if (context.hasGpuPower && context.gpuPowerIsDiscrete) {
        out.gpuW = context.gpuPowerW;
    } else if (context.gpuUsage > 1.0f) {
        out.gpuW = 5.0f + context.gpuUsage * 1.30f;
    }

    const float brightness = std::clamp(
        context.hasDisplayBrightness ? context.displayBrightnessPercent : 50.0f,
        0.0f, 100.0f) / 100.0f;
    // BOE NE180QDM-NZC: 18-inch 2560x1600, 240Hz, 500-nit IPS.
    out.screenW = g815
        ? 2.8f + 7.4f * std::pow(brightness, 1.35f)
        : 2.0f + 5.5f * std::pow(brightness, 1.3f);

    out.fanW =
        EstimateFanPower(context.cpuFanRpm, g815 ? 3.0f : 2.5f) +
        EstimateFanPower(context.gpuFanRpm, g815 ? 3.0f : 2.5f) +
        EstimateFanPower(context.systemFanRpm, g815 ? 2.4f : 2.0f);

    if (g815 && context.diskCount >= 2) {
        // Installed drives: SK hynix PC801 1TB + WD Blue SN570 1TB.
        const float activity = std::clamp(context.diskActivityPercent / 200.0f, 0.0f, 1.0f);
        out.storageW = 0.25f + 10.8f * std::pow(activity, 0.75f);
    } else {
        const float denominator = (std::max)(1, context.diskCount) * 100.0f;
        const float activity = std::clamp(context.diskActivityPercent / denominator, 0.0f, 1.0f);
        out.storageW = (std::max)(1, context.diskCount) * (0.18f + 4.5f * activity);
    }

    const float networkLoad = std::clamp(context.networkUtilizationPercent / 100.0f, 0.0f, 1.0f);
    out.networkW = (g815 ? 0.45f : 0.35f) + (g815 ? 1.9f : 1.4f) * std::pow(networkLoad, 0.65f);
    // G815LR fixed platform: PCH/EC, audio, keyboard, camera, USB and Aura lighting.
    out.boardW = g815 ? 5.2f : 5.8f;

    const float coreRailW = out.cpuSocW + out.memoryW + out.gpuW;
    const float lossRate = 0.055f + 0.025f * (coreRailW / 220.0f);
    out.conversionLossW = 0.6f + coreRailW * lossRate;
    out.totalW = coreRailW + out.screenW + out.fanW + out.storageW +
                 out.networkW + out.boardW + out.conversionLossW;
    out.confidence = g815 ? 45 : 20;
    out.confidence += context.hasCpuPackagePower ? 18 : 5;
    out.confidence += context.hasDiscreteGpuPower ? 15 : (context.hasGpuPower ? 8 : 3);
    out.confidence += (context.hasCpuPlatformPower || context.hasCpuMemoryPower) ? 4 : 0;
    out.confidence += context.hasDisplayBrightness ? 5 : 0;
    out.confidence += (context.cpuFanRpm > 0.0f || context.gpuFanRpm > 0.0f) ? 4 : 0;
    out.confidence += context.diskCount > 0 ? 4 : 0;
    out.confidence += context.networkUtilizationPercent >= 0.0f ? 2 : 0;
    out.confidence = std::clamp(out.confidence, 0, 95);
    out.valid = std::isfinite(out.totalW) && out.totalW > 1.0f;
    return out;
}

} // namespace

void LaptopPowerFeature::LoadSettings(const FeatureSettings& settings)
{
    enabled_ = settings.laptopPowerEnabled;
    showOverlay_ = settings.laptopPowerShowOverlay;
    showDcInputPower_ = settings.laptopPowerShowDcInputPower;
    showSystemPower_ = settings.laptopPowerShowSystemPower;
    showKnownComponentsPower_ = settings.laptopPowerShowKnownComponentsPower;
    showBatteryDischargePower_ = settings.laptopPowerShowBatteryDischargePower;
    showBatteryStatus_ = settings.laptopPowerShowBatteryStatus;
    onlyShowPowerItemsInGame_ = settings.laptopPowerOnlyShowPowerItemsInGame;
    nonGameShowSystemPower_ = settings.laptopPowerNonGameShowSystemPower;
    nonGameShowKnownComponentsPower_ = settings.laptopPowerNonGameShowKnownComponentsPower;
    nonGameShowBatteryDischargePower_ = settings.laptopPowerNonGameShowBatteryDischargePower;
    allowEstimatedSystemPower_ = settings.laptopPowerAllowEstimatedSystemPower;
    showEstimated_ = settings.laptopPowerShowEstimated;
    showSource_ = settings.laptopPowerShowSource;
    samplingMs_ = std::clamp(settings.laptopPowerSamplingMs, 1000, 5000);
    smoothSeconds_ = settings.laptopPowerSmoothSeconds == 10 ? 10 : (settings.laptopPowerSmoothSeconds == 0 ? 0 : 5);
    warningThresholdW_ = std::clamp(settings.laptopPowerWarningThresholdW, 30, 300);
    adapterWarningW_ = std::clamp(settings.laptopPowerAdapterWarningW, 15, 300);
    displayMode_ = std::clamp(settings.laptopPowerDisplayMode, 0, 2);
    batteryAssistAlertEnabled_ = settings.laptopPowerBatteryAssistAlertEnabled;
    batteryAssistThresholdW_ = std::clamp(settings.laptopPowerBatteryAssistThresholdW, 1, 30);
    batteryAssistSeconds_ = std::clamp(settings.laptopPowerBatteryAssistSeconds, 1, 60);
}

void LaptopPowerFeature::SaveSettings(FeatureSettings& settings) const
{
    settings.laptopPowerEnabled = enabled_;
    settings.laptopPowerShowOverlay = showOverlay_;
    settings.laptopPowerShowDcInputPower = showDcInputPower_;
    settings.laptopPowerShowSystemPower = showSystemPower_;
    settings.laptopPowerShowKnownComponentsPower = showKnownComponentsPower_;
    settings.laptopPowerShowBatteryDischargePower = showBatteryDischargePower_;
    settings.laptopPowerShowBatteryStatus = showBatteryStatus_;
    settings.laptopPowerOnlyShowPowerItemsInGame = onlyShowPowerItemsInGame_;
    settings.laptopPowerNonGameShowSystemPower = nonGameShowSystemPower_;
    settings.laptopPowerNonGameShowKnownComponentsPower = nonGameShowKnownComponentsPower_;
    settings.laptopPowerNonGameShowBatteryDischargePower = nonGameShowBatteryDischargePower_;
    settings.laptopPowerAllowEstimatedSystemPower = allowEstimatedSystemPower_;
    settings.laptopPowerShowEstimated = showEstimated_;
    settings.laptopPowerShowSource = showSource_;
    settings.laptopPowerSamplingMs = samplingMs_;
    settings.laptopPowerSmoothSeconds = smoothSeconds_;
    settings.laptopPowerWarningThresholdW = warningThresholdW_;
    settings.laptopPowerAdapterWarningW = adapterWarningW_;
    settings.laptopPowerDisplayMode = displayMode_;
    settings.laptopPowerBatteryAssistAlertEnabled = batteryAssistAlertEnabled_;
    settings.laptopPowerBatteryAssistThresholdW = batteryAssistThresholdW_;
    settings.laptopPowerBatteryAssistSeconds = batteryAssistSeconds_;
}

bool LaptopPowerFeature::ToggleQuickOption(LaptopPowerQuickOption option)
{
    switch (option) {
    case LaptopPowerQuickOption::ShowSystemPower:
        showSystemPower_ = !showSystemPower_;
        return showSystemPower_;
    case LaptopPowerQuickOption::ShowKnownComponentsPower:
        showKnownComponentsPower_ = !showKnownComponentsPower_;
        return showKnownComponentsPower_;
    case LaptopPowerQuickOption::ShowBatteryDischargePower:
        showBatteryDischargePower_ = !showBatteryDischargePower_;
        return showBatteryDischargePower_;
    case LaptopPowerQuickOption::AllowEstimatedSystemPower:
        allowEstimatedSystemPower_ = !allowEstimatedSystemPower_;
        return allowEstimatedSystemPower_;
    }
    return false;
}

bool LaptopPowerFeature::ValidLaptopPower(float value)
{
    return std::isfinite(value) && value > 1.0f;
}

float LaptopPowerFeature::KnownComponentsPower(const FeatureContext& context)
{
    return (context.hasCpuPackagePower ? context.cpuPackagePowerW : 0.0f) +
           (context.hasDiscreteGpuPower ? context.discreteGpuPowerW :
            (context.hasGpuPower ? context.gpuPowerW : 0.0f));
}

void LaptopPowerFeature::WriteDebugAvailability(const FeatureContext& context)
{
    const int state = (context.hasBatteryDischargePower ? 1 : 0) |
                      (context.hasAcInputPower ? 2 : 0) |
                      (context.acOnline ? 4 : 0);
    if (state == lastDebugAvailability_)
        return;
    lastDebugAvailability_ = state;

    char line[320] = {};
    snprintf(line, sizeof(line),
             "LaptopPowerFeature: Battery discharge available: %s; "
             "AC input available: %s; AC input failure reason: %s\n",
             context.hasBatteryDischargePower ? "yes" : "no",
             context.hasAcInputPower ? "yes" : "no",
             context.hasAcInputPower ? "none" :
                 (context.powerFailureReason[0] ? context.powerFailureReason : "sensor not exposed"));
    OutputDebugStringA(line);
}

LaptopPowerFeature::CalibrationState& LaptopPowerFeature::CalibrationForPowerState(bool acOnline)
{
    return acOnline ? acCalibration_ : batteryCalibration_;
}

void LaptopPowerFeature::UpdateFusionCalibration(CalibrationState& state,
                                                 float referenceW,
                                                 float fusionW)
{
    if (!ValidLaptopPower(referenceW) || !ValidLaptopPower(fusionW))
        return;

    const float targetOffset = referenceW - fusionW;
    if (!std::isfinite(targetOffset))
        return;

    if (state.samples == 0) {
        // Do not trust one measurement completely, but allow calibration to
        // converge beyond any fixed offset range over subsequent samples.
        state.offsetW = targetOffset * 0.5f;
        state.meanAbsErrorW = std::fabs(referenceW - (fusionW + state.offsetW));
    } else {
        const float innovation = targetOffset - state.offsetW;
        const float robustStepW = (std::max)(5.0f, referenceW * 0.10f);
        const float boundedInnovation = std::clamp(innovation, -robustStepW, robustStepW);
        const float learnRate = state.samples < 8 ? 0.22f : 0.08f;
        state.offsetW += boundedInnovation * learnRate;

        const float residual = std::fabs(referenceW - (fusionW + state.offsetW));
        state.meanAbsErrorW =
            state.meanAbsErrorW * 0.90f + residual * 0.10f;
    }
    state.samples = (std::min)(state.samples + 1, 100000);
}

float LaptopPowerFeature::SmoothEstimate(float rawValue,
                                         EstimateSource source,
                                         unsigned long long now,
                                         bool sampleDue,
                                         bool reset)
{
    if (!ValidLaptopPower(rawValue))
        return 0.0f;

    if (smoothSeconds_ <= 0 || reset || !hasSmoothedPower_) {
        smoothedPowerW_ = rawValue;
        hasSmoothedPower_ = true;
        smoothedSource_ = source;
        lastSmoothTick_ = now;
        return smoothedPowerW_;
    }

    if (!sampleDue)
        return smoothedPowerW_;

    const float elapsedSeconds = lastSmoothTick_ > 0
        ? (float)(now - lastSmoothTick_) / 1000.0f
        : (float)samplingMs_ / 1000.0f;
    float alpha = std::clamp(
        elapsedSeconds / (float)(std::max)(1, smoothSeconds_),
        0.05f, 1.0f);

    // Large real load changes should remain responsive, while source changes
    // and small sensor noise still transition over multiple samples.
    const float delta = std::fabs(rawValue - smoothedPowerW_);
    const float largeChangeW = (std::max)(18.0f, rawValue * 0.22f);
    if (delta > largeChangeW)
        alpha = (std::min)(0.65f, alpha * 1.35f);
    if (source != smoothedSource_)
        alpha = (std::min)(alpha, 0.50f);

    smoothedPowerW_ += (rawValue - smoothedPowerW_) * alpha;
    smoothedSource_ = source;
    lastSmoothTick_ = now;
    return smoothedPowerW_;
}

LaptopPowerFeature::BatteryRateResult LaptopPowerFeature::QueryWindowsBatteryRate()
{
    BatteryRateResult result;
    if (batteryIoAbandoned_.load(std::memory_order_acquire))
        return result;

    auto deviceIoControlWithTimeout = [this](
        HANDLE& handle, DWORD controlCode, void* input, DWORD inputBytes,
        void* output, DWORD outputBytes, DWORD* bytesReturned) {
        auto overlapped = std::make_unique<OVERLAPPED>();
        overlapped->hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped->hEvent)
            return false;

        BOOL completed = DeviceIoControl(
            handle, controlCode, input, inputBytes, output, outputBytes,
            bytesReturned, overlapped.get());
        if (!completed && GetLastError() == ERROR_IO_PENDING) {
            const DWORD waitResult = WaitForSingleObject(
                overlapped->hEvent, 1500);
            if (waitResult == WAIT_OBJECT_0) {
                completed = GetOverlappedResult(
                    handle, overlapped.get(), bytesReturned, FALSE);
            } else {
                CancelIoEx(handle, overlapped.get());
                CloseHandle(handle);
                handle = INVALID_HANDLE_VALUE;
                const DWORD cancelWait = WaitForSingleObject(
                    overlapped->hEvent, 1500);
                if (cancelWait != WAIT_OBJECT_0) {
                    batteryIoAbandoned_.store(true, std::memory_order_release);
                    // The driver still owns this OVERLAPPED. Keep the single
                    // context alive for process lifetime and stop future polls.
                    overlapped.release();
                    return false;
                }
                completed = FALSE;
            }
        }
        CloseHandle(overlapped->hEvent);
        return completed != FALSE;
    };

    HDEVINFO devInfo = SetupDiGetClassDevs(&kBatteryDeviceGuid, nullptr, nullptr,
                                           DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE)
        return result;

    SP_DEVICE_INTERFACE_DATA iface = {};
    iface.cbSize = sizeof(iface);

    for (DWORD index = 0;
         index < 8 && !shuttingDown_.load(std::memory_order_acquire) &&
         SetupDiEnumDeviceInterfaces(
             devInfo, nullptr, &kBatteryDeviceGuid, index, &iface);
         ++index) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetail(devInfo, &iface, nullptr, 0, &needed, nullptr);
        if (needed == 0)
            continue;

        auto detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA)malloc(needed);
        if (!detail)
            continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

        if (!SetupDiGetDeviceInterfaceDetail(devInfo, &iface, detail, needed, &needed, nullptr)) {
            free(detail);
            continue;
        }

        HANDLE hBattery = CreateFile(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
        free(detail);
        if (hBattery == INVALID_HANDLE_VALUE)
            continue;

        BATTERY_QUERY_INFORMATION query = {};
        DWORD bytes = 0;
        if (!deviceIoControlWithTimeout(
                hBattery, IOCTL_BATTERY_QUERY_TAG, nullptr, 0,
                &query.BatteryTag, sizeof(query.BatteryTag), &bytes) ||
            query.BatteryTag == 0) {
            if (hBattery != INVALID_HANDLE_VALUE)
                CloseHandle(hBattery);
            continue;
        }

        BATTERY_WAIT_STATUS wait = {};
        wait.BatteryTag = query.BatteryTag;
        BATTERY_STATUS status = {};
        if (deviceIoControlWithTimeout(
                hBattery, IOCTL_BATTERY_QUERY_STATUS, &wait, sizeof(wait),
                &status, sizeof(status), &bytes)) {
            result.statusAvailable = true;

            const LONG rate = status.Rate;
            if (rate != BATTERY_UNKNOWN_RATE && rate != 0) {
                const float watts = std::fabs((float)rate) / 1000.0f;
                if (ValidLaptopPower(watts)) {
                    const bool charging = (status.PowerState & BATTERY_CHARGING) != 0;
                    const bool discharging = (status.PowerState & BATTERY_DISCHARGING) != 0;
                    if (charging || (!discharging && rate > 0)) {
                        result.hasCharge = true;
                        result.chargeW = watts;
                    } else {
                        result.hasDischarge = true;
                        result.dischargeW = watts;
                    }
                }
            } else if (rate == 0) {
                result.idle = true;
            }
        }
        if (hBattery != INVALID_HANDLE_VALUE)
            CloseHandle(hBattery);
        if (result.statusAvailable)
            break;
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return result;
}

void LaptopPowerFeature::ApplyCompletedBatteryPoll()
{
    if (!batteryPollInFlight_.load(std::memory_order_acquire) &&
        batteryPollThread_.joinable()) {
        batteryPollThread_.join();
    }

    BatteryRateResult result;
    bool ready = false;
    {
        std::lock_guard<std::mutex> lock(batteryResultMutex_);
        if (pendingBatteryResultReady_) {
            result = pendingBatteryResult_;
            pendingBatteryResultReady_ = false;
            ready = true;
        }
    }
    if (!ready)
        return;

    cachedBatteryStatusAvailable_ = result.statusAvailable;
    cachedBatteryIdle_ = result.idle;
    cachedHasCharge_ = result.hasCharge;
    cachedHasDischarge_ = result.hasDischarge;
    cachedChargeW_ = result.chargeW;
    cachedDischargeW_ = result.dischargeW;
}

void LaptopPowerFeature::ScheduleBatteryPoll()
{
    if (shuttingDown_.load(std::memory_order_acquire))
        return;
    if (batteryPollInFlight_.exchange(true, std::memory_order_acq_rel))
        return;
    if (batteryPollThread_.joinable())
        batteryPollThread_.join();

    try {
        batteryPollThread_ = std::thread([this]() {
            try {
                const BatteryRateResult result = QueryWindowsBatteryRate();
                if (!shuttingDown_.load(std::memory_order_acquire)) {
                    std::lock_guard<std::mutex> lock(batteryResultMutex_);
                    pendingBatteryResult_ = result;
                    pendingBatteryResultReady_ = true;
                }
            } catch (...) {
            }
            batteryPollInFlight_.store(false, std::memory_order_release);
        });
    } catch (...) {
        batteryPollInFlight_.store(false, std::memory_order_release);
    }
}

void LaptopPowerFeature::Update(FeatureContext& context)
{
    if (!enabled_)
        return;

    ApplyCompletedBatteryPoll();

    const bool hasDirectSystemPower = context.hasEstimatedSystemPower &&
                                      ValidLaptopPower(context.estimatedSystemPowerW);
    const float directSystemPowerW = hasDirectSystemPower
        ? context.estimatedSystemPowerW
        : 0.0f;
    const unsigned long long now = GetTickCount64();
    bool powerStateChanged = false;

    if (!hasLastAcState_ || lastAcOnline_ != context.acOnline) {
        hasLastAcState_ = true;
        lastAcOnline_ = context.acOnline;
        powerStateChanged = true;
        lastSampleTick_ = 0;
        lastModelSampleTick_ = 0;
        hasSmoothedPower_ = false;
        smoothedSource_ = EstimateSource::None;
        lastRawSource_ = EstimateSource::None;
        cachedHasCharge_ = false;
        cachedHasDischarge_ = false;
    }

    if (lastSampleTick_ == 0 || now - lastSampleTick_ >= (unsigned long long)samplingMs_) {
        lastSampleTick_ = now;
        ScheduleBatteryPoll();
    }

    if (!context.acOnline)
        context.hasBatteryChargePower = false;

    if (cachedHasCharge_ && context.acOnline) {
        context.batteryChargePowerW = cachedChargeW_;
        context.hasBatteryChargePower = true;
        SafeCatSource(context.powerSource, sizeof(context.powerSource), "Windows Battery");
    }
    if (cachedHasDischarge_) {
        context.batteryDischargePowerW = cachedDischargeW_;
        context.hasBatteryDischargePower = true;
        SafeCatSource(context.powerSource, sizeof(context.powerSource), "Windows Battery");
    }

    if (context.cpuPackagePowerW > 0.0f || context.gpuPowerW > 0.0f)
        SafeCatSource(context.powerSource, sizeof(context.powerSource), "LHM components");

    const FusionEstimate fusion = BuildFusionEstimate(context);
    const bool useG815OutletCalibration =
        context.acOnline && context.isRogStrixG815LR && fusion.valid;
    const float fusionReferenceW = useG815OutletCalibration
        ? ApplyG815OutletCalibration(fusion.totalW)
        : fusion.totalW;
    hasComparisonFusionInternalPower_ =
        fusion.valid && ValidLaptopPower(fusion.totalW);
    comparisonFusionInternalPowerW_ =
        hasComparisonFusionInternalPower_ ? fusion.totalW : 0.0f;
    hasComparisonFusionOutletPower_ =
        useG815OutletCalibration && ValidLaptopPower(fusionReferenceW);
    comparisonFusionOutletPowerW_ =
        hasComparisonFusionOutletPower_ ? fusionReferenceW : 0.0f;
    float calibrationReferenceW = 0.0f;
    if (hasDirectSystemPower) {
        calibrationReferenceW = directSystemPowerW;
    } else if (context.acOnline && context.hasAcInputPower) {
        calibrationReferenceW =
            context.acInputPowerW +
            (context.hasBatteryDischargePower ? context.batteryDischargePowerW : 0.0f) -
            (context.hasBatteryChargePower ? context.batteryChargePowerW : 0.0f);
    } else if (!context.acOnline && context.hasBatteryDischargePower) {
        calibrationReferenceW = context.batteryDischargePowerW;
    }

    EstimateSource rawSource = EstimateSource::None;
    float rawPowerW = 0.0f;
    bool rawEstimated = false;
    int rawConfidence = 0;
    float rawUncertaintyW = 0.0f;
    char automaticSource[sizeof(context.automaticSystemPowerSource)] = {};
    char estimateBreakdown[sizeof(context.systemPowerEstimateBreakdown)] = {};

    if (hasDirectSystemPower) {
        rawSource = EstimateSource::DirectSystem;
        rawPowerW = directSystemPowerW;
        rawEstimated = false;
        rawConfidence = 98;
        rawUncertaintyW = 2.0f;
        snprintf(automaticSource, sizeof(automaticSource), "%s",
                 context.automaticSystemPowerSource[0]
                     ? context.automaticSystemPowerSource
                     : "真实整机功耗传感器");
    } else if (allowEstimatedSystemPower_ && context.acOnline && context.hasAcInputPower) {
        float systemPower = context.acInputPowerW;
        if (context.hasBatteryDischargePower)
            systemPower += context.batteryDischargePowerW;
        if (context.hasBatteryChargePower)
            systemPower -= context.batteryChargePowerW;
        if (ValidLaptopPower(systemPower)) {
            rawSource = EstimateSource::AcInputDerived;
            rawPowerW = systemPower;
            rawEstimated = true;
            rawConfidence = 94;
            rawUncertaintyW = 3.0f +
                (context.hasBatteryChargePower ? 1.0f : 0.0f) +
                (context.hasBatteryDischargePower ? 1.0f : 0.0f);
            snprintf(automaticSource, sizeof(automaticSource),
                     "%s%s%s",
                     context.acInputPowerSource[0]
                         ? context.acInputPowerSource
                         : "DC 输入",
                     context.hasBatteryDischargePower ? " + 电池放电" : "",
                     context.hasBatteryChargePower ? " - 电池充电" : "");
            snprintf(estimateBreakdown, sizeof(estimateBreakdown),
                     "DC 输入 %.1fW%s%s = %.1fW",
                     context.acInputPowerW,
                     context.hasBatteryDischargePower ? " + 电池辅助供电" : "",
                     context.hasBatteryChargePower ? " - 电池充电" : "",
                     systemPower);
        }
    } else if (!context.acOnline && context.hasBatteryDischargePower) {
        rawSource = EstimateSource::BatteryDischarge;
        rawPowerW = context.batteryDischargePowerW;
        rawEstimated = false;
        rawConfidence = 98;
        rawUncertaintyW = (std::max)(1.0f, rawPowerW * 0.03f);
        snprintf(context.powerSource, sizeof(context.powerSource), "%s", "电池放电");
        snprintf(automaticSource, sizeof(automaticSource), "%s", "电池放电");
    } else if (allowEstimatedSystemPower_ && fusion.valid) {
        CalibrationState& calibration = CalibrationForPowerState(context.acOnline);
        const float calibratedPower = fusionReferenceW + calibration.offsetW;
        const int calibrationBonus = calibration.samples >= 30 ? 8 :
                                     (calibration.samples >= 10 ? 5 :
                                      (calibration.samples >= 3 ? 2 : 0));
        const int calibrationPenalty = (int)std::clamp(
            calibration.meanAbsErrorW * 0.6f, 0.0f, 18.0f);
        const int confidence = std::clamp(
            fusion.confidence +
            (useG815OutletCalibration ? 6 : 0) +
            calibrationBonus - calibrationPenalty, 0, 96);
        const float uncertainty = (std::max)(
            useG815OutletCalibration ? 6.0f : 5.0f,
            calibratedPower * (100.0f - confidence) * 0.0035f +
            calibration.meanAbsErrorW * 0.5f +
            (useG815OutletCalibration ? kG815OutletStableMaeW : 3.0f));
        if (ValidLaptopPower(calibratedPower)) {
            rawSource = EstimateSource::Fusion;
            rawPowerW = calibratedPower;
            rawEstimated = true;
            rawConfidence = confidence;
            rawUncertaintyW = uncertainty;
            snprintf(automaticSource, sizeof(automaticSource),
                     "%s",
                     useG815OutletCalibration
                         ? "枪神9 Plus G815LR 插座实测校准"
                         : context.isRogStrixG815LR
                         ? "枪神9 Plus G815LR 专用融合模型"
                         : "本机传感器融合估算");
            snprintf(estimateBreakdown, sizeof(estimateBreakdown),
                     "CPU/平台 %.0fW + 独显 %.0fW + 内存 %.1fW + 屏幕 %.1fW（亮度 %.0f%%）"
                     " + 风扇 %.1fW + 双SSD %.1fW + 网络 %.1fW + 主板/灯效 %.1fW"
                     " + 供电损耗 %.1fW%s + %s残差校准 %+.1fW（%d 个有效样本）",
                     fusion.cpuSocW, fusion.gpuW, fusion.memoryW, fusion.screenW,
                     context.hasDisplayBrightness ? context.displayBrightnessPercent : 50.0f,
                     fusion.fanW, fusion.storageW, fusion.networkW, fusion.boardW,
                     fusion.conversionLossW,
                     useG815OutletCalibration
                         ? "；插座表校准 1.022× + 25.9W"
                         : "",
                     context.acOnline ? "插电" : "电池",
                     calibration.offsetW, calibration.samples);
        }
    }

    const bool sourceChanged = rawSource != lastRawSource_;
    const unsigned long long modelSamplingMs =
        comparisonRecording_ ? 250ULL : (unsigned long long)samplingMs_;
    const bool sampleDue =
        lastModelSampleTick_ == 0 ||
        now - lastModelSampleTick_ >= modelSamplingMs ||
        sourceChanged ||
        powerStateChanged;
    if (sampleDue)
        lastModelSampleTick_ = now;

    if (sampleDue && fusion.valid && ValidLaptopPower(calibrationReferenceW)) {
        CalibrationState& calibration = CalibrationForPowerState(context.acOnline);
        UpdateFusionCalibration(calibration, calibrationReferenceW, fusionReferenceW);
        if (rawSource == EstimateSource::Fusion) {
            rawPowerW = fusionReferenceW + calibration.offsetW;
            const int calibrationBonus = calibration.samples >= 30 ? 8 :
                                         (calibration.samples >= 10 ? 5 :
                                          (calibration.samples >= 3 ? 2 : 0));
            const int calibrationPenalty = (int)std::clamp(
                calibration.meanAbsErrorW * 0.6f, 0.0f, 18.0f);
            rawConfidence = std::clamp(
                fusion.confidence +
                (useG815OutletCalibration ? 6 : 0) +
                calibrationBonus - calibrationPenalty, 0, 96);
            rawUncertaintyW = (std::max)(
                useG815OutletCalibration ? 6.0f : 5.0f,
                rawPowerW * (100.0f - rawConfidence) * 0.0035f +
                calibration.meanAbsErrorW * 0.5f +
                (useG815OutletCalibration ? kG815OutletStableMaeW : 3.0f));
        }
    }

    hasComparisonRawPower_ =
        rawSource != EstimateSource::None && ValidLaptopPower(rawPowerW);
    comparisonRawPowerW_ = hasComparisonRawPower_ ? rawPowerW : 0.0f;
    lastRawSource_ = rawSource;

    if (rawSource == EstimateSource::None || !ValidLaptopPower(rawPowerW)) {
        context.hasEstimatedSystemPower = false;
        context.estimatedSystemPowerW = 0.0f;
        context.systemPowerEstimated = false;
        context.systemPowerEstimateConfidence = 0;
        context.systemPowerEstimateUncertaintyW = 0.0f;
        context.systemPowerEstimateBreakdown[0] = '\0';
        snprintf(context.powerFailureReason, sizeof(context.powerFailureReason),
                 "%s", context.acOnline ? "system power sensor not found" : "battery discharge rate unavailable");
        context.automaticSystemPowerSource[0] = '\0';
    } else {
        const float smoothed = SmoothEstimate(
            rawPowerW, rawSource, now, sampleDue, powerStateChanged);
        const float transitionGapW = std::fabs(rawPowerW - smoothed);
        const int transitionPenalty = (int)std::clamp(
            transitionGapW / 5.0f, 0.0f, 15.0f);
        context.estimatedSystemPowerW = smoothed;
        context.hasEstimatedSystemPower = ValidLaptopPower(smoothed);
        context.systemPowerEstimated = rawEstimated;
        context.systemPowerEstimateConfidence =
            std::clamp(rawConfidence - transitionPenalty, 0, 100);
        context.systemPowerEstimateUncertaintyW =
            rawUncertaintyW + transitionGapW * 0.35f;
        context.powerFailureReason[0] = '\0';
        snprintf(context.automaticSystemPowerSource,
                 sizeof(context.automaticSystemPowerSource),
                 "%s", automaticSource);
        snprintf(context.systemPowerEstimateBreakdown,
                 sizeof(context.systemPowerEstimateBreakdown),
                 "%s", estimateBreakdown);
        warnedNoSource_ = false;
    }

    if (!context.hasEstimatedSystemPower && !warnedNoSource_) {
        OutputDebugStringA("LaptopPowerFeature: no total power source available; showing N/A.\n");
        warnedNoSource_ = true;
    }

    const bool batteryAssistDetected =
        batteryAssistAlertEnabled_ &&
        context.acOnline &&
        context.hasBatteryDischargePower &&
        context.batteryDischargePowerW > (float)batteryAssistThresholdW_;
    if (batteryAssistDetected) {
        if (batteryAssistStartTick_ == 0)
            batteryAssistStartTick_ = now;
        batteryAssistAlertActive_ =
            now - batteryAssistStartTick_ >= (unsigned long long)batteryAssistSeconds_ * 1000ULL;
    } else {
        batteryAssistStartTick_ = 0;
        batteryAssistAlertActive_ = false;
    }

    WriteDebugAvailability(context);
    RecordComparisonSample(context, now);
}

bool LaptopPowerFeature::StartComparisonRecording()
{
    if (shuttingDown_.load(std::memory_order_acquire))
        return false;
    if (comparisonRecording_)
        return true;

    comparisonError_[0] = '\0';
    wchar_t exePath[32768] = {};
    const DWORD exeLength =
        GetModuleFileNameW(nullptr, exePath, ARRAYSIZE(exePath));
    if (exeLength == 0 || exeLength >= ARRAYSIZE(exePath)) {
        snprintf(comparisonError_, sizeof(comparisonError_),
                 "无法读取程序目录，错误码 %lu",
                 (unsigned long)GetLastError());
        return false;
    }

    const std::filesystem::path executablePath(exePath);
    const std::filesystem::path executableDirectory =
        executablePath.parent_path();
    if (executableDirectory.empty()) {
        snprintf(comparisonError_, sizeof(comparisonError_),
                 "%s", "程序目录格式无效");
        return false;
    }

    const std::filesystem::path comparisonDirectory =
        executableDirectory / L"PowerComparisonRecords";
    std::error_code directoryError;
    std::filesystem::create_directories(
        comparisonDirectory, directoryError);
    std::error_code directoryCheckError;
    const bool directoryReady =
        std::filesystem::is_directory(
            comparisonDirectory, directoryCheckError);
    if (directoryError || directoryCheckError || !directoryReady) {
        const int errorCode = directoryError
            ? directoryError.value()
            : directoryCheckError.value();
        snprintf(comparisonError_, sizeof(comparisonError_),
                 "无法创建记录目录，错误码 %d",
                 errorCode);
        return false;
    }
    comparisonDirectoryW_ = comparisonDirectory.wstring();
    if (!WideToUtf8Text(comparisonDirectoryW_,
                        comparisonDirectoryUtf8_,
                        sizeof(comparisonDirectoryUtf8_))) {
        snprintf(comparisonDirectoryUtf8_,
                 sizeof(comparisonDirectoryUtf8_),
                 "%s", "PowerComparisonRecords");
    }

    SYSTEMTIME local = {};
    GetLocalTime(&local);
    snprintf(comparisonSessionId_, sizeof(comparisonSessionId_),
             "%04u%02u%02u-%02u%02u%02u",
             local.wYear, local.wMonth, local.wDay,
             local.wHour, local.wMinute, local.wSecond);
    char fileName[96] = {};
    snprintf(fileName, sizeof(fileName),
             "power_comparison_%s.csv", comparisonSessionId_);
    std::wstring fileNameW;
    for (const char* p = fileName; *p; ++p)
        fileNameW.push_back(static_cast<unsigned char>(*p));
    const std::filesystem::path comparisonFile =
        comparisonDirectory / fileNameW;
    comparisonFilePathW_ = comparisonFile.wstring();
    if (!WideToUtf8Text(comparisonFilePathW_,
                        comparisonFilePathUtf8_,
                        sizeof(comparisonFilePathUtf8_))) {
        snprintf(comparisonFilePathUtf8_,
                 sizeof(comparisonFilePathUtf8_),
                 "%s", fileName);
    }

    comparisonStream_.open(
        comparisonFile,
        std::ios::out | std::ios::trunc | std::ios::binary);
    if (!comparisonStream_) {
        snprintf(comparisonError_, sizeof(comparisonError_),
                 "无法创建 CSV 文件，状态 0x%X",
                 (unsigned int)comparisonStream_.rdstate());
        comparisonFilePathW_.clear();
        comparisonFilePathUtf8_[0] = '\0';
        return false;
    }

    comparisonStream_.write("\xEF\xBB\xBF", 3);
    comparisonStream_
        << "session_id,local_time,elapsed_ms,sample_index,ac_online,"
           "system_power_w,raw_system_power_w,fusion_internal_w,"
           "fusion_outlet_calibrated_w,sensor_sample_sequence,"
           "sensor_sample_age_ms,system_power_available,software_estimated,"
           "source,confidence_percent,uncertainty_w,cpu_package_w,"
           "discrete_gpu_w,gpu_w,ac_input_w,battery_charge_w,"
           "battery_discharge_w,fps,in_game\n";
    comparisonStream_.flush();
    if (!comparisonStream_) {
        snprintf(comparisonError_, sizeof(comparisonError_),
                 "%s", "CSV 表头写入失败");
        StopComparisonRecording();
        return false;
    }

    comparisonStartTick_ = GetTickCount64();
    lastComparisonSampleTick_ = 0;
    comparisonSampleCount_ = 0;
    {
        std::lock_guard<std::mutex> lock(comparisonWriterMutex_);
        comparisonWriterQueue_.clear();
        comparisonWriterStop_ = false;
    }
    comparisonWriterFailed_.store(false, std::memory_order_release);
    try {
        comparisonWriterThread_ = std::thread(
            &LaptopPowerFeature::ComparisonWriterMain, this);
    } catch (...) {
        snprintf(comparisonError_, sizeof(comparisonError_),
                 "%s", "无法启动 CSV 写入线程");
        comparisonStream_.close();
        comparisonFilePathW_.clear();
        comparisonFilePathUtf8_[0] = '\0';
        std::lock_guard<std::mutex> lock(comparisonWriterMutex_);
        comparisonWriterQueue_.clear();
        comparisonWriterStop_ = true;
        return false;
    }
    comparisonRecording_ = true;
    return true;
}

void LaptopPowerFeature::ComparisonWriterMain()
{
    unsigned int rowsSinceFlush = 0;
    for (;;) {
        std::deque<std::string> batch;
        {
            std::unique_lock<std::mutex> lock(comparisonWriterMutex_);
            comparisonWriterCv_.wait(lock, [this]() {
                return comparisonWriterStop_ || !comparisonWriterQueue_.empty();
            });
            if (comparisonWriterQueue_.empty() && comparisonWriterStop_)
                break;
            batch.swap(comparisonWriterQueue_);
        }

        for (const std::string& row : batch) {
            comparisonStream_ << row;
            ++rowsSinceFlush;
            if (rowsSinceFlush >= 4) {
                comparisonStream_.flush();
                rowsSinceFlush = 0;
            }
            if (!comparisonStream_) {
                comparisonWriterFailed_.store(true, std::memory_order_release);
                break;
            }
        }
        if (comparisonWriterFailed_.load(std::memory_order_acquire))
            break;
    }
    comparisonStream_.flush();
    if (!comparisonStream_)
        comparisonWriterFailed_.store(true, std::memory_order_release);
}

void LaptopPowerFeature::StopComparisonRecording()
{
    comparisonRecording_ = false;
    {
        std::lock_guard<std::mutex> lock(comparisonWriterMutex_);
        comparisonWriterStop_ = true;
    }
    comparisonWriterCv_.notify_one();
    if (comparisonWriterThread_.joinable())
        comparisonWriterThread_.join();
    if (comparisonStream_.is_open()) {
        comparisonStream_.close();
    }
    {
        std::lock_guard<std::mutex> lock(comparisonWriterMutex_);
        comparisonWriterQueue_.clear();
    }
}

void LaptopPowerFeature::RecordComparisonSample(
    const FeatureContext& context,
    unsigned long long now)
{
    constexpr unsigned long long kComparisonIntervalMs = 250;
    if (!comparisonRecording_)
        return;
    if (comparisonWriterFailed_.load(std::memory_order_acquire)) {
        snprintf(comparisonError_, sizeof(comparisonError_),
                 "%s", "CSV 写入失败，记录已停止");
        StopComparisonRecording();
        return;
    }
    if (lastComparisonSampleTick_ != 0 &&
        now - lastComparisonSampleTick_ < kComparisonIntervalMs) {
        return;
    }
    lastComparisonSampleTick_ = now;

    SYSTEMTIME local = {};
    GetLocalTime(&local);
    char timeText[40] = {};
    snprintf(timeText, sizeof(timeText),
             "%04u-%02u-%02u %02u:%02u:%02u.%03u",
             local.wYear, local.wMonth, local.wDay,
             local.wHour, local.wMinute, local.wSecond,
             local.wMilliseconds);

    auto valueOrBlank = [](bool available, float value) {
        if (!available || !std::isfinite(value))
            return std::string();
        char buffer[32] = {};
        snprintf(buffer, sizeof(buffer), "%.3f", value);
        return std::string(buffer);
    };
    const bool comparisonPowerFresh =
        !context.systemPowerEstimated || context.hasPowerSensorSample;

    std::ostringstream row;
    row
        << comparisonSessionId_ << ','
        << CsvText(timeText) << ','
        << (now - comparisonStartTick_) << ','
        << comparisonSampleCount_++ << ','
        << (context.acOnline ? 1 : 0) << ','
        << valueOrBlank(context.hasEstimatedSystemPower &&
                            comparisonPowerFresh,
                        context.estimatedSystemPowerW) << ','
        << valueOrBlank(hasComparisonRawPower_ &&
                            comparisonPowerFresh,
                        comparisonRawPowerW_) << ','
        << valueOrBlank(hasComparisonFusionInternalPower_ &&
                            comparisonPowerFresh,
                        comparisonFusionInternalPowerW_) << ','
        << valueOrBlank(hasComparisonFusionOutletPower_ &&
                            comparisonPowerFresh,
                        comparisonFusionOutletPowerW_) << ','
        << context.powerSensorSampleSequence << ','
        << (context.hasPowerSensorSample
                ? std::to_string(context.powerSensorSampleAgeMs)
                : std::string()) << ','
        << (context.hasEstimatedSystemPower &&
                comparisonPowerFresh ? 1 : 0) << ','
        << (context.systemPowerEstimated ? 1 : 0) << ','
        << CsvText(context.automaticSystemPowerSource) << ','
        << context.systemPowerEstimateConfidence << ','
        << valueOrBlank(context.systemPowerEstimateConfidence > 0,
                        context.systemPowerEstimateUncertaintyW) << ','
        << valueOrBlank(context.hasCpuPackagePower &&
                            comparisonPowerFresh,
                        context.cpuPackagePowerW) << ','
        << valueOrBlank(context.hasDiscreteGpuPower &&
                            comparisonPowerFresh,
                        context.discreteGpuPowerW) << ','
        << valueOrBlank(context.hasGpuPower &&
                            comparisonPowerFresh,
                        context.gpuPowerW) << ','
        << valueOrBlank(context.hasAcInputPower,
                        context.acInputPowerW) << ','
        << valueOrBlank(context.hasBatteryChargePower,
                        context.batteryChargePowerW) << ','
        << valueOrBlank(context.hasBatteryDischargePower,
                        context.batteryDischargePowerW) << ','
        << valueOrBlank(context.fps > 0.0f, context.fps) << ','
        << (context.isInGame ? 1 : 0) << '\n';

    {
        std::lock_guard<std::mutex> lock(comparisonWriterMutex_);
        constexpr size_t kMaximumQueuedRows = 4096;
        if (comparisonWriterQueue_.size() >= kMaximumQueuedRows) {
            comparisonWriterFailed_.store(true, std::memory_order_release);
            return;
        }
        comparisonWriterQueue_.push_back(row.str());
    }
    comparisonWriterCv_.notify_one();
}

void LaptopPowerFeature::Shutdown()
{
    shuttingDown_.store(true, std::memory_order_release);
    StopComparisonRecording();
    if (batteryPollThread_.joinable())
        batteryPollThread_.join();
}

void LaptopPowerFeature::DrawOverlay(const FeatureContext& context)
{
    if (!enabled_ || context.gameppLayout)
        return;

    if (showOverlay_ && showBatteryStatus_ && context.hasBatteryPercent) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored(ImVec4(.52f, .90f, .70f, 1.0f),
                           "电池 %d%%  %s",
                           context.batteryPercent,
                           context.batteryCharging ? "充电中" :
                               (context.acOnline ? "已接入电源" : "电池供电"));
    }

    if (!batteryAssistAlertEnabled_ || !batteryAssistAlertActive_ ||
        !context.acOnline || !context.hasBatteryDischargePower)
        return;

    ImGui::Spacing();
    ImGui::Separator();
    if (context.batteryDischargePowerW > 10.0f) {
        ImGui::TextColored(ImVec4(1.0f, 0.18f, 0.12f, 1.0f),
                           "警告：电池辅助供电 %.1fW",
                           context.batteryDischargePowerW);
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.18f, 1.0f),
                           "电池辅助供电 %.1fW",
                           context.batteryDischargePowerW);
    }
}

size_t LaptopPowerFeature::GetInlineOverlayMetrics(const FeatureContext& context,
                                                   InlineOverlayMetric* output,
                                                   size_t capacity) const
{
    if (!enabled_ || !showOverlay_ || !output || capacity == 0)
        return 0;

    const float known = KnownComponentsPower(context);
    const bool hasKnown = ValidLaptopPower(known);
    const bool filterForNonGame = onlyShowPowerItemsInGame_ && !context.isInGame;
    const bool displaySystemPower =
        showSystemPower_ && (!filterForNonGame || nonGameShowSystemPower_);
    const bool displayKnownComponentsPower =
        showKnownComponentsPower_ && (!filterForNonGame || nonGameShowKnownComponentsPower_);
    const bool displayBatteryDischargePower =
        showBatteryDischargePower_ &&
        (!filterForNonGame || nonGameShowBatteryDischargePower_);
    size_t count = 0;

    auto addMetric = [&](const char* label, const char* value, InlineMetricTone tone) {
        if (count >= capacity)
            return;
        snprintf(output[count].label, sizeof(output[count].label), "%s", label);
        snprintf(output[count].value, sizeof(output[count].value), "%s", value);
        output[count].tone = tone;
        ++count;
    };

    char systemBuf[32] = "N/A";
    if (context.hasEstimatedSystemPower)
        snprintf(systemBuf, sizeof(systemBuf), "%s%.0fW",
                 context.systemPowerEstimated ? "≈" : "",
                 context.estimatedSystemPowerW);

    char inputBuf[32] = "N/A";
    if (context.hasAcInputPower)
        snprintf(inputBuf, sizeof(inputBuf), "%.0fW", context.acInputPowerW);

    char knownBuf[32] = "N/A";
    if (hasKnown)
        snprintf(knownBuf, sizeof(knownBuf), "%.0fW", known);

    char batteryDischargeBuf[32] = "N/A";
    if (context.hasBatteryDischargePower)
        snprintf(batteryDischargeBuf, sizeof(batteryDischargeBuf), "%.1fW",
                 context.batteryDischargePowerW);

    char batteryStatusBuf[48] = "N/A";
    if (context.hasBatteryPercent) {
        snprintf(batteryStatusBuf, sizeof(batteryStatusBuf), "%d%% %s",
                 context.batteryPercent,
                 context.batteryCharging ? "充电" :
                     (context.acOnline ? "插电" : "电池"));
    }

    if (showDcInputPower_) {
        addMetric("DC输入", inputBuf,
                  context.hasAcInputPower ? InlineMetricTone::Warning : InlineMetricTone::Neutral);
    }
    if (displaySystemPower) {
        addMetric("整机", systemBuf,
                  context.hasEstimatedSystemPower ? InlineMetricTone::Warning : InlineMetricTone::Neutral);
    }
    if (displayKnownComponentsPower && hasKnown)
        addMetric("已知", knownBuf, InlineMetricTone::Info);
    if (displayBatteryDischargePower) {
        addMetric("电池放电", batteryDischargeBuf,
                  context.hasBatteryDischargePower
                      ? InlineMetricTone::Warning
                      : InlineMetricTone::Neutral);
    }
    if (showBatteryStatus_) {
        addMetric("电池", batteryStatusBuf,
                  context.hasBatteryPercent
                      ? InlineMetricTone::Good
                      : InlineMetricTone::Neutral);
    }
    return count;
}

bool LaptopPowerFeature::DrawSettings(FeatureContext& context)
{
    bool changed = false;
    const float known = KnownComponentsPower(context);
    char input[32], automaticPower[32], discharge[32], charge[32], cpu[32], gpu[32], knownBuf[32];

    SettingsUi::Subsection("悬浮窗显示", "选择要加入现有横向状态栏的功耗指标");
    if (ImGui::BeginTable("##power_display_grid", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        changed |= ImGui::Checkbox("DC 实时输入", &showDcInputPower_);
        ImGui::TableNextColumn();
        changed |= ImGui::Checkbox("整机功耗", &showSystemPower_);
        ImGui::TableNextColumn();
        changed |= ImGui::Checkbox("已知组件功耗", &showKnownComponentsPower_);
        ImGui::TableNextColumn();
        changed |= ImGui::Checkbox("电池放电功耗", &showBatteryDischargePower_);
        ImGui::TableNextColumn();
        changed |= ImGui::Checkbox("电池电量与供电状态", &showBatteryStatus_);
        ImGui::EndTable();
    }
    changed |= ImGui::Checkbox("仅游戏中显示功耗项", &onlyShowPowerItemsInGame_);
    ImGui::BeginDisabled(!onlyShowPowerItemsInGame_);
    ImGui::Indent(18.0f);
    SettingsUi::Muted("未识别到游戏时仍显示");
    if (ImGui::BeginTable("##non_game_power_grid", 3, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        changed |= ImGui::Checkbox("整机##non_game", &nonGameShowSystemPower_);
        ImGui::TableNextColumn();
        changed |= ImGui::Checkbox("已知组件##non_game", &nonGameShowKnownComponentsPower_);
        ImGui::TableNextColumn();
        changed |= ImGui::Checkbox("电池放电##non_game", &nonGameShowBatteryDischargePower_);
        ImGui::EndTable();
    }
    ImGui::Unindent(18.0f);
    ImGui::EndDisabled();

    SettingsUi::Subsection("估算与提醒", "控制传感器融合模型和插电辅助供电提醒");
    changed |= ImGui::Checkbox("允许估算整机功耗", &allowEstimatedSystemPower_);
    changed |= ImGui::Checkbox("启用电池偷电提醒", &batteryAssistAlertEnabled_);
    ImGui::BeginDisabled(!batteryAssistAlertEnabled_);
    SettingsUi::Muted("提醒阈值");
    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::SliderInt("##battery_assist_threshold", &batteryAssistThresholdW_, 1, 30, "%d W");
    SettingsUi::Muted("持续时间");
    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::SliderInt("##battery_assist_duration", &batteryAssistSeconds_, 1, 60, "%d 秒");
    ImGui::EndDisabled();

    SettingsUi::Subsection("实时状态", "当前数据不受悬浮窗显示开关影响");
    SettingsUi::Status("供电模式：", context.acOnline ? "已接入电源" : "电池供电", true);
    char batteryState[64] = "N/A";
    if (context.hasBatteryPercent) {
        snprintf(batteryState, sizeof(batteryState), "%d%% / %s",
                 context.batteryPercent,
                 context.batteryCharging ? "充电中" :
                     (context.acOnline ? "已接入电源" : "电池供电"));
    }
    if (ImGui::BeginTable("##power_live_values", 2,
                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("项目", ImGuiTableColumnFlags_WidthStretch, 0.62f);
        ImGui::TableSetupColumn("当前值", ImGuiTableColumnFlags_WidthStretch, 0.38f);
        auto row = [](const char* label, const char* value) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(ImVec4(.55f, .61f, .70f, 1.0f), "%s", label);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(value);
        };
        row("DC 实时输入",
            PowerText(context.acInputPowerW, context.hasAcInputPower, input, sizeof(input)));
        row("自动整机功耗",
            PowerText(context.estimatedSystemPowerW, context.hasEstimatedSystemPower,
                      automaticPower, sizeof(automaticPower)));
        row("电池放电",
            PowerText(context.batteryDischargePowerW, context.hasBatteryDischargePower,
                      discharge, sizeof(discharge)));
        row("电池充电",
            PowerText(context.batteryChargePowerW, context.hasBatteryChargePower,
                      charge, sizeof(charge)));
        row("电池电量 / 状态", batteryState);
        row("CPU Package",
            PowerText(context.cpuPackagePowerW, context.hasCpuPackagePower, cpu, sizeof(cpu)));
        row("GPU",
            PowerText(context.gpuPowerW, context.hasGpuPower, gpu, sizeof(gpu)));
        row("已知组件合计",
            PowerText(known, ValidLaptopPower(known), knownBuf, sizeof(knownBuf)));
        ImGui::EndTable();
    }

    if (context.systemPowerEstimated) {
        ImGui::TextColored(ImVec4(.80f, .68f, .35f, 1.0f), "当前整机功耗为软件估算值");
    }
    if (!showSystemPower_) {
        SettingsUi::Muted("整机功耗当前已从悬浮窗隐藏");
    }
    ImGui::TextWrapped("自动来源：%s",
                       context.automaticSystemPowerSource[0]
                           ? context.automaticSystemPowerSource
                           : "未找到");
    if (context.systemPowerEstimateConfidence > 0) {
        ImGui::ProgressBar(context.systemPowerEstimateConfidence / 100.0f,
                           ImVec2(-1.0f, 0.0f), "模型置信度");
        SettingsUi::Muted("置信度 %d%% · 预计误差 ±%.0fW",
                          context.systemPowerEstimateConfidence,
                          context.systemPowerEstimateUncertaintyW);
    }
    if (context.systemPowerEstimateBreakdown[0])
        ImGui::TextWrapped("估算构成：%s", context.systemPowerEstimateBreakdown);
    if (context.acOnline && context.hasBatteryDischargePower) {
        ImGui::TextColored(ImVec4(1.00f, .55f, .25f, 1.0f),
                           "检测到插电状态下电池辅助供电");
    }

    SettingsUi::Subsection("实测对比记录",
                           "录制电力检测仪画面时，同步保存软件功耗时间轴");
    if (!comparisonRecording_) {
        if (ImGui::Button("开始实时记录", ImVec2(-1.0f, 0.0f)))
            StartComparisonRecording();
    } else {
        const unsigned long long elapsed =
            GetTickCount64() - comparisonStartTick_;
        char recordingStatus[128] = {};
        snprintf(recordingStatus, sizeof(recordingStatus),
                 "记录中 · %llu.%01llu 秒 · %llu 个样本",
                 elapsed / 1000ULL, (elapsed % 1000ULL) / 100ULL,
                 comparisonSampleCount_);
        SettingsUi::Status("当前状态：", recordingStatus, true);
        if (context.hasEstimatedSystemPower) {
            ImGui::TextColored(ImVec4(.30f, .78f, 1.0f, 1.0f),
                               "当前整机估算：%.1fW",
                               context.estimatedSystemPowerW);
        }
        ImGui::Text("记录编号：%s", comparisonSessionId_);
        if (ImGui::Button("停止并保存记录", ImVec2(-1.0f, 0.0f)))
            StopComparisonRecording();
    }
    if (comparisonFilePathUtf8_[0])
        ImGui::TextWrapped("CSV：%s", comparisonFilePathUtf8_);
    if (!comparisonDirectoryW_.empty() &&
        ImGui::Button("打开记录目录", ImVec2(-1.0f, 0.0f))) {
        ShellExecuteW(nullptr, L"open", comparisonDirectoryW_.c_str(),
                      nullptr, nullptr, SW_SHOWNORMAL);
    }
    if (comparisonError_[0])
        ImGui::TextColored(ImVec4(1.0f, .42f, .32f, 1.0f),
                           "%s", comparisonError_);
    SettingsUi::MutedWrapped(
        "建议手机开拍后先同时拍到设置页的记录编号和电力表，再只拍电力表；"
        "结束时停止记录。发送视频和对应 CSV 即可按 250ms 时间轴对齐。");

    SettingsUi::Subsection("传感器工具");
    SettingsUi::Muted("AIDA64 需在“外部应用程序”中开启共享内存并重启 AIDA64。");
    if (ImGui::Button("重新扫描功耗传感器", ImVec2(-1.0f, 0.0f)) &&
        context.rescanPowerSensors) {
        context.rescanPowerSensors();
    }
    return changed;
}
