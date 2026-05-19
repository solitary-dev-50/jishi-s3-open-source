#include "mcp_protocol_controller.h"

#include <algorithm>
#include <cstring>
#include <esp_app_desc.h>
#include <esp_log.h>

#include "application.h"
#include "mcp_server.h"

namespace {
constexpr char kTag[] = "McpProtocol";
}

void McpProtocolController::ReplyResult(McpServer& server, int id, const std::string& result) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id) + ",\"result\":";
    payload += result;
    payload += "}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpProtocolController::ReplyError(McpServer& server, int id, const std::string& message) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id);
    payload += ",\"error\":{\"message\":\"";
    payload += message;
    payload += "\"}}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpProtocolController::GetToolsList(McpServer& server, int id, const std::string& cursor, bool list_user_only_tools) {
    const int max_payload_size = 8000;
    std::string json = "{\"tools\":[";

    bool found_cursor = cursor.empty();
    auto it = server.tools_.begin();
    std::string next_cursor;

    while (it != server.tools_.end()) {
        if (!found_cursor) {
            if ((*it)->name() == cursor) {
                found_cursor = true;
            } else {
                ++it;
                continue;
            }
        }

        if (!list_user_only_tools && (*it)->user_only()) {
            ++it;
            continue;
        }

        std::string tool_json = (*it)->to_json() + ",";
        if (json.length() + tool_json.length() + 30 > max_payload_size) {
            next_cursor = (*it)->name();
            break;
        }

        json += tool_json;
        ++it;
    }

    if (json.back() == ',') {
        json.pop_back();
    }

    if (json.back() == '[' && !server.tools_.empty()) {
        ESP_LOGE(kTag, "tools/list: Failed to add tool %s because of payload size limit", next_cursor.c_str());
        ReplyError(server, id, "Failed to add tool " + next_cursor + " because of payload size limit");
        return;
    }

    if (next_cursor.empty()) {
        json += "]}";
    } else {
        json += "],\"nextCursor\":\"" + next_cursor + "\"}";
    }

    ReplyResult(server, id, json);
}

void McpProtocolController::DoToolCall(McpServer& server, int id, const std::string& tool_name, const cJSON* tool_arguments) {
    auto tool_iter = std::find_if(server.tools_.begin(), server.tools_.end(),
                                 [&tool_name](const McpTool* tool) {
                                     return tool->name() == tool_name;
                                 });

    if (tool_iter == server.tools_.end()) {
        ESP_LOGE(kTag, "tools/call: Unknown tool: %s", tool_name.c_str());
        ReplyError(server, id, "Unknown tool: " + tool_name);
        return;
    }

    PropertyList arguments = (*tool_iter)->properties();
    try {
        for (auto& argument : arguments) {
            bool found = false;
            if (cJSON_IsObject(tool_arguments)) {
                auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
                if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value)) {
                    argument.set_value<bool>(value->valueint == 1);
                    found = true;
                } else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value)) {
                    argument.set_value<int>(value->valueint);
                    found = true;
                } else if (argument.type() == kPropertyTypeString && cJSON_IsString(value)) {
                    argument.set_value<std::string>(value->valuestring);
                    found = true;
                }
            }

            if (!argument.has_default_value() && !found) {
                ESP_LOGE(kTag, "tools/call: Missing valid argument: %s", argument.name().c_str());
                ReplyError(server, id, "Missing valid argument: " + argument.name());
                return;
            }
        }
    } catch (const std::exception& e) {
        ESP_LOGE(kTag, "tools/call: %s", e.what());
        ReplyError(server, id, e.what());
        return;
    }

    auto& app = Application::GetInstance();
    app.Schedule([&server, id, tool_iter, arguments = std::move(arguments)]() {
        try {
            ReplyResult(server, id, (*tool_iter)->Call(arguments));
        } catch (const std::exception& e) {
            ESP_LOGE(kTag, "tools/call: %s", e.what());
            ReplyError(server, id, e.what());
        }
    });
}

void McpProtocolController::ParseMessage(McpServer& server, const std::string& message) {
    cJSON* json = cJSON_Parse(message.c_str());
    if (json == nullptr) {
        ESP_LOGE(kTag, "Failed to parse MCP message: %s", message.c_str());
        return;
    }
    ParseMessage(server, json);
    cJSON_Delete(json);
}

void McpProtocolController::ParseMessage(McpServer& server, const cJSON* json) {
    auto version = cJSON_GetObjectItem(json, "jsonrpc");
    if (version == nullptr || !cJSON_IsString(version) || strcmp(version->valuestring, "2.0") != 0) {
        ESP_LOGE(kTag, "Invalid JSONRPC version: %s", version ? version->valuestring : "null");
        return;
    }

    auto method = cJSON_GetObjectItem(json, "method");
    if (method == nullptr || !cJSON_IsString(method)) {
        ESP_LOGE(kTag, "Missing method");
        return;
    }

    auto method_str = std::string(method->valuestring);
    if (method_str.find("notifications") == 0) {
        return;
    }

    auto params = cJSON_GetObjectItem(json, "params");
    if (params != nullptr && !cJSON_IsObject(params)) {
        ESP_LOGE(kTag, "Invalid params for method: %s", method_str.c_str());
        return;
    }

    auto id = cJSON_GetObjectItem(json, "id");
    if (id == nullptr || !cJSON_IsNumber(id)) {
        ESP_LOGE(kTag, "Invalid id for method: %s", method_str.c_str());
        return;
    }
    auto id_int = id->valueint;

    if (method_str == "initialize") {
        if (cJSON_IsObject(params)) {
            auto capabilities = cJSON_GetObjectItem(params, "capabilities");
            if (cJSON_IsObject(capabilities)) {
                server.ParseCapabilities(capabilities);
            }
        }
        auto app_desc = esp_app_get_description();
        std::string message = "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"" BOARD_NAME "\",\"version\":\"";
        message += app_desc->version;
        message += "\"}}";
        ReplyResult(server, id_int, message);
    } else if (method_str == "tools/list") {
        std::string cursor_str;
        bool list_user_only_tools = false;
        if (params != nullptr) {
            auto cursor = cJSON_GetObjectItem(params, "cursor");
            if (cJSON_IsString(cursor)) {
                cursor_str = std::string(cursor->valuestring);
            }
            auto with_user_tools = cJSON_GetObjectItem(params, "withUserTools");
            if (cJSON_IsBool(with_user_tools)) {
                list_user_only_tools = with_user_tools->valueint == 1;
            }
        }
        GetToolsList(server, id_int, cursor_str, list_user_only_tools);
    } else if (method_str == "tools/call") {
        if (!cJSON_IsObject(params)) {
            ESP_LOGE(kTag, "tools/call: Missing params");
            ReplyError(server, id_int, "Missing params");
            return;
        }
        auto tool_name = cJSON_GetObjectItem(params, "name");
        if (!cJSON_IsString(tool_name)) {
            ESP_LOGE(kTag, "tools/call: Missing name");
            ReplyError(server, id_int, "Missing name");
            return;
        }
        auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
        if (tool_arguments != nullptr && !cJSON_IsObject(tool_arguments)) {
            ESP_LOGE(kTag, "tools/call: Invalid arguments");
            ReplyError(server, id_int, "Invalid arguments");
            return;
        }
        DoToolCall(server, id_int, std::string(tool_name->valuestring), tool_arguments);
    } else {
        ESP_LOGE(kTag, "Method not implemented: %s", method_str.c_str());
        ReplyError(server, id_int, "Method not implemented: " + method_str);
    }
}
