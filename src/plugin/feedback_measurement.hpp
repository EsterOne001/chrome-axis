#pragma once

#include "chrome_axis/diagnostics.hpp"
#include "chrome_axis/feedback.hpp"

namespace chrome_axis {

class FeedbackMeasurement {
public:
    void invalidate(PerformanceReport& report) noexcept
    {
        if (report.state != PerformanceState::capturing)
            return;
        invalidatedRequest_ = report.requestId;
        report.referenceFeedbackGain = 0;
        report.feedbackSetupSignature = 0;
        report.feedbackProfile = {};
    }

    void identify(PerformanceReport& report, const FeedbackSettings& settings, const Profile& profile,
                  const DiagnosticSnapshot& snapshot) const noexcept
    {
        if (report.forceSamples || report.requestId == invalidatedRequest_)
            return;
        report.referenceFeedbackGain = settings.strength;
        report.feedbackSetupSignature = feedbackMeasurementSignature(settings, profile);
        report.feedbackProfile = snapshot.profile;
    }

private:
    std::uint64_t invalidatedRequest_{};
};

} // namespace chrome_axis
