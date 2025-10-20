/**
 * @file smartdoorF455.cpp
 * @brief Main program for smartdoorF455 facial authentication door control system
 * @copyright MIT License. See LICENSE file in root directory.
 * @copyright Copyright (C) 2025 Joerg Wallmersperger
 *
 * @details
 * This program uses facial authentication to control a door buzzer/opener system.
 * Tested on Raspberry PI 4b with Raspberry PI OS and Ubuntu 20.4.
 *
 * Hardware Requirements:
 * ======================
 * - Raspberry PI 4 with >= 4GB RAM
 * - Intel RealSense ID F455 camera 
 * - P2.5 64x32 RGB LED Matrix Panel (160x80 mm) 
 *   optional: HUB75 connector + Adafruit RGB Matrix Bonnet hat
 *   https://learn.adafruit.com/adafruit-rgb-matrix-bonnet-for-raspberry-pi
 * - MEANWELL IRM-60-5ST 5V 10A power supply
 *   https://www.meanwell.com/Upload/PDF/IRM-60/IRM-60-spec.pdf
 * - Lindby Severina LED outdoor wall lamp to be used as casing
 * - IP Interface to door buzzer/opener (e.g. Siedle Gateway with MQTT interface)
 * - Optional: GeeekPi Raspberry Pi 4 CNC Armor Case with passive cooling
 * - Presence sensor - choose from 2 Options 
 * 1. PIR Sensor (HC-SR501):
 *    - Pin 1 (5V) -> RPI4b Pin 4 (5V)
 *    - Pin 2 (Signal) -> RPI4b Pin 29 (GPIO 5/19)
 *    - Pin 3 (GND) -> RPI4b Pin 6 (Ground)
 * -> use PIR sensor, if there is no reflective, opposite wall to the system
 * 2. IR photoelectric barrier (E18-D80NK):
 *    - Brown (5V) -> RPI4b Pin 4 (5V) 
 *    - Black (Signal) -> RPI4b Pin 29 (GPIO 5/19)
 *    - Blue (GND) -> RPI4b Pin 6 (Ground)
 * -> use IR photoelectric barrier, if there is a reflective, opposite wall to the system
 * 
 *
 * Software components used:
 * =========================
 * - Operating System: Raspberry Pi OS 
 *   https://www.raspberrypi.com/software/operating-systems/
 * - Intel RealSense ID SDK (Apache License 2.0)
 *   https://github.com/IntelRealSense/RealSenseID.git
 * - rpi-rgb-led-matrix library (GNU GPL v2.0)
 *   https://github.com/hzeller/rpi-rgb-led-matrix.git
 * - Paho MQTT C/C++ Client library (Eclipse Public License 2.0)
 *   https://github.com/eclipse-paho/paho.mqtt.cpp.git
 * - Mosquitto MQTT Broker (Eclipse Public License 2.0)
 *   https://mosquitto.org/
 * - WiringPi GPIO library (GNU GPL v3.0)
 *   https://github.com/WiringPi/WiringPi.git
 * - tomlplusplus library (MIT License)
 *   https://marzer.github.io/tomlplusplus/
 * - OpenCV library (Apache License 2.0)
 *   https://opencv.org/
 * 
 * smartdoorF455 creates 3 threads:
 * ================================
 * - wiringPiISR2 registers a callback on a GPIO pin interrupt, when presence sensor triggers
 *   consumes < 4% CPU time on RPI4b
 * - matrix_task.start() - creates a low CPU consuming thread with function matrixLEDTask::task_function
 *   to control the LED matrix panel
 * - inside matrixLEDTask::task_function a further thread is created to refresh the
 *   LED Matrix display  (from rpi-rgb-led-matrix library)
 *   the line "matrix = RGBMatrix::CreateFromOptions" - creates this CPU heavy thread 
 *   which may take up to 50% of a single Raspberry Pi 4 CPU core. CPU usage of this 
 *   thread is dependand upon the Matrix configuration e.g. whether direct GPIO cabling or a Adafruit matrix
 *   bonnet is used to connect the LED matrix to the Raspberry Pi.
 *   
 * With the following linux shell command you may observe the processes 
 * associated to the above described threads:
 * % top -H -p $(pgrep -x smartdoorF455)
 *   
 * 
 * smartdoorF455 is free software: you can redistribute it and/or modify
 *
 * @note See installation instructions in README file
 */
