#include "test.hpp"

#include <circud/core.hpp>
#include <circud/endpoint_gateway.hpp>
#include <circud/package_runtime.hpp>

#include <boost/asio/post.hpp>
#include <boost/json/serialize.hpp>

#include <minizip/zip.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <thread>

namespace {

class TempDir {
public:
    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("circud-runtime-test-" + std::to_string(
                                             std::chrono::steady_clock::now()
                                                 .time_since_epoch()
                                                 .count()));
        std::filesystem::create_directories(path);
    }
    ~TempDir() { std::filesystem::remove_all(path); }
    std::filesystem::path path;
};

class CoreLoop {
public:
    explicit CoreLoop(circud::Core& core) : core_(core) {}
    ~CoreLoop() { stop(); }

    void start() {
        core_.start();
        thread_ = std::thread([this] { core_.io().run(); });
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(3);
        for (const auto& package : core_.packages()) {
            if (!package.enabled) continue;
            while (core_.runtime_state(package.manifest.id) !=
                circud::RuntimeState::running) {
                if (std::chrono::steady_clock::now() >= deadline)
                    throw std::runtime_error(
                        "package runtime did not become ready: " +
                        package.manifest.id);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    void stop() {
        core_.stop();
        if (thread_.joinable()) thread_.join();
    }

private:
    circud::Core& core_;
    std::thread thread_;
};

void add_zip_file(zipFile archive, const char* name, std::string_view contents) {
    zip_fileinfo info{};
    if (zipOpenNewFileInZip(archive, name, &info, nullptr, 0, nullptr, 0,
                            nullptr, Z_DEFLATED, Z_DEFAULT_COMPRESSION) !=
            ZIP_OK ||
        zipWriteInFileInZip(archive, contents.data(),
                            static_cast<unsigned>(contents.size())) != ZIP_OK ||
        zipCloseFileInZip(archive) != ZIP_OK)
        throw std::runtime_error("cannot create runtime test package");
}

void make_package(const std::filesystem::path& archive_path,
                  std::string_view outside_path) {
    zipFile archive = zipOpen64(archive_path.c_str(), APPEND_STATUS_CREATE);
    if (!archive) throw std::runtime_error("cannot create runtime test ZIP");
    try {
        add_zip_file(archive, "meta.json", R"({
          "schema":1,"id":"io.example.runtime","name":"runtime test",
          "version":"1.0.0","entry":"management.js"
        })");
        add_zip_file(archive, "helper.js",
                     "export const expected = 'persisted';\n");
        std::string entry = R"JS(
import { expected } from './helper.js';
import { info } from 'circud';
const evbus = import.meta.use('evbus');
const fs = import.meta.use('fs');
const runtimeInfo = info();
const bytes = value => new Uint8Array([...value].map(character =>
  character.charCodeAt(0)));
const text = value => String.fromCharCode(...new Uint8Array(value));

evbus.on('circud.host', 'call', event => {
  if (event.target !== 'probe') return;
  const path = runtimeInfo.dataDir + '/state.txt';
  fs.writeFile(path, bytes(expected));
  const value = text(fs.readFile(path));
  let outsideDenied = false;
  try {
    fs.writeFile()JS";
        entry += "'";
        entry += outside_path;
        entry += R"JS(', bytes('escaped'));
  } catch (error) {
    outsideDenied = error instanceof TypeError &&
                    String(error.message).includes('capability denied');
  }
  event.respondWith({ value, outsideDenied });
});
)JS";
        add_zip_file(archive, "management.js", entry);
    } catch (...) {
        zipClose(archive, nullptr);
        throw;
    }
    if (zipClose(archive, nullptr) != ZIP_OK)
        throw std::runtime_error("cannot close runtime test ZIP");
}

void make_wait_package(const std::filesystem::path& archive_path) {
    zipFile archive = zipOpen64(archive_path.c_str(), APPEND_STATUS_CREATE);
    if (!archive) throw std::runtime_error("cannot create wait test ZIP");
    add_zip_file(archive, "meta.json", R"({
      "schema":1,"id":"io.example.wait","name":"wait test",
      "version":"1.0.0","entry":"management.js","shutdownTimeoutMs":100
    })");
    add_zip_file(archive, "management.js", R"JS(
import { info } from 'circud';
const evbus = import.meta.use('evbus');
const fs = import.meta.use('fs');
evbus.on('circud.host', 'call', event => {
  if (event.target === 'wait') {
    event.signal.addEventListener('abort', () => {
      fs.writeFile(info().dataDir + '/call-aborted', new Uint8Array([1]));
    });
    event.respondWith(new Promise(() => {}));
  }
  if (event.target === 'status.get')
    event.respondWith(new Promise(() => {}));
});
)JS");
    if (zipClose(archive, nullptr) != ZIP_OK)
        throw std::runtime_error("cannot close wait test ZIP");
}

void make_runtime_revoke_package(
    const std::filesystem::path& archive_path) {
    zipFile archive = zipOpen64(archive_path.c_str(), APPEND_STATUS_CREATE);
    if (!archive)
        throw std::runtime_error("cannot create runtime revoke test ZIP");
    add_zip_file(archive, "meta.json", R"({
      "schema":1,"id":"io.example.runtime-revoke",
      "name":"runtime revoke test","version":"1.0.0",
      "entry":"management.js","shutdownTimeoutMs":500
    })");
    add_zip_file(archive, "management.js", R"JS(
import { info } from 'circud';
const evbus = import.meta.use('evbus');
const fs = import.meta.use('fs');
evbus.on('circud.host', 'call', event => {
  if (event.target === 'wait') {
    fs.writeFile(info().dataDir + '/call-bound', new Uint8Array([1]));
    event.respondWith(new Promise(() => {}));
  }
  if (event.target === 'lifecycle.stop')
    event.respondWith(new Promise(() => {}));
});
)JS");
    if (zipClose(archive, nullptr) != ZIP_OK)
        throw std::runtime_error(
            "cannot close runtime revoke test ZIP");
}

void make_policy_package(const std::filesystem::path& archive_path,
                         std::string_view id, bool accepts_calls,
                         std::string_view entry) {
    zipFile archive = zipOpen64(archive_path.c_str(), APPEND_STATUS_CREATE);
    if (!archive) throw std::runtime_error("cannot create policy test ZIP");
    const std::string manifest =
        "{\"schema\":1,\"id\":\"" + std::string(id) +
        "\",\"name\":\"policy test\",\"version\":\"1.0.0\","
        "\"entry\":\"management.js\",\"acceptPackageCalls\":" +
        (accepts_calls ? "true" : "false") + "}";
    add_zip_file(archive, "meta.json", manifest);
    add_zip_file(archive, "management.js", entry);
    if (zipClose(archive, nullptr) != ZIP_OK)
        throw std::runtime_error("cannot close policy test ZIP");
}

void make_abort_signal_package(const std::filesystem::path& archive_path) {
    make_policy_package(archive_path, "io.example.abort-signal", true, R"JS(
import { AbortSignal } from 'circud';
const evbus = import.meta.use('evbus');

const observed = {
  constructorRejected: false,
  legacyUseHidden: false,
  instance: false,
  initialAborted: true,
  initialReasonUndefined: false,
  onabortCount: 0,
  listenerCount: 0,
  objectListenerCount: 0,
  listenerAfterThrowCount: 0,
  removedCount: 0,
  listenerThis: false,
  eventShape: false,
  throwSameReason: false,
  reasonName: '',
  onabortRetained: false,
  normalSignalUnaffected: false,
  cancelled: false,
};
let cancelledSignal;
let normalSignal;

try {
  new AbortSignal();
} catch (error) {
  observed.constructorRejected = error instanceof TypeError;
}
observed.legacyUseHidden = import.meta.use('circud') === null &&
  import.meta.use('circud:host') === null;

evbus.on('circud.host', 'call', event => {
  if (event.target === 'wait') {
    const signal = event.signal;
    cancelledSignal = signal;
    observed.instance = signal instanceof AbortSignal;
    observed.initialAborted = signal.aborted;
    observed.initialReasonUndefined = signal.reason === undefined;

    const removed = () => { ++observed.removedCount; };
    signal.addEventListener('abort', removed);
    signal.removeEventListener('abort', removed);

    function listener(abortEvent) {
      ++observed.listenerCount;
      observed.listenerThis = this === signal;
      observed.eventShape = abortEvent.type === 'abort' &&
        abortEvent.target === signal && abortEvent.currentTarget === signal;
      observed.reasonName = String(signal.reason?.name ?? '');
      try {
        signal.throwIfAborted();
      } catch (error) {
        observed.throwSameReason = error === signal.reason;
      }
      observed.cancelled = true;
    }
    signal.addEventListener('abort', listener);
    signal.addEventListener('abort', listener);
    signal.addEventListener('abort', () => {
      throw new Error('listener failure must be isolated');
    });
    signal.addEventListener('abort', {
      handleEvent(abortEvent) {
        if (abortEvent.target === signal) ++observed.objectListenerCount;
      },
    });
    signal.addEventListener('abort', () => {
      ++observed.listenerAfterThrowCount;
    });
    signal.onabort = () => { ++observed.onabortCount; };
    event.respondWith(new Promise(() => {}));
    return;
  }
  if (event.target === 'normal') {
    normalSignal = event.signal;
    event.respondWith(true);
    return;
  }
  if (event.target === 'probe') {
    observed.onabortRetained =
      typeof cancelledSignal?.onabort === 'function';
    observed.normalSignalUnaffected = normalSignal?.aborted === false &&
      normalSignal?.reason === undefined;
    event.respondWith(observed);
  }
});
)JS");
}

