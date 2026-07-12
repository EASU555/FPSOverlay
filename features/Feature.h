#pragma once

#include <cstddef>

#include "FeatureContext.h"

enum class InlineMetricTone {
    Neutral,
    Good,
    Warning,
    Info
};

struct InlineOverlayMetric {
    char label[24] = "";
    char value[64] = "";
    InlineMetricTone tone = InlineMetricTone::Neutral;
};

class Feature {
public:
    virtual ~Feature() = default;

    virtual const char* name() const = 0;
    virtual const char* configKey() const = 0;

    virtual bool enabled() const = 0;
    virtual void setEnabled(bool value) = 0;

    virtual void Init() {}
    virtual void Update(FeatureContext& context) { (void)context; }
    virtual void DrawOverlay(const FeatureContext& context) { (void)context; }
    virtual size_t GetInlineOverlayMetrics(const FeatureContext& context,
                                           InlineOverlayMetric* output,
                                           size_t capacity) const
    {
        (void)context;
        (void)output;
        (void)capacity;
        return 0;
    }
    virtual bool DrawSettings(FeatureContext& context) { (void)context; return false; }
    virtual void Shutdown() {}
};
