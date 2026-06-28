/// coronet generator demo: iota with ranges
#include <coronet/generator.hpp>

#include <iostream>
#include <ranges>

coronet::generator<int> iota(int x) {
    while (true) {
        co_yield x;
        ++x;
    }
}

int main() {
    using std::views::drop, std::views::take;

    for (auto &&x : iota(1) | drop(5) | take(3)) {
        std::cout << x << " ";
    }
    std::cout << std::endl;
    return 0;
}
