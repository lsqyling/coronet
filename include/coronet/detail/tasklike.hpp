#pragma once

#include <type_traits>

namespace coronet::detail {

/*
 * tasklike concept —— 用于识别"类似协程任务"的类型。
 *
 * 当泛型代码需要同时处理 task<T>、shared_task<T>、generator<T> 等
 * 多种协程返回类型时，可以通过此 concept 约束模板参数。
 *
 * 检查三个嵌套类型：
 * 1. promise_type —— 所有协程返回类型都必须有 promise_type
 * 2. value_type —— 协程产生的值类型（可能是 void）
 * 3. is_task_like —— 可选的标记类型，用于显式声明"我是协程任务"
 *    （如果没有该标记，即使有 promise_type 也可能不是 task-like，
 *      因为标准库的某些类型也可能有 promise_type 嵌套类型）
 *
 * 这种"三个检查"比单检查 promise_type 更严格和准确。
 * 相比虚基类的运行时多态，concept 是编译期的零开销抽象。
 */
/// Concept for "task-like" types (have a promise_type)
template<typename T>
concept tasklike = requires {
    typename T::promise_type;
    typename T::value_type;
    typename T::is_task_like;
};

} // namespace coronet::detail