void make_reload_package(const std::filesystem::path& archive_path) {
    zipFile archive = zipOpen64(archive_path.c_str(), APPEND_STATUS_CREATE);
    if (!archive) throw std::runtime_error("cannot create reload test ZIP");
    add_zip_file(archive, "meta.json", R"({
      "schema":1,"id":"io.example.reload","name":"reload test",
      "version":"1.0.0","entry":"management.js","shutdownTimeoutMs":100
    })");
    add_zip_file(archive, "management.js", R"JS(
import { info, reload } from 'circud';
const evbus = import.meta.use('evbus');
const fs = import.meta.use('fs');
const path = info().dataDir + '/generation.txt';
let generation = 1;
if (fs.exists(path)) {
  const value = new Uint8Array(fs.readFile(path));
  generation = Number(String.fromCharCode(...value)) + 1;
}
fs.writeFile(path, new Uint8Array([...String(generation)].map(c =>
  c.charCodeAt(0))));
evbus.on('circud.host', 'call', event => {
  if (event.target === 'generation') event.respondWith(generation);
  if (event.target === 'reload') {
    event.respondWith(true);
    reload();
  }
});
)JS");
    if (zipClose(archive, nullptr) != ZIP_OK)
        throw std::runtime_error("cannot close reload test ZIP");
}

void make_upgrade_package(const std::filesystem::path& archive_path,
                          std::string_view response) {
    zipFile archive = zipOpen64(archive_path.c_str(), APPEND_STATUS_CREATE);
    if (!archive) throw std::runtime_error("cannot create upgrade test ZIP");
    add_zip_file(archive, "meta.json", R"({
      "schema":1,"id":"io.example.upgrade","name":"upgrade test",
      "version":"1.0.0","entry":"management.js","shutdownTimeoutMs":100
    })");
    if (!response.empty()) {
        std::string entry = R"JS(
const evbus = import.meta.use('evbus');
evbus.on('circud.host', 'call', event => {
  if (event.target === 'probe') event.respondWith(')JS";
        entry += response;
        entry += R"JS(');
});
)JS";
        add_zip_file(archive, "management.js", entry);
    }
    else {
        add_zip_file(archive, "management.js", "this is not valid JavaScript {");
    }
    if (zipClose(archive, nullptr) != ZIP_OK)
        throw std::runtime_error("cannot close upgrade test ZIP");
}

void make_scoped_upgrade_package(const std::filesystem::path& archive_path,
                                 std::string_view id,
                                 std::string_view prefix,
                                 std::string_view response) {
    zipFile archive = zipOpen64(archive_path.c_str(), APPEND_STATUS_CREATE);
    if (!archive)
        throw std::runtime_error("cannot create scoped upgrade test ZIP");
    try {
        std::string manifest = R"({"schema":1,"id":")";
        manifest += id;
        manifest += R"(","name":"scoped upgrade test","version":"1.0.0","entry":"management.js","shutdownTimeoutMs":100,"prefix":")";
        manifest += prefix;
        manifest += R"("})";
        add_zip_file(archive, "meta.json", manifest);

        std::string entry = R"JS(
const evbus = import.meta.use('evbus');
evbus.on('circud.host', 'call', event => {
  if (event.target === 'probe') event.respondWith(')JS";
        entry += response;
        entry += R"JS(');
});
evbus.on('circud.host', 'endpoint', event => {
  if (event.path === '/') event.respondWith({ body: ')JS";
        entry += response;
        entry += R"JS(' });
});
)JS";
        add_zip_file(archive, "management.js", entry);
    }
    catch (...) {
        zipClose(archive, nullptr);
        throw;
    }
    if (zipClose(archive, nullptr) != ZIP_OK)
        throw std::runtime_error("cannot close scoped upgrade test ZIP");
}

void make_endpoint_generation_package(
    const std::filesystem::path& archive_path) {
    zipFile archive = zipOpen64(archive_path.c_str(), APPEND_STATUS_CREATE);
    if (!archive)
        throw std::runtime_error("cannot create endpoint generation test ZIP");
    add_zip_file(archive, "meta.json", R"({
      "schema":1,"id":"io.example.endpoint-generation",
      "name":"endpoint generation test","version":"1.0.0",
      "entry":"management.js","shutdownTimeoutMs":100
    })");
    add_zip_file(archive, "management.js", R"JS(
import { info } from 'circud';
const evbus = import.meta.use('evbus');
const fs = import.meta.use('fs');
evbus.on('circud.host', 'endpoint', event => {
  if (event.path === '/socket') event.acceptWebSocket();
  if (event.path === '/events') event.acceptSse();
  if (event.path === '/pending') event.respondWith(new Promise(() => {}));
});
evbus.on('circud.host', 'endpoint.close', event => {
  const text = String(event.id);
  fs.writeFile(info().dataDir + '/closed-id',
    new Uint8Array([...text].map(character => character.charCodeAt(0))));
});
)JS");
    if (zipClose(archive, nullptr) != ZIP_OK)
        throw std::runtime_error(
            "cannot close endpoint generation test ZIP");
}

class CapturingEndpointSink final : public circud::EndpointSink {
public:
    bool emit(circud::EndpointOutput output) override {
        {
            std::lock_guard lock(mutex_);
            outputs_.push_back(std::move(output));
        }
        changed_.notify_all();
        return true;
    }

    bool wait_for(std::size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout,
                                 [&] { return outputs_.size() >= count; });
    }

    std::vector<circud::EndpointOutput> outputs() const {
        std::lock_guard lock(mutex_);
        return outputs_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<circud::EndpointOutput> outputs_;
};

std::shared_ptr<circud::PackageRuntime> make_endpoint_runtime(
    const TempDir& temp, std::string id, std::string_view entry,
    circud::EndpointConfig endpoint_config = {}) {
    const auto package_root = temp.path / id;
    const auto version_dir = package_root / "release";
    const auto data_dir = package_root / "data";
    std::filesystem::create_directories(version_dir);
    std::filesystem::create_directories(data_dir);
    std::ofstream(version_dir / "management.js") << entry;

    circud::PackageManifest manifest;
    manifest.id = std::move(id);
    manifest.name = "endpoint runtime test";
    manifest.version = "1.0.0";
    manifest.shutdown_timeout = std::chrono::milliseconds(100);
    circud::StoredPackage package{
        std::move(manifest), true, version_dir, data_dir };
    circud::OutboundCall outbound = [](std::string, std::string,
                                        circud::ClonePtr,
                                        std::chrono::milliseconds,
                                        std::function<void(circud::CallResult)>
                                            complete) {
        complete(circud::CallResult::failure(
            circud::CallErrorCode::unavailable,
            "outbound calls are not available in this test"));
    };
    auto runtime = std::make_shared<circud::PackageRuntime>(
        std::move(package), std::move(outbound), [] {},
        std::move(endpoint_config));
    std::promise<circud::RuntimeState> ready;
    auto ready_future = ready.get_future();
    runtime->async_start([&ready](circud::RuntimeState state) {
        ready.set_value(state);
    });
    circud::test::require(
        ready_future.wait_for(std::chrono::seconds(2)) ==
            std::future_status::ready,
        "package runtime did not become ready");
    (void)ready_future.get();
    circud::test::require(runtime->state() == circud::RuntimeState::running,
                          runtime->failure());
    return runtime;
}

circud::EndpointRequest endpoint_request(std::uint64_t id,
                                         std::string path) {
    using namespace std::chrono_literals;
    circud::EndpointRequest request;
    request.id = id;
    request.protocol = circud::EndpointProtocol::http;
    request.method = "GET";
    request.path = std::move(path);
    request.deadline = std::chrono::steady_clock::now() + 2s;
    return request;
}

std::string endpoint_text(const circud::EndpointBytes& bytes) {
    return { reinterpret_cast<const char*>(bytes.data()), bytes.size() };
}

} // namespace

CIRCUD_TEST(runtime_can_use_data_directory_but_not_arbitrary_files) {
    using namespace std::chrono_literals;
    TempDir temp;
    const auto outside = temp.path / "outside.txt";
    const auto archive = temp.path / "package.zip";
    make_package(archive, outside.string());

    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    circud::Core core(config);
    CoreLoop loop(core);
    const auto package = core.install(archive);
    loop.start();
    auto result = core.call("web", package.manifest.id, "probe", "null", 2s).get();
    loop.stop();

    circud::test::require(!result.error,
                          result.error ? result.error->message : "call failed");
    circud::test::require(result.json &&
                          result.json->find(R"("value":"persisted")") !=
                              std::string::npos);
    circud::test::require(result.json &&
                          result.json->find(R"("outsideDenied":true)") !=
                              std::string::npos);
    circud::test::require(std::filesystem::is_regular_file(
        package.data_dir / "state.txt"));
    circud::test::require(!std::filesystem::exists(outside));
}

