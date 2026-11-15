#include "test.hpp"

#include <circud/auth.hpp>
#include <circud/core.hpp>
#include <circud/http_background.hpp>
#include <circud/http_server.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

#include <minizip/zip.h>
#include <sys/socket.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <string_view>
#include <thread>
#include <tuple>

namespace {

using tcp = boost::asio::ip::tcp;
namespace http = boost::beast::http;
namespace websocket = boost::beast::websocket;

class ServerHarness {
public:
    explicit ServerHarness(circud::Core& core) : core_(core), server_(core) {
        core_.start();
        io_thread_ = std::thread([this] { core_.io().run(); });
        try {
            server_.start();
        }
        catch (...) {
            core_.stop();
            if (io_thread_.joinable()) io_thread_.join();
            throw;
        }
    }

    ~ServerHarness() {
        server_.stop();
        sync_io();
        core_.stop();
        if (io_thread_.joinable()) io_thread_.join();
    }

    void stop() { server_.stop(); }

    void start() { server_.start(); }

private:
    void sync_io() {
        auto completed = std::make_shared<std::promise<void>>();
        auto ready = completed->get_future();
        boost::asio::post(core_.io(), [completed] { completed->set_value(); });
        if (ready.wait_for(std::chrono::seconds(2)) !=
            std::future_status::ready) {
            return;
        }
    }

    circud::Core& core_;
    circud::HttpServer server_;
    std::thread io_thread_;
};

class TempDir {
public:
    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("circud-http-test-" + std::to_string(
                                          std::chrono::steady_clock::now()
                                              .time_since_epoch()
                                              .count()));
        std::filesystem::create_directories(path);
    }
    ~TempDir() { std::filesystem::remove_all(path); }
    std::filesystem::path path;
};

std::uint16_t available_port() {
    boost::asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(
                                   boost::asio::ip::address_v4::loopback(), 0));
    return acceptor.local_endpoint().port();
}

void set_socket_io_timeout(tcp::socket& socket,
                           std::chrono::milliseconds timeout) {
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(timeout);
    const auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(
        timeout - seconds);
    const timeval value{
        static_cast<decltype(timeval::tv_sec)>(seconds.count()),
        static_cast<decltype(timeval::tv_usec)>(microseconds.count())};
    if (::setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &value,
                     sizeof(value)) != 0 ||
        ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_SNDTIMEO, &value,
                     sizeof(value)) != 0) {
        throw std::runtime_error("cannot set endpoint test socket timeout");
    }
}

bool request_health(std::uint16_t port) {
    boost::asio::io_context io;
    tcp::socket socket(io);
    socket.connect(tcp::endpoint(boost::asio::ip::address_v4::loopback(), port));
    http::request<http::empty_body> request{http::verb::get, "/health", 11};
    request.set(http::field::host, "127.0.0.1");
    request.keep_alive(false);
    http::write(socket, request);
    boost::beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(socket, buffer, response);
    return response.result() == http::status::ok &&
           response.body() == R"({"ok":true})";
}

http::response<http::string_body> request(
    std::uint16_t port, http::verb method, std::string_view target,
    std::string body = {}, std::string_view content_type = {}) {
    boost::asio::io_context io;
    tcp::socket socket(io);
    socket.connect(tcp::endpoint(boost::asio::ip::address_v4::loopback(), port));
    http::request<http::string_body> value{
        method, std::string(target), 11};
    value.set(http::field::host, "127.0.0.1");
    value.keep_alive(false);
    if (!content_type.empty()) value.set(http::field::content_type, content_type);
    value.body() = std::move(body);
    value.prepare_payload();
    http::write(socket, value);
    boost::beast::flat_buffer buffer;
    http::response_parser<http::string_body> parser;
    if (method == http::verb::head) parser.skip(true);
    http::read(socket, buffer, parser);
    return parser.release();
}

http::response<http::string_body> request_with_headers(
    std::uint16_t port, http::verb method, std::string_view target,
    std::string body, std::string_view content_type,
    const std::vector<std::pair<std::string, std::string>>& headers) {
    boost::asio::io_context io;
    tcp::socket socket(io);
    socket.connect(tcp::endpoint(boost::asio::ip::address_v4::loopback(), port));
    http::request<http::string_body> value{
        method, std::string(target), 11};
    value.set(http::field::host, "127.0.0.1");
    value.keep_alive(false);
    if (!content_type.empty()) value.set(http::field::content_type, content_type);
    for (const auto& [name, header_value] : headers)
        value.set(name, header_value);
    value.body() = std::move(body);
    value.prepare_payload();
    http::write(socket, value);
    boost::beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(socket, buffer, response);
    return response;
}

std::string response_cookie(
    const http::response<http::string_body>& response) {
    const auto header = response[http::field::set_cookie];
    std::string cookie(header.data(), header.size());
    if (const auto attributes = cookie.find(';');
        attributes != std::string::npos)
        cookie.resize(attributes);
    return cookie;
}

std::string json_string_field(std::string_view json, std::string_view name) {
    const auto marker = "\"" + std::string(name) + "\":\"";
    const auto begin = json.find(marker);
    if (begin == std::string_view::npos)
        throw std::runtime_error("missing JSON string field: " +
                                 std::string(name));
    const auto value_begin = begin + marker.size();
    const auto end = json.find('"', value_begin);
    if (end == std::string_view::npos)
        throw std::runtime_error("unterminated JSON string field: " +
                                 std::string(name));
    return std::string(json.substr(value_begin, end - value_begin));
}

http::response<http::string_body> request_method(
    std::uint16_t port, std::string_view method, std::string_view target,
    std::string body = {}, std::string_view content_type = {}) {
    boost::asio::io_context io;
    tcp::socket socket(io);
    socket.connect(tcp::endpoint(boost::asio::ip::address_v4::loopback(), port));
    http::request<http::string_body> value;
    value.method_string(method);
    value.target(target);
    value.version(11);
    value.set(http::field::host, "127.0.0.1");
    value.keep_alive(false);
    if (!content_type.empty()) value.set(http::field::content_type, content_type);
    value.body() = std::move(body);
    value.prepare_payload();
    http::write(socket, value);
    boost::beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(socket, buffer, response);
    return response;
}

void make_public_dir(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    std::ofstream(root / "index.html") << "<!doctype html><title>test UI</title>";
    std::ofstream(root / "app.css") << "body { color: orange; }";
}

std::string patterned_bytes(std::size_t size) {
    std::string result(size, '\0');
    for (std::size_t index = 0; index < size; ++index)
        result[index] = static_cast<char>('!' + index % 90);
    return result;
}

void create_package_zip(const std::filesystem::path& path,
                        std::size_t padding_bytes = 0) {
    std::vector<std::pair<std::string, std::string>> files{
        {"meta.json",
         R"({"schema":1,"id":"io.example.upload","name":"upload","version":"1.0.0"})"},
        {"management.js", R"JS(
const evbus = import.meta.use('evbus');
evbus.on('circud.host', 'call', event => {
  if (event.target === 'echo') event.respondWith(event.arguments);
});
)JS"},
    };
    if (padding_bytes != 0)
        files.emplace_back("payload.bin", patterned_bytes(padding_bytes));
    zipFile archive = zipOpen64(path.c_str(), APPEND_STATUS_CREATE);
    if (!archive) throw std::runtime_error("cannot create HTTP test ZIP");
    for (const auto& [name, contents] : files) {
        zip_fileinfo info{};
        if (zipOpenNewFileInZip(archive, name.c_str(), &info, nullptr, 0,
                                nullptr, 0, nullptr, Z_DEFLATED,
                                Z_DEFAULT_COMPRESSION) != ZIP_OK ||
            zipWriteInFileInZip(archive, contents.data(), contents.size()) !=
                ZIP_OK ||
            zipCloseFileInZip(archive) != ZIP_OK) {
            zipClose(archive, nullptr);
            throw std::runtime_error("cannot write HTTP test ZIP");
        }
    }
    if (zipClose(archive, nullptr) != ZIP_OK)
        throw std::runtime_error("cannot close HTTP test ZIP");
}

