#pragma once

#include <memory>

namespace circud {

    class Core;

    class HttpServer {
    public:
        explicit HttpServer(Core& core);
        ~HttpServer();

        HttpServer(const HttpServer&) = delete;
        HttpServer& operator=(const HttpServer&) = delete;

        void start();
        void stop();

    private:
        class Impl;
        std::shared_ptr<Impl> impl_;
    };

} // namespace circud
