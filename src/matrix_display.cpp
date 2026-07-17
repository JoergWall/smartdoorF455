#include "matrix_display.hpp"
#include <array>
#include <filesystem>
#include <iomanip>

/**
 * @file matrix_display.cpp
 * @brief RGB matrix rendering and lifecycle implementation.
 */
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

namespace {
std::string resolveFontPath(const std::string& fontFile, const std::string& fontDir)
{
    std::vector<std::filesystem::path> candidates;

    auto addCandidate = [&](const std::filesystem::path& path) {
        if (!path.empty()) {
            candidates.push_back(path);
        }
    };

    // Configured font directory takes precedence when provided.
    if (!fontDir.empty()) {
        addCandidate(std::filesystem::path(fontDir) / fontFile);
    }

    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    if (!ec) {
        addCandidate(cwd / fontFile);
        addCandidate(cwd / "fonts" / fontFile);
        addCandidate(cwd / ".." / "fonts" / fontFile);
        addCandidate(cwd / ".." / "build" / "external" / "rpi-rgb-led-matrix-src" / "fonts" / fontFile);
        addCandidate(cwd / ".." / ".." / "build" / "external" / "rpi-rgb-led-matrix-src" / "fonts" / fontFile);
        addCandidate(cwd / ".." / ".." / "external" / "rpi-rgb-led-matrix-src" / "fonts" / fontFile);
    }

    std::array<char, 4096> exeBuffer{};
    const ssize_t exeLength = readlink("/proc/self/exe", exeBuffer.data(), exeBuffer.size() - 1);
    if (exeLength > 0) {
        std::filesystem::path exePath(exeBuffer.data(), exeBuffer.data() + exeLength);
        const std::filesystem::path exeDir = exePath.parent_path();
        addCandidate(exeDir / fontFile);
        addCandidate(exeDir / "fonts" / fontFile);
        addCandidate(exeDir / ".." / "fonts" / fontFile);
        addCandidate(exeDir / ".." / "build" / "external" / "rpi-rgb-led-matrix-src" / "fonts" / fontFile);
        addCandidate(exeDir / ".." / ".." / "build" / "external" / "rpi-rgb-led-matrix-src" / "fonts" / fontFile);
        addCandidate(exeDir / ".." / ".." / "external" / "rpi-rgb-led-matrix-src" / "fonts" / fontFile);
    }

    for (const auto& candidate : candidates) {
        std::error_code existsError;
        if (std::filesystem::exists(candidate, existsError) && !existsError) {
            return candidate.string();
        }
    }

    return {};
}
}

MatrixDisplay::MatrixDisplay(const Config& config, NameProvider nameProvider)
    : config_(config), nameProvider_(std::move(nameProvider))
{
    intervalMs_ = config_.getInt("matrix_options.update_interval_ms", 100);
}

MatrixDisplay::~MatrixDisplay()
{
    stop();
}

bool MatrixDisplay::loadFonts()
{
    const std::string fontDir = config_.getString("matrix_options.font_dir", "");
    const std::string path6x12 = resolveFontPath("6x12.bdf", fontDir);
    const std::string path4x6 = resolveFontPath("4x6.bdf", fontDir);

    const bool ok = !path6x12.empty() && !path4x6.empty() &&
        font6x12_.LoadFont(path6x12.c_str()) &&
        font4x6_.LoadFont(path4x6.c_str());

    if (!ok) {
        std::cerr << "Warning: failed to load matrix fonts; continuing without LED display" << std::endl;
    }
    return ok;
}

std::string MatrixDisplay::formatTime(const std::tm& timeinfo, const char* format) const
{
    std::ostringstream ss;
    ss << std::put_time(&timeinfo, format);
    return ss.str();
}