void create_endpoint_package_zip(const std::filesystem::path& path) {
    const std::vector<std::pair<std::string, std::string>> files{
        {"meta.json",
         R"({"schema":1,"id":"io.example.endpoint","name":"endpoint","version":"1.0.0"})"},
        {"management.js", R"JS(
const evbus = import.meta.use('evbus');
const { setTimeout } = import.meta.use('timers');

evbus.on('circud.host', 'endpoint', event => {
  if (event.path === '/') {
    event.respondWith({ body: 'package scope root' });
  } else if (event.path === '/http') {
    event.respondWith({
      status: 201,
      headers: {
        'content-type': 'application/json',
        'x-endpoint-test': 'yes'
      },
      body: { method: event.method, path: event.path, query: event.query }
    });
  } else if (event.path === '/async') {
    event.respondWith(new Promise(resolve => {
      setTimeout(() => resolve({
        status: 202,
        headers: { 'x-async-endpoint': 'yes' },
        body: { method: event.method, bodyLength: event.body.byteLength }
      }), 100);
    }));
  } else if (event.path === '/method') {
    event.respondWith({ body: event.method });
  } else if (event.path === '/authorization') {
    event.respondWith({ body: { isAuthorized: event.isAuthorized } });
  } else if (event.path === '/forwarded-headers') {
    const cookies = event.headers
      .filter(([name]) => name.toLowerCase() === 'cookie')
      .map(([, value]) => value);
    const hasCsrf = event.headers.some(
      ([name]) => name.toLowerCase() === 'x-circud-csrf');
    event.respondWith({ body: { cookies, hasCsrf } });
  } else if (event.path === '/static') {
    event.serveStatic('assets/served.txt');
  } else if (event.path === '/static-large') {
    event.serveStatic('assets/large.bin');
  } else if (event.path.startsWith('/empty-status-')) {
    event.respondWith({ status: Number(event.path.slice(14)) });
  } else if (event.path.startsWith('/body-status-')) {
    event.respondWith({
      status: Number(event.path.slice(13)),
      body: 'body forbidden by this status'
    });
  } else if (event.path === '/sse-method') {
    try {
      event.acceptSse();
    } catch (error) {
      event.respondWith({
        status: 405,
        body: error instanceof TypeError ? String(error.message) : 'wrong error'
      });
    }
  } else if (event.path === '/sse-status') {
    try {
      event.acceptSse({ status: 204 });
    } catch (error) {
      event.respondWith({
        status: 400,
        body: error instanceof RangeError ? String(error.message) : 'wrong error'
      });
    }
  } else if (event.path === '/sse') {
    const stream = event.acceptSse();
    stream.send('event: ready\ndata: first\n\n');
    setTimeout(() => {
      stream.send('data: second\n\n');
      stream.close();
    }, 500);
  } else if (event.path === '/ws') {
    const socket = event.acceptWebSocket({ subprotocol: 'echo.v1' });
    socket.onmessage = message => socket.send(message.data);
  }
});
)JS"},
        {"assets/served.txt", "served only after explicit delegation\n"},
        {"assets/large.bin", patterned_bytes(3 * 64 * 1024 + 91)},
        {"assets/hidden.txt", "must never be exposed automatically\n"},
    };
    zipFile archive = zipOpen64(path.c_str(), APPEND_STATUS_CREATE);
    if (!archive) throw std::runtime_error("cannot create endpoint test ZIP");
    for (const auto& [name, contents] : files) {
        zip_fileinfo info{};
        if (zipOpenNewFileInZip(archive, name.c_str(), &info, nullptr, 0,
                                nullptr, 0, nullptr, Z_DEFLATED,
                                Z_DEFAULT_COMPRESSION) != ZIP_OK ||
            zipWriteInFileInZip(archive, contents.data(), contents.size()) !=
                ZIP_OK ||
            zipCloseFileInZip(archive) != ZIP_OK) {
            zipClose(archive, nullptr);
            throw std::runtime_error("cannot write endpoint test ZIP");
        }
    }
    if (zipClose(archive, nullptr) != ZIP_OK)
        throw std::runtime_error("cannot close endpoint test ZIP");
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot read HTTP test file");
    return {std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()};
}

} // namespace

CIRCUD_TEST(many_idle_http_clients_do_not_block_other_clients) {
    using namespace std::chrono_literals;
    TempDir temp;
    circud::CoreConfig config;
    config.state_dir = temp.path;
    config.listen_address = "127.0.0.1";
    config.listen_port = available_port();
    config.public_dir = temp.path / "public";
    config.http_background_workers = 1;
    make_public_dir(config.public_dir);
    circud::Core core(config);
    ServerHarness server(core);

    boost::asio::io_context idle_io;
    std::vector<tcp::socket> idle;
    for (unsigned index = 0; index < 12; ++index) {
        idle.emplace_back(idle_io);
        idle.back().connect(tcp::endpoint(
            boost::asio::ip::address_v4::loopback(), config.listen_port));
        boost::asio::write(idle.back(), boost::asio::buffer("G", 1));
    }
    std::this_thread::sleep_for(50ms);

    auto health = std::async(std::launch::async, request_health,
                             config.listen_port);
    const bool ready = health.wait_for(2s) == std::future_status::ready;
    boost::system::error_code ignored;
    for (auto& socket : idle) socket.close(ignored);
    if (!ready) server.stop();
    const bool healthy = health.get();
    server.stop();
    circud::test::require(ready, "second HTTP client was blocked");
    circud::test::require(healthy, "health response was invalid");
}

CIRCUD_TEST(http_background_bounds_work_and_cancels_off_control_thread) {
    using namespace std::chrono_literals;
    const auto control_thread = std::this_thread::get_id();
    auto entered = std::make_shared<std::promise<void>>();
    auto entered_ready = entered->get_future();
    auto release = std::make_shared<std::promise<void>>();
    auto release_ready = release->get_future().share();
    auto worker = circud::HttpBackground::create(1);

    circud::test::require(worker->post(
        circud::HttpBackground::max_queued_bytes,
        [entered, release_ready] {
            entered->set_value();
            release_ready.wait();
        }), "background executor rejected its exact byte budget");
    circud::test::require(entered_ready.wait_for(2s) ==
        std::future_status::ready,
        "background executor did not start admitted work");
    circud::test::require(!worker->post(1, [] {}),
        "background executor exceeded its byte budget");

    std::atomic<unsigned> cancelled{ 0 };
    std::atomic<bool> cancel_on_control{ false };
    circud::test::require(worker->post(0, [] {
        throw std::runtime_error("queued work ran after stop");
    }, [&] {
        cancel_on_control.store(std::this_thread::get_id() == control_thread,
            std::memory_order_release);
        cancelled.fetch_add(1, std::memory_order_release);
    }), "background executor rejected a bounded cancellation task");
    worker->stop();
    release->set_value();
    worker->join();
    circud::test::require(cancelled.load(std::memory_order_acquire) == 1,
        "stopped background generation did not cancel queued work");
    circud::test::require(!cancel_on_control.load(std::memory_order_acquire),
        "queued filesystem cleanup ran on the control thread");
    circud::test::require(!worker->post(0, [] {}),
        "stopped background generation accepted new work");
}

CIRCUD_TEST(http_1_1_keep_alive_and_pipelining_preserve_response_order) {
    TempDir temp;
    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    config.listen_address = "127.0.0.1";
    config.listen_port = available_port();
    config.public_dir = temp.path / "public";
    make_public_dir(config.public_dir);
    circud::Core core(config);
    ServerHarness server(core);

    boost::asio::io_context io;
    tcp::socket socket(io);
    socket.connect(tcp::endpoint(
        boost::asio::ip::address_v4::loopback(), config.listen_port));

    http::request<http::empty_body> first{
        http::verb::get, "/health", 11 };
    first.set(http::field::host, "127.0.0.1");
    first.keep_alive(true);
    http::request<http::empty_body> second{
        http::verb::get, "/app.css", 11 };
    second.set(http::field::host, "127.0.0.1");
    second.keep_alive(false);

    http::write(socket, first);
    http::write(socket, second);

    boost::beast::flat_buffer buffer;
    http::response<http::string_body> first_response;
    http::response<http::string_body> second_response;
    http::read(socket, buffer, first_response);
    http::read(socket, buffer, second_response);
    server.stop();

    circud::test::require(first_response.version() == 11,
                          "health response was not HTTP/1.1");
    circud::test::require(first_response.keep_alive(),
                          "server did not preserve HTTP/1.1 keep-alive");
    circud::test::require(first_response.result() == http::status::ok);
    circud::test::require(first_response.body() == R"({"ok":true})",
                          "pipelined responses were returned out of order");
    circud::test::require(second_response.version() == 11,
                          "static response was not HTTP/1.1");
    circud::test::require(!second_response.keep_alive(),
                          "Connection: close was not honored");
    circud::test::require(second_response.result() == http::status::ok);
    circud::test::require(
        second_response.body().find("color: orange") != std::string::npos,
        "second pipelined response did not match the second request");
}

