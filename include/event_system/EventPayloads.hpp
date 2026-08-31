#pragma once

#include "common_types/GraphTypes.hpp"
#include "common_types/SFlowType.hpp"
#include "event_system/PayloadTypes.hpp"
#include <boost/graph/graph_traits.hpp>
#include <functional>

struct FlowAddedEventData
{
    FlowAddedEventPayload payload;
    std::function<void(std::optional<sflow::Path>)> callback;
};

struct LinkFailureEventData
{
    Graph::edge_descriptor failedEdge;
};

struct IdleFlowPurgedEventData
{
    sflow::FlowKey flowKey;
};