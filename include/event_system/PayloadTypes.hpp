#pragma once

#include "common_types/SFlowType.hpp"
#include <vector>

// Payload structure for FlowAdded event
struct FlowAddedEventPayload
{
    sflow::FlowKey flowKey;                     // Source and destination IP
    std::vector<sflow::Path> allAvailablePaths; // Candidate paths for the flow
};

// Payload structure for LinkFailureDetected event
struct LinkFailedEventPayload
{
    uint64_t srcDpid;
    uint32_t srcInterface;
    uint64_t dstDpid;
    uint32_t dstInterface;
};