CIRCUD_TEST(async_rpc_call_completes_when_started_on_core_io_thread) {
    using namespace std::chrono_literals;
    TempDir temp;
    const auto archive = temp.path / "package.zip";
    make_package(archive, (temp.path / "outside.txt").string());

    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    circud::Core core(config);
    CoreLoop loop(core);
    const auto package = core.install(archive);
    loop.start();

    auto promise = std::make_shared<std::promise<circud::RpcOutcome>>();
    auto future = promise->get_future();
    boost::asio::post(core.io(), [&core, promise, id = package.manifest.id] {
        core.async_handle_rpc(boost::json::object{
            { "method", "call" },
            { "params", boost::json::object{
                { "package", id },
                { "target", "probe" },
                { "arguments", nullptr },
                { "timeoutMs", 2000 },
            } },
        }, [promise](circud::RpcOutcome outcome) mutable {
            promise->set_value(std::move(outcome));
        });
    });

    circud::test::require(future.wait_for(3s) == std::future_status::ready,
        "async RPC call deadlocked the core I/O thread");
    auto outcome = future.get();
    loop.stop();

    circud::test::require(
        std::holds_alternative<boost::json::value>(outcome));
    const auto& result = std::get<boost::json::value>(outcome).as_object();
    circud::test::require(result.at("ok").as_bool());
    circud::test::require(
        result.at("value").as_object().at("value").as_string() ==
        "persisted");
}

CIRCUD_TEST(synchronous_lifecycle_adapters_fail_fast_on_control_thread) {
    using namespace std::chrono_literals;
    TempDir temp;
    const auto archive = temp.path / "package.zip";
    make_package(archive, (temp.path / "outside.txt").string());

    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    circud::Core core(config);
    CoreLoop loop(core);
    const auto package = core.install(archive);
    loop.start();

    auto promise = std::make_shared<std::promise<std::array<bool, 4>>>();
    auto future = promise->get_future();
    boost::asio::post(core.io(),
        [&core, promise, archive, id = package.manifest.id] {
            const auto rejects = [](auto&& operation) {
                try {
                    operation();
                }
                catch (const std::logic_error&) {
                    return true;
                }
                catch (...) {}
                return false;
            };
            promise->set_value({
                rejects([&] { (void)core.start_package(id); }),
                rejects([&] { (void)core.stop_package(id); }),
                rejects([&] { (void)core.reload_package(id); }),
                rejects([&] { (void)core.install(archive); }),
            });
        });

    circud::test::require(future.wait_for(500ms) ==
            std::future_status::ready,
        "synchronous lifecycle adapter blocked the control thread");
    const auto rejected = future.get();
    loop.stop();

    circud::test::require(std::ranges::all_of(rejected,
            [](bool value) { return value; }),
        "a synchronous lifecycle adapter did not fail fast on control thread");
}

CIRCUD_TEST(core_rejects_starts_while_stopping_and_restarts_after_completion) {
    using namespace std::chrono_literals;
    TempDir temp;
    const auto archive = temp.path / "package.zip";
    make_package(archive, (temp.path / "outside.txt").string());

    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    circud::Core core(config);
    CoreLoop loop(core);
    const auto package = core.install(archive);
    loop.start();

    core.stop();
    bool daemon_start_rejected = false;
    try {
        core.start();
    }
    catch (const std::runtime_error&) {
        daemon_start_rejected = true;
    }
    auto package_start = std::make_shared<
        std::promise<std::optional<std::string>>>();
    auto package_start_future = package_start->get_future();
    core.async_start_package(package.manifest.id,
        [package_start](std::optional<std::string> error) {
            package_start->set_value(std::move(error));
        });
    loop.stop();

    circud::test::require(daemon_start_rejected,
        "daemon start was accepted while the previous stop was in progress");
    circud::test::require(package_start_future.wait_for(500ms) ==
            std::future_status::ready,
        "package start submitted during daemon stop did not complete");
    const auto package_start_error = package_start_future.get();
    circud::test::require(package_start_error &&
            package_start_error->find("not running") != std::string::npos,
        "package start submitted during daemon stop was not rejected");

    loop.start();
    const auto result = core.call("web", package.manifest.id, "probe", "null",
        2s).get();
    loop.stop();
    circud::test::require(!result.error,
        result.error ? result.error->message
                     : "core did not restart after stop completion");
}

CIRCUD_TEST(runtime_revoke_immediately_completes_bound_pending_call) {
    using namespace std::chrono_literals;
    TempDir temp;
    const auto archive = temp.path / "runtime-revoke.zip";
    make_runtime_revoke_package(archive);

    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    circud::Core core(config);
    CoreLoop loop(core);
    const auto package = core.install(archive);
    loop.start();

    auto call = core.call("web", package.manifest.id, "wait", "null", 30s);
    const auto bound_marker = package.data_dir / "call-bound";
    bool bound = false;
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (std::filesystem::exists(bound_marker)) {
            bound = true;
            break;
        }
        std::this_thread::sleep_for(5ms);
    }
    auto stop = core.stop_package(package.manifest.id);
    const bool call_completed_before_stop =
        call.wait_for(200ms) == std::future_status::ready &&
        stop.wait_for(0ms) != std::future_status::ready;
    const auto result = call.get();
    stop.get();
    loop.stop();

    circud::test::require(bound,
        "never-settled call was not bound to the runtime before revocation");
    circud::test::require(call_completed_before_stop,
        "bound pending call waited for graceful runtime stop");
    circud::test::require(result.error &&
            result.error->code == circud::CallErrorCode::unavailable,
        "runtime revocation did not fail the bound pending call as unavailable");
}

CIRCUD_TEST(core_stop_completes_pending_calls) {
    using namespace std::chrono_literals;
    TempDir temp;
    const auto archive = temp.path / "wait.zip";
    make_wait_package(archive);
    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    circud::Core core(config);
    CoreLoop loop(core);
    const auto package = core.install(archive);
    loop.start();
    auto result = core.call("web", package.manifest.id, "wait", "null", 30s);
    std::this_thread::sleep_for(20ms);
    loop.stop();
    circud::test::require(result.wait_for(500ms) == std::future_status::ready,
                          "pending call did not finish during shutdown");
    auto value = result.get();
    circud::test::require(value.error &&
                          value.error->code == circud::CallErrorCode::unavailable);
    circud::test::require(
        std::filesystem::exists(package.data_dir / "call-aborted"),
        "runtime shutdown did not abort a pending management call signal");
}

CIRCUD_TEST(native_circud_esm_exports_runtime_owned_abort_signal) {
    using namespace std::chrono_literals;
    TempDir temp;
    const auto archive = temp.path / "abort-signal.zip";
    make_abort_signal_package(archive);
    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    config.status_polling_enabled = false;
    circud::Core core(config);
    CoreLoop loop(core);
    core.install(archive);
    loop.start();

    const auto cancelled = core.call("web", "io.example.abort-signal",
        "wait", "null", 50ms).get();
    circud::test::require(cancelled.error &&
        cancelled.error->code == circud::CallErrorCode::timeout,
        "timed call did not exercise AbortSignal cancellation");
    const auto normal = core.call("web", "io.example.abort-signal",
        "normal", "null", 200ms).get();
    circud::test::require(!normal.error,
        "normal call used to verify non-abort state failed");

    std::optional<circud::CallResult> observed;
    for (int attempt = 0; attempt < 100; ++attempt) {
        auto result = core.call("web", "io.example.abort-signal",
            "probe", "null", 200ms).get();
        if (!result.error && result.json &&
            result.json->find(R"("cancelled":true)") != std::string::npos) {
            observed.emplace(std::move(result));
            break;
        }
        std::this_thread::sleep_for(5ms);
    }
    loop.stop();

    circud::test::require(observed && observed->json,
        "AbortSignal listener did not run on the package runtime");
    const auto& json = *observed->json;
    circud::test::require(
        json.find(R"("constructorRejected":true)") != std::string::npos,
        "packages could construct AbortSignal directly");
    circud::test::require(
        json.find(R"("legacyUseHidden":true)") != std::string::npos,
        "the circud API remained visible through import.meta.use()");
    circud::test::require(
        json.find(R"("instance":true)") != std::string::npos,
        "event signal was not an instance of the ESM AbortSignal export");
    circud::test::require(
        json.find(R"("initialAborted":false)") != std::string::npos &&
        json.find(R"("initialReasonUndefined":true)") != std::string::npos,
        "fresh AbortSignal state was incorrect");
    circud::test::require(
        json.find(R"("onabortCount":1)") != std::string::npos &&
        json.find(R"("listenerCount":1)") != std::string::npos &&
        json.find(R"("objectListenerCount":1)") != std::string::npos &&
        json.find(R"("listenerAfterThrowCount":1)") != std::string::npos &&
        json.find(R"("removedCount":0)") != std::string::npos,
        "AbortSignal listener isolation/single-fire/remove semantics were incorrect");
    circud::test::require(
        json.find(R"("listenerThis":true)") != std::string::npos &&
        json.find(R"("eventShape":true)") != std::string::npos,
        "AbortSignal listener receiver or event shape was incorrect");
    circud::test::require(
        json.find(R"("throwSameReason":true)") != std::string::npos &&
        json.find(R"("reasonName":"AbortError")") != std::string::npos,
        "AbortSignal reason or throwIfAborted contract was incorrect");
    circud::test::require(
        json.find(R"("onabortRetained":true)") != std::string::npos &&
        json.find(R"("normalSignalUnaffected":true)") != std::string::npos,
        "normal completion incorrectly aborted a runtime-owned signal");
}