void MatrixDisplay::render()
{
    if (!matrix_ || !offscreen_) {
        return;
    }

    auto now = std::chrono::system_clock::now();
    std::time_t in_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm timeinfo;
#if defined(_WIN32)
    localtime_s(&timeinfo, &in_time_t);
#else
    localtime_r(&in_time_t, &timeinfo);
#endif

    offscreen_->Fill(bgColor_.r, bgColor_.g, bgColor_.b);
    rgb_matrix::DrawText(offscreen_, font6x12_, 0, 7, clockColor_, nullptr, formatTime(timeinfo, "%H:%M").c_str(), 0);
    rgb_matrix::DrawText(offscreen_, font4x6_, 0, 13, dayColor_, nullptr, formatTime(timeinfo, "%A").c_str(), 0);
    rgb_matrix::DrawText(offscreen_, font6x12_, 0, 21, dateColor_, nullptr, formatTime(timeinfo, "%d.%m").c_str(), 0);

    const std::string userName = nameProvider_();
    if (!userName.empty()) {
        rgb_matrix::DrawText(offscreen_, font6x12_, 0, 29, usernameColor_, nullptr, userName.c_str(), 0);
    }

    offscreen_ = matrix_->SwapOnVSync(offscreen_);
}

bool MatrixDisplay::start()
{
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }

    auto clockColor = config_.getColor("matrix_options.clock_color", {255, 28, 0});
    auto dateColor = config_.getColor("matrix_options.date_color", {255, 28, 0});
    auto dayColor = config_.getColor("matrix_options.day_color", {255, 28, 0});
    auto usernameColor = config_.getColor("matrix_options.username_color", {255, 0, 255});
    auto bgColor = config_.getColor("matrix_options.bg_color", {0, 0, 0});
    auto outlineColor = config_.getColor("matrix_options.outline_color", {0, 0, 0});

    clockColor_ = rgb_matrix::Color(clockColor[0], clockColor[1], clockColor[2]);
    dateColor_ = rgb_matrix::Color(dateColor[0], dateColor[1], dateColor[2]);
    dayColor_ = rgb_matrix::Color(dayColor[0], dayColor[1], dayColor[2]);
    usernameColor_ = rgb_matrix::Color(usernameColor[0], usernameColor[1], usernameColor[2]);
    bgColor_ = rgb_matrix::Color(bgColor[0], bgColor[1], bgColor[2]);
    outlineColor_ = rgb_matrix::Color(outlineColor[0], outlineColor[1], outlineColor[2]);

    const bool fontsLoaded = loadFonts();
    if (!fontsLoaded) {
        std::cerr << "Continuing without matrix display" << std::endl;
        running_.store(true, std::memory_order_release);
        return true;
    }

    matrixOptions_.led_rgb_sequence = "RBG";
    matrixOptions_.rows = 32;
    matrixOptions_.cols = 64;
    matrixOptions_.brightness = config_.getInt("matrix_options.brightness", 80);
    pixelMapperConfig_ = config_.getString("matrix_options.pixel_mapper_config", "");
    matrixOptions_.pixel_mapper_config = pixelMapperConfig_.c_str();
    matrixOptions_.disable_hardware_pulsing = true;
    hardwareMapping_ = config_.getString("matrix_options.hardware_mapping", "");
    matrixOptions_.hardware_mapping = hardwareMapping_.c_str();

    matrix_ = rgb_matrix::RGBMatrix::CreateFromOptions(matrixOptions_, runtimeOptions_);
    if (matrix_ == nullptr) {
        std::cerr << "Warning: failed to create RGBMatrix; continuing without LED display" << std::endl;
        running_.store(true, std::memory_order_release);
        return true;
    }

    offscreen_ = matrix_->CreateFrameCanvas();
    if (offscreen_ == nullptr) {
        std::cerr << "Warning: failed to create offscreen canvas; continuing without LED display" << std::endl;
        delete matrix_;
        matrix_ = nullptr;
        running_.store(true, std::memory_order_release);
        return true;
    }

    running_.store(true, std::memory_order_release);
    executor_.start(std::chrono::milliseconds(intervalMs_), [this]() { render(); });
    return true;
}

void MatrixDisplay::stop()
{
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    running_.store(false, std::memory_order_release);
    executor_.stop();
    if (matrix_) {
        matrix_->Clear();
        delete matrix_;
        matrix_ = nullptr;
    }
    offscreen_ = nullptr;
}