#include "smartdoorF455.hpp"
#define STDOUT_ADDTL_INFO  /* provides additional information on stdout e.g. prints date/time when movement sensor triggers camera */
#define DISPLAY_NAME_IN_ITERATIONS 5  // how long name of authenticated person is displayed, when door opens in main loop iterations
#define DATE_FORMAT_STRING "%d.%m" // DD.MM.YY format
#define DAY_FORMAT_STRING "%A" // name of day according to LOCALE 
// #define TIME_FORMAT_STRING "%H:%M"    // HH:MM format
#define FONT_PATH "./external/rpi-rgb-led-matrix-src/fonts/" 
#define FONT_TIME FONT_PATH "6x12.bdf"
#define FONT_DAY  FONT_PATH "4x6.bdf"
#define FONT_DATE FONT_PATH "6x12.bdf"
#define FONT_NAME FONT_PATH "6x12.bdf"
#define MAX_NAME_LENGTH 5 /* maximum name length displayed of authenticated person */
#define LINE_OFFSET_1 7
#define LINE_OFFSET_2 (LINE_OFFSET_1+6)
#define LINE_OFFSET_3 (LINE_OFFSET_2+8)
#define LINE_OFFSET_4 (LINE_OFFSET_3+8)
#define DELAY_MSEC  1000 /* delay in milliseconds; adjust frequency to match potential scrolling or animation patterns */
#define DEBOUNCE_PERIOD 1000 // in us; 1.000 equals = 1 ms; debounce filter for presence sensor
/* global variables ...
   are ugly, however the following are used both in main and callback functions
   any hint how to eliminate this global variable greatly appreciated */
std::unique_ptr<RealSenseID::FaceAuthenticator> authenticator; // object instace used to communicate with F455 camera for authentication 
RealSenseID::SerialConfig serial_config; // serial configuration for F455 camera
std::string serial_conf_string;
toml::table config_toml; // toml config file
// see https://www.reddit.com/r/learnprogramming/comments/18w4ifo/c_how_to_share_a_string_across_threads/
std::atomic<std::string *> name_lastauthenticated = new std::string(); // string is shared among threads, therefore trivially copyable atomic variable required
bool use_mosquitto = false; // is MQTT protocol used to communicate with outer world e.g. to activate door buzzer?
struct mosquitto *mosq; // used both in main an authentication callback functions
const char *topic_door;
volatile bool interrupt_received = false;
bool use_telegram;  // check if telegram bot is used
bool send_snapshot;  // check if telegram bot shall be used to send photo
const char *bot_token; // every bot has its unique token
long chat_id; 
TgBot::Bot* bot;  //  telegram bot object
std::string usb_device; // USB device for Intel RealSenseID F455 camera
DeviceInfo device_info; // type of Intel RealSense camera 

/**
 * @brief Returns the current date and time as a formatted string
 * 
 * This function retrieves the current system time and converts it to a formatted string.
 * The format used is "YYYY-MM-DD HH:MM:SS" in local time.
 * 
 * @return std::string Formatted string containing current date and time
 * 
 * @note Uses the system's local time settings
 */
std::string return_current_time_and_date()
{
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);

    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %X");
    return ss.str();
}

/**
 * @class MyAuthClbk
 * @brief Callback class for authentication results.
 * 
 * This class is used to receive authentication results from the Intel RealSenseID F455 camera.
 * It implements the RealSenseID::AuthenticationCallback interface and is used to handle authentication results.
 */