CIRCUD_TEST(core_stop_completes_coalesced_status_refreshes) {
    using namespace std::chrono_literals;
    TempDir temp;
    const auto archive = temp.path / "wait.zip";
    make_wait_package(archive);
    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    config.status_timeout = 30s;
    circud::Core core(config);
    CoreLoop loop(core);
    const auto package = core.install(archive);
    loop.start();
    auto first = core.refresh_status(package.manifest.id);
    auto second = core.refresh_status(package.manifest.id);
    std::this_thread::sleep_for(20ms);
    loop.stop();
    circud::test::require(first.wait_for(500ms) == std::future_status::ready,
                          "first status refresh did not finish during shutdown");
    circud::test::require(second.wait_for(500ms) == std::future_status::ready,
                          "coalesced status refresh did not finish during shutdown");
    circud::test::require(first.get().stale && second.get().stale);
}

CIRCUD_TEST(core_events_cover_lifecycle_and_redact_rpc_contents) {
    using namespace std::chrono_literals;
    TempDir temp;
    const auto archive = temp.path / "package.zip";
    const std::string secret = "do-not-record-this-value";
    make_package(archive, (temp.path / "outside.txt").string());
    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    circud::Core core(config);
    CoreLoop loop(core);
    const auto package = core.install(archive);
    loop.start();
    const auto failed = core.call("web", package.manifest.id, "missing",
                                  "\"" + secret + "\"", 2s).get();
    core.reload_package(package.manifest.id).get();
    core.stop_package(package.manifest.id).get();
    const auto response = core.handle_rpc(boost::json::object{
        { "method", "events.list" },
        { "params", boost::json::object{} },
    });
    loop.stop();

    circud::test::require(failed.error.has_value());
    const auto serialized = boost::json::serialize(response);
    circud::test::require(serialized.find("installed") != std::string::npos);
    circud::test::require(serialized.find("reloaded") != std::string::npos);
    circud::test::require(serialized.find("unhandled_call") != std::string::npos);
    circud::test::require(serialized.find(secret) == std::string::npos,
                          "RPC arguments leaked into management events");
    circud::test::require(serialized.find(temp.path.string()) ==
                          std::string::npos,
                          "filesystem path leaked into management events");
}

CIRCUD_TEST(core_can_restart_without_stale_calls) {
    using namespace std::chrono_literals;
    TempDir temp;
    const auto archive = temp.path / "package.zip";
    make_package(archive, (temp.path / "outside.txt").string());
    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    circud::Core core(config);
    CoreLoop loop(core);
    const auto package = core.install(archive);

    loop.start();
    auto first = core.call("web", package.manifest.id, "probe", "null", 2s).get();
    loop.stop();
    loop.start();
    auto second = core.call("web", package.manifest.id, "probe", "null", 2s).get();
    loop.stop();
    loop.start();
    auto third = core.call("web", package.manifest.id, "probe", "null", 2s).get();
    loop.stop();

    circud::test::require(!first.error);
    circud::test::require(!second.error);
    circud::test::require(!third.error);
}

CIRCUD_TEST(package_call_policy_rejects_external_packages_but_allows_web) {
    using namespace std::chrono_literals;
    TempDir temp;
    const auto target_archive = temp.path / "target.zip";
    const auto caller_archive = temp.path / "caller.zip";
    make_policy_package(target_archive, "io.example.target", false, R"JS(
const evbus = import.meta.use('evbus');
evbus.on('circud.host', 'call', event => {
  if (event.target === 'echo') event.respondWith(event.arguments);
});
)JS");
    make_policy_package(caller_archive, "io.example.caller", true, R"JS(
import { call } from 'circud';
const evbus = import.meta.use('evbus');
evbus.on('circud.host', 'call', event => {
  if (event.target === 'probe') {
    event.respondWith(call('io.example.target', 'echo', 'blocked')
      .then(() => 'unexpected-success', error => error.code));
  }
});
)JS");

    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    circud::Core core(config);
    CoreLoop loop(core);
    core.install(target_archive);
    core.install(caller_archive);
    loop.start();
    auto web = core.call("web", "io.example.target", "echo", "\"allowed\"", 2s)
                   .get();
    auto package = core.call("web", "io.example.caller", "probe", "null", 2s)
                       .get();
    loop.stop();

    circud::test::require(!web.error && web.json == "\"allowed\"");
    circud::test::require(!package.error &&
                          package.json == "\"package_calls_disabled\"");
}

