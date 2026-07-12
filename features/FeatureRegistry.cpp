#include "FeatureRegistry.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "imgui.h"
#include "LaptopPowerFeature.h"
#include "SettingsUi.h"

namespace {

class TemperatureAlertFeature final : public Feature {
public:
    const char* name() const override { return "温度警告"; }
    const char* configKey() const override { return "feature.temperature_alert"; }
    bool enabled() const override { return enabled_; }
    void setEnabled(bool value) override { enabled_ = value; }

    void SetThreshold(int value) { thresholdC_ = std::clamp(value, 50, 110); }
    int Threshold() const { return thresholdC_; }

    void DrawOverlay(const FeatureContext& context) override
    {
        if (!enabled_) return;
        const bool cpuHot = context.hasCpuTemp && context.cpuTempC >= (float)thresholdC_;
        const bool gpuHot = context.hasGpuTemp && context.gpuTempC >= (float)thresholdC_;
        if (!cpuHot && !gpuHot) return;

        ImGui::Spacing();
        ImGui::Separator();
        if (cpuHot)
            ImGui::TextColored(ImVec4(1.0f, 0.25f, 0.18f, 1.0f), "CPU 温度警告 %.0f°C", context.cpuTempC);
        if (gpuHot)
            ImGui::TextColored(ImVec4(1.0f, 0.25f, 0.18f, 1.0f), "GPU 温度警告 %.0f°C", context.gpuTempC);
    }

    bool DrawSettings(FeatureContext&) override
    {
        bool changed = false;
        bool en = enabled_;
        if (ImGui::Checkbox("启用温度警告", &en)) {
            enabled_ = en;
            changed = true;
        }
        ImGui::BeginDisabled(!enabled_);
        changed |= ImGui::SliderInt("温度阈值##temperature_alert", &thresholdC_, 50, 110, "%d°C");
        ImGui::EndDisabled();
        return changed;
    }

private:
    bool enabled_ = true;
    int thresholdC_ = 85;
};

class LowFpsAlertFeature final : public Feature {
public:
    const char* name() const override { return "低帧率提醒"; }
    const char* configKey() const override { return "feature.low_fps_alert"; }
    bool enabled() const override { return enabled_; }
    void setEnabled(bool value) override
    {
        enabled_ = value;
        lowFpsAccum_ = 0.0f;
        lastUpdateTick_ = 0;
        active_ = false;
    }

    void SetThreshold(int value) { threshold_ = std::clamp(value, 15, 240); }
    int Threshold() const { return threshold_; }
    void SetSeconds(float value) { seconds_ = std::clamp(value, 1.0f, 15.0f); }
    float Seconds() const { return seconds_; }

    void Update(FeatureContext& context) override
    {
        if (!enabled_ || !context.isInGame || context.fps <= 0.0f) {
            lowFpsAccum_ = 0.0f;
            active_ = false;
            lastUpdateTick_ = 0;
            return;
        }

        const ULONGLONG now = GetTickCount64();
        if (lastUpdateTick_ == 0) {
            lastUpdateTick_ = now;
            return;
        }

        const ULONGLONG elapsedMs = now - lastUpdateTick_;
        lastUpdateTick_ = now;
        if (elapsedMs > 1000) {
            // Do not turn a sleep/resume or a long UI stall into a false
            // continuous low-FPS alert.
            lowFpsAccum_ = 0.0f;
            active_ = false;
            return;
        }

        const float dt = static_cast<float>(elapsedMs) / 1000.0f;
        if (context.fps < (float)threshold_) {
            lowFpsAccum_ += dt;
        } else {
            lowFpsAccum_ = 0.0f;
        }
        active_ = lowFpsAccum_ >= seconds_;
    }

