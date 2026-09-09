#pragma once

#include "event.hpp"

#include <memory>

class EventStore {
public:
    virtual ~EventStore() = default;
    virtual void append(const RuntimeEvent& event) = 0;
};

using EventStorePtr = std::shared_ptr<EventStore>;