CIRCUD_TEST(package_can_request_its_own_reload) {
    using namespace std::chrono_literals;
    TempDir temp;
    const auto archive = temp.path / "reload.zip";
    make_reload_package(archive);
    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    circud::Core core(config);
    CoreLoop loop(core);
    core.install(archive);
    loop.start();
    auto before = core.call("web", "io.example.reload", "generation", "null", 2s)
                      .get();
    auto reload = core.call("web", "io.example.reload", "reload", "null", 2s);
    (void)reload.get();

    std::optional<circud::CallResult> after;
    for (int attempt = 0; attempt < 100; ++attempt) {
        auto value = core.call("web", "io.example.reload", "generation", "null",
                               200ms)
                         .get();
        if (!value.error && value.json == "2") {
            after.emplace(std::move(value));
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    loop.stop();

    circud::test::require(!before.error && before.json == "1");
    circud::test::require(after.has_value(), "reload() did not replace runtime");
}

CIRCUD_TEST(failed_same_version_upgrade_restores_previous_generation) {
    using namespace std::chrono_literals;
    TempDir temp;
    const auto good_archive = temp.path / "good.zip";
    const auto bad_archive = temp.path / "bad.zip";
    make_upgrade_package(good_archive, "old-generation");
    make_upgrade_package(bad_archive, {});
    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    circud::Core core(config);
    CoreLoop loop(core);
    const auto previous = core.install(good_archive);
    loop.start();

    bool rejected = false;
    try {
        (void)core.install(bad_archive);
    } catch (const std::exception&) {
        rejected = true;
    }
    const auto result = core.call("web", "io.example.upgrade", "probe", "null",
                                  2s).get();
    const auto current = core.packages();
    loop.stop();

    circud::test::require(rejected, "invalid upgrade was reported as installed");
    circud::test::require(!result.error && result.json == "\"old-generation\"",
                          result.error ? result.error->message
                                       : "old generation did not respond");
    circud::test::require(current.size() == 1);
    circud::test::require(current.front().version_dir == previous.version_dir);
    circud::test::require(std::filesystem::exists(previous.version_dir));
    std::size_t releases = 0;
    for (const auto& item : std::filesystem::directory_iterator(
             previous.version_dir.parent_path())) {
        if (item.is_directory()) ++releases;
    }
    circud::test::require(releases == 1,
                          "failed upgrade release was not cleaned up");
}

CIRCUD_TEST(conflicting_prefix_upgrade_keeps_previous_runtime_database_and_route) {
    using namespace std::chrono_literals;
    TempDir temp;
    const auto original_archive = temp.path / "original.zip";
    const auto blocker_archive = temp.path / "blocker.zip";
    const auto conflicting_archive = temp.path / "conflicting.zip";
    make_scoped_upgrade_package(original_archive, "io.example.scope-alpha",
        "/alpha", "old-generation");
    make_scoped_upgrade_package(blocker_archive, "io.example.scope-beta",
        "/blocked", "blocker");
    make_scoped_upgrade_package(conflicting_archive, "io.example.scope-alpha",
        "/blocked/child", "new-generation");

    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    circud::Core core(config);
    CoreLoop loop(core);
    const auto previous = core.install(original_archive);
    (void)core.install(blocker_archive);
    loop.start();

    bool rejected = false;
    try {
        (void)core.install(conflicting_archive);
    }
    catch (const std::exception&) {
        rejected = true;
    }
    const auto call = core.call("web", "io.example.scope-alpha", "probe",
        "null", 2s).get();
    const auto packages = core.packages();

    auto sink = std::make_shared<CapturingEndpointSink>();
    const auto opened = core.endpoint_gateway().open(
        endpoint_request(0, "/alpha"), sink);
    const bool responded = sink->wait_for(1, 2s);
    const auto outputs = sink->outputs();
    loop.stop();

    const auto stored = std::ranges::find_if(packages, [](const auto& package) {
        return package.manifest.id == "io.example.scope-alpha";
    });
    const auto* endpoint = outputs.empty() ? nullptr
        : std::get_if<circud::EndpointResponse>(&outputs.front());
    circud::test::require(rejected,
        "an upgrade with a conflicting endpoint prefix was accepted");
    circud::test::require(!call.error && call.json == "\"old-generation\"",
        call.error ? call.error->message
                   : "the previous runtime did not survive prefix rejection");
    circud::test::require(stored != packages.end() &&
            stored->version_dir == previous.version_dir &&
            stored->manifest.endpoint_prefix == "/alpha",
        "a rejected prefix upgrade changed the stored package");
    circud::test::require(opened && responded && endpoint &&
            endpoint_text(endpoint->body) == "old-generation",
        "a rejected prefix upgrade changed or disabled the previous route");
}

CIRCUD_TEST(endpoint_gateway_rejects_route_snapshot_for_different_runtime_prefix) {
    TempDir temp;
    const auto archive = temp.path / "scoped.zip";
    make_scoped_upgrade_package(archive, "io.example.scope-generation",
        "/original", "current-generation");

    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    circud::Core core(config);
    CoreLoop loop(core);
    (void)core.install(archive);
    loop.start();

    auto mismatched = core.packages();
    circud::test::require(mismatched.size() == 1);
    mismatched.front().manifest.endpoint_prefix = "/replacement";
    core.endpoint_gateway().publish(mismatched);

    auto sink = std::make_shared<CapturingEndpointSink>();
    const auto opened = core.endpoint_gateway().open(
        endpoint_request(0, "/replacement"), sink);
    loop.stop();

    circud::test::require(
        opened.status == circud::EndpointOpenStatus::unavailable,
        "a route snapshot was dispatched to a runtime with another prefix");
    circud::test::require(sink->outputs().empty(),
        "a mismatched route reached the current package runtime");
}

CIRCUD_TEST(successful_same_version_upgrade_switches_generation) {
    using namespace std::chrono_literals;
    TempDir temp;
    const auto first_archive = temp.path / "first.zip";
    const auto second_archive = temp.path / "second.zip";
    make_upgrade_package(first_archive, "old-generation");
    make_upgrade_package(second_archive, "new-generation");
    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    circud::Core core(config);
    CoreLoop loop(core);
    const auto previous = core.install(first_archive);
    loop.start();
    const auto replacement = core.install(second_archive);
    const auto result = core.call("web", "io.example.upgrade", "probe", "null",
                                  2s).get();
    const auto current = core.packages();
    loop.stop();

    circud::test::require(previous.version_dir != replacement.version_dir);
    circud::test::require(!result.error && result.json == "\"new-generation\"",
                          result.error ? result.error->message
                                       : "new generation did not respond");
    circud::test::require(current.size() == 1);
    circud::test::require(current.front().version_dir == replacement.version_dir);
    circud::test::require(!std::filesystem::exists(previous.version_dir),
                          "previous release was not cleaned up");
    circud::test::require(std::filesystem::exists(replacement.version_dir));
}

CIRCUD_TEST(upgrade_preserves_disabled_package_state) {
    TempDir temp;
    const auto first_archive = temp.path / "first.zip";
    const auto second_archive = temp.path / "second.zip";
    make_upgrade_package(first_archive, "old-generation");
    make_upgrade_package(second_archive, "new-generation");
    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    circud::Core core(config);
    CoreLoop loop(core);
    (void)core.install(first_archive);
    loop.start();
    core.stop_package("io.example.upgrade").get();
    const auto replacement = core.install(second_archive);
    const auto state = core.runtime_state("io.example.upgrade");
    const auto packages = core.packages();
    loop.stop();

    circud::test::require(!replacement.enabled);
    circud::test::require(state == circud::RuntimeState::disabled);
    circud::test::require(packages.size() == 1 && !packages.front().enabled);
    circud::test::require(packages.front().version_dir ==
                          replacement.version_dir);
}

CIRCUD_TEST(remove_preserves_data_unless_purge_is_requested) {
    TempDir temp;
    const auto archive = temp.path / "package.zip";
    make_upgrade_package(archive, "generation");
    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    circud::Core core(config);
    const auto installed = core.install(archive);
    const auto marker = installed.data_dir / "keep.txt";
    std::ofstream(marker) << "persistent";

    core.remove(installed.manifest.id, false);
    circud::test::require(std::filesystem::exists(marker),
                          "normal remove deleted package data");
    (void)core.install(archive);
    circud::test::require(std::filesystem::exists(marker),
                          "reinstall did not preserve package data");
    core.remove(installed.manifest.id, true);
    circud::test::require(!std::filesystem::exists(installed.data_dir),
                          "purge remove preserved package data");
}

CIRCUD_TEST(endpoint_response_getters_cannot_reenter_terminal_selection) {
    using namespace std::chrono_literals;
    TempDir temp;
    auto runtime = make_endpoint_runtime(temp, "io.example.endpoint.reentry",
        R"JS(
const evbus = import.meta.use('evbus');
evbus.on('circud.host', 'endpoint', event => {
  if (event.path !== '/reenter') return;
  event.respondWith({
    get status() {
      try { event.acceptSse(); } catch (_) {}
      return 207;
    },
    get body() {
      try { event.respondWith({ status: 202, body: 'second' }); } catch (_) {}
      return 'first';
    }
  });
});
)JS");
    auto sink = std::make_shared<CapturingEndpointSink>();
    circud::test::require(
        runtime->open_endpoint(endpoint_request(1, "/reenter"), sink),
        "endpoint request was not accepted by runtime");
    circud::test::require(sink->wait_for(1, 2s),
                          "reentrant endpoint produced no terminal output");
    runtime->stop_and_join();

    const auto outputs = sink->outputs();
    circud::test::require(outputs.size() == 1,
        "response getter reentrancy selected more than one endpoint outcome");
    const auto* response = std::get_if<circud::EndpointResponse>(&outputs[0]);
    circud::test::require(response != nullptr,
        "response getter replaced the outer response protocol");
    circud::test::require(response->status == 207,
                          "outer endpoint response status changed");
    circud::test::require(endpoint_text(response->body) == "first",
                          "reentrant respondWith replaced the outer body");
}

CIRCUD_TEST(endpoint_accept_getters_cannot_reenter_terminal_selection) {
    using namespace std::chrono_literals;
    TempDir temp;
    auto runtime = make_endpoint_runtime(temp,
        "io.example.endpoint.accept-reentry", R"JS(
const evbus = import.meta.use('evbus');
evbus.on('circud.host', 'endpoint', event => {
  if (event.path === '/accept') {
    let blocked = 0;
    const stream = event.acceptSse({
      get status() {
        try { event.respondWith({ status: 202 }); }
        catch (error) { if (error instanceof TypeError) ++blocked; }
        return 200;
      },
      get headers() {
        try { event.acceptSse(); }
        catch (error) { if (error instanceof TypeError) ++blocked; }
        return { 'x-reentry-blocked': String(blocked) };
      }
    });
    stream.close();
    return;
  }
  try {
    event.acceptSse({ get status() { throw new Error('options failed'); } });
  } catch (_) {}
  event.respondWith({ status: 204 });
});
)JS");

    auto accepted_sink = std::make_shared<CapturingEndpointSink>();
    circud::test::require(runtime->open_endpoint(
        endpoint_request(31, "/accept"), accepted_sink));
    circud::test::require(accepted_sink->wait_for(2, 2s));
    const auto accepted_outputs = accepted_sink->outputs();
    const auto* accept = accepted_outputs.size() == 2
        ? std::get_if<circud::EndpointAccept>(&accepted_outputs[0]) : nullptr;
    const auto* close = accepted_outputs.size() == 2
        ? std::get_if<circud::EndpointClose>(&accepted_outputs[1]) : nullptr;
    circud::test::require(accept != nullptr && close != nullptr &&
            accept->headers.size() == 3 &&
            accept->headers[0].second == "2",
        "accept option getter reentered terminal selection");

    auto rollback_sink = std::make_shared<CapturingEndpointSink>();
    circud::test::require(runtime->open_endpoint(
        endpoint_request(32, "/rollback"), rollback_sink));
    circud::test::require(rollback_sink->wait_for(1, 2s));
    const auto rollback_outputs = rollback_sink->outputs();
    const auto* response = rollback_outputs.size() == 1
        ? std::get_if<circud::EndpointResponse>(&rollback_outputs.front())
        : nullptr;
    circud::test::require(response != nullptr && response->status == 204,
        "failed accept options did not release terminal selection");
    runtime->stop_and_join();
}

CIRCUD_TEST(endpoint_sse_rejects_empty_send_without_closing_the_stream) {
    using namespace std::chrono_literals;
    TempDir temp;
    auto runtime = make_endpoint_runtime(temp,
        "io.example.endpoint.sse-empty", R"JS(
const evbus = import.meta.use('evbus');
evbus.on('circud.host', 'endpoint', event => {
  const stream = event.acceptSse();
  let rejected = false;
  try { stream.send(''); }
  catch (error) { rejected = error instanceof RangeError; }
  stream.send(rejected ? 'data: still-open\n\n' : 'data: empty-accepted\n\n');
  stream.close();
});
)JS");
    auto sink = std::make_shared<CapturingEndpointSink>();
    circud::test::require(runtime->open_endpoint(
        endpoint_request(33, "/events"), sink));
    circud::test::require(sink->wait_for(3, 2s));
    const auto outputs = sink->outputs();
    const auto* message = outputs.size() == 3
        ? std::get_if<circud::EndpointMessage>(&outputs[1]) : nullptr;
    circud::test::require(message != nullptr &&
            endpoint_text(message->data) == "data: still-open\n\n",
        "empty SSE send was emitted or terminated the stream");
    runtime->stop_and_join();
}

CIRCUD_TEST(endpoint_numeric_fields_reject_wrapping_fractional_and_reserved_values) {
    using namespace std::chrono_literals;
    TempDir temp;
    auto runtime = make_endpoint_runtime(temp,
        "io.example.endpoint.numeric", R"JS(
const evbus = import.meta.use('evbus');
evbus.on('circud.host', 'endpoint', event => {
  if (event.path === '/overflow') {
    event.respondWith({ status: 4294967496 });
    return;
  }
  if (event.path === '/fraction') {
    event.respondWith({ status: 200.5 });
    return;
  }
  const socket = event.acceptWebSocket();
  let rejected = 0;
  for (const code of [1005, 1000.5, 4294968296]) {
    try { socket.close(code); }
    catch (error) { if (error instanceof RangeError) ++rejected; }
  }
  socket.close(rejected === 3 ? 1000 : 1011);
});
)JS");
    for (const auto& [id, path] :
         std::array<std::pair<std::uint64_t, std::string>, 2>{
             std::pair{ 34ULL, std::string("/overflow") },
             std::pair{ 35ULL, std::string("/fraction") } }) {
        auto sink = std::make_shared<CapturingEndpointSink>();
        circud::test::require(runtime->open_endpoint(
            endpoint_request(id, path), sink));
        circud::test::require(sink->wait_for(1, 2s));
        const auto outputs = sink->outputs();
        const auto* response = outputs.size() == 1
            ? std::get_if<circud::EndpointResponse>(&outputs.front()) : nullptr;
        circud::test::require(response != nullptr && response->status == 500,
            "invalid endpoint status was truncated or wrapped");
    }
    auto close_sink = std::make_shared<CapturingEndpointSink>();
    auto request = endpoint_request(36, "/close");
    request.protocol = circud::EndpointProtocol::websocket;
    circud::test::require(runtime->open_endpoint(std::move(request), close_sink));
    circud::test::require(close_sink->wait_for(2, 2s));
    const auto outputs = close_sink->outputs();
    const auto* close = outputs.size() == 2
        ? std::get_if<circud::EndpointClose>(&outputs[1]) : nullptr;
    circud::test::require(close != nullptr && close->code == 1000,
        "invalid WebSocket close code was truncated, wrapped or rewritten");
    runtime->stop_and_join();
}

CIRCUD_TEST(endpoint_sse_accept_requires_get_synchronously) {
    using namespace std::chrono_literals;
    TempDir temp;
    auto runtime = make_endpoint_runtime(temp,
        "io.example.endpoint.sse-method", R"JS(
const evbus = import.meta.use('evbus');
evbus.on('circud.host', 'endpoint', event => {
  let rejected = false;
  try {
    event.acceptSse();
  } catch (error) {
    rejected = error instanceof TypeError &&
      String(error.message).includes('GET request');
  }
  event.respondWith({ status: rejected ? 204 : 500 });
});
)JS");

    for (const auto& [id, method] :
         std::array<std::pair<std::uint64_t, std::string_view>, 2>{
             std::pair{ 21ULL, std::string_view("POST") },
             std::pair{ 22ULL, std::string_view("HEAD") } }) {
        auto sink = std::make_shared<CapturingEndpointSink>();
        auto request = endpoint_request(id, "/sse-method");
        request.method = method;
        circud::test::require(
            runtime->open_endpoint(std::move(request), sink),
            "non-GET SSE probe was not accepted by runtime");
        circud::test::require(sink->wait_for(1, 2s),
            "non-GET acceptSse() did not synchronously settle");
        const auto outputs = sink->outputs();
        const auto* response = outputs.size() == 1
            ? std::get_if<circud::EndpointResponse>(&outputs.front())
            : nullptr;
        circud::test::require(response != nullptr && response->status == 204,
            "acceptSse() did not synchronously reject a non-GET request");
    }
    runtime->stop_and_join();
}

CIRCUD_TEST(endpoint_rejected_promise_emits_one_bounded_500_response) {
    using namespace std::chrono_literals;
    TempDir temp;
    circud::EndpointConfig endpoint_config;
    endpoint_config.max_response_body_bytes = 24;
    auto runtime = make_endpoint_runtime(temp, "io.example.endpoint.reject",
        R"JS(
const evbus = import.meta.use('evbus');
evbus.on('circud.host', 'endpoint', event => {
  if (event.path === '/reject')
    event.respondWith(Promise.reject(new Error('x'.repeat(200000))));
});
)JS",
        endpoint_config);
    auto sink = std::make_shared<CapturingEndpointSink>();
    circud::test::require(
        runtime->open_endpoint(endpoint_request(2, "/reject"), sink),
        "promise endpoint request was not accepted by runtime");
    circud::test::require(sink->wait_for(1, 2s),
                          "rejected endpoint Promise did not settle");
    runtime->stop_and_join();

    const auto outputs = sink->outputs();
    circud::test::require(outputs.size() == 1,
                          "rejected endpoint Promise emitted multiple outcomes");
    const auto* response = std::get_if<circud::EndpointResponse>(&outputs[0]);
    circud::test::require(response != nullptr && response->status == 500,
                          "rejected endpoint Promise did not become HTTP 500");
    circud::test::require(
        response->body.size() <= endpoint_config.max_response_body_bytes,
        "rejected endpoint Promise bypassed the response body bound");
}

CIRCUD_TEST(closing_never_settled_endpoint_does_not_delay_runtime_shutdown) {
    using namespace std::chrono_literals;
    TempDir temp;
    auto runtime = make_endpoint_runtime(temp, "io.example.endpoint.pending",
        R"JS(
import { AbortSignal, info } from 'circud';
const evbus = import.meta.use('evbus');
const fs = import.meta.use('fs');
evbus.on('circud.host', 'endpoint', event => {
  if (event.path === '/pending') {
    if (!(event.signal instanceof AbortSignal))
      throw new Error('endpoint signal has the wrong runtime class');
    event.signal.addEventListener('abort', () => {
      if (event.signal.aborted && event.signal.reason?.name === 'AbortError')
        fs.writeFile(info().dataDir + '/endpoint-aborted',
          new Uint8Array([1]));
    });
    event.respondWith(new Promise(() => {}));
  }
});
evbus.on('circud.host', 'endpoint.close', () => {
  fs.writeFile(info().dataDir + '/endpoint-closed', new Uint8Array([1]));
});
)JS");
    const auto closed_marker = runtime->package().data_dir / "endpoint-closed";
    const auto aborted_marker =
        runtime->package().data_dir / "endpoint-aborted";
    auto sink = std::make_shared<CapturingEndpointSink>();
    circud::test::require(
        runtime->open_endpoint(endpoint_request(3, "/pending"), sink),
        "never-settled endpoint request was not accepted by runtime");
    circud::test::require(runtime->close_endpoint(
                              circud::EndpointClose{ 3, 1001, "idle timeout" }),
                          "endpoint close was not accepted by runtime");

    const auto started = std::chrono::steady_clock::now();
    runtime->stop_and_join();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    circud::test::require(elapsed < 1s,
        "never-settled endpoint Promise delayed runtime shutdown");
    circud::test::require(std::filesystem::exists(closed_marker),
                          "endpoint close was not delivered to the package");
    circud::test::require(std::filesystem::exists(aborted_marker),
                          "endpoint close did not abort its signal");
    circud::test::require(sink->outputs().empty(),
        "client close caused a stale pending Promise to emit a response");
}

CIRCUD_TEST(endpoint_close_bypasses_saturated_data_command_queue) {
    using namespace std::chrono_literals;
    TempDir temp;
    auto runtime = make_endpoint_runtime(temp,
        "io.example.endpoint.close-control", R"JS(
import { info } from 'circud';
const evbus = import.meta.use('evbus');
const fs = import.meta.use('fs');
evbus.on('circud.host', 'endpoint', event => {
  if (event.path === '/socket') event.acceptWebSocket();
});
evbus.on('circud.host', 'endpoint.close', () => {
  fs.writeFile(info().dataDir + '/endpoint-closed', new Uint8Array([1]));
});
)JS");
    const auto marker = runtime->package().data_dir / "endpoint-closed";
    auto sink = std::make_shared<CapturingEndpointSink>();
    auto request = endpoint_request(4, "/socket");
    request.protocol = circud::EndpointProtocol::websocket;
    const bool opened = runtime->open_endpoint(std::move(request), sink);
    const bool accepted = sink->wait_for(1, 2s);

    std::mutex blocker_mutex;
    std::condition_variable blocker_changed;
    bool owner_blocked = false;
    bool release_owner = false;
    runtime->post([&](JSContext*) {
        std::unique_lock lock(blocker_mutex);
        owner_blocked = true;
        blocker_changed.notify_all();
        blocker_changed.wait(lock, [&] { return release_owner; });
    });
    {
        std::unique_lock lock(blocker_mutex);
        blocker_changed.wait_for(lock, 2s, [&] { return owner_blocked; });
    }

    bool filled = owner_blocked;
    for (std::size_t index = 0; index < circud::kMaxEndpointCommands; ++index) {
        circud::EndpointMessage message;
        message.id = 4;
        message.data.push_back(static_cast<std::byte>('x'));
        filled = runtime->deliver_endpoint_message(std::move(message)) && filled;
    }
    circud::EndpointMessage overflow;
    overflow.id = 4;
    overflow.data.push_back(static_cast<std::byte>('x'));
    const bool data_queue_saturated =
        !runtime->deliver_endpoint_message(std::move(overflow));
    const bool close_accepted = runtime->close_endpoint(
        circud::EndpointClose{ 4, 1000, "client disconnected" });

    {
        std::lock_guard lock(blocker_mutex);
        release_owner = true;
    }
    blocker_changed.notify_all();
    bool close_delivered = false;
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (std::filesystem::exists(marker)) {
            close_delivered = true;
            break;
        }
        std::this_thread::sleep_for(5ms);
    }
    runtime->stop_and_join();

    circud::test::require(opened && accepted,
        "WebSocket endpoint was not accepted before saturation");
    circud::test::require(filled && data_queue_saturated,
        "endpoint data command queue did not reach its configured bound");
    circud::test::require(close_accepted,
        "endpoint close competed with the saturated data queue");
    circud::test::require(close_delivered,
        "accepted endpoint close was not dispatched on the owner thread");
}

