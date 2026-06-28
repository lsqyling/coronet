#pragma once

/// Convenience header: includes all public coronet headers.

#include "coronet/coronet.hpp"

// Synchronization primitives
#include "coronet/co/mutex.hpp"
#include "coronet/co/condition_variable.hpp"
#include "coronet/co/semaphore.hpp"
#include "coronet/co/channel.hpp"
#include "coronet/co/when_all.hpp"

// Utilities
#include "coronet/utility/defer.hpp"
#include "coronet/log/log.hpp"
