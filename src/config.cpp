#include "config.hpp"
#include <algorithm>

/**
 * @file config.cpp
 * @brief Implementation of the configuration loader and typed accessors.
 */
#include <sstream>

const std::unordered_map<std::string, RealSenseID::DeviceConfig::CameraRotation> Config::s_cameraRotationMap = {
    {"0", RealSenseID::DeviceConfig::CameraRotation::Rotation_0_Deg},
    {"90", RealSenseID::DeviceConfig::CameraRotation::Rotation_90_Deg},
    {"180", RealSenseID::DeviceConfig::CameraRotation::Rotation_180_Deg},
    {"270", RealSenseID::DeviceConfig::CameraRotation::Rotation_270_Deg},
};

const std::unordered_map<std::string, RealSenseID::DeviceConfig::SecurityLevel> Config::s_securityLevelMap = {
    {"High", RealSenseID::DeviceConfig::SecurityLevel::High},
    {"Medium", RealSenseID::DeviceConfig::SecurityLevel::Medium},
    {"Low", RealSenseID::DeviceConfig::SecurityLevel::Low},
};

const std::unordered_map<std::string, RealSenseID::DeviceConfig::AlgoFlow> Config::s_algoFlowMap = {
    {"All", RealSenseID::DeviceConfig::AlgoFlow::All},
    {"FaceDetectionOnly", RealSenseID::DeviceConfig::AlgoFlow::FaceDetectionOnly},
    {"SpoofOnly", RealSenseID::DeviceConfig::AlgoFlow::SpoofOnly},
    {"RecognitionOnly", RealSenseID::DeviceConfig::AlgoFlow::RecognitionOnly},
};

const std::unordered_map<std::string, RealSenseID::DeviceConfig::DumpMode> Config::s_dumpModeMap = {
    {"None", RealSenseID::DeviceConfig::DumpMode::None},
    {"CroppedFace", RealSenseID::DeviceConfig::DumpMode::CroppedFace},
    {"FullFrame", RealSenseID::DeviceConfig::DumpMode::FullFrame},
};

const std::unordered_map<std::string, RealSenseID::DeviceConfig::MatcherConfidenceLevel> Config::s_matcherConfidenceMap = {
    {"High", RealSenseID::DeviceConfig::MatcherConfidenceLevel::High},
    {"Medium", RealSenseID::DeviceConfig::MatcherConfidenceLevel::Medium},
    {"Low", RealSenseID::DeviceConfig::MatcherConfidenceLevel::Low},
};

const std::unordered_map<std::string, RealSenseID::DeviceConfig::FrontalFacePolicy> Config::s_frontalFacePolicyMap = {
    {"Strict", RealSenseID::DeviceConfig::FrontalFacePolicy::Strict},
    {"Moderate", RealSenseID::DeviceConfig::FrontalFacePolicy::Moderate},
    {"None", RealSenseID::DeviceConfig::FrontalFacePolicy::None},
};

bool Config::loadFile(const std::string& path, std::string& error)
{
    try {
        config_ = toml::parse_file(path);
    } catch (const toml::parse_error& err) {
        std::ostringstream ss;
        ss << "Failed to parse config file '" << path << "': " << err;
        error = ss.str();
        return false;
    }
    return true;
}

const toml::table& Config::table() const
{
    return config_;
}

bool Config::has(const std::string& dottedPath) const
{
    return findNode(dottedPath).has_value();
}

std::optional<toml::node_view<const toml::node>> Config::findNode(const std::string& dottedPath) const
{
    const toml::node* current = &config_;
    std::string token;
    std::istringstream iss(dottedPath);
    while (std::getline(iss, token, '.')) {
        if (!current->is_table()) {
            return std::nullopt;
        }
        auto table = current->as_table();
        if (!table->contains(token)) {
            return std::nullopt;
        }
        current = &table->at(token);
    }
    return toml::node_view<const toml::node>(*current);
}

bool Config::getBool(const std::string& dottedPath, bool defaultValue) const
{
    auto node = findNode(dottedPath);
    if (!node) {
        return defaultValue;
    }
    if (node->is_boolean()) {
        return node->value<bool>().value_or(defaultValue);
    }
    if (node->is_string()) {
        auto str = node->value<std::string>().value_or("");
        std::transform(str.begin(), str.end(), str.begin(), ::tolower);
        return (str == "true" || str == "1" || str == "yes" || str == "on");
    }
    if (node->is_integer()) {
        return node->value<long long>().value_or(0) != 0;
    }
    return defaultValue;
}