    void DrawOverlay(const FeatureContext& context) override
    {
        if (!enabled_ || !active_) return;
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.15f, 1.0f),
                           "FPS 偏低 %.0f < %d，已持续 %.1fs",
                           context.fps, threshold_, lowFpsAccum_);
    }

    bool DrawSettings(FeatureContext&) override
    {
        bool changed = false;
        bool en = enabled_;
        if (ImGui::Checkbox("启用低 FPS 提醒", &en)) {
            setEnabled(en);
            changed = true;
        }
        ImGui::BeginDisabled(!enabled_);
        changed |= ImGui::SliderInt("FPS 阈值##low_fps", &threshold_, 15, 240);
        changed |= ImGui::SliderFloat("持续秒数##low_fps", &seconds_, 1.0f, 15.0f, "%.1fs");
        ImGui::EndDisabled();
        return changed;
    }

private:
    bool enabled_ = true;
    int threshold_ = 45;
    float seconds_ = 3.0f;
    float lowFpsAccum_ = 0.0f;
    bool active_ = false;
    ULONGLONG lastUpdateTick_ = 0;
};

class GameAutoOverlayFeature final : public Feature {
public:
    const char* name() const override { return "游戏自动悬浮窗"; }
    const char* configKey() const override { return "feature.game_auto_overlay"; }
    bool enabled() const override { return enabled_; }
    void setEnabled(bool value) override
    {
        enabled_ = value;
        wasInGame_ = false;
        featureMadeOverlayVisible_ = false;
    }

    void Update(FeatureContext& context) override
    {
        if (!enabled_) {
            wasInGame_ = context.isInGame;
            return;
        }

        if (context.isInGame && !wasInGame_) {
            featureMadeOverlayVisible_ = !context.overlayVisible;
            if (featureMadeOverlayVisible_) {
                context.requestOverlayVisible = true;
                context.hasOverlayVisibilityRequest = true;
            }
        } else if (!context.isInGame && wasInGame_) {
            if (featureMadeOverlayVisible_ && context.overlayVisible) {
                context.requestOverlayVisible = false;
                context.hasOverlayVisibilityRequest = true;
            }
            featureMadeOverlayVisible_ = false;
        }
        wasInGame_ = context.isInGame;
    }

    bool DrawSettings(FeatureContext& context) override
    {
        bool changed = false;
        bool en = enabled_;
        if (ImGui::Checkbox("检测到游戏时自动显示覆盖层", &en)) {
            setEnabled(en);
            changed = true;
        }
        ImGui::TextColored(ImVec4(.55f, .55f, .60f, 1.0f), "当前状态：%s / %s",
                           context.isInGame ? "游戏中" : "未识别到游戏",
                           context.overlayVisible ? "覆盖层显示" : "覆盖层隐藏");
        return changed;
    }

private:
    bool enabled_ = false;
    bool wasInGame_ = false;
    bool featureMadeOverlayVisible_ = false;
};

class GamePeakStatsFeature final : public Feature {
public:
    const char* name() const override { return "游戏实时峰值"; }
    const char* configKey() const override { return "feature.game_peak_stats"; }
    bool enabled() const override { return enabled_; }
    void setEnabled(bool value) override
    {
        enabled_ = value;
        if (!enabled_)
            Reset();
    }

    void Update(FeatureContext& context) override
    {
        const bool active = enabled_ && context.gameProcessId != 0 &&
                            (context.isInGame || context.fps > 0.0f);
        if (!active) {
            Reset();
            return;
        }
        if (trackedPid_ != context.gameProcessId) {
            Reset();
            trackedPid_ = context.gameProcessId;
        }

        cpuUsagePeak_ = (std::max)(cpuUsagePeak_, context.cpuUsage);
        gpuUsagePeak_ = (std::max)(gpuUsagePeak_, context.gpuUsage);
        if (context.hasCpuTemp)
            cpuTempPeak_ = (std::max)(cpuTempPeak_, context.cpuTempC);
        if (context.hasGpuTemp)
            gpuTempPeak_ = (std::max)(gpuTempPeak_, context.gpuTempC);
        if (context.hasCpuPackagePower)
            cpuPowerPeak_ = (std::max)(cpuPowerPeak_, context.cpuPackagePowerW);
        if (context.hasGpuPower)
            gpuPowerPeak_ = (std::max)(gpuPowerPeak_, context.gpuPowerW);
    }

