#include "application.hpp"
#include <iostream>

/**
 * @brief Program entry point.
 *
 * The binary accepts an optional configuration file path and starts the
 * application runtime once initialization is successful.
 */
int main(int argc, char* argv[]) {
    const std::string configPath = (argc > 1) ? argv[1] : "./config.toml";

    Application app;
    if (!app.initialize(configPath)) {
        std::cerr << "Failed to initialize application with config: " << configPath << std::endl;
        return 1;
    }

    return app.run();
}
