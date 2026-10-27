// MCP tools on a document without a window (FileHost), and the JSON-RPC layer.
#include "app/McpServer.h"
#include "app/ModelApi.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

#include <cmath>
#include <cstdio>
#include <numbers>
#include <utility>

static int failures = 0;

#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
      ++failures;                                                        \
    }                                                                    \
  } while (0)

static bool near(double a, double b, double tol = 0.01) { return std::abs(a - b) <= tol; }

// Calls a tool; returns its JSON reply (or {"error": text} when it failed).
static QJsonObject call(mcp::Host& host, const char* tool, const char* args = "{}") {
  const QJsonObject r = mcp::callTool(host, tool, QJsonDocument::fromJson(args).object());
  const QString text = std::as_const(r)["content"].toArray().at(0)["text"].toString();
  if (std::as_const(r)["isError"].toBool()) return {{"error", text}};
  return QJsonDocument::fromJson(text.toUtf8()).object();
}

static double volume(const QJsonObject& reply) { return reply["body"]["volume_mm3"].toDouble(); }

static void buildBox() {
  mcp::FileHost host;
  QJsonObject r = call(host, "set_parameters", R"({"parameters": [{"name": "w", "expr": "40"},
                                                                  {"name": "h", "expr": "w * 0.75"},
                                                                  {"name": "t", "expr": "10"}]})");
  CHECK(!r.contains("error"));

  r = call(host, "create_sketch", R"({"plane": "XY", "curves": [{"type": "rect", "corner": [0, 0], "width": "w", "height": "h"}]})");
  CHECK(!r.contains("error"));
  CHECK(std::as_const(r)["step"]["closed_profile"].toBool());
  CHECK(std::as_const(r)["step"]["degrees_of_freedom"].toInt() == 2); // sized, but free to move
  CHECK(std::as_const(r)["created"].toArray().at(0)["curves"].toArray().size() == 4);

  r = call(host, "add_feature", R"({"feature": {"type": "extrude", "sketch": 0, "distance": "t"}})");
  CHECK(!r.contains("error"));
  CHECK(near(volume(r), 40 * 30 * 10));
  CHECK(std::as_const(r)["step"]["expressions"]["distance"].toString() == "t");

  r = call(host, "set_parameters", R"({"parameters": [{"name": "t", "expr": "20"}]})");
  CHECK(near(volume(r), 40 * 30 * 20));

  // A hole from a circle on a plane at the top face.
  r = call(host, "find_faces", R"({"normal": [0, 0, 1]})");
  CHECK(std::as_const(r)["faces"].toArray().size() == 1);
  CHECK(near(std::as_const(r)["faces"].toArray().at(0)["center"].toArray().at(2).toDouble(), 20));
  r = call(host, "create_sketch", R"({"plane": "XY", "offset": 20, "curves": [{"type": "circle", "center": [20, 15], "radius": 1}]})");
  CHECK(!r.contains("error"));
  r = call(host, "add_feature", R"({"feature": {"type": "hole", "sketch": 2, "diameter": 5}})");
  CHECK(!r.contains("error"));
  const double withHole = 40 * 30 * 20 - std::numbers::pi * 2.5 * 2.5 * 20;
  CHECK(near(volume(r), withHole));

  // Round the four vertical outer edges.
  r = call(host, "find_edges", R"({"type": "line"})");
  QJsonArray vertical;
  for (const QJsonValue& e : std::as_const(r)["edges"].toArray())
    if (std::abs(e["direction"].toArray().at(2).toDouble()) > 0.99 && e["faces"].toArray().size() == 2 &&
        std::abs(e["midpoint"].toArray().at(0).toDouble() - 20) > 19) // not the hole's seam
      vertical.append(e["index"]);
  CHECK(vertical.size() == 4);
  r = call(host, "add_feature",
           QJsonDocument(QJsonObject{{"feature", QJsonObject{{"type", "fillet"}, {"edges", vertical}, {"radius", 2}}}})
               .toJson()
               .constData());
  CHECK(!r.contains("error"));
  const double rounded = withHole - 4 * (4 - std::numbers::pi) * 20;
  CHECK(near(volume(r), rounded));

  // Changing an earlier size keeps the fillet on its edges.
  r = call(host, "update_feature", R"({"index": 1, "changes": {"distance": 25}})");
  CHECK(!r.contains("error"));
  CHECK(!r.contains("steps_with_errors"));
  // The hole sketch stays at z = 20 and drills down from there.
  CHECK(near(volume(r), 40 * 30 * 25 - std::numbers::pi * 2.5 * 2.5 * 20 - 4 * (4 - std::numbers::pi) * 25, 0.5));

  // Sketch dimensions: constraint values by index.
  const QJsonObject model = call(host, "get_model");
  int widthConstraint = -1;
  for (const QJsonValue& c : model["steps"].toArray().at(0)["constraints"].toArray())
    if (c["expr"].toString() == "w") widthConstraint = c["index"].toInt();
  CHECK(widthConstraint >= 0);
  CHECK(model["parameters"].toArray().at(1)["value"].toDouble() == 30);

  r = call(host, "undo");
  CHECK(!r.contains("error"));
  CHECK(near(volume(call(host, "get_model")), rounded));
  r = call(host, "redo");
  CHECK(!r.contains("error"));

  // Errors come back as messages, and nothing changes.
  CHECK(call(host, "add_feature", R"({"feature": {"type": "extrude", "sketch": 0, "depth": 5}})").contains("error"));
  CHECK(call(host, "add_feature", R"({"feature": {"type": "fillet", "edges": [999], "radius": 1}})").contains("error"));
  CHECK(call(host, "update_feature", R"({"index": 42, "changes": {}})").contains("error"));
  CHECK(call(host, "set_parameters", R"({"parameters": [{"name": "x", "expr": "nope + 1"}]})").contains("error"));
  CHECK(call(host, "screenshot").contains("error")); // needs the window
  CHECK(call(host, "get_model")["steps"].toArray().size() == 5);

  // Save, start over, open again.
  const QString path = QDir::temp().filePath("opencaliper-mcp-test.ocal");
  CHECK(!call(host, "save_project", QJsonDocument(QJsonObject{{"path", path}}).toJson().constData()).contains("error"));
  CHECK(!call(host, "new_project").contains("error"));
  CHECK(call(host, "get_model")["steps"].toArray().isEmpty());
  CHECK(!call(host, "open_project", QJsonDocument(QJsonObject{{"path", path}}).toJson().constData()).contains("error"));
  CHECK(call(host, "get_model")["steps"].toArray().size() == 5);
  QFile::remove(path);

  r = call(host, "delete_feature", R"({"index": 4})");
  CHECK(std::as_const(r)["steps_removed"].toInt() == 1);
}

