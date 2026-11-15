#include "host_module.hpp"

#include "message_clone.h"
#include "tjs.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace circud::host {
    namespace {

        struct PendingOutbound {
            JSValue resolve = JS_UNDEFINED;
            JSValue reject = JS_UNDEFINED;
        };

        struct AbortListener {
            JSValue callback = JS_UNDEFINED;
            bool capture = false;
        };

        struct AbortSignalData {
            bool aborted = false;
            JSValue reason = JS_UNDEFINED;
            JSValue onabort = JS_NULL;
            std::vector<AbortListener> listeners;
        };

        constexpr std::size_t kMaxAbortListeners = 64;
        thread_local JSClassID abort_signal_class_id = 0;

        struct IncomingState;
        struct EndpointState;

        struct Binding {
            explicit Binding(BindingConfig value) : config(std::move(value)) {}
            BindingConfig config;
            JSContext* context = nullptr;
            bool closing = false;
            std::uint64_t next_outbound = 1;
            std::unordered_map<std::uint64_t, PendingOutbound> outbound;
            std::unordered_map<std::uint64_t, std::shared_ptr<IncomingState>> incoming;
            std::unordered_map<std::uint64_t, std::shared_ptr<EndpointState>> endpoints;
        };

        struct IncomingState {
            std::weak_ptr<Binding> binding;
            RuntimeCall call;
            JSValue signal = JS_UNDEFINED;
            bool active = true;
            bool responded = false;
            bool finished = false;
        };

        struct EndpointState {
            std::weak_ptr<Binding> binding;
            EndpointRequest request;
            std::shared_ptr<EndpointSink> sink;
            JSValue signal = JS_UNDEFINED;
            JSValue handle = JS_UNDEFINED;
            EndpointProtocol accepted_protocol = EndpointProtocol::http;
            bool active = true;
            bool responded = false;
            bool accepted = false;
            bool closing = false;
            bool finished = false;
        };

        class EndpointSelectionReservation {
        public:
            explicit EndpointSelectionReservation(
                std::shared_ptr<EndpointState> state)
                : state_(std::move(state)) {
                state_->responded = true;
            }

            ~EndpointSelectionReservation() {
                if (!committed_ && !state_->finished)
                    state_->responded = false;
            }

            void commit() noexcept { committed_ = true; }

        private:
            std::shared_ptr<EndpointState> state_;
            bool committed_ = false;
        };

        std::mutex bindings_mutex;
        std::unordered_map<TJSRuntime*, std::shared_ptr<Binding>> bindings;

        void abort_signal_finalizer(JSRuntime* runtime, JSValue value) {
            auto* signal = static_cast<AbortSignalData*>(
                JS_GetOpaque(value, abort_signal_class_id));
            if (!signal) return;
            JS_FreeValueRT(runtime, signal->reason);
            JS_FreeValueRT(runtime, signal->onabort);
            for (auto& listener : signal->listeners)
                JS_FreeValueRT(runtime, listener.callback);
            delete signal;
        }

        void abort_signal_mark(JSRuntime* runtime, JSValue value,
            JS_MarkFunc* mark) {
            auto* signal = static_cast<AbortSignalData*>(
                JS_GetOpaque(value, abort_signal_class_id));
            if (!signal) return;
            JS_MarkValue(runtime, signal->reason, mark);
            JS_MarkValue(runtime, signal->onabort, mark);
            for (const auto& listener : signal->listeners)
                JS_MarkValue(runtime, listener.callback, mark);
        }

        JSClassDef abort_signal_class{
            "AbortSignal",
            abort_signal_finalizer,
            abort_signal_mark,
            nullptr,
            nullptr,
        };

        AbortSignalData* get_abort_signal(JSContext* ctx,
            JSValueConst value) {
            return static_cast<AbortSignalData*>(
                JS_GetOpaque2(ctx, value, abort_signal_class_id));
        }

        JSValue abort_signal_constructor(JSContext* ctx, JSValueConst,
            int, JSValueConst*) {
            return JS_ThrowTypeError(ctx,
                "AbortSignal cannot be constructed directly");
        }

        JSValue abort_signal_get_aborted(JSContext* ctx,
            JSValueConst this_value) {
            auto* signal = get_abort_signal(ctx, this_value);
            return signal ? JS_NewBool(ctx, signal->aborted) : JS_EXCEPTION;
        }

        JSValue abort_signal_get_reason(JSContext* ctx,
            JSValueConst this_value) {
            auto* signal = get_abort_signal(ctx, this_value);
            return signal ? JS_DupValue(ctx, signal->reason) : JS_EXCEPTION;
        }

        JSValue abort_signal_get_onabort(JSContext* ctx,
            JSValueConst this_value) {
            auto* signal = get_abort_signal(ctx, this_value);
            return signal ? JS_DupValue(ctx, signal->onabort) : JS_EXCEPTION;
        }

        JSValue abort_signal_set_onabort(JSContext* ctx,
            JSValueConst this_value, JSValueConst callback) {
            auto* signal = get_abort_signal(ctx, this_value);
            if (!signal) return JS_EXCEPTION;
            if (!JS_IsNull(callback) && !JS_IsUndefined(callback) &&
                !JS_IsFunction(ctx, callback))
                return JS_ThrowTypeError(ctx,
                    "AbortSignal.onabort must be a function or null");
            JSValue replacement = JS_IsFunction(ctx, callback)
                ? JS_DupValue(ctx, callback)
                : JS_NULL;
            JS_FreeValue(ctx, signal->onabort);
            signal->onabort = replacement;
            return JS_UNDEFINED;
        }

        bool read_listener_capture(JSContext* ctx, int argc,
            JSValueConst* argv, bool& capture) {
            capture = false;
            if (argc < 3 || JS_IsUndefined(argv[2]) || JS_IsNull(argv[2]))
                return true;
            if (!JS_IsObject(argv[2])) {
                const int converted = JS_ToBool(ctx, argv[2]);
                if (converted < 0) return false;
                capture = converted != 0;
                return true;
            }
            JSValue value = JS_GetPropertyStr(ctx, argv[2], "capture");
            if (JS_IsException(value)) return false;
            const int converted = JS_ToBool(ctx, value);
            JS_FreeValue(ctx, value);
            if (converted < 0) return false;
            capture = converted != 0;
            return true;
        }

        bool is_abort_event_type(JSContext* ctx, int argc,
            JSValueConst* argv, bool& is_abort) {
            if (argc < 1)
                return JS_ThrowTypeError(ctx,
                    "event type is required"), false;
            size_t length = 0;
            const char* text = JS_ToCStringLen(ctx, &length, argv[0]);
            if (!text) return false;
            is_abort = std::string_view(text, length) == "abort";
            JS_FreeCString(ctx, text);
            return true;
        }

        JSValue abort_signal_add_event_listener(JSContext* ctx,
            JSValueConst this_value, int argc, JSValueConst* argv) {
            auto* signal = get_abort_signal(ctx, this_value);
            if (!signal) return JS_EXCEPTION;
            bool is_abort = false;
            if (!is_abort_event_type(ctx, argc, argv, is_abort))
                return JS_EXCEPTION;
            if (!is_abort || argc < 2 || JS_IsNull(argv[1]) ||
                JS_IsUndefined(argv[1]) || signal->aborted)
                return JS_UNDEFINED;
            if (!JS_IsFunction(ctx, argv[1]) && !JS_IsObject(argv[1]))
                return JS_ThrowTypeError(ctx,
                    "event listener must be a function or object");
            bool capture = false;
            if (!read_listener_capture(ctx, argc, argv, capture))
                return JS_EXCEPTION;
            const auto duplicate = std::ranges::find_if(signal->listeners,
                [&](const AbortListener& listener) {
                    return listener.capture == capture &&
                        JS_IsStrictEqual(ctx, listener.callback, argv[1]);
                });
            if (duplicate != signal->listeners.end()) return JS_UNDEFINED;
            if (signal->listeners.size() >= kMaxAbortListeners)
                return JS_ThrowRangeError(ctx,
                    "too many AbortSignal listeners");
            signal->listeners.push_back(
                AbortListener{ JS_DupValue(ctx, argv[1]), capture });
            return JS_UNDEFINED;
        }

        JSValue abort_signal_remove_event_listener(JSContext* ctx,
            JSValueConst this_value, int argc, JSValueConst* argv) {
            auto* signal = get_abort_signal(ctx, this_value);
            if (!signal) return JS_EXCEPTION;
            bool is_abort = false;
            if (!is_abort_event_type(ctx, argc, argv, is_abort))
                return JS_EXCEPTION;
            if (!is_abort || argc < 2) return JS_UNDEFINED;
            bool capture = false;
            if (!read_listener_capture(ctx, argc, argv, capture))
                return JS_EXCEPTION;
            for (auto iterator = signal->listeners.begin();
                 iterator != signal->listeners.end(); ++iterator) {
                if (iterator->capture == capture &&
                    JS_IsStrictEqual(ctx, iterator->callback, argv[1])) {
                    JS_FreeValue(ctx, iterator->callback);
                    signal->listeners.erase(iterator);
                    break;
                }
            }
            return JS_UNDEFINED;
        }

        JSValue abort_signal_throw_if_aborted(JSContext* ctx,
            JSValueConst this_value, int, JSValueConst*) {
            auto* signal = get_abort_signal(ctx, this_value);
            if (!signal) return JS_EXCEPTION;
            if (!signal->aborted) return JS_UNDEFINED;
            return JS_Throw(ctx, JS_DupValue(ctx, signal->reason));
        }

        const JSCFunctionListEntry abort_signal_prototype[] = {
            JS_CGETSET_DEF("aborted", abort_signal_get_aborted, nullptr),
            JS_CGETSET_DEF("reason", abort_signal_get_reason, nullptr),
            JS_CGETSET_DEF("onabort", abort_signal_get_onabort,
                abort_signal_set_onabort),
            JS_CFUNC_DEF("addEventListener", 2,
                abort_signal_add_event_listener),
            JS_CFUNC_DEF("removeEventListener", 2,
                abort_signal_remove_event_listener),
            JS_CFUNC_DEF("throwIfAborted", 0,
                abort_signal_throw_if_aborted),
        };

        bool ensure_abort_signal_class(JSContext* ctx) {
            auto* runtime = JS_GetRuntime(ctx);
            if (abort_signal_class_id != 0 &&
                JS_IsRegisteredClass(runtime, abort_signal_class_id))
                return true;
            abort_signal_class_id = 0;
            JS_NewClassID(runtime, &abort_signal_class_id);
            if (abort_signal_class_id == 0 ||
                JS_NewClass(runtime, abort_signal_class_id,
                    &abort_signal_class) < 0)
                return false;
            JSValue prototype = JS_NewObject(ctx);
            if (JS_IsException(prototype)) return false;
            if (JS_SetPropertyFunctionList(ctx, prototype,
                    abort_signal_prototype,
                    static_cast<int>(std::size(abort_signal_prototype))) < 0) {
                JS_FreeValue(ctx, prototype);
                return false;
            }
            JS_SetClassProto(ctx, abort_signal_class_id, prototype);
            return true;
        }

        JSValue make_abort_signal(JSContext* ctx) {
            if (!ensure_abort_signal_class(ctx)) return JS_EXCEPTION;
            JSValue result = JS_NewObjectClass(ctx, abort_signal_class_id);
            if (JS_IsException(result)) return result;
            auto signal = std::make_unique<AbortSignalData>();
            if (JS_SetOpaque(result, signal.get()) < 0) {
                JS_FreeValue(ctx, result);
                return JS_ThrowInternalError(ctx,
                    "cannot initialize AbortSignal");
            }
            (void)signal.release();
            return result;
        }

        JSValue make_abort_signal_constructor(JSContext* ctx) {
            if (!ensure_abort_signal_class(ctx)) return JS_EXCEPTION;
            JSValue prototype =
                JS_GetClassProto(ctx, abort_signal_class_id);
            if (JS_IsException(prototype)) return prototype;
            JSValue constructor = JS_NewCFunction2(ctx,
                abort_signal_constructor, "AbortSignal", 0,
                JS_CFUNC_constructor, 0);
            if (JS_IsException(constructor) ||
                JS_SetConstructor(ctx, constructor, prototype) < 0) {
                JS_FreeValue(ctx, prototype);
                JS_FreeValue(ctx, constructor);
                return JS_EXCEPTION;
            }
            JS_FreeValue(ctx, prototype);
            return constructor;
        }

        JSValue make_abort_reason(JSContext* ctx, std::string_view message) {
            JSValue reason = JS_NewError(ctx);
            if (JS_IsException(reason)) return reason;
            JS_SetPropertyStr(ctx, reason, "name",
                JS_NewString(ctx, "AbortError"));
            JS_SetPropertyStr(ctx, reason, "message",
                JS_NewStringLen(ctx, message.data(), message.size()));
            return reason;
        }

        void discard_callback_result(JSContext* ctx, JSValue result) {
            if (JS_IsException(result)) TJS_DumpException(ctx);
            else JS_FreeValue(ctx, result);
        }

        void invoke_abort_listener(JSContext* ctx, JSValueConst signal,
            JSValueConst listener, JSValueConst event) {
            if (JS_IsFunction(ctx, listener)) {
                JSValue argument = JS_DupValue(ctx, event);
                JSValue result = JS_Call(ctx, listener, signal, 1, &argument);
                JS_FreeValue(ctx, argument);
                discard_callback_result(ctx, result);
                return;
            }
            JSValue callback = JS_GetPropertyStr(ctx, listener, "handleEvent");
            if (JS_IsException(callback)) {
                discard_callback_result(ctx, callback);
                return;
            }
            if (JS_IsFunction(ctx, callback)) {
                JSValue argument = JS_DupValue(ctx, event);
                JSValue result = JS_Call(
                    ctx, callback, listener, 1, &argument);
                JS_FreeValue(ctx, argument);
                discard_callback_result(ctx, result);
            }
            JS_FreeValue(ctx, callback);
        }

        void abort_signal(JSContext* ctx, JSValueConst value,
            std::string_view message) {
            auto* signal = static_cast<AbortSignalData*>(
                JS_GetOpaque(value, abort_signal_class_id));
            if (!signal || signal->aborted) return;
            JSValue reason = make_abort_reason(ctx, message);
            if (JS_IsException(reason)) {
                TJS_DumpException(ctx);
                reason = JS_UNDEFINED;
            }
            signal->aborted = true;
            JS_FreeValue(ctx, signal->reason);
            signal->reason = reason;

            // Event handler attributes remain observable after dispatch, as
            // they do on the platform AbortSignal. Keep the signal-owned
            // reference and hold a temporary duplicate across user code,
            // which may replace onabort while it is running.
            JSValue onabort = JS_DupValue(ctx, signal->onabort);
            auto listeners = std::move(signal->listeners);
            signal->listeners.clear();

            JSValue event = JS_NewObject(ctx);
            if (!JS_IsException(event)) {
                JS_SetPropertyStr(ctx, event, "type",
                    JS_NewString(ctx, "abort"));
                JS_SetPropertyStr(ctx, event, "target",
                    JS_DupValue(ctx, value));
                JS_SetPropertyStr(ctx, event, "currentTarget",
                    JS_DupValue(ctx, value));
                if (JS_IsFunction(ctx, onabort))
                    invoke_abort_listener(ctx, value, onabort, event);
                for (const auto& listener : listeners)
                    invoke_abort_listener(
                        ctx, value, listener.callback, event);
                JS_FreeValue(ctx, event);
            }
            else {
                TJS_DumpException(ctx);
            }
            JS_FreeValue(ctx, onabort);
            for (auto& listener : listeners)
                JS_FreeValue(ctx, listener.callback);
        }

        const char* error_code(CallErrorCode value) {
            switch (value) {
            case CallErrorCode::not_found: return "not_found";
            case CallErrorCode::unavailable: return "unavailable";
            case CallErrorCode::package_calls_disabled:
                return "package_calls_disabled";
            case CallErrorCode::unhandled_call: return "unhandled_call";
            case CallErrorCode::overloaded: return "overloaded";
            case CallErrorCode::timeout: return "timeout";
            case CallErrorCode::cancelled: return "cancelled";
            case CallErrorCode::remote_error: return "remote_error";
            }
            return "remote_error";
        }

        std::string json_text(JSContext* ctx, JSValueConst value) {
            JSValue encoded = JS_JSONStringify(ctx, value, JS_UNDEFINED, JS_UNDEFINED);
            if (JS_IsException(encoded)) return {};
            if (JS_IsUndefined(encoded)) {
                JS_FreeValue(ctx, encoded);
                JS_ThrowTypeError(ctx, "value is not JSON serializable");
                return {};
            }
            const char* text = JS_ToCString(ctx, encoded);
            if (!text) {
                JS_FreeValue(ctx, encoded);
                return {};
            }
            std::string result(text);
            JS_FreeCString(ctx, text);
            JS_FreeValue(ctx, encoded);
            return result;
        }

        const char* endpoint_protocol_name(EndpointProtocol protocol) {
            switch (protocol) {
            case EndpointProtocol::http: return "http";
            case EndpointProtocol::sse: return "sse";
            case EndpointProtocol::websocket: return "websocket";
            }
            return "http";
        }

        EndpointBytes bytes_from_string(std::string_view text) {
            EndpointBytes bytes;
            bytes.reserve(text.size());
            for (const unsigned char character : text)
                bytes.push_back(static_cast<std::byte>(character));
            return bytes;
        }

        JSValue bytes_to_js(JSContext* ctx, const EndpointBytes& bytes) {
            return JS_NewUint8ArrayCopy(ctx,
                reinterpret_cast<const std::uint8_t*>(bytes.data()),
                bytes.size());
        }

        bool copy_js_bytes(JSContext* ctx, JSValueConst value,
            bool allow_json, EndpointBytes& output, bool* binary = nullptr,
            std::size_t max_bytes = kMaxEndpointEnvelopeBytes) {
            if (binary) *binary = false;
            if (JS_IsUndefined(value) || JS_IsNull(value)) {
                output.clear();
                return true;
            }
            if (JS_IsString(value)) {
                size_t length = 0;
                const char* text = JS_ToCStringLen(ctx, &length, value);
                if (!text) return false;
                if (length > max_bytes) {
                    JS_FreeCString(ctx, text);
                    JS_ThrowRangeError(ctx,
                        "endpoint payload exceeds configured limit");
                    return false;
                }
                output.resize(length);
                if (length != 0) std::memcpy(output.data(), text, length);
                JS_FreeCString(ctx, text);
                return true;
            }

            JSValue buffer = JS_UNDEFINED;
            size_t offset = 0;
            size_t length = 0;
            if (JS_IsArrayBuffer(value)) {
                buffer = JS_DupValue(ctx, value);
                const auto* data = JS_GetArrayBuffer(ctx, &length, buffer);
                if (!data && length != 0) {
                    JS_FreeValue(ctx, buffer);
                    return false;
                }
            }
            else if (JS_GetTypedArrayType(value) >= 0) {
                size_t bytes_per_element = 0;
                buffer = JS_GetTypedArrayBuffer(ctx, value, &offset, &length,
                    &bytes_per_element);
                if (JS_IsException(buffer)) return false;
            }
            else if (JS_IsDataView(value)) {
                buffer = JS_GetDataViewBuffer(ctx, value, &offset, &length);
                if (JS_IsException(buffer)) return false;
            }

            if (!JS_IsUndefined(buffer)) {
                size_t buffer_length = 0;
                const auto* data = JS_GetArrayBuffer(ctx, &buffer_length, buffer);
                if (!data || offset > buffer_length ||
                    length > buffer_length - offset) {
                    JS_FreeValue(ctx, buffer);
                    JS_ThrowTypeError(ctx,
                        "endpoint payload has a detached buffer");
                    return false;
                }
                if (length > max_bytes) {
                    JS_FreeValue(ctx, buffer);
                    JS_ThrowRangeError(ctx,
                        "endpoint payload exceeds configured limit");
                    return false;
                }
                output.resize(length);
                if (length != 0)
                    std::memcpy(output.data(), data + offset, length);
                JS_FreeValue(ctx, buffer);
                if (binary) *binary = true;
                return true;
            }

            if (!allow_json) {
                JS_ThrowTypeError(ctx,
                    "endpoint payload must be a string or byte buffer");
                return false;
            }
            auto encoded = json_text(ctx, value);
            if (JS_HasException(ctx)) return false;
            if (encoded.size() > max_bytes) {
                JS_ThrowRangeError(ctx,
                    "endpoint payload exceeds configured limit");
                return false;
            }
            output = bytes_from_string(encoded);
            return true;
        }

        bool has_header(const EndpointHeaders& headers,
            std::string_view wanted) {
            for (const auto& [name, value] : headers) {
                (void)value;
                if (name.size() != wanted.size()) continue;
                bool same = true;
                for (std::size_t index = 0; index < name.size(); ++index) {
                    const auto left = static_cast<unsigned char>(name[index]);
                    const auto right =
                        static_cast<unsigned char>(wanted[index]);
                    if (std::tolower(left) != std::tolower(right)) {
                        same = false;
                        break;
                    }
                }
                if (same) return true;
            }
            return false;
        }

        bool endpoint_header_token_character(unsigned char character) noexcept {
            if ((character >= 'a' && character <= 'z') ||
                (character >= 'A' && character <= 'Z') ||
                (character >= '0' && character <= '9'))
                return true;
            constexpr std::string_view punctuation = "!#$%&'*+-.^_`|~";
            return punctuation.find(static_cast<char>(character)) !=
                std::string_view::npos;
        }

        bool valid_endpoint_header_name(std::string_view name) noexcept {
            return !name.empty() &&
                std::ranges::all_of(name, [](unsigned char character) {
                    return endpoint_header_token_character(character);
                });
        }

        bool valid_endpoint_header_value(std::string_view value) noexcept {
            return std::ranges::all_of(value, [](unsigned char character) {
                return character != '\r' && character != '\n' &&
                    character != '\0' &&
                    (character == '\t' || character >= 0x20);
                });
        }

        bool endpoint_header_name_is(std::string_view name,
            std::string_view expected) noexcept {
            if (name.size() != expected.size()) return false;
            for (std::size_t index = 0; index < name.size(); ++index) {
                const auto left = static_cast<unsigned char>(name[index]);
                const auto right = static_cast<unsigned char>(expected[index]);
                if (std::tolower(left) != std::tolower(right)) return false;
            }
            return true;
        }

        bool forbidden_endpoint_response_header(
            std::string_view name) noexcept {
            constexpr std::string_view forbidden[]{
                "connection", "content-length", "keep-alive",
                "proxy-authenticate", "proxy-authorization",
                "proxy-connection", "te", "trailer",
                "transfer-encoding", "upgrade",
            };
            return std::ranges::any_of(forbidden,
                [name](std::string_view candidate) {
                    return endpoint_header_name_is(name, candidate);
                });
        }

        bool read_headers(JSContext* ctx, JSValueConst value,
            EndpointHeaders& output) {
            if (JS_IsUndefined(value) || JS_IsNull(value)) return true;
            if (!JS_IsObject(value)) {
                JS_ThrowTypeError(ctx, "endpoint headers must be an object");
                return false;
            }
            JSPropertyEnum* properties = nullptr;
            uint32_t count = 0;
            if (JS_GetOwnPropertyNames(ctx, &properties, &count, value,
                    JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
                return false;
            const auto free_properties = [&] {
                for (uint32_t index = 0; index < count; ++index)
                    JS_FreeAtom(ctx, properties[index].atom);
                js_free(ctx, properties);
            };
            if (count > 64) {
                free_properties();
                JS_ThrowRangeError(ctx, "too many endpoint headers");
                return false;
            }
            std::size_t total = 0;
            for (uint32_t index = 0; index < count; ++index) {
                size_t name_length = 0;
                const char* name = JS_AtomToCStringLen(
                    ctx, &name_length, properties[index].atom);
                if (!name) {
                    free_properties();
                    return false;
                }
                const std::string_view name_view(name, name_length);
                if (!valid_endpoint_header_name(name_view) ||
                    forbidden_endpoint_response_header(name_view)) {
                    JS_FreeCString(ctx, name);
                    free_properties();
                    JS_ThrowTypeError(ctx,
                        "invalid or forbidden endpoint response header");
                    return false;
                }
                JSValue item = JS_GetProperty(ctx, value,
                    properties[index].atom);
                if (JS_IsException(item)) {
                    JS_FreeCString(ctx, name);
                    JS_FreeValue(ctx, item);
                    free_properties();
                    return false;
                }
                size_t text_length = 0;
                const char* text = JS_ToCStringLen(
                    ctx, &text_length, item);
                if (!text) {
                    JS_FreeCString(ctx, name);
                    JS_FreeValue(ctx, item);
                    free_properties();
                    return false;
                }
                const std::string_view text_view(text, text_length);
                if (!valid_endpoint_header_value(text_view)) {
                    JS_FreeCString(ctx, name);
                    JS_FreeCString(ctx, text);
                    JS_FreeValue(ctx, item);
                    free_properties();
                    JS_ThrowTypeError(ctx,
                        "invalid endpoint response header value");
                    return false;
                }
                if (name_length > kMaxEndpointEnvelopeBytes ||
                    text_length > kMaxEndpointEnvelopeBytes - name_length ||
                    total > kMaxEndpointEnvelopeBytes -
                        name_length - text_length) {
                    JS_FreeCString(ctx, name);
                    JS_FreeCString(ctx, text);
                    JS_FreeValue(ctx, item);
                    free_properties();
                    JS_ThrowRangeError(ctx,
                        "endpoint headers exceed 64 KiB");
                    return false;
                }
                total += name_length + text_length;
                output.emplace_back(
                    std::string(name_view), std::string(text_view));
                JS_FreeCString(ctx, name);
                JS_FreeCString(ctx, text);
                JS_FreeValue(ctx, item);
            }
            free_properties();
            return true;
        }

        std::string take_exception(JSContext* ctx, std::string* name = nullptr) {
            JSValue exception = JS_GetException(ctx);
            JSValue message_value = JS_GetPropertyStr(ctx, exception, "message");
            JSValue name_value = JS_GetPropertyStr(ctx, exception, "name");
            const char* message = JS_ToCString(ctx, message_value);
            const char* exception_name = JS_ToCString(ctx, name_value);
            std::string result = message ? message : "JavaScript exception";
            if (name) *name = exception_name ? exception_name : "Error";
            if (message) JS_FreeCString(ctx, message);
            if (exception_name) JS_FreeCString(ctx, exception_name);
            JS_FreeValue(ctx, message_value);
            JS_FreeValue(ctx, name_value);
            JS_FreeValue(ctx, exception);
            return result;
        }

        std::shared_ptr<Binding> find_binding(JSContext* ctx) {
            std::lock_guard lock(bindings_mutex);
            auto found = bindings.find(TJS_GetRuntime(ctx));
            return found == bindings.end() ? nullptr : found->second;
        }

        JSValue make_error(JSContext* ctx, const CallError& error) {
            JSValue value = JS_NewError(ctx);
            JS_SetPropertyStr(ctx, value, "message",
                JS_NewString(ctx, error.message.c_str()));
            JS_SetPropertyStr(ctx, value, "code",
                JS_NewString(ctx, error_code(error.code)));
            if (!error.remote_name.empty())
                JS_SetPropertyStr(ctx, value, "remoteName",
                    JS_NewString(ctx, error.remote_name.c_str()));
            if (!error.runtime_state.empty())
                JS_SetPropertyStr(ctx, value, "runtimeState",
                    JS_NewString(ctx, error.runtime_state.c_str()));
            return value;
        }

        void finish_incoming(const std::shared_ptr<IncomingState>& state,
            CallResult result) {
            if (state->finished) return;
            state->finished = true;
            state->active = false;
            if (auto binding = state->binding.lock()) {
                binding->incoming.erase(state->call.id);
                if (!JS_IsUndefined(state->signal)) {
                    JS_FreeValue(binding->context, state->signal);
                    state->signal = JS_UNDEFINED;
                }
            }
            auto completion = std::move(state->call.complete);
            if (completion) completion(std::move(result));
        }

        CallResult clone_response(JSContext* ctx, JSValueConst value, bool wants_json) {
            if (wants_json) {
                auto encoded = json_text(ctx, value);
                if (JS_HasException(ctx)) {
                    std::string name;
                    auto message = take_exception(ctx, &name);
                    auto result = CallResult::failure(CallErrorCode::remote_error,
                        std::move(message));
                    result.error->remote_name = std::move(name);
                    return result;
                }
                if (encoded.size() > kMaxEnvelopeBytes)
                    return CallResult::failure(CallErrorCode::remote_error,
                        "response exceeds 64 KiB envelope limit");
                return CallResult::success_json(std::move(encoded));
            }
            TJSMessageClone* clone =
                tjs__message_clone_prepare(ctx, value, JS_UNDEFINED);
            if (!clone) {
                std::string name;
                auto message = take_exception(ctx, &name);
                auto result = CallResult::failure(CallErrorCode::remote_error,
                    std::move(message));
                result.error->remote_name = std::move(name);
                return result;
            }
            if (tjs__message_clone_size(clone) > kMaxEnvelopeBytes) {
                tjs__message_clone_abort(ctx, clone);
                return CallResult::failure(CallErrorCode::remote_error,
                    "response exceeds 64 KiB envelope limit");
            }
            if (tjs__message_clone_transfer_count(clone) > kMaxBulkReferences) {
                tjs__message_clone_abort(ctx, clone);
                return CallResult::failure(CallErrorCode::remote_error,
                    "response exceeds bulk reference limit");
            }
            if (tjs__message_clone_commit(ctx, clone) < 0) {
                std::string name;
                auto message = take_exception(ctx, &name);
                tjs__message_clone_free(clone);
                auto result = CallResult::failure(CallErrorCode::remote_error,
                    std::move(message));
                result.error->remote_name = std::move(name);
                return result;
            }
            return CallResult::success(ClonePtr(clone));
        }

        struct IncomingCallback {
            std::shared_ptr<IncomingState> state;
            bool reject = false;
        };

        void free_incoming_callback(void* opaque) {
            delete static_cast<IncomingCallback*>(opaque);
        }

        JSValue settle_incoming(JSContext* ctx, JSValueConst, int argc,
            JSValueConst* argv, int, void* opaque) {
            auto* callback = static_cast<IncomingCallback*>(opaque);
            auto state = callback->state;
            if (state->finished) return JS_UNDEFINED;
            try {
                if (callback->reject) {
                    JSValue reason = argc ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
                    JSValue message_value = JS_GetPropertyStr(ctx, reason, "message");
                    JSValue name_value = JS_GetPropertyStr(ctx, reason, "name");
                    const char* message = JS_ToCString(ctx, message_value);
                    const char* name = JS_ToCString(ctx, name_value);
                    auto result = CallResult::failure(
                        CallErrorCode::remote_error,
                        message ? message : "management promise rejected");
                    if (name) result.error->remote_name = name;
                    if (message) JS_FreeCString(ctx, message);
                    if (name) JS_FreeCString(ctx, name);
                    JS_FreeValue(ctx, message_value);
                    JS_FreeValue(ctx, name_value);
                    JS_FreeValue(ctx, reason);
                    finish_incoming(state, std::move(result));
                }
                else {
                    finish_incoming(
                        state, clone_response(ctx, argc ? argv[0] : JS_UNDEFINED,
                            state->call.wants_json));
                }
            }
            catch (const std::exception& error) {
                finish_incoming(state, CallResult::failure(
                    CallErrorCode::remote_error, error.what()));
            }
            return JS_UNDEFINED;
        }

        JSValue respond_with(JSContext* ctx, JSValueConst, int argc,
            JSValueConst* argv, int, void* opaque) {
            auto* state = static_cast<std::shared_ptr<IncomingState>*>(opaque);
            if (!(*state)->active)
                return JS_ThrowTypeError(ctx,
                    "respondWith() must be called during dispatch");
            if ((*state)->responded)
                return JS_ThrowTypeError(ctx, "respondWith() may only be called once");
            (*state)->responded = true;
            JSValueConst value = argc ? argv[0] : JS_UNDEFINED;
            if (!JS_IsPromise(value)) {
                try {
                    finish_incoming(*state,
                        clone_response(ctx, value, (*state)->call.wants_json));
                }
                catch (const std::exception& error) {
                    finish_incoming(*state, CallResult::failure(
                        CallErrorCode::remote_error, error.what()));
                }
                return JS_UNDEFINED;
            }

            auto* fulfilled = new IncomingCallback{ *state, false };
            auto* rejected = new IncomingCallback{ *state, true };
            JSValue on_fulfilled = JS_NewCClosure(ctx, settle_incoming, "respondFulfilled",
                free_incoming_callback, 1, 0,
                fulfilled);
            JSValue on_rejected = JS_NewCClosure(ctx, settle_incoming, "respondRejected",
                free_incoming_callback, 1, 0,
                rejected);
            JSValue chained = JS_PromiseThen(ctx, value, on_fulfilled, on_rejected);
            JS_FreeValue(ctx, on_fulfilled);
            JS_FreeValue(ctx, on_rejected);
            if (JS_IsException(chained)) {
                std::string name;
                auto message = take_exception(ctx, &name);
                auto result = CallResult::failure(CallErrorCode::remote_error,
                    std::move(message));
                result.error->remote_name = std::move(name);
                finish_incoming(*state, std::move(result));
                return JS_EXCEPTION;
            }
            JS_FreeValue(ctx, chained);
            return JS_UNDEFINED;
        }

        void free_respond_state(void* opaque) {
            delete static_cast<std::shared_ptr<IncomingState>*>(opaque);
        }

        bool emit_endpoint(const std::shared_ptr<EndpointState>& state,
            EndpointOutput output) noexcept {
            if (!state->sink) return false;
            try {
                return state->sink->emit(std::move(output));
            }
            catch (...) {
                return false;
            }
        }

        void emit_sink_noexcept(const std::shared_ptr<EndpointSink>& sink,
            EndpointOutput output) noexcept {
            if (!sink) return;
            try {
                (void)sink->emit(std::move(output));
            }
            catch (...) {
                // Sinks must not be able to unwind through the package loop.
            }
        }

        void finish_endpoint(const std::shared_ptr<EndpointState>& state) {
            if (state->finished) return;
            state->finished = true;
            state->closing = true;
            state->active = false;
            if (auto binding = state->binding.lock()) {
                binding->endpoints.erase(state->request.id);
                if (!JS_IsUndefined(state->signal)) {
                    JS_FreeValue(binding->context, state->signal);
                    state->signal = JS_UNDEFINED;
                }
                if (!JS_IsUndefined(state->handle)) {
                    JS_FreeValue(binding->context, state->handle);
                    state->handle = JS_UNDEFINED;
                }
            }
            state->sink.reset();
        }

        void abort_endpoint(const std::shared_ptr<EndpointState>& state,
            std::string_view message) {
            if (auto binding = state->binding.lock();
                binding && !JS_IsUndefined(state->signal))
                abort_signal(binding->context, state->signal, message);
        }

        bool read_status(JSContext* ctx, JSValueConst object,
            unsigned default_status, unsigned minimum_status,
            unsigned maximum_status, unsigned& output) {
            output = default_status;
            if (!JS_IsObject(object)) return true;
            JSValue status = JS_GetPropertyStr(ctx, object, "status");
            if (JS_IsException(status)) return false;
            if (!JS_IsUndefined(status)) {
                double number = 0;
                if (!JS_IsNumber(status) ||
                    JS_ToFloat64(ctx, &number, status) < 0) {
                    JS_FreeValue(ctx, status);
                    JS_ThrowTypeError(ctx,
                        "endpoint status must be a number");
                    return false;
                }
                if (!std::isfinite(number) || std::trunc(number) != number ||
                    number < static_cast<double>(minimum_status) ||
                    number > static_cast<double>(maximum_status)) {
                    JS_FreeValue(ctx, status);
                    JS_ThrowRangeError(ctx,
                        "endpoint status must be an integer between %u and %u",
                        minimum_status, maximum_status);
                    return false;
                }
                output = static_cast<unsigned>(number);
            }
            JS_FreeValue(ctx, status);
            return true;
        }

        bool read_response_options(JSContext* ctx, JSValueConst object,
            unsigned default_status, unsigned minimum_status,
            unsigned maximum_status, unsigned& status,
            EndpointHeaders& headers) {
            if (!JS_IsObject(object)) {
                JS_ThrowTypeError(ctx, "endpoint response must be an object");
                return false;
            }
            if (!read_status(ctx, object, default_status, minimum_status,
                    maximum_status, status))
                return false;
            JSValue value = JS_GetPropertyStr(ctx, object, "headers");
            if (JS_IsException(value)) return false;
            const bool ok = read_headers(ctx, value, headers);
            JS_FreeValue(ctx, value);
            return ok;
        }

        bool valid_websocket_close_code(unsigned code) noexcept {
            if (code >= 3000 && code <= 4999) return true;
            return code == 1000 || code == 1001 || code == 1002 ||
                code == 1003 || (code >= 1007 && code <= 1013);
        }

        bool response_status_forbids_body(unsigned status,
            std::string_view request_method) noexcept {
            return status < 200 || status == 204 || status == 205 ||
                status == 304 ||
                (request_method == "CONNECT" && status < 300);
        }

        bool make_endpoint_response(JSContext* ctx,
            const std::shared_ptr<EndpointState>& state,
            JSValueConst value, EndpointResponse& response) {
            if (!JS_IsObject(value) || JS_IsPromise(value)) {
                JS_ThrowTypeError(ctx,
                    "endpoint response must resolve to a response object");
                return false;
            }
            response.id = state->request.id;
            if (!read_response_options(ctx, value, 200, 200, 599,
                    response.status, response.headers))
                return false;
            JSValue body = JS_GetPropertyStr(ctx, value, "body");
            if (JS_IsException(body)) return false;
            const bool json_body = !JS_IsUndefined(body) && !JS_IsNull(body) &&
                !JS_IsString(body) && !JS_IsArrayBuffer(body) &&
                JS_GetTypedArrayType(body) < 0 && !JS_IsDataView(body);
            const auto binding = state->binding.lock();
            const auto max_bytes = binding
                ? binding->config.max_endpoint_response_bytes
                : kMaxEndpointEnvelopeBytes;
            if (!copy_js_bytes(ctx, body, true, response.body, nullptr,
                    max_bytes)) {
                JS_FreeValue(ctx, body);
                return false;
            }
            JS_FreeValue(ctx, body);
            if (!response.body.empty() && response_status_forbids_body(
                    response.status, state->request.method)) {
                JS_ThrowTypeError(ctx,
                    "endpoint response status does not permit a body");
                return false;
            }
            if (state->request.method == "CONNECT" &&
                response.status >= 200 && response.status < 300) {
                JS_ThrowTypeError(ctx,
                    "successful CONNECT requires a tunnel API");
                return false;
            }
            if (json_body && !has_header(response.headers, "content-type"))
                response.headers.emplace_back("content-type",
                    "application/json; charset=utf-8");
            return true;
        }

        void fail_endpoint_response(const std::shared_ptr<EndpointState>& state,
            std::string message) {
            if (state->finished) return;
            const auto binding = state->binding.lock();
            const auto max_bytes = binding
                ? binding->config.max_endpoint_response_bytes
                : kMaxEndpointEnvelopeBytes;
            if (message.size() > max_bytes) message.resize(max_bytes);
            EndpointResponse response;
            response.id = state->request.id;
            response.status = 500;
            response.headers.emplace_back("content-type",
                "text/plain; charset=utf-8");
            response.body = bytes_from_string(message);
            (void)emit_endpoint(state, std::move(response));
            finish_endpoint(state);
        }

        struct EndpointResponseCallback {
            std::weak_ptr<EndpointState> state;
            bool reject = false;
        };

        void free_endpoint_response_callback(void* opaque) {
            delete static_cast<EndpointResponseCallback*>(opaque);
        }

        JSValue settle_endpoint_response(JSContext* ctx, JSValueConst,
            int argc, JSValueConst* argv, int, void* opaque) {
            auto* callback = static_cast<EndpointResponseCallback*>(opaque);
            const auto state = callback->state.lock();
            if (!state || state->finished) return JS_UNDEFINED;
            if (callback->reject) {
                (void)argc;
                (void)argv;
                fail_endpoint_response(state,
                    "endpoint response promise rejected");
                return JS_UNDEFINED;
            }

            EndpointResponse response;
            if (!make_endpoint_response(ctx, state,
                    argc != 0 ? argv[0]
                              : static_cast<JSValueConst>(JS_UNDEFINED),
                    response)) {
                auto message = JS_HasException(ctx)
                    ? take_exception(ctx)
                    : std::string("invalid asynchronous endpoint response");
                fail_endpoint_response(state, std::move(message));
                return JS_UNDEFINED;
            }
            (void)emit_endpoint(state, std::move(response));
            finish_endpoint(state);
            return JS_UNDEFINED;
        }

        JSValue endpoint_respond_with(JSContext* ctx, JSValueConst, int argc,
            JSValueConst* argv, int, void* opaque) {
            auto state = *static_cast<std::shared_ptr<EndpointState>*>(opaque);
            if (!state->active)
                return JS_ThrowTypeError(ctx,
                    "respondWith() must be called during endpoint dispatch");
            if (state->responded)
                return JS_ThrowTypeError(ctx,
                    "an endpoint may only be accepted or responded to once");
            if (state->request.protocol == EndpointProtocol::websocket)
                return JS_ThrowTypeError(ctx,
                    "WebSocket upgrades require acceptWebSocket()");
            if (argc < 1 || !JS_IsObject(argv[0]))
                return JS_ThrowTypeError(ctx,
                    "respondWith() requires a response object or Promise");

            // Reserve the one terminal choice before touching any user
            // getters, header conversions, or JSON serialization. Those can
            // execute JavaScript and must not be able to re-enter this event
            // and select a second response protocol.
            state->responded = true;
            if (JS_IsPromise(argv[0])) {
                auto* fulfilled = new EndpointResponseCallback{ state, false };
                auto* rejected = new EndpointResponseCallback{ state, true };
                JSValue on_fulfilled = JS_NewCClosure(ctx,
                    settle_endpoint_response, "endpointResponseFulfilled",
                    free_endpoint_response_callback, 1, 0, fulfilled);
                JSValue on_rejected = JS_NewCClosure(ctx,
                    settle_endpoint_response, "endpointResponseRejected",
                    free_endpoint_response_callback, 1, 0, rejected);
                JSValue chained = JS_PromiseThen(ctx, argv[0], on_fulfilled,
                    on_rejected);
                JS_FreeValue(ctx, on_fulfilled);
                JS_FreeValue(ctx, on_rejected);
                if (JS_IsException(chained)) {
                    const auto message = take_exception(ctx);
                    fail_endpoint_response(state, message);
                    return JS_UNDEFINED;
                }
                JS_FreeValue(ctx, chained);
                return JS_UNDEFINED;
            }

            EndpointResponse response;
            if (!make_endpoint_response(ctx, state, argv[0], response)) {
                const auto message = JS_HasException(ctx)
                    ? take_exception(ctx)
                    : std::string("invalid endpoint response");
                fail_endpoint_response(state, message);
                return JS_UNDEFINED;
            }
            (void)emit_endpoint(state, std::move(response));
            finish_endpoint(state);
            return JS_UNDEFINED;
        }

        JSValue endpoint_serve_static(JSContext* ctx, JSValueConst, int argc,
            JSValueConst* argv, int, void* opaque) {
            auto state = *static_cast<std::shared_ptr<EndpointState>*>(opaque);
            if (!state->active)
                return JS_ThrowTypeError(ctx,
                    "serveStatic() must be called during endpoint dispatch");
            if (state->responded)
                return JS_ThrowTypeError(ctx,
                    "an endpoint may only be served, accepted, or responded to once");
            if (state->request.protocol == EndpointProtocol::websocket)
                return JS_ThrowTypeError(ctx,
                    "WebSocket upgrades cannot serve static files");
            if (state->request.method == "CONNECT")
                return JS_ThrowTypeError(ctx,
                    "CONNECT cannot serve a static response");
            if (argc < 1 || !JS_IsString(argv[0]))
                return JS_ThrowTypeError(ctx,
                    "serveStatic() requires a relative path string");
            size_t length = 0;
            const char* path = JS_ToCStringLen(ctx, &length, argv[0]);
            if (!path) return JS_EXCEPTION;
            if (length == 0 || length > kMaxEndpointEnvelopeBytes) {
                JS_FreeCString(ctx, path);
                return JS_ThrowRangeError(ctx,
                    "static relative path must be 1..65536 bytes");
            }
            if (path[0] == '/') {
                JS_FreeCString(ctx, path);
                return JS_ThrowTypeError(ctx,
                    "serveStatic() path must be relative");
            }

            EndpointStatic output;
            output.id = state->request.id;
            output.relative_path.assign(path, length);
            JS_FreeCString(ctx, path);
            state->responded = true;
            (void)emit_endpoint(state, std::move(output));
            finish_endpoint(state);
            return JS_UNDEFINED;
        }

        JSValue endpoint_send(JSContext* ctx, JSValueConst, int argc,
            JSValueConst* argv, int, void* opaque) {
            auto state = *static_cast<std::shared_ptr<EndpointState>*>(opaque);
            if (state->finished || state->closing || !state->accepted ||
                !state->sink)
                return JS_FALSE;
            EndpointMessage message;
            message.id = state->request.id;
            bool binary = false;
            const auto binding = state->binding.lock();
            const auto max_bytes = binding
                ? binding->config.max_endpoint_message_bytes
                : kMaxEndpointEnvelopeBytes;
            if (!copy_js_bytes(ctx, argc ? argv[0] : JS_UNDEFINED, false,
                    message.data, &binary, max_bytes))
                return JS_EXCEPTION;
            if (state->accepted_protocol == EndpointProtocol::sse) {
                if (binary)
                    return JS_ThrowTypeError(ctx,
                        "SSE send() requires a pre-framed UTF-8 string");
                if (message.data.empty())
                    return JS_ThrowRangeError(ctx,
                        "SSE send() requires a non-empty pre-framed UTF-8 string");
                message.binary = false;
            }
            else {
                message.binary = binary;
            }
            const bool accepted = emit_endpoint(state, std::move(message));
            if (!accepted) {
                state->closing = true;
                abort_endpoint(state, "endpoint output was closed");
                finish_endpoint(state);
            }
            return JS_NewBool(ctx, accepted);
        }

        JSValue endpoint_close(JSContext* ctx, JSValueConst, int argc,
            JSValueConst* argv, int, void* opaque) {
            auto state = *static_cast<std::shared_ptr<EndpointState>*>(opaque);
            if (state->finished || state->closing || !state->accepted ||
                !state->sink)
                return JS_FALSE;
            EndpointClose close;
            close.id = state->request.id;
            if (argc >= 1 && !JS_IsUndefined(argv[0])) {
                double code = 0;
                if (!JS_IsNumber(argv[0]) ||
                    JS_ToFloat64(ctx, &code, argv[0]) < 0)
                    return JS_ThrowTypeError(ctx,
                        "endpoint close code must be a number");
                if (!std::isfinite(code) || std::trunc(code) != code ||
                    code < 1000 || code > 4999)
                    return JS_ThrowRangeError(ctx,
                        "endpoint close code must be an integer between 1000 and 4999");
                if (!valid_websocket_close_code(
                        static_cast<unsigned>(code)))
                    return JS_ThrowRangeError(ctx,
                        "endpoint close code is reserved and cannot be sent");
                close.code = static_cast<std::uint16_t>(code);
            }
            if (argc >= 2 && !JS_IsUndefined(argv[1])) {
                size_t length = 0;
                const char* reason = JS_ToCStringLen(ctx, &length, argv[1]);
                if (!reason) return JS_EXCEPTION;
                if (length > 123) {
                    JS_FreeCString(ctx, reason);
                    return JS_ThrowRangeError(ctx,
                        "endpoint close reason exceeds 123 bytes");
                }
                close.reason.assign(reason, length);
                JS_FreeCString(ctx, reason);
            }
            state->closing = true;
            abort_endpoint(state,
                close.reason.empty() ? std::string_view("endpoint closed")
                                     : std::string_view(close.reason));
            const bool accepted = emit_endpoint(state, std::move(close));
            finish_endpoint(state);
            return JS_NewBool(ctx, accepted);
        }

        void free_endpoint_state(void* opaque) {
            delete static_cast<std::shared_ptr<EndpointState>*>(opaque);
        }

        JSValue make_endpoint_handle(JSContext* ctx,
            const std::shared_ptr<EndpointState>& state) {
            JSValue handle = JS_NewObject(ctx);
            auto* send_state = new std::shared_ptr<EndpointState>(state);
            JS_SetPropertyStr(ctx, handle, "send",
                JS_NewCClosure(ctx, endpoint_send, "send", free_endpoint_state,
                    1, 0, send_state));
            auto* close_state = new std::shared_ptr<EndpointState>(state);
            JS_SetPropertyStr(ctx, handle, "close",
                JS_NewCClosure(ctx, endpoint_close, "close",
                    free_endpoint_state, 2, 0, close_state));
            JS_SetPropertyStr(ctx, handle, "id",
                JS_NewInt64(ctx, state->request.id));
            return handle;
        }

        JSValue endpoint_accept(JSContext* ctx, JSValueConst, int argc,
            JSValueConst* argv, int magic, void* opaque) {
            auto state = *static_cast<std::shared_ptr<EndpointState>*>(opaque);
            const auto protocol = magic == 0 ? EndpointProtocol::sse
                                             : EndpointProtocol::websocket;
            if (!state->active)
                return JS_ThrowTypeError(ctx,
                    "endpoint accept must be called during dispatch");
            if (state->responded)
                return JS_ThrowTypeError(ctx,
                    "an endpoint may only be accepted or responded to once");
            const bool protocol_matches = protocol == EndpointProtocol::sse
                ? state->request.protocol == EndpointProtocol::http ||
                    state->request.protocol == EndpointProtocol::sse
                : state->request.protocol == EndpointProtocol::websocket;
            if (!protocol_matches)
                return JS_ThrowTypeError(ctx,
                    protocol == EndpointProtocol::sse
                        ? "SSE requires an HTTP request"
                        : "request is not a WebSocket upgrade");
            if (protocol == EndpointProtocol::sse &&
                state->request.method != "GET")
                return JS_ThrowTypeError(ctx,
                    "SSE requires a GET request");
            const JSValueConst options =
                argc ? argv[0] : static_cast<JSValueConst>(JS_UNDEFINED);
            if (!JS_IsUndefined(options) && !JS_IsObject(options))
                return JS_ThrowTypeError(ctx,
                    "endpoint accept options must be an object");

            // Reading status, headers and subprotocol can invoke package
            // getters. Reserve the one terminal selection first so a getter
            // cannot select a second response. If option parsing throws, the
            // reservation rolls back and the listener may catch and respond.
            EndpointSelectionReservation reservation(state);

            EndpointAccept accept;
            accept.id = state->request.id;
            accept.protocol = protocol;
            if (!JS_IsUndefined(options) &&
                !read_response_options(ctx, options,
                    protocol == EndpointProtocol::sse ? 200 : 101,
                    protocol == EndpointProtocol::sse ? 200 : 101,
                    protocol == EndpointProtocol::sse ? 200 : 101,
                    accept.status, accept.headers))
                return JS_EXCEPTION;
            if (protocol == EndpointProtocol::sse) {
                if (!has_header(accept.headers, "content-type"))
                    accept.headers.emplace_back("content-type",
                        "text/event-stream; charset=utf-8");
                if (!has_header(accept.headers, "cache-control"))
                    accept.headers.emplace_back("cache-control", "no-cache");
            }
            else if (!JS_IsUndefined(options)) {
                JSValue selected =
                    JS_GetPropertyStr(ctx, options, "subprotocol");
                if (!JS_IsUndefined(selected)) {
                    const char* text = JS_ToCString(ctx, selected);
                    if (!text) {
                        JS_FreeValue(ctx, selected);
                        return JS_EXCEPTION;
                    }
                    accept.subprotocol = text;
                    JS_FreeCString(ctx, text);
                    if (std::find(state->request.offered_subprotocols.begin(),
                            state->request.offered_subprotocols.end(),
                            accept.subprotocol) ==
                        state->request.offered_subprotocols.end()) {
                        JS_FreeValue(ctx, selected);
                        return JS_ThrowRangeError(ctx,
                            "WebSocket subprotocol was not offered by client");
                    }
                }
                JS_FreeValue(ctx, selected);
            }

            state->accepted = true;
            state->accepted_protocol = protocol;
            JSValue handle = make_endpoint_handle(ctx, state);
            state->handle = JS_DupValue(ctx, handle);
            reservation.commit();
            if (!emit_endpoint(state, std::move(accept))) {
                state->closing = true;
                abort_endpoint(state, "endpoint accept was closed");
                finish_endpoint(state);
                JS_FreeValue(ctx, handle);
                return JS_NULL;
            }
            return handle;
        }

        void resolve_outbound(JSContext* ctx, const std::shared_ptr<Binding>& binding,
            std::uint64_t id, CallResult result) {
            auto found = binding->outbound.find(id);
            if (found == binding->outbound.end()) return;
            PendingOutbound pending = found->second;
            binding->outbound.erase(found);

            JSValue argument = JS_UNDEFINED;
            JSValue function = pending.resolve;
            if (result.error) {
                function = pending.reject;
                argument = make_error(ctx, *result.error);
            }
            else if (result.value) {
                argument = tjs__message_clone_read(ctx, result.value.get());
                if (JS_IsException(argument)) {
                    std::string name;
                    auto message = take_exception(ctx, &name);
                    CallError error{ CallErrorCode::remote_error, std::move(message),
                                    std::move(name),{} };
                    function = pending.reject;
                    argument = make_error(ctx, error);
                }
            }
            else if (result.json) {
                argument = JS_ParseJSON(ctx, result.json->data(), result.json->size(),
                    "<circud-response>");
            }
            JSValue called = JS_Call(ctx, function, JS_UNDEFINED, 1, &argument);
            JS_FreeValue(ctx, called);
            JS_FreeValue(ctx, argument);
            JS_FreeValue(ctx, pending.resolve);
            JS_FreeValue(ctx, pending.reject);
        }

        JSValue host_call(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
            auto binding = find_binding(ctx);
            if (!binding || binding->closing)
                return JS_ThrowTypeError(ctx, "circud host is unavailable");
            if (argc < 3)
                return JS_ThrowTypeError(ctx,
                    "call(packageId, target, arguments[, options])");
            if (binding->outbound.size() >= kMaxPendingCalls)
                return JS_ThrowInternalError(ctx, "too many pending package calls");
            const char* package_id = JS_ToCString(ctx, argv[0]);
            const char* target = JS_ToCString(ctx, argv[1]);
            if (!package_id || !target) {
                if (package_id) JS_FreeCString(ctx, package_id);
                if (target) JS_FreeCString(ctx, target);
                return JS_EXCEPTION;
            }
            std::chrono::milliseconds timeout = kDefaultCallTimeout;
            JSValue transfer = JS_UNDEFINED;
            if (argc >= 4 && JS_IsObject(argv[3])) {
                JSValue timeout_value = JS_GetPropertyStr(ctx, argv[3], "timeoutMs");
                if (!JS_IsUndefined(timeout_value)) {
                    int64_t value = 0;
                    if (JS_ToInt64(ctx, &value, timeout_value) < 0 || value <= 0) {
                        JS_FreeValue(ctx, timeout_value);
                        JS_FreeCString(ctx, package_id);
                        JS_FreeCString(ctx, target);
                        return JS_ThrowRangeError(ctx, "timeoutMs must be positive");
                    }
                    timeout = std::min(std::chrono::milliseconds(value),
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            kMaxCallTimeout));
                }
                JS_FreeValue(ctx, timeout_value);
                transfer = JS_GetPropertyStr(ctx, argv[3], "transfer");
            }

            TJSMessageClone* raw = tjs__message_clone_prepare(
                ctx, argv[2], JS_IsUndefined(transfer) ? JS_UNDEFINED : transfer);
            JS_FreeValue(ctx, transfer);
            if (!raw) {
                JS_FreeCString(ctx, package_id);
                JS_FreeCString(ctx, target);
                return JS_EXCEPTION;
            }
            if (tjs__message_clone_size(raw) > kMaxEnvelopeBytes) {
                tjs__message_clone_abort(ctx, raw);
                JS_FreeCString(ctx, package_id);
                JS_FreeCString(ctx, target);
                return JS_ThrowRangeError(ctx, "call envelope exceeds 64 KiB");
            }
            if (tjs__message_clone_transfer_count(raw) > kMaxBulkReferences) {
                tjs__message_clone_abort(ctx, raw);
                JS_FreeCString(ctx, package_id);
                JS_FreeCString(ctx, target);
                return JS_ThrowRangeError(ctx,
                    "call exceeds bulk reference limit");
            }
            if (tjs__message_clone_commit(ctx, raw) < 0) {
                tjs__message_clone_free(raw);
                JS_FreeCString(ctx, package_id);
                JS_FreeCString(ctx, target);
                return JS_EXCEPTION;
            }

            JSValue callbacks[2];
            JSValue promise = JS_NewPromiseCapability(ctx, callbacks);
            if (JS_IsException(promise)) {
                tjs__message_clone_free(raw);
                JS_FreeCString(ctx, package_id);
                JS_FreeCString(ctx, target);
                return promise;
            }
            const auto local_id = binding->next_outbound++;
            binding->outbound.emplace(
                local_id,
                PendingOutbound{ JS_DupValue(ctx, callbacks[0]),
                                JS_DupValue(ctx, callbacks[1]) });
            JS_FreeValue(ctx, callbacks[0]);
            JS_FreeValue(ctx, callbacks[1]);

            std::string package_copy(package_id);
            std::string target_copy(target);
            JS_FreeCString(ctx, package_id);
            JS_FreeCString(ctx, target);
            std::weak_ptr<Binding> weak = binding;
            binding->config.outbound(
                std::move(package_copy), std::move(target_copy), ClonePtr(raw), timeout,
                [weak, local_id](CallResult result) mutable {
                    auto locked = weak.lock();
                    if (!locked) return;
                    auto shared = std::make_shared<CallResult>(std::move(result));
                    locked->config.post([weak, local_id, shared](JSContext* owner_ctx) mutable {
                        if (auto owner = weak.lock(); owner && !owner->closing)
                            resolve_outbound(owner_ctx, owner, local_id,
                                std::move(*shared));
                        });
                });
            return promise;
        }

        JSValue host_reload(JSContext* ctx, JSValueConst, int, JSValueConst*) {
            auto binding = find_binding(ctx);
            if (!binding || binding->closing)
                return JS_ThrowTypeError(ctx, "circud host is unavailable");
            binding->config.request_reload();
            return JS_UNDEFINED;
        }

        JSValue host_info(JSContext* ctx, JSValueConst, int, JSValueConst*) {
            auto binding = find_binding(ctx);
            if (!binding) return JS_NULL;
            JSValue result = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, result, "packageId",
                JS_NewString(ctx, binding->config.package_id.c_str()));
            JS_SetPropertyStr(ctx, result, "packageDir",
                JS_NewString(ctx, binding->config.package_dir.c_str()));
            JS_SetPropertyStr(ctx, result, "dataDir",
                JS_NewString(ctx, binding->config.data_dir.c_str()));
            return result;
        }

    } // namespace

    void register_runtime(TJSRuntime* runtime, BindingConfig config) {
        auto binding = std::make_shared<Binding>(std::move(config));
        binding->context = TJS_GetMainContext(runtime);
        std::lock_guard lock(bindings_mutex);
        bindings[runtime] = std::move(binding);
    }

    void shutdown_runtime(JSContext* ctx) {
        auto binding = find_binding(ctx);
        if (!binding) return;
        binding->closing = true;
        for (auto& [id, pending] : binding->outbound) {
            JSValue error = JS_NewError(ctx);
            JS_SetPropertyStr(ctx, error, "message",
                JS_NewString(ctx, "package runtime stopped"));
            JSValue called = JS_Call(ctx, pending.reject, JS_UNDEFINED, 1, &error);
            JS_FreeValue(ctx, called);
            JS_FreeValue(ctx, error);
            JS_FreeValue(ctx, pending.resolve);
            JS_FreeValue(ctx, pending.reject);
        }
        binding->outbound.clear();
        std::vector<std::shared_ptr<IncomingState>> incoming;
        for (auto& item : binding->incoming) incoming.push_back(item.second);
        for (auto& state : incoming) {
            if (!JS_IsUndefined(state->signal))
                abort_signal(ctx, state->signal, "package runtime stopped");
            finish_incoming(state, CallResult::failure(CallErrorCode::unavailable,
                "package runtime stopped"));
        }
        std::vector<std::shared_ptr<EndpointState>> endpoints;
        for (auto& item : binding->endpoints)
            endpoints.push_back(item.second);
        for (auto& state : endpoints) {
            state->closing = true;
            abort_endpoint(state, "package runtime stopped");
            if (state->sink) {
                EndpointClose close;
                close.id = state->request.id;
                close.code = 1001;
                close.reason = "package runtime stopped";
                (void)emit_endpoint(state, std::move(close));
            }
            finish_endpoint(state);
        }
    }

    void unregister_runtime(TJSRuntime* runtime) {
        std::lock_guard lock(bindings_mutex);
        bindings.erase(runtime);
    }

    void dispatch_call(JSContext* ctx, RuntimeCall call) {
        auto binding = find_binding(ctx);
        if (!binding || binding->closing) {
            call.complete(CallResult::failure(CallErrorCode::unavailable,
                "package runtime is stopping"));
            return;
        }
        auto state = std::make_shared<IncomingState>();
        state->binding = binding;
        state->call = std::move(call);
        binding->incoming[state->call.id] = state;

        JSValue arguments = JS_UNDEFINED;
        if (state->call.arguments)
            arguments = tjs__message_clone_read(ctx, state->call.arguments.get());
        else if (state->call.json_arguments)
            arguments = JS_ParseJSON(ctx, state->call.json_arguments->data(),
                state->call.json_arguments->size(),
                "<circud-request>");
        if (JS_IsException(arguments)) {
            std::string name;
            auto message = take_exception(ctx, &name);
            auto result = CallResult::failure(CallErrorCode::remote_error,
                std::move(message));
            result.error->remote_name = std::move(name);
            finish_incoming(state, std::move(result));
            return;
        }

        JSValue event = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, event, "id", JS_NewInt64(ctx, state->call.id));
        JS_SetPropertyStr(ctx, event, "source",
            JS_NewString(ctx, state->call.source.c_str()));
        JS_SetPropertyStr(ctx, event, "target",
            JS_NewString(ctx, state->call.target.c_str()));
        JS_SetPropertyStr(ctx, event, "arguments", arguments);
        JSValue signal = make_abort_signal(ctx);
        if (JS_IsException(signal)) {
            auto message = take_exception(ctx);
            finish_incoming(state, CallResult::failure(
                CallErrorCode::remote_error, std::move(message)));
            JS_FreeValue(ctx, event);
            return;
        }
        state->signal = JS_DupValue(ctx, signal);
        JS_SetPropertyStr(ctx, event, "signal", signal);
        auto* closure_state = new std::shared_ptr<IncomingState>(state);
        JSValue responder = JS_NewCClosure(ctx, respond_with, "respondWith",
            free_respond_state, 1, 0, closure_state);
        JS_SetPropertyStr(ctx, event, "respondWith", responder);

        JSValue emitted = TJS_EvBusEmit(ctx, "circud.host", "call", event);
        state->active = false;
        JS_FreeValue(ctx, event);
        if (JS_IsException(emitted)) {
            std::string name;
            auto message = take_exception(ctx, &name);
            auto result = CallResult::failure(CallErrorCode::remote_error,
                std::move(message));
            result.error->remote_name = std::move(name);
            finish_incoming(state, std::move(result));
            return;
        }
        const bool handled = JS_IsBool(emitted) && JS_VALUE_GET_BOOL(emitted);
        JS_FreeValue(ctx, emitted);
        if (!state->responded) {
            finish_incoming(
                state,
                CallResult::failure(CallErrorCode::unhandled_call,
                    handled ? "listener did not call respondWith()"
                    : "no listener handled the call"));
        }
    }

    void dispatch_cancel(JSContext* ctx, std::uint64_t id) {
        auto binding = find_binding(ctx);
        if (!binding) return;
        auto found = binding->incoming.find(id);
        if (found == binding->incoming.end()) return;
        auto state = found->second;
        if (!JS_IsUndefined(state->signal))
            abort_signal(ctx, state->signal, "call was cancelled");
        finish_incoming(state, CallResult::failure(CallErrorCode::cancelled,
            "call was cancelled"));
    }

    void dispatch_endpoint(JSContext* ctx, EndpointRequest request,
        std::shared_ptr<EndpointSink> sink) {
        auto binding = find_binding(ctx);
        if (!binding || binding->closing) {
            EndpointClose close;
            close.id = request.id;
            close.code = 1001;
            close.reason = "package runtime is stopping";
            emit_sink_noexcept(sink, std::move(close));
            return;
        }
        if (binding->endpoints.contains(request.id)) {
            EndpointClose close;
            close.id = request.id;
            close.code = 1011;
            close.reason = "duplicate endpoint session id";
            emit_sink_noexcept(sink, std::move(close));
            return;
        }

        auto state = std::make_shared<EndpointState>();
        state->binding = binding;
        state->request = std::move(request);
        state->sink = std::move(sink);
        binding->endpoints[state->request.id] = state;

        JSValue event = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, event, "id",
            JS_NewInt64(ctx, state->request.id));
        JS_SetPropertyStr(ctx, event, "protocol",
            JS_NewString(ctx,
                endpoint_protocol_name(state->request.protocol)));
        JS_SetPropertyStr(ctx, event, "method",
            JS_NewString(ctx, state->request.method.c_str()));
        JS_SetPropertyStr(ctx, event, "path",
            JS_NewString(ctx, state->request.path.c_str()));
        JS_SetPropertyStr(ctx, event, "query",
            JS_NewString(ctx, state->request.query.c_str()));
        JS_SetPropertyStr(ctx, event, "remoteAddress",
            JS_NewString(ctx, state->request.remote_address.c_str()));
        JS_SetPropertyStr(ctx, event, "isAuthorized",
            JS_NewBool(ctx, state->request.is_authorized));
        JS_SetPropertyStr(ctx, event, "body",
            bytes_to_js(ctx, state->request.body));

        JSValue headers = JS_NewArray(ctx);
        for (std::size_t index = 0; index < state->request.headers.size();
             ++index) {
            JSValue pair = JS_NewArray(ctx);
            JS_SetPropertyUint32(ctx, pair, 0,
                JS_NewString(ctx,
                    state->request.headers[index].first.c_str()));
            JS_SetPropertyUint32(ctx, pair, 1,
                JS_NewString(ctx,
                    state->request.headers[index].second.c_str()));
            JS_SetPropertyUint32(ctx, headers,
                static_cast<std::uint32_t>(index), pair);
        }
        JS_SetPropertyStr(ctx, event, "headers", headers);

        JSValue protocols = JS_NewArray(ctx);
        for (std::size_t index = 0;
             index < state->request.offered_subprotocols.size(); ++index) {
            JS_SetPropertyUint32(ctx, protocols,
                static_cast<std::uint32_t>(index),
                JS_NewString(ctx,
                    state->request.offered_subprotocols[index].c_str()));
        }
        JS_SetPropertyStr(ctx, event, "offeredSubprotocols", protocols);

        JSValue signal = make_abort_signal(ctx);
        if (JS_IsException(signal)) {
            (void)take_exception(ctx);
            fail_endpoint_response(state,
                "cannot initialize endpoint cancellation signal");
            JS_FreeValue(ctx, event);
            return;
        }
        state->signal = JS_DupValue(ctx, signal);
        JS_SetPropertyStr(ctx, event, "signal", signal);

        auto* response_state = new std::shared_ptr<EndpointState>(state);
        JS_SetPropertyStr(ctx, event, "respondWith",
            JS_NewCClosure(ctx, endpoint_respond_with, "respondWith",
                free_endpoint_state, 1, 0, response_state));
        auto* static_state = new std::shared_ptr<EndpointState>(state);
        JS_SetPropertyStr(ctx, event, "serveStatic",
            JS_NewCClosure(ctx, endpoint_serve_static, "serveStatic",
                free_endpoint_state, 1, 0, static_state));
        auto* sse_state = new std::shared_ptr<EndpointState>(state);
        JS_SetPropertyStr(ctx, event, "acceptSse",
            JS_NewCClosure(ctx, endpoint_accept, "acceptSse",
                free_endpoint_state, 1, 0, sse_state));
        auto* websocket_state = new std::shared_ptr<EndpointState>(state);
        JS_SetPropertyStr(ctx, event, "acceptWebSocket",
            JS_NewCClosure(ctx, endpoint_accept, "acceptWebSocket",
                free_endpoint_state, 1, 1, websocket_state));

        JSValue emitted =
            TJS_EvBusEmit(ctx, "circud.host", "endpoint", event);
        state->active = false;
        JS_FreeValue(ctx, event);
        if (JS_IsException(emitted)) {
            (void)take_exception(ctx);
            fail_endpoint_response(state, "package endpoint listener failed");
            return;
        }
        const bool handled =
            JS_IsBool(emitted) && JS_VALUE_GET_BOOL(emitted);
        JS_FreeValue(ctx, emitted);
        if (!state->responded) {
            EndpointResponse response;
            response.id = state->request.id;
            response.status = handled ? 500 : 404;
            response.headers.emplace_back("content-type",
                "text/plain; charset=utf-8");
            response.body = bytes_from_string(handled
                    ? "endpoint listener did not respond or accept"
                    : "no package endpoint listener");
            (void)emit_endpoint(state, std::move(response));
            finish_endpoint(state);
        }
    }

    void dispatch_endpoint_message(JSContext* ctx, EndpointMessage message) {
        auto binding = find_binding(ctx);
        if (!binding || binding->closing) return;
        const auto found = binding->endpoints.find(message.id);
        if (found == binding->endpoints.end()) return;
        const auto state = found->second;
        if (state->finished || !state->accepted ||
            state->accepted_protocol != EndpointProtocol::websocket)
            return;

        JSValue event = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, event, "id", JS_NewInt64(ctx, message.id));
        JS_SetPropertyStr(ctx, event, "session",
            JS_DupValue(ctx, state->handle));
        if (message.binary)
            JS_SetPropertyStr(ctx, event, "data",
                bytes_to_js(ctx, message.data));
        else {
            const auto* data =
                reinterpret_cast<const char*>(message.data.data());
            JS_SetPropertyStr(ctx, event, "data",
                JS_NewStringLen(ctx, data, message.data.size()));
        }
        JS_SetPropertyStr(ctx, event, "binary",
            JS_NewBool(ctx, message.binary));
        JSValue emitted = TJS_EvBusEmit(ctx, "circud.host",
            "endpoint.message", event);
        if (JS_IsException(emitted)) TJS_DumpException(ctx);
        else JS_FreeValue(ctx, emitted);

        // Convenience property on the thin handle; EvBus remains the primary
        // registration and dispatch mechanism.
        JSValue callback =
            JS_GetPropertyStr(ctx, state->handle, "onmessage");
        if (JS_IsFunction(ctx, callback)) {
            JSValue argument = JS_DupValue(ctx, event);
            JSValue called =
                JS_Call(ctx, callback, state->handle, 1, &argument);
            JS_FreeValue(ctx, argument);
            if (JS_IsException(called)) TJS_DumpException(ctx);
            else JS_FreeValue(ctx, called);
        }
        JS_FreeValue(ctx, callback);
        JS_FreeValue(ctx, event);
    }

    void dispatch_endpoint_close(JSContext* ctx, EndpointClose close) {
        auto binding = find_binding(ctx);
        if (!binding) return;
        const auto found = binding->endpoints.find(close.id);
        if (found == binding->endpoints.end()) return;
        const auto state = found->second;

        state->closing = true;
        abort_endpoint(state,
            close.reason.empty() ? std::string_view("endpoint closed")
                                 : std::string_view(close.reason));

        JSValue event = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, event, "id", JS_NewInt64(ctx, close.id));
        JS_SetPropertyStr(ctx, event, "session",
            JS_DupValue(ctx, state->handle));
        JS_SetPropertyStr(ctx, event, "code", JS_NewInt32(ctx, close.code));
        JS_SetPropertyStr(ctx, event, "reason",
            JS_NewString(ctx, close.reason.c_str()));
        JSValue emitted = TJS_EvBusEmit(ctx, "circud.host",
            "endpoint.close", event);
        if (JS_IsException(emitted)) TJS_DumpException(ctx);
        else JS_FreeValue(ctx, emitted);

        JSValue callback = JS_IsUndefined(state->handle)
            ? JS_UNDEFINED
            : JS_GetPropertyStr(ctx, state->handle, "onclose");
        if (JS_IsFunction(ctx, callback)) {
            JSValue argument = JS_DupValue(ctx, event);
            JSValue called =
                JS_Call(ctx, callback, state->handle, 1, &argument);
            JS_FreeValue(ctx, argument);
            if (JS_IsException(called)) TJS_DumpException(ctx);
            else JS_FreeValue(ctx, called);
        }
        JS_FreeValue(ctx, callback);
        JS_FreeValue(ctx, event);
        finish_endpoint(state);
    }

} // namespace circud::host

extern "C" void circud_module_init(JSContext* ctx, JSValue ns) {
    JS_SetPropertyStr(ctx, ns, "AbortSignal",
        circud::host::make_abort_signal_constructor(ctx));
    JS_SetPropertyStr(ctx, ns, "call",
        JS_NewCFunction(ctx, circud::host::host_call, "call", 4));
    JS_SetPropertyStr(ctx, ns, "reload",
        JS_NewCFunction(ctx, circud::host::host_reload, "reload", 0));
    JS_SetPropertyStr(ctx, ns, "info",
        JS_NewCFunction(ctx, circud::host::host_info, "info", 0));
}
