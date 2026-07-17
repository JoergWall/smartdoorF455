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
    : authCallback_(*this)
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

    // Start the notification worker so Telegram/snapshot I/O runs off the interrupt thread.
    notificationThread_ = std::thread([this]() { notificationWorkerLoop(); });

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

    while (!interruptRequested_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::cout << "Interrupt received, shutting down." << std::endl;
    cleanup();
    return 0;
}

/**
 * @brief Release peripherals and stop background services during shutdown.
 */
void Application::cleanup()
{
    if (cleanedUp_) {
        return;
    }
    cleanedUp_ = true;

    // Stop the notification worker before tearing down bot_/other resources it uses.
    {
        std::lock_guard<std::mutex> lock(notificationMutex_);
        notificationStop_ = true;
    }
    notificationCv_.notify_all();
    if (notificationThread_.joinable()) {
        notificationThread_.join();
    }

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

    // Own an HTTP client with a bounded timeout so a stalled connection cannot hang the
    // notification worker indefinitely. It must outlive bot_, which keeps a reference to it.
    httpClient_ = std::make_unique<TgBot::BoostHttpOnlySslClient>();
    httpClient_->_timeout = TELEGRAM_TIMEOUT_SEC;

    bot_ = std::make_unique<TgBot::Bot>(botToken_, *httpClient_);
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
    // Prevent concurrent authentication attempts
    if (authenticationInProgress_.exchange(true, std::memory_order_acq_rel)) {
        return; // Authentication already in progress
    }

    const auto now = std::chrono::steady_clock::now();
    if (!firstPresence_) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastPresenceTime_);
        if (wfiStatus.statusOK != 1 || elapsed.count() < waitTimeUntilReauth_) {
            authenticationInProgress_.store(false, std::memory_order_release);
            return;
        }
    }

    firstPresence_ = false;
    lastPresenceTime_ = now;
    if (!authenticator_) {
        std::cerr << "No authenticator available" << std::endl;
        authenticationInProgress_.store(false, std::memory_order_release);
        return;
    }

    authenticator_->Authenticate(authCallback_);
    if (sendSnapshot_ && useTelegram_) {
        // Offload the snapshot + Telegram upload so it never delays reauthentication.
        enqueueNotification([this]() { takeSnapshotAndSend(); });
    }
    authenticationInProgress_.store(false, std::memory_order_release);
}

void Application::handleAuthenticationResult(const RealSenseID::AuthenticateStatus status, const char* userId)
{
    if (status == RealSenseID::AuthenticateStatus::Success) {
        setLastAuthenticatedName(userId ? userId : "");
        if (useMosquitto_) {
            publishDoorOpen();
        }
        if (useTelegram_) {
            const std::string message = "Door opened for " + std::string(userId ? userId : "unknown");
            enqueueNotification([this, message]() { sendTelegramText(message); });
        }
    } else {
        if (useTelegram_) {
            enqueueNotification([this]() {
                sendTelegramText("RealSenseID::AuthenticateStatus: unauthorized person tried to access");
            });
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

void Application::enqueueNotification(std::function<void()> task)
{
    {
        std::lock_guard<std::mutex> lock(notificationMutex_);
        if (notificationStop_) {
            return;
        }
        // Bound the backlog: if the network is stalling, drop the oldest pending task
        // rather than let work pile up unbounded.
        if (notificationQueue_.size() >= MAX_PENDING_NOTIFICATIONS) {
            notificationQueue_.pop_front();
            std::cerr << "Notification queue full; dropping oldest pending task" << std::endl;
        }
        notificationQueue_.push_back(std::move(task));
    }
    notificationCv_.notify_one();
}

void Application::notificationWorkerLoop()
{
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(notificationMutex_);
            notificationCv_.wait(lock, [this]() {
                return notificationStop_ || !notificationQueue_.empty();
            });
            if (notificationStop_ && notificationQueue_.empty()) {
                return;
            }
            task = std::move(notificationQueue_.front());
            notificationQueue_.pop_front();
        }
        // Executed off the interrupt thread; blocking network/camera work is safe here.
        task();
    }
}

std::string Application::getLastAuthenticatedName() const
{
    std::lock_guard<std::mutex> lock(lastAuthenticatedNameMutex_);
    
    // Check if timeout has expired (5 seconds)
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - nameSetTime_);
    
    if (!lastAuthenticatedName_.empty() && elapsed.count() >= NAME_DISPLAY_TIMEOUT_SEC) {
        // Clear the name after timeout
        lastAuthenticatedName_.clear();
        return "";
    }
    
    return lastAuthenticatedName_;
}

void Application::setLastAuthenticatedName(const std::string& name)
{
    std::lock_guard<std::mutex> lock(lastAuthenticatedNameMutex_);
    lastAuthenticatedName_ = name;
    nameSetTime_ = std::chrono::steady_clock::now(); // Record when the name was set
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
    // Async-signal-safe: only touch the atomic flag; no I/O or allocation here.
    (void)signum;
    if (instance_) {
        instance_->interruptRequested_.store(true, std::memory_order_release);
    }
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