static void sketchShapes() {
  mcp::FileHost host;
  // Slot and polygon with a hole inside: loops inside loops are holes.
  QJsonObject r = call(host, "create_sketch", R"({"plane": "XZ", "curves": [
      {"type": "polygon", "center": [0, 0], "radius": 20, "sides": 6},
      {"type": "slot", "from": [-5, 0], "to": [5, 0], "width": 4}]})");
  CHECK(!r.contains("error"));
  CHECK(std::as_const(r)["step"]["closed_profile"].toBool());
  r = call(host, "add_feature", R"({"feature": {"type": "extrude", "sketch": 0, "distance": 2}})");
  const double hexagon = 3 * std::sqrt(3.0) / 2 * 20 * 20;
  const double slot = 10 * 4 + std::numbers::pi * 2 * 2;
  CHECK(near(volume(r), (hexagon - slot) * 2, 0.05));
  // XZ: extrudes along -Y.
  CHECK(near(std::as_const(r)["body"]["max"].toArray().at(1).toDouble(), 0) && near(std::as_const(r)["body"]["min"].toArray().at(1).toDouble(), -2));

  // Constraints by coordinates: a line made horizontal and 25 long.
  r = call(host, "create_sketch", R"({"plane": "XY", "curves": [{"type": "line", "from": [0, 0], "to": [10, 1]}],
      "constraints": [{"kind": "horizontal", "a": [5, 0.5]}, {"kind": "length", "a": 0, "value": 25}]})");
  CHECK(!r.contains("error"));
  const QJsonArray pts = std::as_const(r)["step"]["points"].toArray();
  CHECK(near(std::abs(pts[1]["x"].toDouble() - pts[0]["x"].toDouble()), 25));
  CHECK(near(pts[1]["y"].toDouble(), pts[0]["y"].toDouble()));

  CHECK(call(host, "create_sketch", R"({"plane": "XY", "curves": [{"type": "blob"}]})").contains("error"));
  CHECK(call(host, "create_sketch", R"({"plane": "XY", "curves": [{"type": "line", "from": [0, 0], "to": [1, 0]}],
      "constraints": [{"kind": "length", "a": 0, "value": 10}, {"kind": "length", "a": 0, "value": 20}]})")
            .contains("error")); // contradiction
}

static void protocol() {
  int calls = 0;
  const mcp::ToolCall tool = [&](const QString&, const QJsonObject&) {
    ++calls;
    return mcp::textResult("ok");
  };
  QJsonObject r = mcp::handle({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
                               {"params", QJsonObject{{"protocolVersion", "2025-06-18"}}}},
                              tool);
  CHECK(std::as_const(r)["id"].toInt() == 1);
  CHECK(std::as_const(r)["result"]["protocolVersion"].toString() == "2025-06-18");
  CHECK(std::as_const(r)["result"]["serverInfo"]["name"].toString() == "opencaliper");
  CHECK(std::as_const(r)["result"]["capabilities"].toObject().contains("tools"));

  CHECK(mcp::handle({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}, tool).isEmpty());

  r = mcp::handle({{"jsonrpc", "2.0"}, {"id", "a"}, {"method", "tools/list"}}, tool);
  const QJsonArray tools = std::as_const(r)["result"]["tools"].toArray();
  CHECK(tools.size() == 15);
  for (const QJsonValue& t : tools) {
    CHECK(!t["name"].toString().isEmpty());
    CHECK(!t["description"].toString().isEmpty());
    CHECK(t["inputSchema"]["type"].toString() == "object");
  }

  r = mcp::handle({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"},
                   {"params", QJsonObject{{"name", "get_model"}, {"arguments", QJsonObject{}}}}},
                  tool);
  CHECK(calls == 1 && std::as_const(r)["result"]["content"].toArray().at(0)["text"].toString() == "ok");

  r = mcp::handle({{"jsonrpc", "2.0"}, {"id", 3}, {"method", "resources/list"}}, tool);
  CHECK(std::as_const(r)["error"]["code"].toInt() == -32601);
}

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  QCoreApplication::setApplicationVersion("test");
  buildBox();
  sketchShapes();
  protocol();
  if (failures) std::printf("%d failure(s)\n", failures);
  else std::printf("all MCP tests passed\n");
  return failures ? 1 : 0;
}
