#pragma once

/**
 * @file matrix_display.hpp
 * @brief Wrapper around the RGB LED matrix display.
 *
 * The MatrixDisplay class initializes the LED panel, renders the clock and user
 * information and provides a safe no-op fallback when the hardware is unavailable.
 */

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "config.hpp"
#include "PeriodicExecutor.hpp"
#include "graphics.h"
#include "led-matrix.h"
#include <ctime>

/**
 * @brief Render time and authentication state on an RGB LED matrix.
 */
class MatrixDisplay
{
public:
    using NameProvider = std::function<std::string()>;

    /**
     * @brief Construct a matrix display renderer.
     * @param config Configuration values for colors, mapping and timing.
     * @param nameProvider Callback used to obtain the currently authenticated user.
     */
    MatrixDisplay(const Config& config, NameProvider nameProvider);

    /**
     * @brief Destroy the display renderer and release the matrix resources.
     */
    ~MatrixDisplay();

    /**
     * @brief Start the display loop and initialize the matrix.
     * @return true when the display is ready or when startup has safely degraded to headless mode.
     */
    bool start();

    /**
     * @brief Stop the display loop and release the matrix.
     */
    void stop();

private:
    void render();
    std::string formatTime(const std::tm& timeinfo, const char* format) const;
    bool loadFonts();

    const Config& config_;
    NameProvider nameProvider_;
    std::atomic<bool> running_{false};
    PeriodicExecutor<> executor_;

    rgb_matrix::Color clockColor_{};
    rgb_matrix::Color dateColor_{};
    rgb_matrix::Color dayColor_{};
    rgb_matrix::Color usernameColor_{};
    rgb_matrix::Color bgColor_{};
    rgb_matrix::Color outlineColor_{};

    rgb_matrix::Font fontTime_;
    rgb_matrix::Font fontDate_;
    rgb_matrix::Font fontDay_;
    rgb_matrix::Font fontName_;

    rgb_matrix::RGBMatrix::Options matrixOptions_{};
    rgb_matrix::RuntimeOptions runtimeOptions_{};
    std::string pixelMapperConfig_;
    std::string hardwareMapping_;

    rgb_matrix::FrameCanvas* offscreen_ = nullptr;
    rgb_matrix::RGBMatrix* matrix_ = nullptr;
    int intervalMs_ = 100;
};