CIRCUD_TEST(http_serves_public_files_with_correct_types) {
    TempDir temp;
    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    config.listen_port = available_port();
    config.public_dir = temp.path / "public";
    make_public_dir(config.public_dir);
    circud::Core core(config);
    ServerHarness server(core);
    const auto index = request(config.listen_port, http::verb::get, "/");
    const auto css = request(config.listen_port, http::verb::get, "/app.css");
    const auto head = request(config.listen_port, http::verb::head, "/app.css");
    server.stop();
    circud::test::require(index.result() == http::status::ok);
    circud::test::require(index.body().find("test UI") != std::string::npos);
    circud::test::require(
        css[http::field::content_type].starts_with("text/css"));
    circud::test::require(head.result() == http::status::ok);
    circud::test::require(head.body().empty());
    circud::test::require(
        std::stoull(std::string(head[http::field::content_length])) ==
        css.body().size());
}

CIRCUD_TEST(http_streams_public_static_across_bounded_chunks) {
    TempDir temp;
    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    config.listen_port = available_port();
    config.public_dir = temp.path / "public";
    config.http_max_static_file_bytes = 256 * 1024;
    make_public_dir(config.public_dir);
    const auto expected = patterned_bytes(3 * 64 * 1024 + 137);
    std::ofstream file(config.public_dir / "large.bin", std::ios::binary);
    file.write(expected.data(), static_cast<std::streamsize>(expected.size()));
    file.close();
    circud::Core core(config);
    ServerHarness server(core);
    const auto response = request(
        config.listen_port, http::verb::get, "/large.bin");
    server.stop();
    circud::test::require(response.result() == http::status::ok,
        response.body());
    circud::test::require(response.body() == expected,
        "multi-chunk public static response was corrupted");
}

CIRCUD_TEST(slow_static_client_does_not_block_health_or_listener_stop) {
    using namespace std::chrono_literals;
    TempDir temp;
    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    config.listen_port = available_port();
    config.public_dir = temp.path / "public";
    config.http_max_static_file_bytes = 16 * 1024 * 1024;
    config.http_io_timeout = 5s;
    make_public_dir(config.public_dir);
    std::ofstream(config.public_dir / "slow.bin", std::ios::binary).close();
    std::filesystem::resize_file(
        config.public_dir / "slow.bin", 8 * 1024 * 1024);
    circud::Core core(config);
    ServerHarness server(core);

    boost::asio::io_context io;
    tcp::socket slow(io);
    slow.connect(tcp::endpoint(
        boost::asio::ip::address_v4::loopback(), config.listen_port));
    const int receive_bytes = 4096;
    if (::setsockopt(slow.native_handle(), SOL_SOCKET, SO_RCVBUF,
            &receive_bytes, sizeof(receive_bytes)) != 0)
        throw std::runtime_error("cannot constrain slow static client");
    const std::string wire =
        "GET /slow.bin HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        "Connection: close\r\n\r\n";
    boost::asio::write(slow, boost::asio::buffer(wire));
    std::this_thread::sleep_for(50ms);

    auto health = std::async(std::launch::async, request_health,
        config.listen_port);
    circud::test::require(health.wait_for(2s) == std::future_status::ready &&
            health.get(),
        "slow static client blocked the network/control loop");
    auto stopped = std::async(std::launch::async, [&server] { server.stop(); });
    circud::test::require(stopped.wait_for(2s) == std::future_status::ready,
        "slow static client blocked listener stop");
    stopped.get();
    boost::system::error_code ignored;
    slow.close(ignored);
}

CIRCUD_TEST(http_rejects_static_path_escape) {
    TempDir temp;
    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    config.listen_port = available_port();
    config.public_dir = temp.path / "public";
    make_public_dir(config.public_dir);
    std::ofstream(temp.path / "secret.txt") << "secret";
    circud::Core core(config);
    ServerHarness server(core);
    const auto response =
        request(config.listen_port, http::verb::get, "/%2e%2e/secret.txt");
    server.stop();
    circud::test::require(response.result() == http::status::bad_request);
    circud::test::require(response.body().find("secret") == std::string::npos);
}

CIRCUD_TEST(http_rejects_static_symlink_escape) {
    TempDir temp;
    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    config.listen_port = available_port();
    config.public_dir = temp.path / "public";
    make_public_dir(config.public_dir);
    std::ofstream(temp.path / "secret.txt") << "secret";
    std::error_code error;
    std::filesystem::create_symlink(temp.path / "secret.txt",
                                    config.public_dir / "leak.txt", error);
    if (error) return;
    circud::Core core(config);
    ServerHarness server(core);
    const auto response =
        request(config.listen_port, http::verb::get, "/leak.txt");
    server.stop();
    circud::test::require(response.result() == http::status::not_found);
    circud::test::require(response.body().find("secret") == std::string::npos);
}

CIRCUD_TEST(http_rpc_requires_json_content_type) {
    TempDir temp;
    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    config.listen_port = available_port();
    config.public_dir = temp.path / "public";
    make_public_dir(config.public_dir);
    circud::Core core(config);
    ServerHarness server(core);
    const auto response = request(config.listen_port, http::verb::post, "/rpc",
                                  R"({"method":"packages.list"})",
                                  "text/plain");
    server.stop();
    circud::test::require(response.result() ==
                          http::status::unsupported_media_type);
}

CIRCUD_TEST(http_core_router_reserves_api_tree_and_reports_allowed_methods) {
    TempDir temp;
    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    config.listen_port = available_port();
    config.public_dir = temp.path / "public";
    make_public_dir(config.public_dir);
    std::filesystem::create_directories(config.public_dir / "api");
    std::ofstream(config.public_dir / "api" / "encoded.txt")
        << "must not bypass the core router";
    circud::Core core(config);
    ServerHarness server(core);

    const auto unknown = request(
        config.listen_port, http::verb::get, "/api/not-a-route");
    const auto health_method = request(
        config.listen_port, http::verb::post, "/health");
    const auto login_method = request(
        config.listen_port, http::verb::get, "/api/auth/login");
    const auto unsafe = request(
        config.listen_port, http::verb::get, "/api%2Fauth/session");
    const auto encoded_api = request(
        config.listen_port, http::verb::get, "/ap%69/encoded.txt");
    server.stop();

    circud::test::require(unknown.result() == http::status::not_found,
                          unknown.body());
    circud::test::require(health_method.result() ==
                          http::status::method_not_allowed,
                          health_method.body());
    circud::test::require(health_method[http::field::allow] == "GET, HEAD");
    circud::test::require(login_method.result() ==
                          http::status::method_not_allowed,
                          login_method.body());
    circud::test::require(login_method[http::field::allow] == "POST");
    circud::test::require(unsafe.result() == http::status::bad_request,
                          unsafe.body());
    circud::test::require(encoded_api.result() == http::status::not_found,
                          encoded_api.body());
    circud::test::require(
        encoded_api.body().find("must not bypass") == std::string::npos,
        "an encoded core path bypassed into the public directory");
}

CIRCUD_TEST(http_head_errors_do_not_corrupt_the_next_keep_alive_response) {
    TempDir temp;
    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    config.listen_port = available_port();
    config.public_dir = temp.path / "public";
    make_public_dir(config.public_dir);
    circud::Core core(config);
    ServerHarness server(core);

    boost::asio::io_context io;
    tcp::socket socket(io);
    socket.connect(tcp::endpoint(
        boost::asio::ip::address_v4::loopback(), config.listen_port));
    http::request<http::empty_body> head{
        http::verb::head, "/api/auth/login", 11};
    head.set(http::field::host, "127.0.0.1");
    head.keep_alive(true);
    http::request<http::empty_body> health{
        http::verb::get, "/health", 11};
    health.set(http::field::host, "127.0.0.1");
    health.keep_alive(false);
    http::write(socket, head);
    http::write(socket, health);

    boost::beast::flat_buffer buffer;
    http::response_parser<http::string_body> head_parser;
    head_parser.skip(true);
    http::read(socket, buffer, head_parser);
    const auto head_response = head_parser.release();
    http::response<http::string_body> health_response;
    http::read(socket, buffer, health_response);
    server.stop();

    circud::test::require(head_response.result() ==
                          http::status::method_not_allowed);
    circud::test::require(head_response.body().empty());
    circud::test::require(head_response.has_content_length());
    circud::test::require(std::stoull(std::string(
                              head_response[http::field::content_length])) > 0);
    circud::test::require(health_response.result() == http::status::ok);
    circud::test::require(health_response.body() == R"({"ok":true})");
}

