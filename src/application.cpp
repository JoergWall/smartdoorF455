#include "application.hpp"
#include <chrono>

/**
 * @file application.cpp
 * @brief Runtime implementation for the smartdoorF455 application controller.
 */
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>
#include <opencv2/opencv.hpp>

Application* Application::instance_ = nullptr;

Application::Application()
    : authCallback_(*this), enrollCallback_(*this)
{
    instance_ = this;
}

Application::~Application()
{
    cleanup();
    if (instance_ == this) {
        instance_ = nullptr;
    }
}

/**
 * @brief Initialize the application from a TOML configuration file.
 * @param configPath Path to the configuration file.
 * @return true when all subsystems could be initialized.
 */
bool Application::initialize(const std::string& configPath)
{
    std::string error;
    if (!config_.loadFile(configPath, error)) {
        std::cerr << error << std::endl;
        return false;
    }

    useTelegram_ = config_.getBool("telegram.use_telegram", false);
    sendSnapshot_ = config_.getBool("telegram.send_snapshot", false);
    botToken_ = config_.getString("telegram.bot_token", "");
    chatId_ = config_.getLong("telegram.chat_id", 0);
    gpioSensorPin_ = config_.getInt("raspi.gpio_sensor_pin", 0);
    gpioSensorPull_ = config_.getInt("raspi.gpio_sensor_pull", 0);
    waitTimeUntilReauth_ = config_.getInt("raspi.wait_time_until_reauthentication", 3);
    useMosquitto_ = config_.getBool("mosquitto.use_mosquitto", false);
    topicDoor_ = config_.getString("mosquitto.topic_door", "");
    topicControl_ = config_.getString("mosquitto.topic_control", "");

    if (!initWiringPi()) {
        return false;
    }
    if (!initCamera()) {
        return false;
    }
    if (useMosquitto_ && !initMosquitto()) {
        return false;
    }
    if (useTelegram_ && !initTelegram()) {
        return false;
    }

    matrixDisplay_ = std::make_unique<MatrixDisplay>(config_, [this]() { return getLastAuthenticatedName(); });
    if (!matrixDisplay_->start()) {
        std::cerr << "Failed to start MatrixDisplay" << std::endl;
        return false;
    }

    return true;
}

/**
 * @brief Enter the main event loop and wait for shutdown.
 * @return Exit code for the process.
 */
