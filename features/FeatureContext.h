#pragma once

#include <windows.h>

struct FeatureContext {
    float fps = 0.0f;
    float frametimeMs = 0.0f;

    float cpuUsage = 0.0f;
    float cpuTempC = 0.0f;
    bool hasCpuTemp = false;

    float gpuUsage = 0.0f;
    float gpuTempC = 0.0f;
    bool hasGpuTemp = false;
    float gpuPowerW = 0.0f;
    bool hasGpuPower = false;
    float discreteGpuPowerW = 0.0f;
    bool hasDiscreteGpuPower = false;
    float cpuPackagePowerW = 0.0f;
    bool hasCpuPackagePower = false;
    float cpuMemoryPowerW = 0.0f;
    bool hasCpuMemoryPower = false;
    float cpuPlatformPowerW = 0.0f;
    bool hasCpuPlatformPower = false;
    bool gpuPowerIsDiscrete = false;
    unsigned long long powerSensorSampleSequence = 0;
    unsigned long long powerSensorSampleAgeMs = 0;
    bool hasPowerSensorSample = false;
    unsigned long long hardwareSensorSampleAgeMs = 0;
    bool hasHardwareSensorSample = false;
    bool hardwareSensorDataStale = false;

    float vramUsedGb = 0.0f;
    float vramTotalGb = 0.0f;
    float ramUsedGb = 0.0f;
    float ramTotalGb = 0.0f;
    float cpuFanRpm = 0.0f;
    float gpuFanRpm = 0.0f;
    float systemFanRpm = 0.0f;
    float diskActivityPercent = 0.0f;
    int diskCount = 0;
    float networkUtilizationPercent = 0.0f;
    float displayBrightnessPercent = 50.0f;
    bool hasDisplayBrightness = false;
    bool isRogStrixG815LR = false;

    char foregroundWindowTitle[256] = "";
    char gameProcessName[768] = "";
    DWORD gameProcessId = 0;
    bool isInGame = false;
    bool isOnAcPower = true;
    bool acOnline = true;
    int batteryPercent = 0;
    bool hasBatteryPercent = false;
    bool batteryCharging = false;
    float acInputPowerW = 0.0f;
    bool hasAcInputPower = false;
    char acInputPowerSource[96] = "";
    float batteryChargePowerW = 0.0f;
    bool hasBatteryChargePower = false;
    float batteryDischargePowerW = 0.0f;
    bool hasBatteryDischargePower = false;
    float estimatedSystemPowerW = 0.0f;
    bool hasEstimatedSystemPower = false;
    bool systemPowerEstimated = false;
    char powerSource[96] = "";
    char automaticSystemPowerSource[128] = "";
    int systemPowerEstimateConfidence = 0;
    float systemPowerEstimateUncertaintyW = 0.0f;
    char systemPowerEstimateBreakdown[384] = "";
    char powerFailureReason[96] = "";
    void (*rescanPowerSensors)() = nullptr;
    SYSTEMTIME currentTime = {};

    bool overlayVisible = true;
    bool gameppLayout = false;
    bool requestOverlayVisible = false;
    bool hasOverlayVisibilityRequest = false;
};