CIRCUD_TEST(http_auth_disabled_keeps_management_routes_available) {
    TempDir temp;
    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    config.listen_port = available_port();
    config.public_dir = temp.path / "public";
    config.auth.enabled = false;
    make_public_dir(config.public_dir);
    circud::Core core(config);
    ServerHarness server(core);

    const auto session = request(
        config.listen_port, http::verb::get, "/api/auth/session");
    const auto rpc = request(
        config.listen_port, http::verb::post, "/rpc",
        R"({"method":"packages.list","params":{}})", "application/json");
    const auto login = request(
        config.listen_port, http::verb::post, "/api/auth/login",
        R"({"username":"admin","password":"unused"})", "application/json");
    const auto logout = request(
        config.listen_port, http::verb::post, "/api/auth/logout");
    server.stop();

    circud::test::require(session.result() == http::status::ok,
                          session.body());
    circud::test::require(
        session.body().find(R"("authEnabled":false)") != std::string::npos &&
        session.body().find(R"("authenticated":false)") != std::string::npos,
        session.body());
    circud::test::require(rpc.result() == http::status::ok, rpc.body());
    circud::test::require(login.result() == http::status::conflict,
                          login.body());
    circud::test::require(login.body().find("authentication_disabled") !=
                          std::string::npos, login.body());
    circud::test::require(logout.result() == http::status::no_content,
                          logout.body());
    circud::test::require(logout.body().empty(), logout.body());
}

CIRCUD_TEST(http_rpc_exposes_public_system_info) {
    TempDir temp;
    circud::CoreConfig config;
    config.state_dir = temp.path / "private-state";
    config.listen_port = available_port();
    config.public_dir = temp.path / "public";
    config.http_max_upload_bytes = 123456;
    config.status_interval = std::chrono::milliseconds(4321);
    config.event_capacity = 77;
    make_public_dir(config.public_dir);
    circud::Core core(config);
    ServerHarness server(core);
    const auto response = request(config.listen_port, http::verb::post, "/rpc",
                                  R"({"method":"system.info","params":{}})",
                                  "application/json");
    server.stop();
    circud::test::require(response.result() == http::status::ok);
    circud::test::require(response.body().find(R"("apiVersion":1)") !=
                          std::string::npos);
    circud::test::require(response.body().find(R"("uploadBytes":123456)") !=
                          std::string::npos);
    circud::test::require(response.body().find(R"("maxCallTimeoutMs":300000)") !=
                          std::string::npos);
    circud::test::require(response.body().find(R"("intervalMs":4321)") !=
                          std::string::npos);
    circud::test::require(response.body().find(R"("capacity":77)") !=
                          std::string::npos);
    circud::test::require(response.body().find(R"("persistent":false)") !=
                          std::string::npos);
    circud::test::require(response.body().find(temp.path.string()) ==
                          std::string::npos);
}

CIRCUD_TEST(http_rpc_lists_management_events_and_validates_filters) {
    TempDir temp;
    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    config.listen_port = available_port();
    config.public_dir = temp.path / "public";
    make_public_dir(config.public_dir);
    circud::Core core(config);
    ServerHarness server(core);
    const auto response = request(
        config.listen_port, http::verb::post, "/rpc",
        R"({"method":"events.list","params":{"afterId":0,"limit":10,"level":"info","category":"daemon"}})",
        "application/json");
    const auto invalid = request(
        config.listen_port, http::verb::post, "/rpc",
        R"({"method":"events.list","params":{"limit":201}})",
        "application/json");
    server.stop();
    circud::test::require(response.result() == http::status::ok,
                          response.body());
    circud::test::require(response.body().find(R"("category":"daemon")") !=
                          std::string::npos);
    circud::test::require(response.body().find(R"("code":"started")") !=
                          std::string::npos);
    circud::test::require(response.body().find(R"("nextAfterId":)") !=
                          std::string::npos);
    circud::test::require(response.body().find(R"("journalId":)") !=
                          std::string::npos);
    circud::test::require(invalid.result() == http::status::bad_request);
    circud::test::require(invalid.body().find("limit must be 1..200") !=
                          std::string::npos);
}

CIRCUD_TEST(http_rpc_uses_stable_not_found_errors) {
    TempDir temp;
    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    config.listen_port = available_port();
    config.public_dir = temp.path / "public";
    make_public_dir(config.public_dir);
    circud::Core core(config);
    ServerHarness server(core);
    const auto missing_package = request(
        config.listen_port, http::verb::post, "/rpc",
        R"({"method":"package.stop","params":{"id":"io.example.missing"}})",
        "application/json");
    const auto missing_method = request(
        config.listen_port, http::verb::post, "/rpc",
        R"({"method":"missing.method","params":{}})",
        "application/json");
    server.stop();
    circud::test::require(missing_package.result() == http::status::not_found);
    circud::test::require(missing_package.body().find(R"("code":"not_found")") !=
                          std::string::npos);
    circud::test::require(missing_method.result() == http::status::not_found);
    circud::test::require(missing_method.body().find(
                              R"("code":"method_not_found")") !=
                          std::string::npos);
}

CIRCUD_TEST(http_rpc_rejects_unknown_fields) {
    TempDir temp;
    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    config.listen_port = available_port();
    config.public_dir = temp.path / "public";
    make_public_dir(config.public_dir);
    circud::Core core(config);
    ServerHarness server(core);
    const auto envelope = request(
        config.listen_port, http::verb::post, "/rpc",
        R"({"method":"packages.list","param":{}})",
        "application/json");
    const auto params = request(
        config.listen_port, http::verb::post, "/rpc",
        R"({"method":"package.remove","params":{"id":"io.example.demo","purgeDate":true}})",
        "application/json");
    server.stop();
    circud::test::require(envelope.result() == http::status::bad_request);
    circud::test::require(envelope.body().find("unknown RPC field") !=
                          std::string::npos);
    circud::test::require(params.result() == http::status::bad_request);
    circud::test::require(params.body().find("params.purgeDate") !=
                          std::string::npos);
}

CIRCUD_TEST(http_wildcard_listener_stops_without_self_connection) {
    using namespace std::chrono_literals;
    TempDir temp;
    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    config.listen_address = "0.0.0.0";
    config.listen_port = available_port();
    config.public_dir = temp.path / "public";
    make_public_dir(config.public_dir);
    circud::Core core(config);
    ServerHarness server(core);
    auto stopped = std::async(std::launch::async, [&server] { server.stop(); });
    circud::test::require(stopped.wait_for(2s) == std::future_status::ready,
                          "wildcard listener did not stop promptly");
    stopped.get();
}

CIRCUD_TEST(http_listener_can_restart_after_generation_stop) {
    using namespace std::chrono_literals;
    TempDir temp;
    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    config.listen_address = "127.0.0.1";
    config.listen_port = available_port();
    config.public_dir = temp.path / "public";
    make_public_dir(config.public_dir);
    circud::Core core(config);
    ServerHarness server(core);

    circud::test::require(request_health(config.listen_port),
        "first listener generation was unavailable");
    server.stop();
    server.start();
    circud::test::require(request_health(config.listen_port),
        "replacement listener generation was unavailable");
    server.stop();
}

CIRCUD_TEST(http_upload_installs_zip_and_removes_temporary_file) {
    TempDir temp;
    const auto archive = temp.path / "upload.zip";
    create_package_zip(archive);
    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    config.listen_port = available_port();
    config.public_dir = temp.path / "public";
    make_public_dir(config.public_dir);
    std::filesystem::create_directories(config.state_dir / "uploads");
    std::ofstream(config.state_dir / "uploads/.upload-stale.zip") << "stale";
    circud::Core core(config);
    ServerHarness server(core);
    const auto response = request(config.listen_port, http::verb::post,
                                  "/api/packages/install", read_file(archive),
                                  "application/zip");
    server.stop();
    circud::test::require(response.result() == http::status::created,
                          response.body());
    circud::test::require(response.body().find("io.example.upload") !=
                          std::string::npos);
    const auto installed = core.packages();
    circud::test::require(installed.size() == 1);
    circud::test::require(installed.front().manifest.id ==
                          "io.example.upload");
    const auto uploads = config.state_dir / "uploads";
    circud::test::require(!std::filesystem::exists(uploads) ||
                          std::filesystem::is_empty(uploads));
}

CIRCUD_TEST(http_upload_streams_across_bounded_chunks) {
    TempDir temp;
    const auto archive = temp.path / "large-upload.zip";
    create_package_zip(archive, 4 * 64 * 1024 + 211);
    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    config.listen_port = available_port();
    config.public_dir = temp.path / "public";
    config.http_max_upload_bytes = 2 * 1024 * 1024;
    make_public_dir(config.public_dir);
    circud::Core core(config);
    ServerHarness server(core);
    const auto response = request(config.listen_port, http::verb::post,
        "/api/packages/install", read_file(archive), "application/zip");
    server.stop();
    circud::test::require(response.result() == http::status::created,
        response.body());
    const auto packages = core.packages();
    circud::test::require(packages.size() == 1 &&
            packages.front().manifest.id == "io.example.upload",
        "multi-chunk upload did not install the package");
    const auto uploads = config.state_dir / "uploads";
    circud::test::require(!std::filesystem::exists(uploads) ||
            std::filesystem::is_empty(uploads),
        "multi-chunk upload left a temporary file");
}

