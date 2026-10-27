#include "app/McpLink.h"
#include "app/McpServer.h"
#include "app/ModelApi.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QRegularExpression>
#include <QSettings>

#include <cstdio>
#include <iostream>
#include <string>
#include <unistd.h>

namespace mcp {
namespace {

constexpr const char* kSetting = "mcp/enabled";
constexpr int kCallTimeoutMs = 10 * 60 * 1000; // a big rebuild can take a while

// The window and `--mcp` must find the same socket. In Flatpak, instances of
// the app share only $XDG_RUNTIME_DIR/app/<app id>.
QString socketPath() {
  QString dir = qEnvironmentVariable("XDG_RUNTIME_DIR");
  if (!dir.isEmpty() && qEnvironmentVariableIsSet("FLATPAK_ID")) dir += "/app/" + qEnvironmentVariable("FLATPAK_ID");
  if (dir.isEmpty()) dir = QDir::tempPath() + "/opencaliper-" + QString::number(getuid());
  QDir().mkpath(dir);
  return dir + "/opencaliper-mcp";
}

QByteArray line(const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n'; }

QString shellQuote(const QString& s) {
  static const QRegularExpression unsafe("[^A-Za-z0-9_./=+-]");
  if (!s.isEmpty() && !s.contains(unsafe)) return s;
  return "'" + QString(s).replace("'", "'\\''") + "'";
}

} // namespace

bool enabled() {
  QSettings settings;
  settings.sync(); // the window may have changed it since this process started
  return settings.value(kSetting, false).toBool();
}

void setEnabled(bool on) { QSettings().setValue(kSetting, on); }

QString serverCommand() {
  if (qEnvironmentVariableIsSet("FLATPAK_ID")) return "flatpak run " + qEnvironmentVariable("FLATPAK_ID") + " --mcp";
  if (qEnvironmentVariableIsSet("APPIMAGE")) return shellQuote(qEnvironmentVariable("APPIMAGE")) + " --mcp";
  return shellQuote(QCoreApplication::applicationFilePath()) + " --mcp";
}

// --- the window side -------------------------------------------------------------

Listener::Listener(Host& host, QObject* parent) : QObject(parent), m_host(host) {}

bool Listener::listening() const { return m_server && m_server->isListening(); }

void Listener::setEnabled(bool on) {
  if (!on) {
    for (QLocalSocket* s : QList<QLocalSocket*>(m_sockets)) s->abort();
    delete m_server;
    m_server = nullptr;
    m_problem.clear();
    emit stateChanged();
    return;
  }
  if (m_server) return;
  const QString path = socketPath();
  QLocalSocket probe;
  probe.connectToServer(path);
  if (probe.waitForConnected(200)) {
    m_problem = tr("Another OpenCaliper window is already connected to Claude.");
    emit stateChanged();
    return;
  }
  QLocalServer::removeServer(path); // left over from a crash
  m_server = new QLocalServer(this);
  m_server->setSocketOptions(QLocalServer::UserAccessOption);
  if (!m_server->listen(path)) {
    m_problem = m_server->errorString();
    delete m_server;
    m_server = nullptr;
  } else {
    m_problem.clear();
    connect(m_server, &QLocalServer::newConnection, this, [this] {
      while (QLocalSocket* s = m_server->nextPendingConnection()) serve(s);
    });
  }
  emit stateChanged();
}

void Listener::serve(QLocalSocket* socket) {
  socket->setParent(this); // outlives the server when Claude is turned off
  m_sockets << socket;
  connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
    while (socket->canReadLine()) {
      const QJsonObject request = QJsonDocument::fromJson(socket->readLine()).object();
      socket->write(line(callTool(m_host, request["name"].toString(), request["arguments"].toObject())));
      socket->flush();
    }
  });
  connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
    m_sockets.removeOne(socket);
    socket->deleteLater();
    emit stateChanged();
  });
  emit stateChanged();
}

// --- opencaliper --mcp ------------------------------------------------------------

int runStdio() {
  // stdout carries only protocol messages: anything else printed (OCCT, Qt)
  // goes to stderr, which Claude keeps as a log.
  const int protocolFd = dup(STDOUT_FILENO);
  dup2(STDERR_FILENO, STDOUT_FILENO);
  FILE* out = fdopen(protocolFd, "w");
  if (!out) return 1;

  FileHost file;
  QLocalSocket window;
  // A window from the Flatpak listens inside its app directory, which a
  // `--mcp` started outside the sandbox can still reach.
  QStringList paths{socketPath()};
  const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
  if (!runtime.isEmpty() && !qEnvironmentVariableIsSet("FLATPAK_ID"))
    paths << runtime + "/app/io.github.open_workshop_tools.OpenCaliper/opencaliper-mcp";
  const ToolCall call = [&](const QString& name, const QJsonObject& args) -> QJsonObject {
    if (!enabled())
      return textResult("Claude is turned off in OpenCaliper. Ask the user to open OpenCaliper and turn on "
                        "Tools > Claude (MCP).",
                        true);
    // The open window when there is one (checked on every call: it may have
    // been opened or closed since), else a project in this process.
    for (const QString& p : paths) {
      if (window.state() == QLocalSocket::ConnectedState) break;
      window.abort();
      window.connectToServer(p);
      window.waitForConnected(300);
    }
    if (window.state() == QLocalSocket::ConnectedState) {
      window.write(line({{"name", name}, {"arguments", args}}));
      window.waitForBytesWritten(5000);
      while (!window.canReadLine())
        if (!window.waitForReadyRead(kCallTimeoutMs))
          return textResult("The OpenCaliper window did not answer (was it closed?). Try again.", true);
      return QJsonDocument::fromJson(window.readLine()).object();
    }
    return callTool(file, name, args);
  };

  std::string text;
  while (std::getline(std::cin, text)) {
    if (text.find_first_not_of(" \t\r") == std::string::npos) continue;
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(text));
    const QJsonObject reply =
        doc.isObject() ? handle(doc.object(), call)
                       : QJsonObject{{"jsonrpc", "2.0"},
                                     {"id", QJsonValue()},
                                     {"error", QJsonObject{{"code", -32700}, {"message", "Parse error"}}}};
    if (reply.isEmpty()) continue;
    const QByteArray bytes = line(reply);
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), out);
    std::fflush(out);
  }
  return 0;
}

} // namespace mcp