int Application::run()
{
    signal(SIGTERM, signalHandler);
    signal(SIGINT, signalHandler);

    std::mutex mutex;
    std::unique_lock<std::mutex> lock(mutex);

    while (!interruptRequested_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    cleanup();
    return 0;
}

/**
 * @brief Release peripherals and stop background services during shutdown.
 */
void Application::cleanup()
{
    if (matrixDisplay_) {
        matrixDisplay_->stop();
    }
    if (useMosquitto_ && mosq_ != nullptr) {
        mosquitto_destroy(mosq_);
        mosquitto_lib_cleanup();
        mosq_ = nullptr;
    }
    if (authenticator_) {
        authenticator_->Disconnect();
        authenticator_.reset();
    }
}

static constexpr int DEBOUNCE_PERIOD_MS = 1000;

bool Application::initWiringPi()
{
    if (gpioSensorPin_ <= 0) {
        std::cerr << "Invalid GPIO sensor pin configured" << std::endl;
        return false;
    }

    if (wiringPiSetupPinType(WPI_PIN_BCM) == -1) {
        std::cerr << "WiringPi failed to initialize GPIO" << std::endl;
        return false;
    }

    pinMode(gpioSensorPin_, INPUT);
    pullUpDnControl(gpioSensorPin_, gpioSensorPull_);
    wiringPiISR2(gpioSensorPin_, INT_EDGE_BOTH, &presenceDetectedCallback, DEBOUNCE_PERIOD_MS, nullptr);
    return true;
}

bool Application::initCamera()
{
    auto devices = RealSenseID::DiscoverDevices();
    if (devices.empty()) {
        std::cerr << "No RealSenseID devices found" << std::endl;
        return false;
    }

    const auto& device = devices.front();
    if (device.deviceType == RealSenseID::DeviceType::Unknown) {
        std::cerr << "Unknown RealSenseID device type" << std::endl;
        return false;
    }

    deviceInfo_ = device;
    serialPort_ = device.serialPort;
    serialConfig_.port = serialPort_.c_str();

    authenticator_ = std::make_unique<RealSenseID::FaceAuthenticator>(deviceInfo_.deviceType);
    auto connectStatus = authenticator_->Connect(serialConfig_);
    if (connectStatus != RealSenseID::Status::Ok) {
        std::cerr << "Failed connecting to device " << serialPort_ << " status: " << connectStatus << std::endl;
        return false;
    }

    auto config = config_.getDeviceConfig();
    auto status = authenticator_->SetDeviceConfig(config);
    if (status != RealSenseID::Status::Ok) {
        std::cerr << "Failed to set device config: " << status << std::endl;
        return false;
    }
    return true;
}

bool Application::initMosquitto()
{
    if (topicDoor_.empty() || topicControl_.empty()) {
        std::cerr << "Mosquitto topics are not configured" << std::endl;
        return false;
    }

    mosquitto_lib_init();
    mosq_ = mosquitto_new("smartdoorF455", true, nullptr);
    if (!mosq_) {
        std::cerr << "Failed to create mosquitto client" << std::endl;
        return false;
    }

    std::string host = config_.getString("mosquitto.host", "localhost");
    int port = config_.getInt("mosquitto.port", 1883);
    int keepalive = config_.getInt("mosquitto.keepalive", 60);

    if (mosquitto_connect(mosq_, host.c_str(), port, keepalive) != MOSQ_ERR_SUCCESS) {
        std::cerr << "Failed to connect to mosquitto broker" << std::endl;
        return false;
    }

    if (mosquitto_subscribe(mosq_, nullptr, topicDoor_.c_str(), 0) != MOSQ_ERR_SUCCESS) {
        std::cerr << "Failed to subscribe to mosquitto topic_door" << std::endl;
        return false;
    }
    if (mosquitto_subscribe(mosq_, nullptr, topicControl_.c_str(), 0) != MOSQ_ERR_SUCCESS) {
        std::cerr << "Failed to subscribe to mosquitto topic_control" << std::endl;
        return false;
    }

    return true;
}

bool Application::initTelegram()
{
    if (botToken_.empty() || chatId_ == 0) {
        std::cerr << "Telegram token or chat_id missing" << std::endl;
        return false;
    }

    bot_ = std::make_unique<TgBot::Bot>(botToken_);
    try {
        bot_->getApi().sendMessage(chatId_, "smartdoorF455 started ...");
    } catch (const TgBot::TgException& e) {
        std::cerr << "Error sending start message via Telegram: " << e.what() << std::endl;
        return false;
    }
    return true;
}

void Application::onPresenceDetected(const WPIWfiStatus& wfiStatus)
{
    const auto now = std::chrono::steady_clock::now();
    if (!firstPresence_) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastPresenceTime_);
        if (wfiStatus.statusOK != 1 || elapsed.count() < waitTimeUntilReauth_) {
            return;
        }
    }

    firstPresence_ = false;
    lastPresenceTime_ = now;
    if (!authenticator_) {
        std::cerr << "No authenticator available" << std::endl;
        return;
    }

    authenticator_->Authenticate(authCallback_);
    if (sendSnapshot_ && useTelegram_) {
        takeSnapshotAndSend();
    }
}

void Application::handleAuthenticationResult(const RealSenseID::AuthenticateStatus status, const char* userId)
{
    if (status == RealSenseID::AuthenticateStatus::Success) {
        setLastAuthenticatedName(userId ? userId : "");
        if (useMosquitto_) {
            publishDoorOpen();
        }
        if (useTelegram_) {
            sendTelegramText("Door opened for " + std::string(userId ? userId : "unknown"));
        }
    } else {
        if (useTelegram_) {
            sendTelegramText("RealSenseID::AuthenticateStatus: unauthorized person tried to access");
        }
    }
}

void Application::sendTelegramText(const std::string& message)
{
    if (!bot_ || chatId_ == 0) {
        return;
    }
    try {
        bot_->getApi().sendMessage(chatId_, message);
    } catch (const TgBot::TgException& e) {
        std::cerr << "Telegram send error: " << e.what() << std::endl;
    }
}

