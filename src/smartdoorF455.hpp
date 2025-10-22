/**
 * @file smartdoorF455.hpp
 * @brief Facial authentication system for door buzzer/opener control
 * @copyright MIT License. See LICENSE file in root directory.
 * @copyright Copyright (C) 2025 Joerg Wallmersperger
 *
 * @details
 * Uses facial authentication to control a door buzzer/door opener system.
 * Tested on Raspberry PI 4b running Raspberry PI OS and Ubuntu 20.4.
 *
 * @section hw_req Hardware Requirements
 * - Raspberry PI4 or higher with >= 4GB RAM
 * - Intel RealSense ID F455 camera 
 * - Presence sensor (PIR sensor HC-SR501 or E18-D80NK IR reflex photoelectric barrier)
 * - P2.5 64x32 RGB LED Matrix Panel (160x80 mm) with HUB75 connector
 * - MEANWELL IRM-60-5ST 5V 10A power supply
 * - Lindby Severina LED outdoor wall lamp (used as casing)
 * - IP Interface to door buzzer/opener (e.g. Siedle Gateway with MQTT interface for Siedle Bus based door intercom)
 * 
 * @section optional_hw Optional Hardware
 * - Adafruit RGB Matrix Bonnet for Raspberry Pi 40PIN GPIO (for LED Matrix Panel connection)
 * - GeeekPi Raspberry Pi 4 Case with passive cooling/heat dissipation heatsink
 *
 * @section sw_req Software Requirements
 * - Operating System: Raspberry Pi OS or Ubuntu Linux V18.4/V20.4 for Raspberry Pi
 * - Intel RealSense ID SDK (Apache License Version 2.0)
 *   @see https://github.com/IntelRealSense/RealSenseID/
 * - rpi-rgb-led-matrix by Henner Zeller (GNU General Public License Version 2.0)
 *   @see https://github.com/hzeller/rpi-rgb-led-matrix
 * 
 * @note See installation instructions in README file
 */
#pragma once
#include "PeriodicExecutor.hpp"
#include "RealSenseID/FaceAuthenticator.h"
#include "RealSenseID/DeviceConfig.h"
#include "RealSenseID/SerialConfig.h"
#include "RealSenseID/Preview.h"
#include "RealSenseID/DiscoverDevices.h"
#include "led-matrix.h"
#include "graphics.h" 
#include <atomic>
#include <chrono>
#include "mosquittopp.h" // required to communicate with Siedle door intercom
#include <time.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <stdio.h>
#include <stdlib.h> 
#include <csignal> 
#include <wiringPi.h>
#include <unistd.h> 
#include <unordered_map>
#include <filesystem>
#include <mqtt/async_client.h>
#include <tgbot/tgbot.h>
#include <boost/bind/bind.hpp>
#include "toml++/toml.hpp" // @see https://marzer.github.io/tomlplusplus/
#include <opencv2/opencv.hpp> // @see https://docs.opencv.org/4.x/
#include <sys/syscall.h>
#include <sys/types.h> // used for process ids


using namespace rgb_matrix;
using namespace std;
using namespace RealSenseID;
#define CONFIG_FILE "./config.toml" 

/**
 * @see https://stackoverflow.com/questions/7163069/c-string-to-enum
 * @brief maps strings of config file to enum values
 */
static std::unordered_map<std::string,RealSenseID::DeviceConfig::CameraRotation> const camera_rotation = { 
   {"0",RealSenseID::DeviceConfig::CameraRotation::Rotation_0_Deg}, 
   {"180",RealSenseID::DeviceConfig::CameraRotation::Rotation_180_Deg}, 
   {"90",RealSenseID::DeviceConfig::CameraRotation::Rotation_90_Deg},
   {"270",RealSenseID::DeviceConfig::CameraRotation::Rotation_270_Deg} };

static std::unordered_map<std::string,RealSenseID::DeviceConfig::SecurityLevel> const security_level = { 
   {"High",RealSenseID::DeviceConfig::SecurityLevel::High}, 
   {"Medium",RealSenseID::DeviceConfig::SecurityLevel::Medium}, 
   {"Low",RealSenseID::DeviceConfig::SecurityLevel::Low} };

static std::unordered_map<std::string,RealSenseID::DeviceConfig::AlgoFlow> const algo_flow = { 
   {"All",RealSenseID::DeviceConfig::AlgoFlow::All}, 
   {"FaceDetectionOnly",RealSenseID::DeviceConfig::AlgoFlow::FaceDetectionOnly}, 
   {"SpoofOnly",RealSenseID::DeviceConfig::AlgoFlow::SpoofOnly},
   {"RecognitionOnly",RealSenseID::DeviceConfig::AlgoFlow::RecognitionOnly} };

static std::unordered_map<std::string,RealSenseID::DeviceConfig::DumpMode> const dump_mode = { 
   {"None",RealSenseID::DeviceConfig::DumpMode::None}, 
   {"CroppedFace",RealSenseID::DeviceConfig::DumpMode::CroppedFace}, 
   {"FullFrame",RealSenseID::DeviceConfig::DumpMode::FullFrame}, };

static std::unordered_map<std::string,RealSenseID::DeviceConfig::MatcherConfidenceLevel> const matcher_confidence_level = { 
   {"High",RealSenseID::DeviceConfig::MatcherConfidenceLevel::High}, 
   {"Medium",RealSenseID::DeviceConfig::MatcherConfidenceLevel::Medium}, 
   {"Low",RealSenseID::DeviceConfig::MatcherConfidenceLevel::Low} };

static std::unordered_map<std::string,RealSenseID::DeviceConfig::FrontalFacePolicy> const frontal_face_policy = { 
   {"Strict",RealSenseID::DeviceConfig::FrontalFacePolicy::Strict}, 
   {"Moderate",RealSenseID::DeviceConfig::FrontalFacePolicy::Moderate},
   {"None",RealSenseID::DeviceConfig::FrontalFacePolicy::None} };



