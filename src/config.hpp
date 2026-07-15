#pragma once

/**
 * @file config.hpp
 * @brief Configuration loader for smartdoorF455.
 *
 * The Config class exposes helpers for reading typed values from a TOML file,
 * including camera settings and display options.
 */

#include <array>
#include <optional>
#include <string>
#include <vector>
#include <unordered_map>

#include "RealSenseID/DeviceConfig.h"
#include "toml++/toml.hpp"

/**
 * @brief Typed access to TOML configuration values.
 */
class Config
{
public:
    /**
     * @brief Load configuration from a TOML file.
     * @param path Path to the configuration file.
     * @param error Receives a human readable error when parsing fails.
     * @return true on success, false otherwise.
     */
    bool loadFile(const std::string& path, std::string& error);

    /**
     * @brief Check whether a dotted configuration path exists.
     * @param dottedPath Path such as "camera.camera_rotation".
     * @return true when the path exists.
     */
    bool has(const std::string& dottedPath) const;

    /**
     * @brief Read a boolean option from the configuration.
     */
    bool getBool(const std::string& dottedPath, bool defaultValue) const;

    /**
     * @brief Read an integer option from the configuration.
     */
    int getInt(const std::string& dottedPath, int defaultValue) const;

    /**
     * @brief Read a long integer option from the configuration.
     */
    long getLong(const std::string& dottedPath, long defaultValue) const;

    /**
     * @brief Read a string option from the configuration.
     */
    std::string getString(const std::string& dottedPath, const std::string& defaultValue) const;

    /**
     * @brief Read an RGB color definition from the configuration.
     */
    std::array<uint8_t, 3> getColor(const std::string& dottedPath, const std::array<uint8_t, 3>& defaultValue) const;

    /**
     * @brief Read an integer array from the configuration.
     */
    std::vector<int> getIntArray(const std::string& dottedPath, const std::vector<int>& defaultValue) const;

    /**
     * @brief Convert the configuration into the RealSense ID device configuration structure.
     * @return Device configuration suitable for the authenticator.
     */
    RealSenseID::DeviceConfig getDeviceConfig() const;

    /**
     * @brief Access the underlying TOML table.
     * @return Read-only TOML table.
     */
    const toml::table& table() const;

private:
    std::optional<toml::node_view<const toml::node>> findNode(const std::string& dottedPath) const;

    toml::table config_;

    static const std::unordered_map<std::string, RealSenseID::DeviceConfig::CameraRotation> s_cameraRotationMap;
    static const std::unordered_map<std::string, RealSenseID::DeviceConfig::SecurityLevel> s_securityLevelMap;
    static const std::unordered_map<std::string, RealSenseID::DeviceConfig::AlgoFlow> s_algoFlowMap;
    static const std::unordered_map<std::string, RealSenseID::DeviceConfig::DumpMode> s_dumpModeMap;
    static const std::unordered_map<std::string, RealSenseID::DeviceConfig::MatcherConfidenceLevel> s_matcherConfidenceMap;
    static const std::unordered_map<std::string, RealSenseID::DeviceConfig::FrontalFacePolicy> s_frontalFacePolicyMap;
};
