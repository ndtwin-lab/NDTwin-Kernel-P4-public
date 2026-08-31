#pragma once
#include <array>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

/**
 * @brief Fetch raw power-status output from a switch via SSH.
 *
 * Builds a non-interactive SSH command that feeds a small sequence of CLI commands
 * into the remote switch (e.g., "terminal length 0", "show power") and captures
 * the resulting stdout text.
 *
 * Implementation notes:
 *  - Uses popen() to execute a shell pipeline:
 *      (echo ...; sleep ...; echo ...) | ssh ...
 *  - Adds SSH options to improve compatibility with older switch SSH stacks
 *    (KexAlgorithms/Ciphers/HostKeyAlgorithms).
 *  - Disables strict host key checking to avoid interactive prompts.
 *
 * @param ip        Switch management IP address.
 * @param username  SSH username for the switch.
 * @return Full raw CLI output (stdout) as a string.
 * @throws std::runtime_error if popen() fails.
 *
 * @warning This function executes a shell command. Ensure @p ip and @p username are
 *          trusted or sanitized to prevent shell injection.
 */
inline std::string
getPowerReportViaSsh(const std::string& ip, const std::string& username)
{
    // We build a single shell command that does the following:
    // 1. ( ... ) creates a subshell to group our commands.
    // 2. 'echo' sends a command, and then we 'sleep 1' to wait for the switch.
    // 3. The '|' (pipe) sends the entire sequence to the ssh client's input.

    std::stringstream command_stream;

    command_stream
        << "(echo 'terminal length 0'; sleep 1; echo 'show power'; sleep 1; echo 'exit') | "
        << "ssh -T " << "-oKexAlgorithms=+diffie-hellman-group1-sha1 " << "-oCiphers=+aes128-cbc "
        << "-oHostKeyAlgorithms=+ssh-rsa " << "-oStrictHostKeyChecking=no "
        << "-oUserKnownHostsFile=/dev/null " << "-oBatchMode=yes " << "-oConnectTimeout=10 "
        << username << "@" << ip;
    
    std::string command = command_stream.str();


    // This part is correct and remains the same.
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe)
    {
        throw std::runtime_error("popen() failed!");
    }

    std::array<char, 256> buffer;
    std::stringstream result_stream;

    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr)
    {
        result_stream << buffer.data();
    }

    pclose(pipe);
    return result_stream.str();
}

/**
 * @brief Parse power value from the switch CLI output.
 *
 * Scans the CLI output for a known line prefix (targetPrefix) and attempts to read
 * a numeric power value from that line. Returns a scaled value (implementation-defined).
 *
 * @param output Raw CLI output returned by getPowerReportViaSsh().
 * @return Parsed power value (scaled), or 0 if not found or parsing fails.
 *
 * @note This parser relies on a fixed string prefix (targetPrefix) that must match
 *       the switch model/output format. If the CLI format changes, update the prefix
 *       and parsing logic accordingly.
 */
inline uint64_t
parsePowerOutput(const std::string& output)
{
    std::istringstream stream(output);
    std::string line;
    const std::string targetPrefix = "1    ICX7250-24 24-p";

    while (std::getline(stream, line))
    {
        size_t pos = line.find(targetPrefix);
        if (pos != std::string::npos)
        {
            std::string sub = line.substr(pos + targetPrefix.length());
            std::istringstream lineStream(sub);
            uint64_t power;
            if (lineStream >> power)
            {
                return power/10000000;
            }
        }
    }
    return 0;
}