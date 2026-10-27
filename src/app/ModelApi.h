#pragma once

#include "model/Document.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <vector>

// The MCP tools: what Claude can read and change in a project. No widgets,
// so the same code runs inside the window (live, with undo) and in
// `opencaliper --mcp` on a project file when no window is open.
namespace mcp {

// Where the tools act. Every method returns an error message, empty on success.
class Host {
public:
  virtual ~Host() = default;
  virtual const model::Document& document() const = 0;
  virtual bool live() const = 0; // the OpenCaliper window
  // Replaces the document as one undo step; `what` is shown to the user.
  virtual QString apply(model::Document doc, const QString& what) = 0;
  virtual QString undo() = 0;
  virtual QString redo() = 0;
  virtual QString projectPath() const = 0;
  virtual QString newProject() = 0;
  virtual QString openProject(const QString& path) = 0;
  virtual QString saveProject(const QString& path) = 0; // empty: the current path
  // PNG of the 3D view from "iso", "front", "top", ... or "current".
  virtual QString screenshot(const QString& view, QByteArray& png) = 0;
};

// A document in this process, for `opencaliper --mcp` without a window.
class FileHost : public Host {
public:
  const model::Document& document() const override { return m_doc; }
  bool live() const override { return false; }
  QString apply(model::Document doc, const QString& what) override;
  QString undo() override;
  QString redo() override;
  QString projectPath() const override { return m_path; }
  QString newProject() override;
  QString openProject(const QString& path) override;
  QString saveProject(const QString& path) override;
  QString screenshot(const QString& view, QByteArray& png) override;

private:
  model::Document m_doc;
  std::vector<model::Document> m_undo, m_redo;
  QString m_path;
};

// Tool definitions for tools/list.
QJsonArray tools();
// Runs one tool; returns the tools/call result ({content: [...], isError}).
QJsonObject callTool(Host& host, const QString& name, const QJsonObject& args);

// Result helpers.
QJsonObject textResult(const QString& text, bool error = false);

} // namespace mcp
