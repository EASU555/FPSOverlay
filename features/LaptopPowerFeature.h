#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include "Feature.h"
#include "FeatureRegistry.h"

class LaptopPowerFeature final : public Feature {
public:
    const char* name() const override { return "笔记本功耗"; }
    const char* configKey() const override { return "feature.laptop_power"; }

    bool enabled() const override { return enabled_; }
    void setEnabled(bool value) override { enabled_ = value; }

    void LoadSettings(const FeatureSettings& settings);
    void SaveSettings(FeatureSettings& settings) const;
    bool ToggleQuickOption(LaptopPowerQuickOption option);
    bool IsComparisonRecording() const { return comparisonRecording_; }

    void Update(FeatureContext& context) override;
    void DrawOverlay(const FeatureContext& context) override;
    size_t GetInlineOverlayMetrics(const FeatureContext& context,
                                   InlineOverlayMetric* output,
                                   size_t capacity) const override;
    bool DrawSettings(FeatureContext& context) override;
    void Shutdown() override;

private:
    enum class EstimateSource {
        None,
        DirectSystem,
        AcInputDerived,
        BatteryDischarge,
        Fusion,
    };

    struct CalibrationState {
        float offsetW = 0.0f;
        float meanAbsErrorW = 0.0f;
        int samples = 0;
    };

    struct BatteryRateResult {
        bool statusAvailable = false;
        bool idle = false;
        bool hasCharge = false;
        bool hasDischarge = false;
        float chargeW = 0.0f;
        float dischargeW = 0.0f;
    };

    BatteryRateResult QueryWindowsBatteryRate();
    CalibrationState& CalibrationForPowerState(bool acOnline);
    void UpdateFusionCalibration(CalibrationState& state,
                                 float referenceW,
                                 float fusionW);
    float SmoothEstimate(float rawValue,
                         EstimateSource source,
                         unsigned long long now,
                         bool sampleDue,
                         bool reset);
    static bool ValidLaptopPower(float value);
    static float KnownComponentsPower(const FeatureContext& context);
    void WriteDebugAvailability(const FeatureContext& context);
    bool StartComparisonRecording();
    void StopComparisonRecording();
    void RecordComparisonSample(const FeatureContext& context,
                                unsigned long long now);
    void ComparisonWriterMain();
    void ApplyCompletedBatteryPoll();
    void ScheduleBatteryPoll();

    bool enabled_ = true;
    bool showOverlay_ = true;
    bool showDcInputPower_ = true;
    bool showSystemPower_ = true;
    bool showKnownComponentsPower_ = true;
    bool showBatteryDischargePower_ = true;
    bool showBatteryStatus_ = false;
    bool onlyShowPowerItemsInGame_ = false;
    bool nonGameShowSystemPower_ = false;
    bool nonGameShowKnownComponentsPower_ = false;
    bool nonGameShowBatteryDischargePower_ = false;
    bool allowEstimatedSystemPower_ = true;
    bool showEstimated_ = true;
    bool showSource_ = true;
    int samplingMs_ = 2000;
    int smoothSeconds_ = 5;
    int warningThresholdW_ = 100;
    int adapterWarningW_ = 65;
    int displayMode_ = 1; // 0=mini, 1=normal, 2=full
    bool batteryAssistAlertEnabled_ = true;
    int batteryAssistThresholdW_ = 3;
    int batteryAssistSeconds_ = 10;
    unsigned long long batteryAssistStartTick_ = 0;
    bool batteryAssistAlertActive_ = false;

    unsigned long long lastSampleTick_ = 0;
    unsigned long long lastModelSampleTick_ = 0;
    unsigned long long lastSmoothTick_ = 0;
    bool hasSmoothedPower_ = false;
    float smoothedPowerW_ = 0.0f;
    EstimateSource smoothedSource_ = EstimateSource::None;
    EstimateSource lastRawSource_ = EstimateSource::None;
    bool warnedNoSource_ = false;

    bool cachedHasCharge_ = false;
    bool cachedHasDischarge_ = false;
    bool cachedBatteryStatusAvailable_ = false;
    bool cachedBatteryIdle_ = false;
    float cachedChargeW_ = 0.0f;
    float cachedDischargeW_ = 0.0f;
    std::thread batteryPollThread_;
    std::atomic<bool> batteryPollInFlight_{false};
    std::mutex batteryResultMutex_;
    BatteryRateResult pendingBatteryResult_;
    bool pendingBatteryResultReady_ = false;

    bool hasLastAcState_ = false;
    bool lastAcOnline_ = true;
    int lastDebugAvailability_ = -1;
    CalibrationState acCalibration_;
    CalibrationState batteryCalibration_;

    bool comparisonRecording_ = false;
    bool hasComparisonRawPower_ = false;
    bool hasComparisonFusionInternalPower_ = false;
    bool hasComparisonFusionOutletPower_ = false;
    float comparisonRawPowerW_ = 0.0f;
    float comparisonFusionInternalPowerW_ = 0.0f;
    float comparisonFusionOutletPowerW_ = 0.0f;
    unsigned long long comparisonStartTick_ = 0;
    unsigned long long lastComparisonSampleTick_ = 0;
    unsigned long long comparisonSampleCount_ = 0;
    char comparisonSessionId_[32] = "";
    std::wstring comparisonFilePathW_;
    std::wstring comparisonDirectoryW_;
    char comparisonFilePathUtf8_[1024] = "";
    char comparisonDirectoryUtf8_[1024] = "";
    char comparisonError_[160] = "";
    std::ofstream comparisonStream_;
    std::thread comparisonWriterThread_;
    std::mutex comparisonWriterMutex_;
    std::condition_variable comparisonWriterCv_;
    std::deque<std::string> comparisonWriterQueue_;
    bool comparisonWriterStop_ = false;
    std::atomic<bool> comparisonWriterFailed_{false};
};
