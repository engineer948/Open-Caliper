#include "app/MainWindow.h"
#include "view/Viewer.h"

#include <Message.hxx>
#include <Message_Messenger.hxx>
#include <Message_Printer.hxx>

#include <QApplication>
#include <QSurfaceFormat>
#include <QTimer>

int main(int argc, char** argv) {
#if defined(__linux__)
  // OCCT on Linux renders through GLX, so Qt must use X11 (XWayland on Wayland).
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
    qputenv("QT_QPA_PLATFORM", "xcb");
#endif
  QApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
  {
    // Set globally so the top-level window and the 3D view get the same GL
    // config; OCCT binds Qt's context to the top-level window (see Viewer).
    QSurfaceFormat f;
    f.setDepthBufferSize(24);
    f.setStencilBufferSize(8);
    f.setProfile(QSurfaceFormat::CompatibilityProfile);
    QSurfaceFormat::setDefaultFormat(f);
  }

  // OCCT console output: warnings and errors only, no info chatter.
  for (const Handle(Message_Printer)& p : Message::DefaultMessenger()->Printers())
    p->SetTraceLevel(Message_Warning);

  QApplication app(argc, argv);
  QApplication::setApplicationName("OpenCaliper");
  QApplication::setApplicationVersion(OPENCALIPER_VERSION);
  QApplication::setDesktopFileName("io.github.engineer948.OpenCaliper"); // taskbar icon on Wayland/X11
  QApplication::setStyle("Fusion"); // same look on every platform
  MainWindow w;
  w.show();

  // --screenshot <file.png>: save the 3D view and quit (for testing).
  const QStringList args = app.arguments();
  if (args.size() > 1 && !args[1].startsWith("--")) w.openProject(args[1]); // opencaliper file.ocal
  const qsizetype i = args.indexOf("--screenshot");
  if (i >= 0 && i + 1 < args.size()) {
    const QString path = args[i + 1];
    QTimer::singleShot(1500, &w, [&w, path] {
      w.viewer()->grabFramebuffer().save(path);
      QApplication::quit();
    });
  }
  return app.exec();
}
