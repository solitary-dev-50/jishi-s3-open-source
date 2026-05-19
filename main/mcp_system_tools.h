#ifndef MCP_SYSTEM_TOOLS_H
#define MCP_SYSTEM_TOOLS_H

class McpServer;

class McpSystemToolsRegistrar {
public:
    static void AddCommonTools(McpServer& server);
    static void AddUserOnlyTools(McpServer& server);
};

#endif  // MCP_SYSTEM_TOOLS_H
