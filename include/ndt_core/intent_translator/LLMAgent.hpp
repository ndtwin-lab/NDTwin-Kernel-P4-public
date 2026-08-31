#pragma once
#include <nlohmann/json.hpp>
#include <map>
#include <vector>

#include "utils/Logger.hpp"
#include "utils/Utils.hpp"
#include "ndt_core/collection/TopologyAndFlowMonitor.hpp"
#include "ndt_core/power_management/DeviceConfigurationAndPowerManager.hpp"
#include "ndt_core/intent_translator/LLMResponseTypes.hpp"

using json = nlohmann::json;

class LLMAgent
{
    public:
        enum Role : bool {
            USER = true,
            AGENT = false
        };
        LLMAgent(
            std::string systemPromptFilePath,
            std::shared_ptr<TopologyAndFlowMonitor> topologyAndFlowMonitor,
            std::shared_ptr<DeviceConfigurationAndPowerManager> deviceConfigurationAndPowerManager,
            std::string model
        );
        std::unique_ptr<llmResponse::LLMResponse> callOpenAIApi(const std::string &inputText, const std::string &sessionId);
        std::vector<std::pair<Role, json>> getSessionMsgs(const std::string &sessionId);
        void addMsgToSession(const std::string &sessionId, Role role, const json &msg);
        void cleanSession(const std::string &sessionId);

    private:
        std::string shellEscapeSingleQuotes(const std::string &str);
        std::string getLastMsgId(const std::string &sessionId) const;
        std::string getCurrentTopology();
        std::string getCurrentFlowEntries();

        std::string m_systemPromptFilePath;
        std::shared_ptr<TopologyAndFlowMonitor> m_topologyAndFlowMonitor;
        std::shared_ptr<DeviceConfigurationAndPowerManager> m_deviceConfigManager;
        std::string m_model;
        std::string m_apiKey;
        std::map<std::string, std::vector<std::pair<Role, json>>> m_sessionIdToMsgMap;
        std::map<std::string, std::pair<int, int>> m_sessionTokensCount; // session id -> {input tokens, output tokens}
        std::vector<int> m_responseTime;
        bool m_rateLimit = false;
};