    bool DrawSettings(FeatureContext& context) override
    {
        bool changed = false;
        bool value = enabled_;
        if (ImGui::Checkbox("启用游戏峰值监测", &value)) {
            setEnabled(value);
            changed = true;
        }

        SettingsUi::Status("当前目标：",
                           trackedPid_ != 0 && context.gameProcessName[0]
                               ? context.gameProcessName : "未记录",
                           trackedPid_ != 0);
        if (ImGui::BeginTable("##game_peak_values", 2,
                              ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("项目", ImGuiTableColumnFlags_WidthStretch, 0.62f);
            ImGui::TableSetupColumn("峰值", ImGuiTableColumnFlags_WidthStretch, 0.38f);
            auto row = [](const char* label, float value, const char* format) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(ImVec4(.55f, .61f, .70f, 1.0f), "%s", label);
                ImGui::TableNextColumn();
                if (value > 0.0f)
                    ImGui::Text(format, value);
                else
                    ImGui::TextUnformatted("N/A");
            };
            row("CPU 最高占用", cpuUsagePeak_, "%.0f%%");
            row("CPU 最高温度", cpuTempPeak_, "%.0f°C");
            row("CPU 最高功耗", cpuPowerPeak_, "%.0f W");
            row("GPU 最高占用", gpuUsagePeak_, "%.0f%%");
            row("GPU 最高温度", gpuTempPeak_, "%.0f°C");
            row("GPU 最高功耗", gpuPowerPeak_, "%.0f W");
            ImGui::EndTable();
        }
        SettingsUi::Muted("只保留当前游戏内存数据，切换或退出游戏后清零");
        return changed;
    }

private:
    void Reset()
    {
        trackedPid_ = 0;
        cpuUsagePeak_ = 0.0f;
        cpuTempPeak_ = 0.0f;
        cpuPowerPeak_ = 0.0f;
        gpuUsagePeak_ = 0.0f;
        gpuTempPeak_ = 0.0f;
        gpuPowerPeak_ = 0.0f;
    }

    bool enabled_ = true;
    DWORD trackedPid_ = 0;
    float cpuUsagePeak_ = 0.0f;
    float cpuTempPeak_ = 0.0f;
    float cpuPowerPeak_ = 0.0f;
    float gpuUsagePeak_ = 0.0f;
    float gpuTempPeak_ = 0.0f;
    float gpuPowerPeak_ = 0.0f;
};

TemperatureAlertFeature* AsTemperature(Feature* feature)
{
    return dynamic_cast<TemperatureAlertFeature*>(feature);
}

LowFpsAlertFeature* AsLowFps(Feature* feature)
{
    return dynamic_cast<LowFpsAlertFeature*>(feature);
}

LaptopPowerFeature* AsLaptopPower(Feature* feature)
{
    return dynamic_cast<LaptopPowerFeature*>(feature);
}

} // namespace

FeatureRegistry::FeatureRegistry()
{
    features_.push_back(std::make_unique<TemperatureAlertFeature>());
    features_.push_back(std::make_unique<LowFpsAlertFeature>());
    features_.push_back(std::make_unique<GameAutoOverlayFeature>());
    features_.push_back(std::make_unique<GamePeakStatsFeature>());
    features_.push_back(std::make_unique<LaptopPowerFeature>());
}

FeatureRegistry::~FeatureRegistry() = default;

void FeatureRegistry::LoadSettings(const FeatureSettings& settings)
{
    for (auto& feature : features_) {
        if (std::strcmp(feature->configKey(), "feature.temperature_alert") == 0) {
            feature->setEnabled(settings.temperatureAlertEnabled);
            if (auto* f = AsTemperature(feature.get()))
                f->SetThreshold(settings.temperatureThresholdC);
        } else if (std::strcmp(feature->configKey(), "feature.low_fps_alert") == 0) {
            feature->setEnabled(settings.lowFpsAlertEnabled);
            if (auto* f = AsLowFps(feature.get())) {
                f->SetThreshold(settings.lowFpsThreshold);
                f->SetSeconds(settings.lowFpsSeconds);
            }
        } else if (std::strcmp(feature->configKey(), "feature.game_auto_overlay") == 0) {
            feature->setEnabled(settings.gameAutoOverlayEnabled);
        } else if (std::strcmp(feature->configKey(), "feature.game_peak_stats") == 0) {
            feature->setEnabled(settings.gamePeakStatsEnabled);
        } else if (std::strcmp(feature->configKey(), "feature.laptop_power") == 0) {
            feature->setEnabled(settings.laptopPowerEnabled);
            if (auto* f = AsLaptopPower(feature.get()))
                f->LoadSettings(settings);
        }
    }
}