CIRCUD_TEST(runtime_completion_task_queue_is_bounded) {
    using namespace std::chrono_literals;
    TempDir temp;
    auto runtime = make_endpoint_runtime(temp,
        "io.example.runtime.task-bound", R"JS(
const evbus = import.meta.use('evbus');
evbus.on('circud.host', 'endpoint', event => {
  event.respondWith({ status: 204 });
});
)JS");

    std::mutex blocker_mutex;
    std::condition_variable blocker_changed;
    bool owner_blocked = false;
    bool release_owner = false;
    circud::test::require(runtime->post([&](JSContext*) {
        std::unique_lock lock(blocker_mutex);
        owner_blocked = true;
        blocker_changed.notify_all();
        blocker_changed.wait(lock, [&] { return release_owner; });
    }), "runtime owner blocker was not accepted");
    {
        std::unique_lock lock(blocker_mutex);
        blocker_changed.wait_for(lock, 2s, [&] { return owner_blocked; });
    }

    std::promise<void> drained;
    auto drained_future = drained.get_future();
    bool filled = owner_blocked;
    for (std::size_t index = 1; index < circud::kMaxPendingCalls; ++index)
        filled = runtime->post([](JSContext*) {}) && filled;
    filled = runtime->post([&drained](JSContext*) { drained.set_value(); }) &&
             filled;
    const bool overflow_rejected = !runtime->post([](JSContext*) {});

    {
        std::lock_guard lock(blocker_mutex);
        release_owner = true;
    }
    blocker_changed.notify_all();
    const bool all_drained =
        drained_future.wait_for(2s) == std::future_status::ready;
    runtime->stop_and_join();

    circud::test::require(filled,
        "runtime completion task queue rejected work below its bound");
    circud::test::require(overflow_rejected,
        "runtime completion task queue exceeded its bound");
    circud::test::require(all_drained,
        "accepted runtime completion tasks did not drain");
}

