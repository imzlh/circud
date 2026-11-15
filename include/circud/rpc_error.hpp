#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace circud {

    class RpcError : public std::runtime_error {
    public:
        RpcError(std::string code, std::string message)
            : std::runtime_error(std::move(message)), code_(std::move(code)) {}

        std::string_view code() const noexcept { return code_; }

    private:
        std::string code_;
    };

} // namespace circud