class MyAuthClbk : public RealSenseID::AuthenticationCallback
{
    private:
    //
    public:
    /**
     * @memberof MyAuthClbk
     * @brief Called when authentication results are available.
     * 
     * This function is called when the Intel RealSenseID F455 camera has
     * completed the authentication process. It provides the status of the
     * authentication attempt and the user ID of the authenticated user.
     * 
     * @param status The status of the authentication attempt.
     * @param user_id The user ID of the authenticated user.
     */
    void OnResult(const RealSenseID::AuthenticateStatus status, const char* user_id) override
    {
        if (status == RealSenseID::AuthenticateStatus::Success){
            name_lastauthenticated.load()->assign(user_id);

            // old: strncpy(name_lastauthenticated,user_id,MAX_NAME_LENGTH); // copy first MAX_NAME_LENGTH chars of authenticated users
#ifdef STDOUT_ADDTL_INFO
            cout <<  return_current_time_and_date() << " Hallo " << user_id << std::endl;
            cout << "MyAuthClbk::OnResult send_snapshot=" << send_snapshot << ", use_telegram=" << use_telegram << ", chat_id=" << chat_id << std::endl;
#endif /* STDOUT_ADDTL_INFO */
            // TRIGGER DOOR OPENER START - ADAPT THIS CODE according to your interface to 
            //                             your door buzzer

            if(use_mosquitto){   
                // send MQTT message to Siedle gateway to open door            
                if (mosquitto_reconnect(mosq) != MOSQ_ERR_SUCCESS)
                    std::cout << "cannot reconnect to mosquitto "  << std::endl;
                if (mosquitto_publish(mosq, NULL, topic_door, 5, "open", 0, false) != MOSQ_ERR_SUCCESS)
                    std::cout << "cannot publish to mosquitto "  << std::endl;

                // TRIGGER DOOR OPENER END
            } // end if use_mosquitto
            if (use_telegram) {
#ifdef STDOUT_ADDTL_INFO
                cout <<  return_current_time_and_date() << " trying telegram bot " << bot_token << " to send message to chat_id " << chat_id << endl;
#endif /* STDOUT_ADDTL_INFO */
                try {

                    if (chat_id != 0) {
                        bot->getApi().sendMessage(chat_id, std::string("Door opened for ") + user_id);
                    }
                }
                catch (TgBot::TgException& e) {
                    printf("error sending telegram message: %s\n", e.what());
                }

            } // end if (use_telegram)
        } // end if authentication successful
        else // authentication failed
        {
            std::cout << return_current_time_and_date() << " RealSenseID::AuthenticateStatus: " << status << std::endl;
            try {

                if (chat_id != 0) {
                    bot->getApi().sendMessage(chat_id, std::string("RealSenseID::AuthenticateStatus: unauthorized person tried to access"));
                }
            }
            catch (TgBot::TgException& e) {
                printf("error sending telegram message: %s\n", e.what());
            }
        }

    } // end of MyAuthClbk::OnResult()
    /**
     * @memberof MyAuthClbk
     * @brief Called to provide hints during the authentication process.
     * 
     * This function is called when the Intel RealSenseID F455 camera has
     * suggestions for improving the authentication process. It provides
     * feedback to the user based on the current authentication status.
     * 
     * @param hint The current authentication hint or suggestion.
     */
    void OnHint(const RealSenseID::AuthenticateStatus hint) override
    {
        std::cout << "Authentication hint: " << hint << std::endl;
        std::cout << "OnHint: send_snapshot=" << send_snapshot << ", use_telegram=" << use_telegram << ", chat_id=" << chat_id << std::endl;   
    }
    /**
     * @memberof MyAuthClbk
     * @brief Called when faces are detected during authentication.
     * 
     * This function is called when the Intel RealSenseID F455 camera detects
     * faces in the camera's field of view during the authentication process.
     * It provides information about the detected faces, including their
     * bounding rectangles and timestamps.
     * 
     * @param faces A vector of FaceRect structures representing the detected faces.
     * @param ts The timestamp associated with the face detection event.
     */
    void OnFaceDetected(const std::vector<RealSenseID::FaceRect>& faces, const unsigned int ts) override
    {
        for (auto& face : faces)
        {
            printf("** Detected face %u,%u %ux%u (timestamp %u)\n", face.x, face.y, face.w, face.h, ts);
        
        }
    }
}; // end class MyAuthClbk

/**
 * @brief Callback class for handling facial enrollment events.
 *
 * This class implements the RealSenseID::EnrollmentCallback interface to handle
 * various events and status updates during the facial enrollment process using
 * the Intel RealSense ID F455 camera.
 */
class MyEnrollClbk : public RealSenseID::EnrollmentCallback
{
public:
    /**
     * @memberof MyEnrollClbk
     * @brief Called when enrollment process completes or fails.
     * 
     * @param status The final status of the enrollment process.
     *              Contains the result of the enrollment attempt.
     */
    void OnResult(const RealSenseID::EnrollStatus status) override
    {
        std::cout << "on_result: enroll_status: " << status << std::endl;
    }
    /**
     * @memberof MyEnrollClbk
     * @brief Called during enrollment to indicate progress.
     * 
     * @param pose The current face pose detected during enrollment.
     *            Used to guide the user through different required poses.
     */
    void OnProgress(const RealSenseID::FacePose pose) override
    {
        std::cout << "on_progress: enroll_pose: " << pose << std::endl;
    }
   /**
    * @memberof MyEnrollClbk
     * @brief Called to provide hints during the enrollment process.
     * 
     * @param hint The current enrollment hint or suggestion.
     *            Provides feedback to help improve the enrollment process.
     */
    void OnHint(const RealSenseID::EnrollStatus hint) override
    {
        std::cout << "on_hint: enroll_hint: " << hint << std::endl;
    }
};


/**
 * @brief Creates and connects a FaceAuthenticator object for the Intel RealSense ID camera.
 *
 *
 * @return std::unique_ptr<RealSenseID::FaceAuthenticator> A unique pointer to the configured FaceAuthenticator
 * @throws Calls std::exit(1) if Connection to the device fails
 *
 * @note Under RSID_SECURE compilation, uses secure authentication with s_signer
 */
std::unique_ptr<RealSenseID::FaceAuthenticator> createAuthenticator(RealSenseID::SerialConfig serial_config)
{
    std::unique_ptr<RealSenseID::FaceAuthenticator> authenticator;

#ifdef RSID_SECURE
    authenticator = std::make_unique<RealSenseID::FaceAuthenticator>(&s_signer, device_info.deviceType);
#else
    authenticator = std::make_unique<RealSenseID::FaceAuthenticator>(device_info.deviceType);
#endif // RSID_SECURE
    auto connect_status = authenticator->Connect(serial_config);
    if (connect_status != RealSenseID::Status::Ok)
    {
        std::cout << "Failed connecting to port " << serial_config.port << " status:" << connect_status << std::endl;
        std::cout << "serial port: " << serial_config.port << std::endl;
        std::exit(1);
    }
    std::cout << "Connected to device" << std::endl;
    return authenticator;  
} // end createAuthenticator()

