#pragma once

#include <vector>
#include <memory>

#include "Feature.h"

struct FeatureSettings {
    bool temperatureAlertEnabled = true;
    bool lowFpsAlertEnabled = true;
    bool gameAutoOverlayEnabled = false;
    bool gameSessionReportEnabled = true;
    bool gameSessionReportAutoOpen = true;
    bool gameSessionReportSaveCsv = true;
    bool laptopPowerEnabled = true;

    int temperatureThresholdC = 85;
    int lowFpsThreshold = 45;
    float lowFpsSeconds = 3.0f;
    bool laptopPowerShowOverlay = true;
    bool laptopPowerShowDcInputPower = true;
    bool laptopPowerShowSystemPower = true;
    bool laptopPowerShowKnownComponentsPower = true;
    bool laptopPowerShowBatteryDischargePower = true;
    bool laptopPowerShowBatteryStatus = false;
    bool laptopPowerOnlyShowPowerItemsInGame = false;
    bool laptopPowerNonGameShowSystemPower = false;
    bool laptopPowerNonGameShowKnownComponentsPower = false;
    bool laptopPowerNonGameShowBatteryDischargePower = false;
    bool laptopPowerAllowEstimatedSystemPower = true;
    bool laptopPowerShowEstimated = true;
    bool laptopPowerShowSource = true;
    int laptopPowerSamplingMs = 2000;
    int laptopPowerSmoothSeconds = 5;
    int laptopPowerWarningThresholdW = 100;
    int laptopPowerAdapterWarningW = 65;
    int laptopPowerDisplayMode = 1;
    bool laptopPowerBatteryAssistAlertEnabled = true;
    int laptopPowerBatteryAssistThresholdW = 3;
    int laptopPowerBatteryAssistSeconds = 10;
};

enum class LaptopPowerQuickOption {
    ShowSystemPower,
    ShowKnownComponentsPower,
    ShowBatteryDischargePower,
    AllowEstimatedSystemPower,
};

class FeatureRegistry {
public:
    FeatureRegistry();
    ~FeatureRegistry();

    void LoadSettings(const FeatureSettings& settings);
    FeatureSettings GetSettings() const;

    void Init();
    void Update(FeatureContext& context);
    void DrawOverlay(const FeatureContext& context);
    size_t GetInlineOverlayMetrics(const FeatureContext& context,
                                   InlineOverlayMetric* output,
                                   size_t capacity) const;
    bool DrawSettings(FeatureContext& context);
    bool DrawGameSessionReportPage(FeatureContext& context);
    bool ConsumeGameSessionReportOpenRequest();
    bool HasCompletedGameSession() const;
    bool GameSessionReportNeedsSensorPolling() const;
    bool ToggleLaptopPowerQuickOption(LaptopPowerQuickOption option);
    bool IsPowerComparisonRecording() const;
    void Shutdown();

private:
    std::vector<std::unique_ptr<Feature>> features_;
};
