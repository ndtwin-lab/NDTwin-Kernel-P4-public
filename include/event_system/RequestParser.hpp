#pragma once

#include "event_system/PayloadTypes.hpp"
#include "utils/Logger.hpp"
#include "utils/Utils.hpp"
#include <nlohmann/json.hpp>
#include <optional>


inline std::optional<LinkFailedEventPayload>
parseLinkFailedEventPayload(const std::string& jsonStr)
{
    using json = nlohmann::json;
    using sflow::FlowKey;
    using sflow::Path;

    try
    {
        json data = json::parse(jsonStr);

        // Validate presence
        static const std::array<const char*, 4> keys = {"src_dpid",
                                                        "src_interface",
                                                        "dst_dpid",
                                                        "dst_interface"};
        for (auto k : keys)
        {
            if (!data.contains(k))
            {
                SPDLOG_LOGGER_ERROR(Logger::instance(), "Link-failure payload missing key: {}", k);
                return std::nullopt;
            }
        }

        // Extract each field (string or number)
        auto getUint32 = [&](const char* k) -> uint32_t {
            if (data[k].is_number_unsigned())
            {
                return data[k].get<uint32_t>();
            }
            else if (data[k].is_string())
            {
                return static_cast<uint32_t>(std::stoul(data[k].get<std::string>()));
            }
            else
            {
                SPDLOG_LOGGER_ERROR(Logger::instance(),
                                    "Invalid type for '{}', expected number or string",
                                    k);
                throw std::runtime_error("bad link-failure payload");
            }
        };

        auto getUint64 = [&](const char* k) -> uint64_t {
            if (data[k].is_number_unsigned())
            {
                return data[k].get<uint64_t>();
            }
            else if (data[k].is_string())
            {
                return static_cast<uint64_t>(std::stoull(data[k].get<std::string>()));
            }
            else
            {
                SPDLOG_LOGGER_ERROR(Logger::instance(),
                                    "Invalid type for '{}', expected number or string",
                                    k);
                throw std::runtime_error("bad link-failure payload");
            }
        };

        uint64_t srcDpid = getUint64("src_dpid");
        uint32_t srcInterface = getUint32("src_interface");
        uint64_t dstDpid = getUint64("dst_dpid");
        uint32_t dstInterface = getUint32("dst_interface");

        return LinkFailedEventPayload{srcDpid, srcInterface, dstDpid, dstInterface};
    }
    catch (const std::exception& e)
    {
        SPDLOG_LOGGER_ERROR(Logger::instance(),
                            "Exception while parsing LinkFailedEventPayload: {}",
                            e.what());
        return std::nullopt;
    }
}
