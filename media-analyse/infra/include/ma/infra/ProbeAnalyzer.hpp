#pragma once
// ma::infra - concrete Analyzer built on FFmpeg avformat/avcodec.
#include <memory>

#include "ma/core/Analyzer.hpp"

namespace ma::infra {

// Production analyzer. Process-wide FFmpeg init happens on first use.
std::unique_ptr<ma::Analyzer> createProbeAnalyzer();

} // namespace ma::infra
