#pragma once

/// \file result.hpp
/// Error handling for rendy's public API. Fallible operations return
/// Result<T>; rendy never throws across the API boundary.

#include <cassert>
#include <string>
#include <utility>
#include <variant>

namespace rendy {

/// An error: a human-readable message describing what went wrong.
struct Error {
    std::string message;
};

/// Convenience constructor: `return rendy::err("file not found: {}", path);`
/// lives in log.hpp (needs fmt); this one takes a ready-made string.
inline Error err(std::string message) { return Error{std::move(message)}; }

/// Either a value of type T or an Error. Check with `if (result)`, then take
/// the value with value(). Calling value() on an error, or error() on a
/// value, asserts in debug builds and is undefined in release builds.
template <typename T>
class [[nodiscard]] Result {
public:
    Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}

    [[nodiscard]] bool hasValue() const { return storage_.index() == 0; }
    explicit operator bool() const { return hasValue(); }

    [[nodiscard]] T& value() & {
        assert(hasValue());
        return std::get<0>(storage_);
    }
    [[nodiscard]] const T& value() const& {
        assert(hasValue());
        return std::get<0>(storage_);
    }
    [[nodiscard]] T&& value() && {
        assert(hasValue());
        return std::move(std::get<0>(storage_));
    }

    /// The contained value, or `fallback` if this Result holds an error.
    [[nodiscard]] T valueOr(T fallback) const& {
        return hasValue() ? std::get<0>(storage_) : std::move(fallback);
    }

    [[nodiscard]] const Error& error() const {
        assert(!hasValue());
        return std::get<1>(storage_);
    }

private:
    std::variant<T, Error> storage_;
};

/// Result<void>: success carries no value.
template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(std::move(error)), hasValue_(false) {}

    [[nodiscard]] bool hasValue() const { return hasValue_; }
    explicit operator bool() const { return hasValue_; }

    [[nodiscard]] const Error& error() const {
        assert(!hasValue_);
        return error_;
    }

private:
    Error error_;
    bool hasValue_ = true;
};

} // namespace rendy