/**
 * @brief Initializes and configures the Intel RealSense F455 camera
 * 
 * This function performs the following operations:
 * - Discovers connected RealSenseID devices
 * - Stores device and serial port information for the first valid device found
 * - Configures camera parameters from config.toml file including:
 *   - Camera rotation
 *   - Security level
 *   - Frontal face policy
 *   - Matcher confidence level
 *   - Algorithm flow
 *   - Dump mode
 *   - Max spoofs
 *   - GPIO authentication toggling
 * - Creates an authenticator and applies the configuration
 * 
 * @return true if camera is successfully initialized and configured
 * @return false if no devices found or configuration fails
 */
bool init_F455_camera(){
    auto devices = RealSenseID::DiscoverDevices();
    if (!devices.empty())
    {
        for (const auto& device : devices)
        {
            std::cout << "  [*] Found rsid device " << device.deviceType << ". port: " << device.serialPort << std::endl;
            if (device.deviceType == RealSenseID::DeviceType::Unknown)
            {
                std::cout << "Unkown device type for port " << device.serialPort << std::endl;
                std::exit(1);
            }
            else {

                device_info.deviceType = device.deviceType; // Store device information
                serial_conf_string = std::string(device.serialPort);
                std::cout << "serial port string: " << serial_conf_string << std::endl;
                serial_config.port = serial_conf_string.c_str();
                break; // found 1st RealSenseID device - break for loop
            }
        } // for
        DeviceConfig F455_config; // set Intel RealSense F455 camera parameters from config.toml
        F455_config.camera_rotation = camera_rotation.at(config_toml["camera"]["camera_rotation"].value<std::string>().value().c_str()); // map string to enum value
        F455_config.security_level = security_level.at(config_toml["camera"]["security_level"].value<std::string>().value().c_str()); 
        F455_config.frontal_face_policy = frontal_face_policy.at(config_toml["camera"]["frontal_face_policy"].value<std::string>().value().c_str());  //  run authentication on closest face
        F455_config.matcher_confidence_level = matcher_confidence_level.at(config_toml["camera"]["matcher_confidence_level"].value<std::string>().value().c_str());  
        F455_config.algo_flow = algo_flow.at(config_toml["camera"]["algo_flow"].value<std::string>().value().c_str());
        F455_config.dump_mode = dump_mode.at(config_toml["camera"]["dump_mode"].value<std::string>().value().c_str());
        int max_spoofs_int = config_toml["camera"]["max_spoofs"].value_or(0); 
        F455_config.max_spoofs = (unsigned char) max_spoofs_int;   // max_spoofs currently defined as unsigned char in RealSenseID/DeviceConfig.h
        F455_config.gpio_auth_toggling = config_toml["camera"]["gpio_auth_toggling"].value_or(0); 
        std::cout << "F455_config values "  << std::endl;
        std::cout << "camera_rotation: " << F455_config.camera_rotation << std::endl;
        std::cout << "security_level: " << F455_config.security_level << std::endl;
        std::cout << "frontal_face_policy: " << F455_config.frontal_face_policy << std::endl;
        std::cout << "matcher_confidence_level: " << F455_config.matcher_confidence_level << std::endl;
        std::cout << "algo_flow: " << F455_config.algo_flow << std::endl;
        std::cout << "dump_mode: " << F455_config.dump_mode << std::endl;
        std::cout << "max_spoofs_int: " << max_spoofs_int << std::endl;
        std::cout << "max_spoofs: " << (int) F455_config.max_spoofs << std::endl;
        std::cout << "gpio_auth_toggling: " << F455_config.gpio_auth_toggling << std::endl;
        std::cout << "serial port: " << serial_config.port << std::endl;
        authenticator = createAuthenticator(serial_config);
        auto status = authenticator->SetDeviceConfig(F455_config);
        if (status != RealSenseID::Status::Ok) {
            std::cerr << "Failed to set device config: " << status << std::endl;
            return(false);
        }
        else return(true);
    } // no devices found
    return(false);
}

/**
 * @brief Callback function for presence detection.
 *
 * This function is called when the presence sensor (PIR or photoelectric barrier)
 * detects a change in the environment. It is called everytime when gpio_sensor_pin level has changed
 * no matter, whether it be high-to-low or low-to-high. It triggers facial authentication using
 * the Intel RealSense ID F455 camera if enough time has passed since the last
 * authentication.
 * 
 * Thoughts on detecting user presence:
 * - Utilize camera-based computer vision techniques to detect user presence to eliminate the need 
 *   for an (PIR or photoelectric barrier) extra sensor.
 *   However, computer vision techniques won't work at night time if no lighting is provided.
 * - Consider user data privacy and security when capturing and processing images.
 *
 */
