#pragma once
#include <string>


/**
 * @brief Metadata for an application registered with the system.
 *
 * Holds the application name and the callback URL that is invoked
 * when its simulation has completed.
 */
struct RegisteredApp
{
    std::string appName;    
    std::string simulationCompletedUrl; 
};