void Application::publishDoorOpen()
{
    if (!mosq_) {
        return;
    }
    if (mosquitto_reconnect(mosq_) != MOSQ_ERR_SUCCESS) {
        std::cerr << "Cannot reconnect to mosquitto" << std::endl;
        return;
    }
    if (mosquitto_publish(mosq_, nullptr, topicDoor_.c_str(), 5, "open", 0, false) != MOSQ_ERR_SUCCESS) {
        std::cerr << "Cannot publish door open message" << std::endl;
    }
}

void Application::takeSnapshotAndSend()
{
    cv::VideoCapture camera(0, cv::CAP_V4L2);
    if (!camera.isOpened()) {
        std::cerr << "ERROR: Could not open camera for snapshot" << std::endl;
        return;
    }

    const auto homeDir = getHomeDirectory();
    const auto snapshotDir = getSnapshotDirectory();
    if (!std::filesystem::exists(snapshotDir)) {
        std::filesystem::create_directories(snapshotDir);
    }

    const std::string snapshotFile = snapshotDir + "/snapshot_" + std::to_string(std::time(nullptr)) + ".jpg";
    cv::Mat frame;
    camera >> frame;
    if (frame.empty()) {
        std::cerr << "Snapshot frame empty" << std::endl;
        return;
    }
    cv::rotate(frame, frame, cv::ROTATE_90_COUNTERCLOCKWISE);
    cv::imwrite(snapshotFile, frame);

    if (bot_ && chatId_ != 0) {
        try {
            bot_->getApi().sendPhoto(chatId_, TgBot::InputFile::fromFile(snapshotFile, "image/jpeg"));
        } catch (const TgBot::TgException& e) {
            std::cerr << "Error sending Telegram photo: " << e.what() << std::endl;
        }
    }
}

std::string Application::getLastAuthenticatedName() const
{
    std::lock_guard<std::mutex> lock(lastAuthenticatedNameMutex_);
    return lastAuthenticatedName_;
}

void Application::setLastAuthenticatedName(const std::string& name)
{
    std::lock_guard<std::mutex> lock(lastAuthenticatedNameMutex_);
    lastAuthenticatedName_ = name;
}

std::string Application::getHomeDirectory() const
{
    const char* home = std::getenv("HOME");
    return home ? std::string(home) : std::string(".");
}

std::string Application::getSnapshotDirectory() const
{
    return getHomeDirectory() + "/smartdoorF455/snapshots";
}

void Application::signalHandler(int signum)
{
    if (instance_) {
        instance_->interruptRequested_.store(true, std::memory_order_release);
    }
    std::cout << "Interrupt signal (" << signum << ") received." << std::endl;
}

void presenceDetectedCallback(struct WPIWfiStatus wfiStatus, void* userdata)
{
    if (Application::instance_) {
        Application::instance_->onPresenceDetected(wfiStatus);
    }
}

AuthCallback::AuthCallback(Application& application)
    : application_(application)
{
}

void AuthCallback::OnResult(const RealSenseID::AuthenticateStatus status, const char* user_id)
{
    application_.handleAuthenticationResult(status, user_id);
}

void AuthCallback::OnHint(const RealSenseID::AuthenticateStatus hint)
{
    std::cout << "Authentication hint: " << static_cast<int>(hint) << std::endl;
}

void AuthCallback::OnFaceDetected(const std::vector<RealSenseID::FaceRect>& faces, const unsigned int ts)
{
    for (const auto& face : faces) {
        std::cout << "Detected face " << face.x << "," << face.y << " " << face.w << "x" << face.h << " (timestamp " << ts << ")" << std::endl;
    }
}

EnrollCallback::EnrollCallback(Application& application)
    : application_(application)
{
}

void EnrollCallback::OnResult(const RealSenseID::EnrollStatus status)
{
    std::cout << "Enrollment result: " << static_cast<int>(status) << std::endl;
}

void EnrollCallback::OnProgress(const RealSenseID::FacePose pose)
{
    std::cout << "Enroll progress pose: " << static_cast<int>(pose) << std::endl;
}

void EnrollCallback::OnHint(const RealSenseID::EnrollStatus hint)
{
    std::cout << "Enrollment hint: " << static_cast<int>(hint) << std::endl;
}
