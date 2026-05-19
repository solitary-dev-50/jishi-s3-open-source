#ifndef MCP_PROTOCOL_CONTROLLER_H
#define MCP_PROTOCOL_CONTROLLER_H

#include <string>

class cJSON;
class McpServer;

class McpProtocolController {
public:
    static void ParseMessage(McpServer& server, const std::string& message);
    static void ParseMessage(McpServer& server, const cJSON* json);

private:
    static void ReplyResult(McpServer& server, int id, const std::string& result);
    static void ReplyError(McpServer& server, int id, const std::string& message);
    static void GetToolsList(McpServer& server, int id, const std::string& cursor, bool list_user_only_tools);
    static void DoToolCall(McpServer& server, int id, const std::string& tool_name, const cJSON* tool_arguments);
};

#endif  // MCP_PROTOCOL_CONTROLLER_H