CIRCUD_TEST(runtime_shutdown_completes_bounded_call_batch_despite_callback_errors) {
    using namespace std::chrono_literals;
    TempDir temp;
    auto runtime = make_endpoint_runtime(temp,
        "io.example.runtime.shutdown-batch", R"JS(
const evbus = import.meta.use('evbus');
evbus.on('circud.host', 'call', event => {
  event.respondWith(new Promise(() => {}));
});
)JS");

    std::mutex blocker_mutex;
    std::condition_variable blocker_changed;
    bool owner_blocked = false;
    bool release_owner = false;
    circud::test::require(runtime->post([&](JSContext*) {
        std::unique_lock lock(blocker_mutex);
        owner_blocked = true;
        blocker_changed.notify_all();
        blocker_changed.wait(lock, [&] { return release_owner; });
    }), "runtime owner blocker was not accepted");
    {
        std::unique_lock lock(blocker_mutex);
        circud::test::require(blocker_changed.wait_for(lock, 2s,
            [&] { return owner_blocked; }),
            "runtime owner thread did not block");
    }

    std::atomic_size_t completions{ 0 };
    for (std::size_t index = 0; index < circud::kMaxQueueEntries; ++index) {
        circud::RuntimeCall call;
        call.id = index + 1;
        call.source = "test";
        call.target = "wait";
        call.deadline = std::chrono::steady_clock::now() + 2s;
        call.complete = [&completions, index](circud::CallResult result) {
            ++completions;
            circud::test::require(result.error.has_value(),
                "shutdown completed a queued call successfully");
            if (index == 0)
                throw std::runtime_error("test completion failure");
        };
        circud::test::require(runtime->enqueue(std::move(call)),
            "runtime rejected a call below its bounded queue limit");
    }
    runtime->request_shutdown();
    {
        std::lock_guard lock(blocker_mutex);
        release_owner = true;
    }
    blocker_changed.notify_all();
    runtime->stop_and_join();

    circud::test::require(completions.load() == circud::kMaxQueueEntries,
        "one faulty completion prevented the bounded shutdown batch draining");
}

CIRCUD_TEST(runtime_exit_notifications_are_exactly_once_and_isolated) {
    TempDir temp;
    auto runtime = make_endpoint_runtime(temp,
        "io.example.runtime.exit-notify", R"JS(
const evbus = import.meta.use('evbus');
evbus.on('circud.host', 'call', event => {
  event.respondWith({ ok: true });
});
)JS");

    std::atomic_int first{ 0 };
    std::atomic_int second{ 0 };
    std::atomic_int late{ 0 };
    circud::test::require(runtime->notify_on_exit([&] {
        ++first;
        throw std::runtime_error("test exit observer failure");
    }), "first exit observer was not accepted");
    circud::test::require(runtime->notify_on_exit([&] { ++second; }),
        "second exit observer was not accepted");

    runtime->stop_and_join();
    circud::test::require(runtime->notify_on_exit([&] { ++late; }),
        "late exit observer was not synchronously accepted");
    circud::test::require(first.load() == 1 && second.load() == 1 &&
            late.load() == 1,
        "exit observers were overwritten, repeated, or not isolated");
}

CIRCUD_TEST(runtime_completion_exception_still_stops_and_signals_exit) {
    using namespace std::chrono_literals;
    TempDir temp;
    auto runtime = make_endpoint_runtime(temp,
        "io.example.runtime.task-exception", R"JS(
const evbus = import.meta.use('evbus');
evbus.on('circud.host', 'call', event => {
  event.respondWith({ ok: true });
});
)JS");

    std::promise<void> exited;
    auto exited_future = exited.get_future();
    circud::test::require(runtime->notify_on_exit(
        [&exited] { exited.set_value(); }),
        "runtime exit observer was not accepted");
    circud::test::require(runtime->post([](JSContext*) {
        throw std::runtime_error("test owner task failure");
    }), "throwing runtime completion task was not accepted");
    circud::test::require(exited_future.wait_for(2s) ==
            std::future_status::ready,
        "completion exception bypassed mandatory runtime stop");
    runtime->stop_and_join();
    circud::test::require(runtime->state() == circud::RuntimeState::failed,
        "completion exception did not leave the runtime failed");
}