void presence_detected_clbk(struct WPIWfiStatus wfiStatus, void* userdata)
{    
    static MyAuthClbk auth_clbk; // callback object for authentication results
    static bool initial_run = true;
    static uint32_t wait_time_until_reauthentication = config_toml["raspi"]["wait_time_until_reauthentication"].value_or(3); // in seconds
    using Clock = std::chrono::steady_clock;
    // A static variable retains its value between function calls.
    // It's initialized to the current time on the very first call.
    static Clock::time_point last_run = Clock::now();
    // Get the current time.
    Clock::time_point now = Clock::now();
    // We convert the duration to seconds as a floating-point number.
    std::chrono::duration<double> elapsed_seconds = now - last_run;
    bool min_time_elapsed = (elapsed_seconds.count() >= wait_time_until_reauthentication);
    std::cout << return_current_time_and_date()  << " presence sensor triggered presence_detected_clbk " << std::endl;
    std::cout << return_current_time_and_date()  << " wfiStatus.statusOK (should be 1): " << wfiStatus.statusOK << std::endl;
    // validate, whether this function should actually run, return if
    // - min_time_elapsed is false (time since last authentication is less than wait_time_until_reauthentication) or
    // - wfiStatus.statusOK is not OK (!= 1) and
    // - initial run is false
    if (((wfiStatus.statusOK != 1) || !min_time_elapsed) && !initial_run){
        return;
    }
    initial_run = false;
    last_run = Clock::now();
    std::cout << "presence detected - serial port: " << serial_config.port << std::endl;
    authenticator->Authenticate(auth_clbk); // trigger camera authentication process
    std::cout << "authenticator called " << std::endl;
#ifdef STDOUT_ADDTL_INFO /* when presence is detected triggered facial authentication  */
    std::cout << return_current_time_and_date()  << " authentication triggered" << std::endl;
#endif /* STDOUT_ADDTL_INFO */
    // std::this_thread::sleep_for(std::chrono::milliseconds {400});
    if (send_snapshot && use_telegram) { // start preview to save snapshot and send to telegram bot 
        std::string snapshot_file;
        // std::this_thread::sleep_for(std::chrono::milliseconds {400});
        cv::VideoCapture camera(0, cv::CAP_V4L2); // open Intel RealSenseID camera as a webcam and store snapshot as jpg file
        if (!camera.isOpened()) {
            std::cerr << "ERROR: Could not open camera" << std::endl;
        } else { // camera opened for snapshot
            std::string home_dir = getenv("HOME");
            std::string snapshot_dir = home_dir + "/smartdoorF455/snapshots/";
            if (!std::filesystem::exists(snapshot_dir)) {
                std::filesystem::create_directory(snapshot_dir); // create directory if it doesn't exist
            }
            snapshot_file = snapshot_dir +"snapshot_" + std::to_string(std::time(nullptr)) + ".jpg"; // filename with timestamp
            cv::Mat frame;
            camera >> frame;
            cv::rotate(frame, frame, cv::ROTATE_90_COUNTERCLOCKWISE);
            if (!frame.empty()) {
                cv::imwrite(snapshot_file, frame);
            }
            camera.release(); // Closes video file or capturing device
            try {
                if (chat_id != 0) {
                    // take snapshot and send to telegram bot
                    string photoMimeType = "image/jpeg";
                    bot->getApi().sendPhoto(chat_id, TgBot::InputFile::fromFile(snapshot_file, photoMimeType));
                    /* std::ifstream file(snapshot_file, std::ios::binary);
                    if (file) {
                        TgBot::InputFile::Ptr inputFile(new TgBot::InputFile);
                    inputFile->setFile(file, snapshot_file);
                    bot->getApi().sendPhoto(chat_id, inputFile);
                    } */
                } // if (chat_id != 0)
            } // try
            catch (TgBot::TgException& e) {
                std::cout << "error sending telegram photo: " << e.what() << std::endl;
            }
        } // if (!camera.isOpened())
    } // end if (send_snapshot)  
} // end presence_detected_clbk

/**
 * @brief Signal handler for handling interrupt signals.
 *
 * This function is called when the program receives an interrupt signal,
 * such as SIGINT (Ctrl+C) or SIGTERM. It sets the interrupt_received flag
 * to true, which causes the main loop to terminate gracefully.
 *
 * @param signum The signal number that was received.
 */
