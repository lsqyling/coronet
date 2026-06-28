/// RAII defer: executes lambda on scope exit. Aligned with co_context.
#pragma once

namespace coronet {

template<typename Lambda>
struct defer : Lambda {
    ~defer() { Lambda::operator()(); }
};

template<typename Lambda>
defer(Lambda) -> defer<Lambda>;

} // namespace coronet
