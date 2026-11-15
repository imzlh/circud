#pragma once

#include <boost/json/value.hpp>
#include <quickjs.h>

#include <string>

namespace circud {

    boost::json::value parse_json(std::string_view text);
    std::string serialize_json(const boost::json::value& value);
    JSValue json_to_js(JSContext* ctx, const boost::json::value& value);
    boost::json::value js_to_json(JSContext* ctx, JSValueConst value);
    std::string js_exception_message(JSContext* ctx, std::string* name = nullptr);

} // namespace circud