int Config::getInt(const std::string& dottedPath, int defaultValue) const
{
    auto node = findNode(dottedPath);
    if (!node) {
        return defaultValue;
    }
    if (node->is_integer()) {
        return static_cast<int>(node->value<long long>().value_or(defaultValue));
    }
    if (node->is_string()) {
        try {
            return std::stoi(node->value<std::string>().value_or(""));
        } catch (...) {
            return defaultValue;
        }
    }
    return defaultValue;
}

long Config::getLong(const std::string& dottedPath, long defaultValue) const
{
    auto node = findNode(dottedPath);
    if (!node) {
        return defaultValue;
    }
    if (node->is_integer()) {
        return node->value<long long>().value_or(defaultValue);
    }
    if (node->is_string()) {
        try {
            return std::stol(node->value<std::string>().value_or(""));
        } catch (...) {
            return defaultValue;
        }
    }
    return defaultValue;
}

std::string Config::getString(const std::string& dottedPath, const std::string& defaultValue) const
{
    auto node = findNode(dottedPath);
    if (!node) {
        return defaultValue;
    }
    if (node->is_string()) {
        return node->value<std::string>().value_or(defaultValue);
    }
    if (node->is_integer()) {
        return std::to_string(node->value<long long>().value_or(0));
    }
    if (node->is_boolean()) {
        return node->value<bool>().value_or(false) ? "true" : "false";
    }
    return defaultValue;
}

std::array<uint8_t, 3> Config::getColor(const std::string& dottedPath, const std::array<uint8_t, 3>& defaultValue) const
{
    auto node = findNode(dottedPath);
    if (!node || !node->is_array()) {
        return defaultValue;
    }
    std::array<uint8_t, 3> result = defaultValue;
    const auto array = node->as_array();
    for (size_t index = 0; index < 3 && index < array->size(); ++index) {
        const auto& child = array->at(index);
        if (child.is_integer()) {
            result[index] = static_cast<uint8_t>(child.value<long long>().value_or(defaultValue[index]));
        } else if (child.is_string()) {
            try {
                result[index] = static_cast<uint8_t>(std::stoi(child.value<std::string>().value_or("0")));
            } catch (...) {
                result[index] = defaultValue[index];
            }
        }
    }
    return result;
}

std::vector<int> Config::getIntArray(const std::string& dottedPath, const std::vector<int>& defaultValue) const
{
    auto node = findNode(dottedPath);
    if (!node || !node->is_array()) {
        return defaultValue;
    }
    std::vector<int> result;
    const auto array = node->as_array();
    for (const auto& item : *array) {
        if (item.is_integer()) {
            result.push_back(static_cast<int>(item.value<long long>().value_or(0)));
        } else if (item.is_string()) {
            try {
                result.push_back(std::stoi(item.value<std::string>().value_or("0")));
            } catch (...) {
                // ignore invalid entries
            }
        }
    }
    return result;
}

RealSenseID::DeviceConfig Config::getDeviceConfig() const
{
    RealSenseID::DeviceConfig config;
    auto rotation = getString("camera.camera_rotation", "0");
    auto security = getString("camera.security_level", "Medium");
    auto frontalPolicy = getString("camera.frontal_face_policy", "Moderate");
    auto matcherConfidence = getString("camera.matcher_confidence_level", "Medium");
    auto algoFlow = getString("camera.algo_flow", "All");
    auto dumpMode = getString("camera.dump_mode", "None");

    config.camera_rotation = s_cameraRotationMap.count(rotation) ? s_cameraRotationMap.at(rotation) : RealSenseID::DeviceConfig::CameraRotation::Rotation_0_Deg;
    config.security_level = s_securityLevelMap.count(security) ? s_securityLevelMap.at(security) : RealSenseID::DeviceConfig::SecurityLevel::Medium;
    config.frontal_face_policy = s_frontalFacePolicyMap.count(frontalPolicy) ? s_frontalFacePolicyMap.at(frontalPolicy) : RealSenseID::DeviceConfig::FrontalFacePolicy::Moderate;
    config.matcher_confidence_level = s_matcherConfidenceMap.count(matcherConfidence) ? s_matcherConfidenceMap.at(matcherConfidence) : RealSenseID::DeviceConfig::MatcherConfidenceLevel::Medium;
    config.algo_flow = s_algoFlowMap.count(algoFlow) ? s_algoFlowMap.at(algoFlow) : RealSenseID::DeviceConfig::AlgoFlow::All;
    config.dump_mode = s_dumpModeMap.count(dumpMode) ? s_dumpModeMap.at(dumpMode) : RealSenseID::DeviceConfig::DumpMode::None;
    config.max_spoofs = static_cast<unsigned char>(getInt("camera.max_spoofs", 0));
    config.gpio_auth_toggling = static_cast<int>(getInt("camera.gpio_auth_toggling", 0));
    return config;
}
