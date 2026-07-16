#pragma once

#include <memory>

#include "Feature.h"

struct GameSessionSample {
    double elapsedSeconds = 0.0;

    float fps = 0.0f;
    bool fpsValid = false;
    float cpuUsage = 0.0f;
    bool cpuUsageValid = false;
    float cpuTemperature = 0.0f;
    bool cpuTemperatureValid = false;
    float cpuPackagePower = 0.0f;
    bool cpuPackagePowerValid = false;
    float gpuUsage = 0.0f;
    bool gpuUsageValid = false;
    float gpuTemperature = 0.0f;
    bool gpuTemperatureValid = false;
    float gpuPower = 0.0f;
    bool gpuPowerValid = false;
    float ramUsagePercent = 0.0f;
    bool ramUsagePercentValid = false;
    float ramUsedGb = 0.0f;
    bool ramUsedGbValid = false;
    float vramUsagePercent = 0.0f;
    bool vramUsagePercentValid = false;
    float vramUsedGb = 0.0f;
    bool vramUsedGbValid = false;
    float systemPower = 0.0f;
    bool systemPowerValid = false;
    bool systemPowerEstimated = false;
    int powerConfidence = 0;
};

class GameSessionReportFeature final : public Feature {
public:
    GameSessionReportFeature();
    ~GameSessionReportFeature() override;

    const char* name() const override;
    const char* configKey() const override;
    bool enabled() const override;
    void setEnabled(bool value) override;

    void SetAutoOpen(bool value);
    bool AutoOpen() const;
    void SetSaveCsv(bool value);
    bool SaveCsv() const;

    void Init() override;
    void Update(FeatureContext& context) override;
    bool DrawSettings(FeatureContext& context) override;
    void Shutdown() override;

    bool DrawReportPage(FeatureContext& context);
    bool ConsumeOpenRequest();
    bool HasCompletedSession() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