CIRCUD_TEST(http_upload_rejects_oversized_body_without_residue) {
    TempDir temp;
    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    config.listen_port = available_port();
    config.public_dir = temp.path / "public";
    config.http_max_upload_bytes = 1024;
    make_public_dir(config.public_dir);
    circud::Core core(config);
    ServerHarness server(core);
    const auto response = request(config.listen_port, http::verb::post,
                                  "/api/packages/install", std::string(2048, 'x'),
                                  "application/zip");
    server.stop();
    circud::test::require(response.result() ==
                          http::status::payload_too_large,
                          "unexpected upload response: " +
                              std::to_string(response.result_int()) + " " +
                              response.body());
    const auto uploads = config.state_dir / "uploads";
    circud::test::require(!std::filesystem::exists(uploads) ||
                          std::filesystem::is_empty(uploads),
                          "oversized upload temporary file was not removed");
}

CIRCUD_TEST(http_upload_requires_zip_content_type) {
    TempDir temp;
    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    config.listen_port = available_port();
    config.public_dir = temp.path / "public";
    make_public_dir(config.public_dir);
    circud::Core core(config);
    ServerHarness server(core);
    const auto response = request(config.listen_port, http::verb::post,
                                  "/api/packages/install", "not a zip",
                                  "application/octet-stream");
    server.stop();
    circud::test::require(response.result() ==
                          http::status::unsupported_media_type);
}

