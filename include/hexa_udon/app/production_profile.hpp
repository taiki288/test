#pragma once
#include "hexa_udon/app/auto_client.hpp"
#include <filesystem>
namespace hexa_udon::app {
// Only explicitly approved, immutable profiles are accepted. Throws on invalid input.
void validate_production_profile(const std::filesystem::path& path);
void apply_production_profile(AutoClientConfig& config, const std::filesystem::path& path);
void apply_16x16_production_profile(AutoClientConfig& config);
}
