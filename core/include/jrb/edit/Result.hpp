#pragma once

#include <string>
#include <utility>
#include <variant>

namespace jrb {

/// A user-presentable reason an edit was rejected.
struct Error {
    std::string message;
};

/// Value-or-error. (std::expected is C++23; the core targets C++20.)
template <class T>
class [[nodiscard]] Result {
public:
    Result(T value) : storage_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
    Result(Error error) : storage_(std::move(error)) {}      // NOLINT(google-explicit-constructor)

    [[nodiscard]] explicit operator bool() const noexcept { return storage_.index() == 0; }

    [[nodiscard]] T& value() & { return std::get<0>(storage_); }
    [[nodiscard]] const T& value() const& { return std::get<0>(storage_); }
    [[nodiscard]] T&& value() && { return std::get<0>(std::move(storage_)); }
    [[nodiscard]] const Error& error() const { return std::get<1>(storage_); }

private:
    std::variant<T, Error> storage_;
};

struct Ok {};
using Status = Result<Ok>;

[[nodiscard]] inline Status ok() { return Ok{}; }
[[nodiscard]] inline Error fail(std::string message) { return Error{std::move(message)}; }

} // namespace jrb
