#include "json.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/system/error_code.hpp>

#include <stdexcept>

namespace circud {

    boost::json::value parse_json(std::string_view text) {
        boost::system::error_code error;
        auto value = boost::json::parse(text, error);
        if (error) {
            throw std::runtime_error("invalid JSON: " + error.message());
        }
        return value;
    }

    std::string serialize_json(const boost::json::value& value) {
        return boost::json::serialize(value);
    }

    JSValue json_to_js(JSContext* ctx, const boost::json::value& value) {
        const auto text = serialize_json(value);
        return JS_ParseJSON(ctx, text.data(), text.size(), "<circud-json>");
    }

    boost::json::value js_to_json(JSContext* ctx, JSValueConst value) {
        JSValue encoded = JS_JSONStringify(ctx, value, JS_UNDEFINED, JS_UNDEFINED);
        if (JS_IsException(encoded)) {
            throw std::runtime_error(js_exception_message(ctx));
        }
        if (JS_IsUndefined(encoded)) {
            JS_FreeValue(ctx, encoded);
            throw std::runtime_error("value is not JSON serializable");
        }
        const char* text = JS_ToCString(ctx, encoded);
        if (!text) {
            JS_FreeValue(ctx, encoded);
            throw std::runtime_error(js_exception_message(ctx));
        }
        std::string copy(text);
        JS_FreeCString(ctx, text);
        JS_FreeValue(ctx, encoded);
        return parse_json(copy);
    }

    std::string js_exception_message(JSContext* ctx, std::string* name) {
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

} // namespace circud
