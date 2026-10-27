#pragma once

#include <QJsonObject>
#include <QString>

#include <functional>

// Model Context Protocol over JSON-RPC 2.0: initialize, ping, tools/list,
// tools/call. The transport (stdin/stdout, one message per line) is McpLink.
namespace mcp {

using ToolCall = std::function<QJsonObject(const QString& name, const QJsonObject& args)>;

// Reply to one client message; an empty object for notifications (no reply).
QJsonObject handle(const QJsonObject& message, const ToolCall& call);

} // namespace mcp
