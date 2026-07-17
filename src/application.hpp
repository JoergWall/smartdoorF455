#pragma once

/**
 * @file application.hpp
 * @brief Main application controller for the smartdoorF455 door-opener.
 *
 * This header defines the orchestration layer that initializes the camera,
 * GPIO sensor, optional MQTT/Telegram integrations and the LED matrix display.
 */

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "config.hpp"
#include "matrix_display.hpp"
#include "RealSenseID/DiscoverDevices.h"
#include "RealSenseID/FaceAuthenticator.h"
#include <mosquitto.h>
#include <opencv2/opencv.hpp>
#include <tgbot/tgbot.h>
#include <wiringPi.h>

class Application;

/**
 * @brief Callback bridge for authentication events emitted by the RealSense ID SDK.
 */
class AuthCallback : public RealSenseID::AuthenticationCallback
{
public:
    /**
     * @brief Construct a callback that forwards results back to the application.
     * @param application Parent application instance.
     */
    explicit AuthCallback(Application& application);

    /**
     * @brief Handle a completed authentication attempt.
     * @param status Result of the authentication process.
     * @param user_id Identifier returned for a recognized user.
     */
    void OnResult(const RealSenseID::AuthenticateStatus status, const char* user_id) override;

    /**
     * @brief Handle authentication hints provided by the SDK.
     * @param hint Hint code returned during processing.
     */
    void OnHint(const RealSenseID::AuthenticateStatus hint) override;

    /**
     * @brief Handle detected faces reported by the SDK.
     * @param faces Faces detected in the current frame.
     * @param ts Timestamp of the detection event.
     */
    void OnFaceDetected(const std::vector<RealSenseID::FaceRect>& faces, const unsigned int ts) override;

private:
    Application& application_;
};

/**
 * @brief Main controller for the smartdoorF455 application lifecycle.
 *
 * The application wires together the camera authenticator, GPIO presence sensor,
 * MQTT/Telegram messaging and LED matrix rendering into one coherent runtime.
 */
class Application
{
public:
    /**
     * @brief Construct a new application instance.
     */
    Application();

    /**
     * @brief Destroy the application and release owned resources.
     */
    ~Application();

    /**
     * @brief Initialize all subsystems from a TOML configuration file.
     * @param configPath Path to the configuration file.
     * @return true when initialization completed successfully, false otherwise.
     */
    bool initialize(const std::string& configPath);

    /**
     * @brief Run the application event loop until interrupted.
     * @return Exit status for the process.
     */
    int run();

    /**
     * @brief React to a presence event from the sensor.
     * @param wfiStatus WiringPi status information for the event.
     */
    void onPresenceDetected(const WPIWfiStatus& wfiStatus);

    /**
     * @brief Handle the result of an authentication attempt.
     * @param status Authentication result code.
     * @param userId User identifier returned by the camera.
     */
    void handleAuthenticationResult(const RealSenseID::AuthenticateStatus status, const char* userId);

    /**
     * @brief Send a text message through Telegram when enabled.
     * @param message Message body to send.
     */
    void sendTelegramText(const std::string& message);

private:
    bool initWiringPi();
    bool initCamera();
    bool initMosquitto();
    bool initTelegram();
    void cleanup();
    void publishDoorOpen();
    void takeSnapshotAndSend();
    void enqueueNotification(std::function<void()> task);
    void notificationWorkerLoop();
    std::string getLastAuthenticatedName() const;
    void setLastAuthenticatedName(const std::string& name);
    std::string getHomeDirectory() const;
    std::string getSnapshotDirectory() const;

    Config config_;
    std::unique_ptr<RealSenseID::FaceAuthenticator> authenticator_;
    RealSenseID::DeviceInfo deviceInfo_{};
    RealSenseID::SerialConfig serialConfig_{};
    std::string serialPort_;

    std::unique_ptr<MatrixDisplay> matrixDisplay_;
    // Declared before bot_ so it outlives the Bot, which keeps a reference to it.
    std::unique_ptr<TgBot::BoostHttpOnlySslClient> httpClient_;
    std::unique_ptr<TgBot::Bot> bot_;

    std::atomic<bool> interruptRequested_{false};
    mutable std::mutex lastAuthenticatedNameMutex_;
    mutable std::string lastAuthenticatedName_;
    std::chrono::steady_clock::time_point nameSetTime_ = std::chrono::steady_clock::now();
    static constexpr int NAME_DISPLAY_TIMEOUT_SEC = 5; // Display authenticated name for 5 seconds
    bool firstPresence_{true};
    std::chrono::steady_clock::time_point lastPresenceTime_ = std::chrono::steady_clock::now();
    std::atomic<bool> authenticationInProgress_{false};

    bool useTelegram_ = false;
    bool sendSnapshot_ = false;
    bool useMosquitto_ = false;
    long chatId_ = 0;
    std::string botToken_;
    std::string topicDoor_;
    std::string topicControl_;
    struct mosquitto* mosq_ = nullptr;

    // Background worker that runs Telegram/snapshot I/O off the GPIO interrupt thread,
    // so a slow or hanging network send never delays presence handling / reauthentication.
    std::thread notificationThread_;
    std::mutex notificationMutex_;
    std::condition_variable notificationCv_;
    std::deque<std::function<void()>> notificationQueue_;
    bool notificationStop_ = false;
    static constexpr std::size_t MAX_PENDING_NOTIFICATIONS = 8;
    static constexpr int TELEGRAM_TIMEOUT_SEC = 10;

    int gpioSensorPin_ = 0;
    int gpioSensorPull_ = 0;
    int waitTimeUntilReauth_ = 3;

    AuthCallback authCallback_;

    bool cleanedUp_ = false;

    static Application* instance_;
    static void signalHandler(int signum);

    friend void presenceDetectedCallback(struct WPIWfiStatus wfiStatus, void* userdata);
};

void presenceDetectedCallback(struct WPIWfiStatus wfiStatus, void* userdata);