CIRCUD_TEST(http_stop_cleans_partial_chunked_upload) {
    using namespace std::chrono_literals;
    TempDir temp;
    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    config.listen_port = available_port();
    config.public_dir = temp.path / "public";
    make_public_dir(config.public_dir);
    circud::Core core(config);
    ServerHarness server(core);

    boost::asio::io_context io;
    tcp::socket socket(io);
    socket.connect(tcp::endpoint(
        boost::asio::ip::address_v4::loopback(), config.listen_port));
    const std::string header =
        "POST /api/packages/install HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Type: application/zip\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "10000\r\n";
    boost::asio::write(socket, boost::asio::buffer(header));
    const auto partial = patterned_bytes(64 * 1024);
    boost::asio::write(socket, boost::asio::buffer(partial));

    const auto uploads = config.state_dir / "uploads";
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while ((!std::filesystem::exists(uploads) ||
            std::filesystem::is_empty(uploads)) &&
        std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    server.stop();
    boost::system::error_code ignored;
    socket.close(ignored);

    const auto cleanup_deadline = std::chrono::steady_clock::now() + 2s;
    while (std::filesystem::exists(uploads) &&
        !std::filesystem::is_empty(uploads) &&
        std::chrono::steady_clock::now() < cleanup_deadline)
        std::this_thread::yield();
    circud::test::require(!std::filesystem::exists(uploads) ||
            std::filesystem::is_empty(uploads),
        "listener stop left a partial upload behind");
}

CIRCUD_TEST(http_rpc_call_round_trips_through_package_evbus) {
    TempDir temp;
    const auto archive = temp.path / "upload.zip";
    create_package_zip(archive);
    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    config.listen_port = available_port();
    config.public_dir = temp.path / "public";
    make_public_dir(config.public_dir);
    circud::Core core(config);
    ServerHarness server(core);
    const auto upload = request(config.listen_port, http::verb::post,
                                "/api/packages/install", read_file(archive),
                                "application/zip");
    const auto response = request(
        config.listen_port, http::verb::post, "/rpc",
        R"({"method":"call","params":{"package":"io.example.upload","target":"echo","arguments":{"value":42},"timeoutMs":1000}})",
        "application/json");
    server.stop();
    circud::test::require(upload.result() == http::status::created,
                          upload.body());
    circud::test::require(response.result() == http::status::ok,
                          response.body());
    circud::test::require(response.body().find(
                              R"("value":{"value":42})") !=
                          std::string::npos,
                          response.body());
}

CIRCUD_TEST(package_endpoints_support_http_sse_websocket_and_explicit_static) {
    using namespace std::chrono_literals;
    TempDir temp;
    const auto archive = temp.path / "endpoint.zip";
    create_endpoint_package_zip(archive);
    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    config.listen_address = "127.0.0.1";
    config.listen_port = available_port();
    config.public_dir = temp.path / "public";
    config.endpoints.idle_timeout = 2s;
    config.endpoints.max_lifetime = 5s;
    make_public_dir(config.public_dir);
    std::filesystem::create_directories(
        config.public_dir / "io.example.endpoint");
    std::ofstream(config.public_dir / "io.example.endpoint" / "method")
        << "must not bypass the package scope";
    circud::Core core(config);
    ServerHarness server(core);

    const auto upload = request(config.listen_port, http::verb::post,
                                "/api/packages/install", read_file(archive),
                                "application/zip");
    circud::test::require(upload.result() == http::status::created,
                          upload.body());

    const auto bare_root = request(config.listen_port, http::verb::get,
                                   "/io.example.endpoint");
    circud::test::require(bare_root.result() == http::status::ok,
                          bare_root.body());
    circud::test::require(bare_root.body() == "package scope root",
                          "bare package scope root was not routed to the package");
    const auto slash_root = request(config.listen_port, http::verb::get,
                                    "/io.example.endpoint/");
    circud::test::require(slash_root.result() == http::status::ok,
                          slash_root.body());
    circud::test::require(slash_root.body() == "package scope root",
                          "package scope slash root did not map to event path /");
    const auto adjacent = request(config.listen_port, http::verb::get,
                                  "/io.example.endpointx");
    circud::test::require(adjacent.result() == http::status::not_found,
                          "an adjacent package name matched the package scope");

    const auto encoded_scope = request(
        config.listen_port, http::verb::get,
        "/io.example.endpo%69nt/method");
    circud::test::require(encoded_scope.result() == http::status::ok,
                          encoded_scope.body());
    circud::test::require(encoded_scope.body() == "GET",
        "an encoded package scope bypassed package routing");
    const auto encoded_scope_query = request(
        config.listen_port, http::verb::get,
        "/io.example.endpo%69nt/http?value=%69&next=%3f");
    circud::test::require(
        encoded_scope_query.result() == http::status::created,
        encoded_scope_query.body());
    circud::test::require(
        encoded_scope_query.body().find(
            R"("query":"value=%69&next=%3f")") != std::string::npos,
        "canonical path routing changed the package query text");

    {
        boost::asio::io_context io;
        tcp::socket socket(io);
        socket.connect(tcp::endpoint(
            boost::asio::ip::address_v4::loopback(), config.listen_port));
        set_socket_io_timeout(socket, 2s);

        http::request<http::empty_body> first{
            http::verb::get, "/io.example.endpoint/method", 11 };
        first.set(http::field::host, "127.0.0.1");
        first.keep_alive(true);
        http::request<http::empty_body> second{
            http::verb::get, "/io.example.endpoint/http?pipeline=2", 11 };
        second.set(http::field::host, "127.0.0.1");
        second.keep_alive(true);
        http::request<http::empty_body> third{
            http::verb::get, "/health", 11 };
        third.set(http::field::host, "127.0.0.1");
        third.keep_alive(false);

        // Write all requests before reading any response so the endpoint
        // handoff must preserve bytes already buffered by Beast.
        http::write(socket, first);
        http::write(socket, second);
        http::write(socket, third);

        boost::beast::flat_buffer buffer;
        http::response<http::string_body> first_response;
        http::response<http::string_body> second_response;
        http::response<http::string_body> third_response;
        http::read(socket, buffer, first_response);
        http::read(socket, buffer, second_response);
        http::read(socket, buffer, third_response);

        circud::test::require(first_response.keep_alive(),
            "first package endpoint response did not keep the connection");
        circud::test::require(first_response.body() == "GET",
            "first pipelined package response was out of order");
        circud::test::require(second_response.keep_alive(),
            "second package endpoint response did not keep the connection");
        circud::test::require(second_response.result() == http::status::created,
            second_response.body());
        circud::test::require(
            second_response.body().find(R"("query":"pipeline=2")") !=
                std::string::npos,
            "second pipelined package response was out of order");
        circud::test::require(!third_response.keep_alive(),
            "final core response ignored Connection: close");
        circud::test::require(third_response.body() == R"({"ok":true})",
            "package-to-core handoff lost the final request");
    }

    {
        boost::asio::io_context io;
        tcp::socket socket(io);
        socket.connect(tcp::endpoint(
            boost::asio::ip::address_v4::loopback(), config.listen_port));
        set_socket_io_timeout(socket, 2s);

        http::request<http::empty_body> first{
            http::verb::get, "/io.example.endpoint/static", 11 };
        first.set(http::field::host, "127.0.0.1");
        first.keep_alive(true);
        http::request<http::empty_body> second{
            http::verb::get, "/health", 11 };
        second.set(http::field::host, "127.0.0.1");
        second.keep_alive(false);
        http::write(socket, first);
        http::write(socket, second);

        boost::beast::flat_buffer buffer;
        http::response<http::string_body> first_response;
        http::response<http::string_body> second_response;
        http::read(socket, buffer, first_response);
        http::read(socket, buffer, second_response);
        circud::test::require(first_response.keep_alive(),
            "explicit static response did not keep the connection");
        circud::test::require(
            first_response.body() ==
                "served only after explicit delegation\n",
            "explicit static response was corrupted");
        circud::test::require(
            second_response.body() == R"({"ok":true})",
            "static-to-core handoff lost the next request");
    }

    const auto require_closed_after_package_request =
        [&](unsigned version, bool keep_alive, std::string_view description) {
            boost::asio::io_context io;
            tcp::socket socket(io);
            socket.connect(tcp::endpoint(
                boost::asio::ip::address_v4::loopback(),
                config.listen_port));
            set_socket_io_timeout(socket, 2s);

            http::request<http::empty_body> endpoint{
                http::verb::get, "/io.example.endpoint/method", version };
            endpoint.set(http::field::host, "127.0.0.1");
            endpoint.keep_alive(keep_alive);
            http::request<http::empty_body> trailing{
                http::verb::get, "/health", version };
            trailing.set(http::field::host, "127.0.0.1");
            trailing.keep_alive(false);
            http::write(socket, endpoint);
            http::write(socket, trailing);

            boost::beast::flat_buffer buffer;
            http::response<http::string_body> response;
            http::read(socket, buffer, response);
            circud::test::require(!response.keep_alive(),
                std::string(description) + " response advertised keep-alive");
            circud::test::require(response.body() == "GET",
                std::string(description) + " response was invalid");
            circud::test::require(buffer.size() == 0,
                std::string(description) + " processed a pipelined request");
            std::array<char, 1> byte{};
            boost::system::error_code error;
            (void)socket.read_some(boost::asio::buffer(byte), error);
            circud::test::require(
                error == boost::asio::error::eof ||
                    error == boost::asio::error::connection_reset,
                std::string(description) + " connection remained open");
        };
    require_closed_after_package_request(10, true, "HTTP/1.0");
    require_closed_after_package_request(11, false, "Connection: close");

    const auto response = request(config.listen_port, http::verb::post,
        "/io.example.endpoint/http?value=42", "request body", "text/plain");
    circud::test::require(response.result() == http::status::created,
                          response.body());
    circud::test::require(response["x-endpoint-test"] == "yes");
    circud::test::require(response.body().find(R"("method":"POST")") !=
                          std::string::npos, response.body());
    circud::test::require(response.body().find(R"("path":"/http")") !=
                          std::string::npos, response.body());
    circud::test::require(response.body().find(R"("query":"value=42")") !=
                          std::string::npos, response.body());

    const auto async_response = request_method(config.listen_port, "PUT",
        "/io.example.endpoint/async", "updated", "text/plain");
    circud::test::require(async_response.result() == http::status::accepted,
                          async_response.body());
    circud::test::require(async_response["x-async-endpoint"] == "yes");
    circud::test::require(async_response.body().find(R"("method":"PUT")") !=
                          std::string::npos, async_response.body());
    circud::test::require(async_response.body().find(R"("bodyLength":7)") !=
                          std::string::npos, async_response.body());

    for (const std::string_view method : { "PROPFIND", "CUSTOM-METHOD" }) {
        const auto method_response = request_method(config.listen_port, method,
            "/io.example.endpoint/method");
        circud::test::require(method_response.result() == http::status::ok,
                              method_response.body());
        circud::test::require(method_response.body() == method,
            "package endpoint did not receive the original HTTP method");
    }

    const auto delegated = request(config.listen_port, http::verb::get,
                                    "/io.example.endpoint/static");
    circud::test::require(delegated.result() == http::status::ok,
                          delegated.body());
    circud::test::require(
        delegated.body() == "served only after explicit delegation\n");
    const auto large_static = request(config.listen_port, http::verb::get,
        "/io.example.endpoint/static-large");
    circud::test::require(large_static.result() == http::status::ok,
        large_static.body());
    circud::test::require(
        large_static.body() == patterned_bytes(3 * 64 * 1024 + 91),
        "multi-chunk package static response was corrupted");
    const auto hidden = request(config.listen_port, http::verb::get,
                                "/io.example.endpoint/assets/hidden.txt");
    circud::test::require(hidden.result() != http::status::ok,
                          "package file was exposed without serveStatic()");
    circud::test::require(hidden.body().find("must never be exposed") ==
                          std::string::npos,
                          "unserved package file contents leaked");

    for (const unsigned status : { 204U, 205U, 304U }) {
        const auto empty = request(config.listen_port, http::verb::get,
            "/io.example.endpoint/empty-status-" + std::to_string(status));
        circud::test::require(empty.result_int() == status,
            "empty response with a bodyless status was rejected");
        circud::test::require(empty.body().empty(),
            "bodyless response unexpectedly carried content");
        circud::test::require(
            empty.find(http::field::transfer_encoding) == empty.end(),
            "bodyless response used Transfer-Encoding");
        circud::test::require(
            status == 205
                ? empty[http::field::content_length] == "0"
                : empty.find(http::field::content_length) == empty.end(),
            status == 205
                ? "205 response did not declare its zero-length body"
                : "bodyless response used Content-Length");

        const auto invalid = request(config.listen_port, http::verb::get,
            "/io.example.endpoint/body-status-" + std::to_string(status));
        circud::test::require(
            invalid.result() == http::status::internal_server_error,
            "package response supplied a body for a bodyless status");
    }

    {
        boost::asio::io_context io;
        tcp::socket socket(io);
        socket.connect(tcp::endpoint(
            boost::asio::ip::address_v4::loopback(), config.listen_port));
        set_socket_io_timeout(socket, 2s);

        http::request<http::empty_body> reset{
            http::verb::get, "/io.example.endpoint/empty-status-205", 11 };
        reset.set(http::field::host, "127.0.0.1");
        reset.keep_alive(true);
        http::request<http::empty_body> health{
            http::verb::get, "/health", 11 };
        health.set(http::field::host, "127.0.0.1");
        health.keep_alive(false);
        http::write(socket, reset);
        http::write(socket, health);

        boost::beast::flat_buffer buffer;
        http::response<http::string_body> reset_response;
        http::response<http::string_body> health_response;
        http::read(socket, buffer, reset_response);
        http::read(socket, buffer, health_response);
        circud::test::require(reset_response.result_int() == 205);
        circud::test::require(
            reset_response[http::field::content_length] == "0",
            "persistent 205 response did not delimit its empty body");
        circud::test::require(health_response.result() == http::status::ok &&
                health_response.body() == R"({"ok":true})",
            "persistent 205 response corrupted the following response");
    }

    const auto post_sse = request(config.listen_port, http::verb::post,
        "/io.example.endpoint/sse-method");
    circud::test::require(
        post_sse.result() == http::status::method_not_allowed,
        "acceptSse() did not synchronously reject POST");
    circud::test::require(post_sse.body().find("SSE requires a GET request") !=
                          std::string::npos, post_sse.body());
    const auto head_sse = request(config.listen_port, http::verb::head,
        "/io.example.endpoint/sse-method");
    circud::test::require(
        head_sse.result() == http::status::method_not_allowed,
        "acceptSse() did not synchronously reject HEAD");

    const auto invalid_sse_status = request(config.listen_port, http::verb::get,
        "/io.example.endpoint/sse-status");
    circud::test::require(
        invalid_sse_status.result() == http::status::bad_request,
        "acceptSse() accepted a no-content response status");
    circud::test::require(
        invalid_sse_status.body().find("between 200 and 200") !=
            std::string::npos,
        invalid_sse_status.body());

    auto sse = std::async(std::launch::async, [port = config.listen_port] {
        boost::asio::io_context io;
        tcp::socket socket(io);
        socket.connect(tcp::endpoint(
            boost::asio::ip::address_v4::loopback(), port));
        set_socket_io_timeout(socket, std::chrono::seconds(2));
        const std::string request_text =
            "GET /io.example.endpoint/sse HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\nAccept: text/event-stream\r\n"
            "Connection: close\r\n\r\n";
        boost::asio::write(socket, boost::asio::buffer(request_text));
        std::string wire;
        std::array<char, 512> chunk{};
        while (wire.find("data: first\n\n") == std::string::npos) {
            boost::system::error_code error;
            const auto bytes = socket.read_some(
                boost::asio::buffer(chunk), error);
            if (error)
                throw std::runtime_error(
                    "SSE closed before its first event: " + error.message());
            wire.append(chunk.data(), bytes);
        }
        const bool incremental =
            wire.find("data: second\n\n") == std::string::npos;
        for (;;) {
            boost::system::error_code error;
            const auto bytes = socket.read_some(
                boost::asio::buffer(chunk), error);
            wire.append(chunk.data(), bytes);
            if (error == boost::asio::error::eof) break;
            if (error)
                throw std::runtime_error("cannot read SSE response: " +
                                         error.message());
        }
        return std::pair{ std::move(wire), incremental };
    });
    if (sse.wait_for(3s) != std::future_status::ready) {
        server.stop();
        circud::test::require(false, "SSE endpoint did not finish promptly");
    }
    auto [sse_wire, sse_incremental] = sse.get();
    circud::test::require(sse_incremental,
                          "SSE events were not incrementally visible");
    circud::test::require(sse_wire.find("HTTP/1.1 200") != std::string::npos);
    circud::test::require(sse_wire.find("text/event-stream") !=
                          std::string::npos);
    circud::test::require(sse_wire.find("event: ready\ndata: first\n\n") !=
                          std::string::npos);
    circud::test::require(sse_wire.find("data: second\n\n") !=
                          std::string::npos);

    auto websocket_echo = std::async(std::launch::async,
        [port = config.listen_port] {
            boost::asio::io_context io;
            websocket::stream<tcp::socket> socket(io);
            socket.next_layer().connect(tcp::endpoint(
                boost::asio::ip::address_v4::loopback(), port));
            set_socket_io_timeout(socket.next_layer(),
                                  std::chrono::seconds(2));
            socket.set_option(websocket::stream_base::decorator(
                [](websocket::request_type& upgrade) {
                    upgrade.set(http::field::sec_websocket_protocol,
                                "echo.v1");
                }));
            websocket::response_type upgrade_response;
            socket.handshake(upgrade_response, "127.0.0.1",
                             "/io.example.endpoint/ws");
            const std::string message = "hello endpoint";
            socket.text(true);
            socket.write(boost::asio::buffer(message));
            boost::beast::flat_buffer echoed;
            socket.read(echoed);
            const bool text = socket.got_text();
            const auto body = boost::beast::buffers_to_string(echoed.data());
            const auto protocol = std::string(
                upgrade_response[http::field::sec_websocket_protocol]);
            socket.close(websocket::close_code::normal);
            return std::tuple{ protocol, text, body };
        });
    if (websocket_echo.wait_for(3s) != std::future_status::ready) {
        server.stop();
        circud::test::require(false,
                              "WebSocket endpoint did not finish promptly");
    }
    const auto [protocol, echoed_text, echoed_body] = websocket_echo.get();
    circud::test::require(protocol == "echo.v1",
                          "package WebSocket subprotocol was not negotiated");
    circud::test::require(echoed_text,
                          "WebSocket text echo became a binary frame");
    circud::test::require(echoed_body == "hello endpoint",
                          "WebSocket echo body changed");

    {
        boost::asio::io_context io;
        tcp::socket socket(io);
        socket.connect(tcp::endpoint(
            boost::asio::ip::address_v4::loopback(), config.listen_port));
        set_socket_io_timeout(socket, 2s);

        constexpr std::string_view payload = "buffered first frame";
        constexpr std::array<unsigned char, 4> mask{
            0x12, 0x34, 0x56, 0x78 };
        std::string wire =
            "GET /io.example.endpoint/ws HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "Sec-WebSocket-Protocol: echo.v1\r\n"
            "\r\n";
        wire.push_back(static_cast<char>(0x81));
        wire.push_back(static_cast<char>(0x80 | payload.size()));
        for (const auto value : mask)
            wire.push_back(static_cast<char>(value));
        for (std::size_t index = 0; index < payload.size(); ++index) {
            wire.push_back(static_cast<char>(
                static_cast<unsigned char>(payload[index]) ^
                mask[index % mask.size()]));
        }

        // One write deliberately makes Beast read the upgrade and the first
        // masked client frame into the same connection buffer.
        boost::asio::write(socket, boost::asio::buffer(wire));
        boost::beast::flat_buffer response_buffer;
        http::response_parser<http::empty_body> response_parser;
        http::read_header(socket, response_buffer, response_parser);
        const auto response = response_parser.release();
        circud::test::require(
            response.result() == http::status::switching_protocols,
            "buffered WebSocket upgrade was rejected");
        circud::test::require(
            response[http::field::sec_websocket_protocol] == "echo.v1",
            "buffered WebSocket upgrade lost its subprotocol");

        std::string frame = boost::beast::buffers_to_string(
            response_buffer.data());
        std::array<char, 256> chunk{};
        const auto read_until = [&](std::size_t bytes) {
            while (frame.size() < bytes) {
                const auto count = socket.read_some(boost::asio::buffer(chunk));
                frame.append(chunk.data(), count);
            }
        };
        read_until(2);
        const auto first = static_cast<unsigned char>(frame[0]);
        const auto second = static_cast<unsigned char>(frame[1]);
        circud::test::require(first == 0x81,
            "the buffered client text frame was not echoed as one text frame");
        circud::test::require((second & 0x80) == 0,
            "the server incorrectly masked its WebSocket frame");
        circud::test::require((second & 0x7f) == payload.size(),
            "the buffered WebSocket echo changed length");
        read_until(2 + payload.size());
        circud::test::require(
            std::string_view(frame).substr(2, payload.size()) == payload,
            "the buffered first WebSocket frame changed contents");
    }
    server.stop();
}

CIRCUD_TEST(package_endpoint_gateway_errors_preserve_head_semantics) {
    using namespace std::chrono_literals;
    TempDir temp;
    const auto archive = temp.path / "endpoint.zip";
    create_endpoint_package_zip(archive);
    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    config.listen_address = "127.0.0.1";
    config.listen_port = available_port();
    config.public_dir = temp.path / "public";
    config.endpoints.enabled = false;
    make_public_dir(config.public_dir);
    circud::Core core(config);
    ServerHarness server(core);

    const auto upload = request(config.listen_port, http::verb::post,
                                "/api/packages/install", read_file(archive),
                                "application/zip");
    circud::test::require(upload.result() == http::status::created,
                          upload.body());

    boost::asio::io_context io;
    tcp::socket socket(io);
    socket.connect(tcp::endpoint(
        boost::asio::ip::address_v4::loopback(), config.listen_port));
    set_socket_io_timeout(socket, 2s);
    http::request<http::empty_body> head{
        http::verb::head, "/io.example.endpoint/method", 11 };
    head.set(http::field::host, "127.0.0.1");
    head.keep_alive(true);
    http::write(socket, head);
    boost::beast::flat_buffer buffer;
    http::response_parser<http::string_body> parser;
    parser.skip(true);
    http::read(socket, buffer, parser);
    const auto head_response = parser.release();
    circud::test::require(
        head_response.result() == http::status::service_unavailable,
        "disabled package endpoint HEAD did not return 503");
    circud::test::require(head_response.body().empty(),
        "package gateway emitted a body for HEAD");
    circud::test::require(
        head_response[http::field::content_length] == "30",
        "package gateway HEAD did not report the GET representation length");
    circud::test::require(!head_response.keep_alive(),
        "package gateway error unexpectedly kept the connection alive");
    std::array<char, 1> trailing{};
    boost::system::error_code close_error;
    socket.read_some(boost::asio::buffer(trailing), close_error);
    circud::test::require(close_error == boost::asio::error::eof,
        "package gateway did not close after its terminal HEAD error");

    const auto get_response = request(
        config.listen_port, http::verb::get,
        "/io.example.endpoint/method");
    server.stop();
    circud::test::require(
        get_response.result() == http::status::service_unavailable,
        get_response.body());
    circud::test::require(
        get_response.body() == "package endpoints are disabled",
        "GET gateway error body diverged from the HEAD metadata");
}

CIRCUD_TEST(http_auth_protects_management_and_marks_package_requests) {
    TempDir temp;
    const auto archive = temp.path / "endpoint.zip";
    create_endpoint_package_zip(archive);
    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    config.listen_address = "127.0.0.1";
    config.listen_port = available_port();
    config.public_dir = temp.path / "public";
    config.auth.enabled = true;
    config.auth.username = "admin";
    circud::ScryptParameters scrypt;
    scrypt.log_n = 14;
    scrypt.max_memory_bytes = 32ULL * 1024 * 1024;
    config.auth.password_hash = circud::AuthManager::hash_password(
        "correct horse battery staple", scrypt);
    make_public_dir(config.public_dir);
    circud::Core core(config);
    const auto installed = core.install(archive);
    ServerHarness server(core);

    const auto index = request(config.listen_port, http::verb::get, "/");
    const auto css = request(config.listen_port, http::verb::get, "/app.css");
    circud::test::require(index.result() == http::status::ok,
                          "Web index should remain public");
    circud::test::require(css.result() == http::status::ok,
                          "Web assets should remain public");

    const auto anonymous_session = request(
        config.listen_port, http::verb::get, "/api/auth/session");
    circud::test::require(anonymous_session.result() == http::status::ok,
                          anonymous_session.body());
    circud::test::require(
        anonymous_session.body().find(R"("ok":true)") != std::string::npos &&
        anonymous_session.body().find(R"("authEnabled":true)") !=
            std::string::npos &&
        anonymous_session.body().find(R"("authenticated":false)") !=
            std::string::npos,
        "anonymous session did not use the authentication envelope: " +
            anonymous_session.body());

    const auto anonymous_rpc = request(
        config.listen_port, http::verb::post, "/rpc",
        R"({"method":"packages.list","params":{}})", "application/json");
    const auto anonymous_rpc_wrong_media = request(
        config.listen_port, http::verb::post, "/rpc", "not json",
        "text/plain");
    const auto anonymous_install = request(
        config.listen_port, http::verb::post, "/api/packages/install",
        read_file(archive), "application/zip");
    circud::test::require(anonymous_rpc.result() ==
                          http::status::unauthorized, anonymous_rpc.body());
    circud::test::require(anonymous_rpc_wrong_media.result() ==
                          http::status::unauthorized,
                          anonymous_rpc_wrong_media.body());
    circud::test::require(anonymous_install.result() ==
                          http::status::unauthorized,
                          anonymous_install.body());

    boost::asio::io_context expect_io;
    tcp::socket expect_socket(expect_io);
    expect_socket.connect(tcp::endpoint(
        boost::asio::ip::address_v4::loopback(), config.listen_port));
    http::request<http::empty_body> expect_upload{
        http::verb::post, "/api/packages/install", 11 };
    expect_upload.set(http::field::host, "127.0.0.1");
    expect_upload.set(http::field::content_type, "application/zip");
    expect_upload.set(http::field::expect, "100-continue");
    expect_upload.content_length(4096);
    expect_upload.keep_alive(false);
    http::request_serializer<http::empty_body> expect_serializer(expect_upload);
    http::write_header(expect_socket, expect_serializer);
    boost::beast::flat_buffer expect_buffer;
    http::response<http::string_body> expect_response;
    http::read(expect_socket, expect_buffer, expect_response);
    circud::test::require(expect_response.result() ==
                          http::status::unauthorized,
                          "unauthorized upload received an interim response: " +
                              std::to_string(expect_response.result_int()));
    const auto upload_directory = config.state_dir / "uploads";
    circud::test::require(
        !std::filesystem::exists(upload_directory) ||
            std::filesystem::is_empty(upload_directory),
        "unauthorized Expect upload created a temporary file");

    const auto anonymous_endpoint = request(
        config.listen_port, http::verb::get,
        "/io.example.endpoint/authorization");
    circud::test::require(anonymous_endpoint.result() == http::status::ok,
                          anonymous_endpoint.body());
    circud::test::require(
        anonymous_endpoint.body().find(R"("isAuthorized":false)") !=
            std::string::npos,
        "anonymous package endpoint request was marked authorized: " +
            anonymous_endpoint.body());

    const auto login = request(
        config.listen_port, http::verb::post, "/api/auth/login",
        R"({"username":"admin","password":"correct horse battery staple"})",
        "application/json");
    circud::test::require(login.result() == http::status::ok, login.body());
    const auto cookie = response_cookie(login);
    const auto csrf = json_string_field(login.body(), "csrfToken");
    circud::test::require(!cookie.empty(),
                          "login did not return a session cookie");
    circud::test::require(!csrf.empty(), "login did not return a CSRF token");
    circud::test::require(
        login.body().find(R"("ok":true)") != std::string::npos &&
        login.body().find(R"("authEnabled":true)") != std::string::npos &&
        login.body().find(R"("authenticated":true)") != std::string::npos &&
        login.body().find(R"("username":"admin")") != std::string::npos &&
        login.body().find(R"("expiresAt":)") != std::string::npos,
        "login response did not match the authentication envelope: " +
            login.body());

    const auto authenticated_session = request_with_headers(
        config.listen_port, http::verb::get, "/api/auth/session", {}, {},
        { { "Cookie", cookie } });
    circud::test::require(authenticated_session.result() == http::status::ok,
                          authenticated_session.body());
    circud::test::require(
        authenticated_session.body().find(R"("ok":true)") !=
            std::string::npos &&
        authenticated_session.body().find(R"("authenticated":true)") !=
            std::string::npos &&
        authenticated_session.body().find(R"("username":"admin")") !=
            std::string::npos,
        "authenticated session did not use the expected envelope: " +
            authenticated_session.body());

    const auto cookie_only = request_with_headers(
        config.listen_port, http::verb::post, "/rpc",
        R"({"method":"packages.list","params":{}})", "application/json",
        { { "Cookie", cookie } });
    circud::test::require(cookie_only.result() == http::status::forbidden,
                          cookie_only.body());

    const std::vector<std::pair<std::string, std::string>> authenticated{
        { "Cookie", cookie }, { "X-Circud-CSRF", csrf } };
    const auto authorized_rpc = request_with_headers(
        config.listen_port, http::verb::post, "/rpc",
        R"({"method":"packages.list","params":{}})", "application/json",
        authenticated);
    circud::test::require(authorized_rpc.result() == http::status::ok,
                          authorized_rpc.body());
    circud::test::require(
        authorized_rpc.body().find(installed.manifest.id) != std::string::npos,
        authorized_rpc.body());

    const auto wrong_csrf_rpc = request_with_headers(
        config.listen_port, http::verb::post, "/rpc",
        R"({"method":"packages.list","params":{}})", "application/json",
        { { "Cookie", cookie }, { "X-Circud-CSRF", std::string(43, 'A') } });
    circud::test::require(wrong_csrf_rpc.result() == http::status::forbidden,
                          wrong_csrf_rpc.body());
    const auto unknown_cookie_rpc = request_with_headers(
        config.listen_port, http::verb::post, "/rpc",
        R"({"method":"packages.list","params":{}})", "application/json",
        { { "Cookie", "circud_session=" + std::string(43, 'A') },
          { "X-Circud-CSRF", std::string(43, 'A') } });
    circud::test::require(unknown_cookie_rpc.result() ==
                          http::status::unauthorized,
                          unknown_cookie_rpc.body());

    const auto authorized_endpoint = request_with_headers(
        config.listen_port, http::verb::get,
        "/io.example.endpoint/authorization", {}, {},
        { { "Cookie", cookie } });
    circud::test::require(authorized_endpoint.result() == http::status::ok,
                          authorized_endpoint.body());
    circud::test::require(
        authorized_endpoint.body().find(R"("isAuthorized":true)") !=
            std::string::npos,
        "authenticated package endpoint request was marked anonymous: " +
            authorized_endpoint.body());

    const auto stripped_endpoint = request_with_headers(
        config.listen_port, http::verb::get,
        "/io.example.endpoint/forwarded-headers", {}, {},
        { { "Cookie", cookie + "; theme=dark" },
          { "X-Circud-CSRF", csrf } });
    circud::test::require(stripped_endpoint.result() == http::status::ok,
                          stripped_endpoint.body());
    circud::test::require(
        stripped_endpoint.body().find(R"("cookies":["theme=dark"])") !=
            std::string::npos &&
        stripped_endpoint.body().find(R"("hasCsrf":false)") !=
            std::string::npos &&
        stripped_endpoint.body().find("circud_session") == std::string::npos &&
        stripped_endpoint.body().find(csrf) == std::string::npos,
        "core authentication headers leaked to package endpoint: " +
            stripped_endpoint.body());

    const auto logout = request_with_headers(
        config.listen_port, http::verb::post, "/api/auth/logout", {}, {},
        authenticated);
    circud::test::require(logout.result() == http::status::no_content,
                          logout.body());
    const auto revoked = request_with_headers(
        config.listen_port, http::verb::post, "/rpc",
        R"({"method":"packages.list","params":{}})", "application/json",
        authenticated);
    circud::test::require(revoked.result() == http::status::unauthorized,
                          "logout did not revoke the session: " +
                              revoked.body());
    const auto revoked_session = request_with_headers(
        config.listen_port, http::verb::get, "/api/auth/session", {}, {},
        { { "Cookie", cookie } });
    circud::test::require(revoked_session.result() == http::status::ok,
                          revoked_session.body());
    circud::test::require(
        revoked_session.body().find(R"("authenticated":false)") !=
            std::string::npos,
        "revoked session still appeared authenticated: " +
            revoked_session.body());
    server.stop();
}
