#ifndef MCP_CAMERA_TOOLS_H
#define MCP_CAMERA_TOOLS_H

#include <string>

class McpServer;
class cJSON;

class McpCameraToolsRegistrar {
public:
    static void AddCommonTools(McpServer& server);
    static void AddUserOnlyTools(McpServer& server);
    static void ParseCapabilities(McpServer& server, const cJSON* capabilities);
};

#endif  // MCP_CAMERA_TOOLS_H