FeatureSettings FeatureRegistry::GetSettings() const
{
    FeatureSettings settings;
    for (const auto& feature : features_) {
        if (std::strcmp(feature->configKey(), "feature.temperature_alert") == 0) {
            settings.temperatureAlertEnabled = feature->enabled();
            if (auto* f = AsTemperature(feature.get()))
                settings.temperatureThresholdC = f->Threshold();
        } else if (std::strcmp(feature->configKey(), "feature.low_fps_alert") == 0) {
            settings.lowFpsAlertEnabled = feature->enabled();
            if (auto* f = AsLowFps(feature.get())) {
                settings.lowFpsThreshold = f->Threshold();
                settings.lowFpsSeconds = f->Seconds();
            }
        } else if (std::strcmp(feature->configKey(), "feature.game_auto_overlay") == 0) {
            settings.gameAutoOverlayEnabled = feature->enabled();
        } else if (std::strcmp(feature->configKey(), "feature.game_peak_stats") == 0) {
            settings.gamePeakStatsEnabled = feature->enabled();
        } else if (std::strcmp(feature->configKey(), "feature.laptop_power") == 0) {
            settings.laptopPowerEnabled = feature->enabled();
            if (auto* f = AsLaptopPower(feature.get()))
                f->SaveSettings(settings);
        }
    }
    return settings;
}

void FeatureRegistry::Init()
{
    for (auto& feature : features_)
        feature->Init();
}

void FeatureRegistry::Update(FeatureContext& context)
{
    for (auto& feature : features_)
        feature->Update(context);
}

void FeatureRegistry::DrawOverlay(const FeatureContext& context)
{
    for (auto& feature : features_)
        feature->DrawOverlay(context);
}

size_t FeatureRegistry::GetInlineOverlayMetrics(const FeatureContext& context,
                                                InlineOverlayMetric* output,
                                                size_t capacity) const
{
    if (!output || capacity == 0)
        return 0;

    size_t count = 0;
    for (const auto& feature : features_) {
        if (count >= capacity)
            break;
        count += feature->GetInlineOverlayMetrics(context, output + count, capacity - count);
    }
    return count;
}

bool FeatureRegistry::DrawSettings(FeatureContext& context)
{
    bool changed = false;
    for (auto& feature : features_) {
        ImGui::PushID(feature->configKey());
        if (SettingsUi::BeginCard(feature->configKey(), feature->name())) {
            changed |= feature->DrawSettings(context);
        }
        SettingsUi::EndCard();
        ImGui::PopID();
    }
    return changed;
}

bool FeatureRegistry::ToggleLaptopPowerQuickOption(LaptopPowerQuickOption option)
{
    for (auto& feature : features_) {
        if (std::strcmp(feature->configKey(), "feature.laptop_power") == 0) {
            if (auto* laptopPower = AsLaptopPower(feature.get()))
                return laptopPower->ToggleQuickOption(option);
        }
    }
    return false;
}

bool FeatureRegistry::IsPowerComparisonRecording() const
{
    for (const auto& feature : features_) {
        if (std::strcmp(feature->configKey(), "feature.laptop_power") == 0) {
            if (auto* laptopPower = AsLaptopPower(feature.get()))
                return laptopPower->IsComparisonRecording();
        }
    }
    return false;
}

void FeatureRegistry::Shutdown()
{
    for (auto& feature : features_)
        feature->Shutdown();
}