CIRCUD_TEST(endpoint_close_is_idempotent_while_pending_and_after_dispatch) {
    using namespace std::chrono_literals;
    TempDir temp;
    auto runtime = make_endpoint_runtime(temp,
        "io.example.endpoint.close-idempotent", R"JS(
import { info } from 'circud';
const evbus = import.meta.use('evbus');
const fs = import.meta.use('fs');
let closeCount = 0;
evbus.on('circud.host', 'endpoint', event => {
  if (event.path === '/socket') event.acceptWebSocket();
});
evbus.on('circud.host', 'endpoint.close', () => {
  ++closeCount;
  fs.writeFile(info().dataDir + '/close-count',
    new Uint8Array([48 + closeCount]));
});
)JS");
    const auto marker = runtime->package().data_dir / "close-count";
    auto sink = std::make_shared<CapturingEndpointSink>();
    auto request = endpoint_request(5, "/socket");
    request.protocol = circud::EndpointProtocol::websocket;
    const bool opened = runtime->open_endpoint(std::move(request), sink);
    const bool accepted = sink->wait_for(1, 2s);

    std::mutex blocker_mutex;
    std::condition_variable blocker_changed;
    bool owner_blocked = false;
    bool release_owner = false;
    runtime->post([&](JSContext*) {
        std::unique_lock lock(blocker_mutex);
        owner_blocked = true;
        blocker_changed.notify_all();
        blocker_changed.wait(lock, [&] { return release_owner; });
    });
    {
        std::unique_lock lock(blocker_mutex);
        blocker_changed.wait_for(lock, 2s, [&] { return owner_blocked; });
    }

    const bool first = runtime->close_endpoint(
        circud::EndpointClose{ 5, 1000, "client disconnected" });
    const bool duplicate_pending = runtime->close_endpoint(
        circud::EndpointClose{ 5, 1001, "duplicate close" });
    {
        std::lock_guard lock(blocker_mutex);
        release_owner = true;
    }
    blocker_changed.notify_all();

    bool dispatched = false;
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (std::filesystem::exists(marker)) {
            dispatched = true;
            break;
        }
        std::this_thread::sleep_for(5ms);
    }

    std::promise<void> first_close_drained;
    auto first_close_drained_future = first_close_drained.get_future();
    runtime->post([&first_close_drained](JSContext*) {
        first_close_drained.set_value();
    });
    const bool first_drained =
        first_close_drained_future.wait_for(2s) == std::future_status::ready;
    const bool duplicate_finished = runtime->close_endpoint(
        circud::EndpointClose{ 5, 1000, "already closed" });

    std::promise<void> duplicate_drained;
    auto duplicate_drained_future = duplicate_drained.get_future();
    runtime->post([runtime, &duplicate_drained](JSContext*) {
        // Tasks run before closes from the same drain snapshot. A second post
        // observes the follow-up close only after that snapshot completes.
        runtime->post([&duplicate_drained](JSContext*) {
            duplicate_drained.set_value();
        });
    });
    const bool second_drained =
        duplicate_drained_future.wait_for(2s) == std::future_status::ready;

    char close_count = '\0';
    if (std::ifstream input(marker, std::ios::binary); input)
        input.get(close_count);
    runtime->stop_and_join();

    circud::test::require(opened && accepted,
        "WebSocket endpoint was not accepted before close");
    circud::test::require(owner_blocked,
        "runtime owner thread was not blocked for the close race");
    circud::test::require(first && duplicate_pending && duplicate_finished,
        "a repeated endpoint close was not idempotently accepted");
    circud::test::require(dispatched && first_drained && second_drained,
        "endpoint close did not finish on the runtime owner thread");
    circud::test::require(close_count == '1',
        "repeated endpoint close was dispatched more than once");
}

CIRCUD_TEST(endpoint_runtime_release_closes_network_sessions_without_crossing_generations) {
    using namespace std::chrono_literals;
    TempDir temp;
    const auto archive = temp.path / "endpoint-generation.zip";
    make_endpoint_generation_package(archive);
    circud::CoreConfig config;
    config.state_dir = temp.path / "state";
    config.endpoints.max_sessions = 3;
    config.endpoints.max_sessions_per_package = 3;
    circud::Core core(config);
    CoreLoop loop(core);
    const auto package = core.install(archive);
    loop.start();

    const auto accepted_as = [](const auto& sink,
                                 const circud::EndpointOpenResult& opened,
                                 circud::EndpointProtocol protocol) {
        const auto outputs = sink->outputs();
        const auto* accept = outputs.empty()
            ? nullptr
            : std::get_if<circud::EndpointAccept>(&outputs.front());
        return accept && accept->id == opened.session_id &&
            accept->protocol == protocol;
    };

    auto first_sink = std::make_shared<CapturingEndpointSink>();
    auto first_request = endpoint_request(0,
        "/io.example.endpoint-generation/socket");
    first_request.protocol = circud::EndpointProtocol::websocket;
    // This is a generation/ownership test, not a queue-expiry test. The
    // standalone-runtime helper's two-second deadline is too short for a full
    // ASan suite under load.
    first_request.deadline = std::chrono::steady_clock::now() + 10s;
    const auto first = core.endpoint_gateway().open(
        std::move(first_request), first_sink);
    const bool first_output = first_sink->wait_for(1, 5s);
    const bool first_accepted = first_output && accepted_as(first_sink, first,
        circud::EndpointProtocol::websocket);
    auto sse_sink = std::make_shared<CapturingEndpointSink>();
    auto sse_request = endpoint_request(0,
        "/io.example.endpoint-generation/events");
    sse_request.deadline = std::chrono::steady_clock::now() + 10s;
    const auto sse = core.endpoint_gateway().open(
        std::move(sse_request), sse_sink);
    const bool sse_output = sse_sink->wait_for(1, 5s);
    const bool sse_accepted = sse_output && accepted_as(sse_sink, sse,
        circud::EndpointProtocol::sse);
    auto pending_sink = std::make_shared<CapturingEndpointSink>();
    auto pending_request = endpoint_request(0,
        "/io.example.endpoint-generation/pending");
    pending_request.deadline = std::chrono::steady_clock::now() + 10s;
    const auto pending = core.endpoint_gateway().open(
        std::move(pending_request), pending_sink);
    const auto first_reservations =
        core.endpoint_gateway().active_sessions();

    core.reload_package(package.manifest.id).get();
    const auto reservations_after_reload =
        core.endpoint_gateway().active_sessions();
    const auto first_outputs = first_sink->outputs();
    const auto sse_outputs = sse_sink->outputs();
    const auto pending_outputs = pending_sink->outputs();

    const auto released = [](const auto& outputs, std::uint64_t id) {
        return std::ranges::any_of(outputs, [id](const auto& output) {
            const auto* close = std::get_if<circud::EndpointClose>(&output);
            return close && close->id == id && close->code == 1001 &&
                close->reason == "package runtime stopped";
        });
    };

    auto second_sink = std::make_shared<CapturingEndpointSink>();
    auto second_request = endpoint_request(0,
        "/io.example.endpoint-generation/socket");
    second_request.protocol = circud::EndpointProtocol::websocket;
    second_request.deadline = std::chrono::steady_clock::now() + 10s;
    const auto second = core.endpoint_gateway().open(
        std::move(second_request), second_sink);
    const bool second_output = second_sink->wait_for(1, 5s);
    const bool second_accepted = second_output && accepted_as(second_sink,
        second, circud::EndpointProtocol::websocket);
    const bool stale_close = core.endpoint_gateway().close(
        first.session_id, 1000, "stale generation");
    const auto reservations_after_stale =
        core.endpoint_gateway().active_sessions();
    const bool current_close = core.endpoint_gateway().close(
        second.session_id, 1000, "current generation");
    const auto reservations_after_current =
        core.endpoint_gateway().active_sessions();

    const auto marker = package.data_dir / "closed-id";
    std::uint64_t delivered_id = 0;
    bool close_delivered = false;
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (std::ifstream input(marker); input &&
            (input >> delivered_id) && delivered_id == second.session_id) {
            close_delivered = true;
            break;
        }
        std::this_thread::sleep_for(5ms);
    }

    auto stop_sink = std::make_shared<CapturingEndpointSink>();
    auto stop_request = endpoint_request(0,
        "/io.example.endpoint-generation/socket");
    stop_request.protocol = circud::EndpointProtocol::websocket;
    stop_request.deadline = std::chrono::steady_clock::now() + 10s;
    const auto stop_session = core.endpoint_gateway().open(
        std::move(stop_request), stop_sink);
    const bool stop_output = stop_sink->wait_for(1, 5s);
    const bool stop_accepted = stop_output && accepted_as(stop_sink,
        stop_session, circud::EndpointProtocol::websocket);
    loop.stop();
    const auto stop_outputs = stop_sink->outputs();

    circud::test::require(first && first_accepted &&
            sse && sse_accepted && pending && first_reservations == 3,
        "first runtime generation did not reserve all endpoint protocols");
    circud::test::require(reservations_after_reload == 0,
        "reload retained the old runtime endpoint reservation");
    circud::test::require(released(first_outputs, first.session_id),
        "reload did not close the old WebSocket network session");
    circud::test::require(released(sse_outputs, sse.session_id),
        "reload did not close the old SSE network session");
    circud::test::require(released(pending_outputs, pending.session_id),
        "reload did not close the old pending HTTP network session");
    circud::test::require(second && second_accepted &&
            second.session_id != first.session_id,
        "replacement runtime did not receive a distinct endpoint session");
    circud::test::require(!stale_close && reservations_after_stale == 1,
        "stale close affected the replacement runtime reservation");
    circud::test::require(current_close && reservations_after_current == 0,
        "reliably accepted close did not release its gateway reservation");
    circud::test::require(close_delivered,
        "current-generation close was not delivered to its package runtime");
    circud::test::require(stop_session && stop_accepted &&
            released(stop_outputs, stop_session.session_id),
        "daemon stop did not close the active endpoint network session");
}
