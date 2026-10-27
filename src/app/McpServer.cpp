#include "app/McpServer.h"
#include "app/ModelApi.h"

#include <QCoreApplication>
#include <QJsonArray>

namespace mcp {
namespace {

QJsonObject reply(const QJsonValue& id, const QJsonObject& result) {
  return {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}

QJsonObject error(const QJsonValue& id, int code, const QString& message) {
  return {{"jsonrpc", "2.0"}, {"id", id}, {"error", QJsonObject{{"code", code}, {"message", message}}}};
}

const char* kInstructions =
    "OpenCaliper is a parametric 3D CAD program for mechanical parts and 3D printing. A part is a timeline of "
    "steps: sketches (2D curves, splines included, on a plane), then extrudes/revolves/lofts that turn closed "
    "sketches into solid, then fillets, chamfers, shells, holes, mirrors and patterns. For curved, organic shapes "
    "use splines and lofts through cross-sections instead of chains of arcs. Start with get_model. Build with create_sketch and "
    "add_feature; check the reply (errors, volume, size) and use screenshot to look at the result when the window is "
    "open. Use set_parameters for sizes the user may want to change later and refer to them in formulas. Every "
    "change is one undo step for the user.";

} // namespace

QJsonObject handle(const QJsonObject& message, const ToolCall& call) {
  const QString method = message["method"].toString();
  const QJsonValue id = message["id"];
  const QJsonObject params = message["params"].toObject();
  // Notifications (initialized, cancelled, ...) and replies to us get no answer.
  if (!message.contains("id") || method.isEmpty()) return {};

  if (method == "initialize") {
    // Answer with the client's protocol version: these four methods are the same in all of them.
    const QString version = params["protocolVersion"].toString("2024-11-05");
    return reply(id, {{"protocolVersion", version},
                      {"capabilities", QJsonObject{{"tools", QJsonObject{}}}},
                      {"serverInfo", QJsonObject{{"name", "opencaliper"},
                                                 {"version", QCoreApplication::applicationVersion()}}},
                      {"instructions", kInstructions}});
  }
  if (method == "ping") return reply(id, {});
  if (method == "tools/list") return reply(id, {{"tools", tools()}});
  if (method == "tools/call") return reply(id, call(params["name"].toString(), params["arguments"].toObject()));
  return error(id, -32601, "Method not found: " + method);
}

} // namespace mcp