void signalHandler( int signum ) {
    interrupt_received = true; /* CTRL+C pressed - let's terminate this program */ 
    std::cout << "\nInterrupt signal (" << signum << ") received.\n";
}
/**
 * @class matrixLEDTask
 * @brief Class to manage an RGB LED matrix display by calling 
 * @brief task_function every update interval as a separate thread
 *
 * This class handles the initialization and operation of an RGB LED matrix display.
 * It runs a worker thread that periodically updates the display with time, date,
 * and user authentication information. The display uses double buffering for smooth updates.
 *
 * Features:
 * - Configurable LED matrix parameters (rows, columns, brightness, etc.)
 * - Real-time display of clock, date and day of week
 * - Display of authenticated user names
 * - Double buffering for smooth display updates
 * - Configurable colors for different display elements
 *
 * @note Requires rpi-rgb-led-matrix library
 * @note Configuration is read from a TOML config file
 *
 * Example usage:
 * @code
 * matrixLEDTask matrix(100); // Create with 100ms update interval
 * matrixLEDTask.start(); // Start the display thread
 * // ... program main loop ...
 * matrixLEDTask.stop(); // Stop the display thread
 * @endcode
 */
class matrixLEDTask {
private:
    std::atomic<bool> running{false};
    std::thread worker_thread;
    // task_function will invoked in separate thread
    void task_function() {
        int counter = 0;
        static bool first_run = true;
        // Initialization of RGB-Matrix-Display
        FrameCanvas *offscreen;
        RGBMatrix::Options matrix_options; 
        rgb_matrix::RuntimeOptions runtime_opt;
        Color clock_color, date_color, day_color, username_color, bg_color, outline_color;
        rgb_matrix::Font font_time, font_date, font_day, font_name;
        
        // struct tm tm; 
        // struct timespec cur_time;
        RGBMatrix *matrix;
        clock_color = Color((uint8_t)config_toml["matrix_options"]["clock_color"].as_array()->at(0).value_or(255), 
                            (uint8_t)config_toml["matrix_options"]["clock_color"].as_array()->at(1).value_or(28), 
                            (uint8_t)config_toml["matrix_options"]["clock_color"].as_array()->at(2).value_or(0));

        date_color = Color((uint8_t)config_toml["matrix_options"]["date_color"].as_array()->at(0).value_or(255), 
                                (uint8_t)config_toml["matrix_options"]["date_color"].as_array()->at(1).value_or(28), 
                                (uint8_t)config_toml["matrix_options"]["date_color"].as_array()->at(2).value_or(0));
        day_color = Color((uint8_t)config_toml["matrix_options"]["day_color"].as_array()->at(0).value_or(255), 
                                (uint8_t)config_toml["matrix_options"]["day_color"].as_array()->at(1).value_or(28), 
                                (uint8_t)config_toml["matrix_options"]["day_color"].as_array()->at(2).value_or(0));
        username_color = Color((uint8_t)config_toml["matrix_options"]["username_color"].as_array()->at(0).value_or(255), 
                                (uint8_t)config_toml["matrix_options"]["username_color"].as_array()->at(1).value_or(0),      
                                (uint8_t)config_toml["matrix_options"]["username_color"].as_array()->at(2).value_or(255));
        bg_color = Color((uint8_t)config_toml["matrix_options"]["bg_color"].as_array()->at(0).value_or(0), 
                                (uint8_t)config_toml["matrix_options"]["bg_color"].as_array()->at(1).value_or(0), 
                                (uint8_t)config_toml["matrix_options"]["bg_color"].as_array()->at(2).value_or(0));
        outline_color = Color((uint8_t)config_toml["matrix_options"]["outline_color"].as_array()->at(0).value_or(0), 
                                (uint8_t)config_toml["matrix_options"]["outline_color"].as_array()->at(1).value_or(0),            
                                (uint8_t)config_toml["matrix_options"]["outline_color"].as_array()->at(2).value_or(0));
        if (!font_date.LoadFont(FONT_DATE) || !font_time.LoadFont(FONT_TIME) // Load fonts. This needs to be a filename with a bdf bitmap font.
            || !font_day.LoadFont(FONT_DAY) || !font_name.LoadFont(FONT_NAME)) {
            std::cerr << "Couldn't load fontfiles \n";
            std::exit(1);
        }
        matrix_options.led_rgb_sequence = "RBG"; // set options for LED matrix
        matrix_options.rows = 32;
        matrix_options.cols = 64;
        matrix_options.brightness = config_toml["matrix_options"]["brightness"].value_or(80);
        matrix_options.pixel_mapper_config = config_toml["matrix_options"]["pixel_mapper_config"].value<std::string>().value().c_str(); // e.g. "Rotate:90"
        matrix_options.disable_hardware_pulsing = true;
        matrix_options.hardware_mapping = config_toml["matrix_options"]["hardware_mapping"].value<std::string>().value().c_str(); // e.g. "adafruit-hat"
        std::cout << "Using hardware mapping: " << matrix_options.hardware_mapping << " - empty string is direct cable connection" << std::endl;
        matrix = RGBMatrix::CreateFromOptions(matrix_options, runtime_opt);
        if (matrix == NULL){
            std::cerr << "Failed to create RGBMatrix" << std::endl;
            std::exit(1);
        }
        offscreen = matrix->CreateFrameCanvas(); // offscreen canvas for double buffering
        // end Initialization of RGB-Matrix-Display
        while (running) {
            auto now = std::chrono::system_clock::now();
            auto in_time_t = std::chrono::system_clock::to_time_t(now);
            std::stringstream ss;
            // thread being executed every DELAY_MSEC interval
            counter++;
            offscreen->Fill(bg_color.r, bg_color.g, bg_color.b); // blank screen
            ss << std::put_time(std::localtime(&in_time_t), "%H:%M");
            rgb_matrix::DrawText(offscreen, font_time, 0, LINE_OFFSET_1, clock_color, NULL, ss.str().c_str(), 0);
            ss.str("");
            ss << std::put_time(std::localtime(&in_time_t), "%A");
            rgb_matrix::DrawText(offscreen, font_day, 0, LINE_OFFSET_2, day_color,  NULL, ss.str().c_str(), 0);
            ss.str("");
            ss << std::put_time(std::localtime(&in_time_t), "%d.%m"); // %d.%m 
            rgb_matrix::DrawText(offscreen, font_date, 0, LINE_OFFSET_3, date_color,  NULL, ss.str().c_str(), 0);
            if (!name_lastauthenticated.load()->empty()){
                static unsigned int iterations = 0;
                rgb_matrix::DrawText(offscreen, font_name, 0, LINE_OFFSET_4, username_color, NULL, name_lastauthenticated.load()->c_str(), 0);
                if (iterations++ > DISPLAY_NAME_IN_ITERATIONS){
                    iterations = 0;
                    name_lastauthenticated.load()->clear(); // reset last authenticated name
                }
            }

            /* OPEN topic: display authentication hint
            else if (authentication_hint[0] != '\0'){ 
            }*/
            /* be creative and add more information here - scroll stock prices or display weather forecast */
            offscreen = matrix->SwapOnVSync(offscreen); // swap LEDMatrix double buffer
            if (first_run){
                pid_t tid;
                first_run = false;
                tid = syscall(SYS_gettid);
                cout << "process id: " << getpid() << ", task_function process id: " << tid << endl;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(DELAY_MSEC));
        } // while running
        matrix->Clear(); 
        delete matrix;
        std::cout << "matrixLEDTask::task_function ended" << std::endl; 
    } // task_function
    
public:
    // constructor with default  100ms interval
    matrixLEDTask(int interval = 100) : running(false) { } // constructor matrixLEDTask
    void start() {
        if (!running) {
            running = true;
            worker_thread = std::thread(&matrixLEDTask::task_function, this);
            std::cout << "periodic thread task_function started (" << DELAY_MSEC << "ms interval)" << std::endl;
        }
    }
    
