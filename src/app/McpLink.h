#pragma once

#include <QObject>
#include <QString>

class QLocalServer;
class QLocalSocket;

// Connects Claude to OpenCaliper (Linux for now).
//
// Claude starts `opencaliper --mcp` and talks MCP with it over stdin/stdout.
// That process passes tool calls on to the open OpenCaliper window over a
// local socket (live: changes appear at once and can be undone), or works on
// a project file by itself when no window is open. Both need the setting
// Tools > Claude (MCP), off by default.
namespace mcp {

class Host;

bool enabled();
void setEnabled(bool on);
// Shell command that starts this program with --mcp (AppImage, Flatpak or plain binary).
QString serverCommand();

// In the window: answers tool calls from `opencaliper --mcp` while enabled.
class Listener : public QObject {
  Q_OBJECT
public:
  Listener(Host& host, QObject* parent = nullptr);
  void setEnabled(bool on);
  bool listening() const;
  bool connected() const { return !m_sockets.isEmpty(); }
  QString problem() const { return m_problem; } // why it does not listen

signals:
  void stateChanged();

private:
  void serve(QLocalSocket* socket);

  Host& m_host;
  QLocalServer* m_server = nullptr;
  QList<QLocalSocket*> m_sockets;
  QString m_problem;
};

// `opencaliper --mcp`: the MCP server on stdin/stdout. Returns the exit code.
int runStdio();

} // namespace mcp
