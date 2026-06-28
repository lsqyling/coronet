#pragma once

// coronet: A cross-platform high-performance async I/O library using C++20 coroutines.

// Core coroutine types
#include "coronet/task.hpp"
#include "coronet/generator.hpp"
#include "coronet/shared_task.hpp"      // reference-counted multi-waiter task

// I/O context / scheduler
#include "coronet/io_context.hpp"

// Async I/O primitives
#include "coronet/async_io.hpp"
#include "coronet/detail/chained_awaiter.hpp"

// Network abstractions
#include "coronet/net.hpp"

// Synchronization primitives
#include "coronet/co/mutex.hpp"
#include "coronet/co/condition_variable.hpp"
#include "coronet/co/semaphore.hpp"
#include "coronet/co/channel.hpp"

// Platform abstraction
#include "coronet/platform/platform.hpp"