    void stop() {
        if (running) {
            running = false;
            if (worker_thread.joinable()) {
                worker_thread.join(); // wait for the thread to finish
            }
            std::cout << "periodic thread task_function stopped" << std::endl;
        }

    }
    ~matrixLEDTask() {
        stop();
    }
};
/**
 * @brief Main function for the application.
 *
 * This is the entry point of the application. It initializes the necessary
 * components, sets up the signal handlers, and starts the main loop.
 *
 * @param argc The number of command-line arguments.
 * @param argv The array of command-line arguments.
 * @return int The exit status of the application.
 */
int main(int argc, char** argv){ 

    struct tm tm; 
    string bot_token_string;
    try // read and parse toml config file
    {
        config_toml = toml::parse_file(CONFIG_FILE);
        // std::cout << config_toml << "\n"; 
    }
    catch (const toml::parse_error& err)
    {
        std::cerr << "Parsing failed:\n" << err << "\n";
        return 1;
    }
// init variables with values from toml config file

    use_telegram = config_toml["telegram"]["use_telegram"].as_boolean(); // check if telegram bot is used
    if (use_telegram){
        send_snapshot = config_toml["telegram"]["send_snapshot"].as_boolean(); // check if telegram bot shall be used to send photo
        bot_token_string = config_toml["telegram"]["bot_token"].value<std::string>().value();
        bot_token = bot_token_string.c_str();
        chat_id = config_toml["telegram"]["chat_id"].value<long>().value();
        bot = new TgBot::Bot(bot_token);  // create telegram bot object
        try {
            if (chat_id != 0) {
                bot->getApi().sendMessage(chat_id, std::string("smartdoorF455 started ..."));
            }
        } // try send initial telegram message
        catch (TgBot::TgException& e) {
            std::cerr << "error sending telegram message: " << e.what() << std::endl;
        }
    } // use_telegram
// end init global vars
    int setupStatus = wiringPiSetupPinType(WPI_PIN_BCM);; // initialize WiringPi for GPIO usage, see 
                                          // https://github.com/WiringPi/WiringPi/blob/master/documentation/deutsch/functions.md
    if (setupStatus == -1) {
        // Handle the error: GPIO initialization failed
        // You might want to print an error message or exit the program
        // maybe other instance of this program
        cerr << "WiringPi failed to initialize GPIO" << endl;
        cerr << "Please check, if another instance of this program runs concurrently" << endl;
        cerr << "and if a different program is using the GPIO pins!" << endl;
        cerr << "Exiting program..." << endl;
        return 1; // Indicate failure
    } 

    int64_t gpio_sensor_pin = config_toml["raspi"]["gpio_sensor_pin"].value_or(0); 
    int64_t gpio_sensor_pull = config_toml["raspi"]["gpio_sensor_pull"].value_or(0);
    cout << "gpio sensor on pin " << gpio_sensor_pin << endl;
    pinMode(gpio_sensor_pin, INPUT);
    pullUpDnControl(gpio_sensor_pin, gpio_sensor_pull); // pull up/down mode (PUD_OFF, PUD_UP, PUD_DOWN)
    wiringPiISR2(gpio_sensor_pin, INT_EDGE_BOTH,  &presence_detected_clbk, DEBOUNCE_PERIOD, NULL); // presence_detected_clbk will be called everytime when gpio_sensor_pin level changed
                                                               // from  either high-to-low or low-to-high;  
    if (!init_F455_camera()) { // find and initialize Intel RealSenseID camera
        std::cerr << "Failed to initialize F455 camera" << std::endl;
        return 1;
    }
    std::cout << "main() serial port: " << serial_config.port << std::endl;
    // check if mosquitto is used
    use_mosquitto = config_toml["mosquitto"]["use_mosquitto"].as_boolean(); // check if mosquitto is used
    if (use_mosquitto) {

        mosquitto_lib_init(); // initialize MQTT mosquitto client
        mosq = mosquitto_new("Raspberry MQTT client", true, NULL);
        if (!mosq) {
            std::cerr << "Failed to create mosquitto client" << std::endl;
            return 1;
        }
        const char *host = config_toml["mosquitto"]["host"].value<std::string>().value().c_str();
        std::cout << "Connecting to mosquitto broker at " << host << std::endl;
        int port = (int) config_toml["mosquitto"]["port"].value_or(1883); // default port is 1883
        int keepalive = (int) config_toml["mosquitto"]["keepalive"].value_or(60); // default keepalive is 60 seconds
        // connect to mosquitto broker
        if (mosquitto_connect(mosq, host, port, keepalive) != MOSQ_ERR_SUCCESS)
        {
            std::cerr << "Failed to connect to mosquitto broker" << std::endl;
            return 1;
        }
        // subscribe to mosquitto topics topic_door and topic_control
        topic_door = config_toml["mosquitto"]["topic_door"].value<std::string>().value().c_str();
        std::cout << "Subscribing to mosquitto topic: " << topic_door << std::endl;
        if (mosquitto_subscribe(mosq, NULL, topic_door, 0) != MOSQ_ERR_SUCCESS)
        {
            std::cerr << "Failed to subscribe to mosquitto topic_door" << std::endl;
            return 1;
        }
        const char *topic_control = config_toml["mosquitto"]["topic_control"].value<std::string>().value().c_str();
        std::cout << "Subscribing to mosquitto topic: " << topic_control << std::endl;
        if (mosquitto_subscribe(mosq, NULL, topic_control, 0) != MOSQ_ERR_SUCCESS)
        {
            std::cerr << "Failed to subscribe to mosquitto topic_control" << std::endl;
            return 1;
        }
        // need to rewrite for Paho
        // mosquitto_message_callback_set(mosq, mqtt_control_clbk); // set callback function to handle incoming messages
    } // end use_mosquitto
    std::cout << "attention: name_lastauthenticated.load()->clear();" << std::endl;
    name_lastauthenticated.load()->clear(); // Clear last authenticated name
    std::cout << "over: name_lastauthenticated.load()->clear();" << std::endl;
    signal(SIGTERM, signalHandler);
    signal(SIGINT, signalHandler); // hit Ctrl+C to terminate program
    matrixLEDTask matrix_task(DELAY_MSEC); // create matrixLEDTask object with DELAY_MSEC ms interval
    matrix_task.start(); // start matrix LED task
    while (!interrupt_received){

        std::this_thread::sleep_for(std::chrono::seconds(10));
        // std::this_thread::sleep_for(std::chrono::milliseconds(DELAY_MSEC));
        // usleep(DELAY_USEC); // delay in microseconds; adjust frequency to match potential scrolling or animation patterns 
    } // end while (!interrupt_received)
    
    if(use_mosquitto){
        mosquitto_destroy(mosq); // free mosquitto struct
        mosquitto_lib_cleanup(); // and cleanup
    }
    matrix_task.stop();
    authenticator->Disconnect(); // disconnect Intel RealSenseID F455 camera
    // authenticator->reset(nullptr);
    std::cout << "terminating program" << argv[0] << " all cleaned up..." << std::endl;
    return 0;
} // end main

