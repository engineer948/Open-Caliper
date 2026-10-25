#pragma once

#include <AIS_InteractiveContext.hxx>
#include <AIS_ViewController.hxx>
#include <AIS_ViewCube.hxx>
#include <V3d_View.hxx>
#include <gp_Ax3.hxx>
#include <QOpenGLWidget>

class QKeyEvent;

// Gets mouse/keyboard events before the camera does (e.g. the sketch editor).
// Return true to consume the event.
class InputHandler {
public:
  virtual ~InputHandler() = default;
  virtual bool mousePress(QMouseEvent*) { return false; }
  virtual bool mouseMove(QMouseEvent*) { return false; }
  virtual bool mouseRelease(QMouseEvent*) { return false; }
  virtual bool keyPress(QKeyEvent*) { return false; }
};

// OCCT 3D view rendered into a QOpenGLWidget (same code on Linux and Windows).
// Qt owns the GL context; OCCT draws into Qt's framebuffer.
// Mouse: left = orbit, middle = pan, wheel = zoom (AIS_ViewController defaults).
class Viewer : public QOpenGLWidget, public AIS_ViewController {
  Q_OBJECT
public:
  explicit Viewer(QWidget* parent = nullptr);
  ~Viewer() override;

  const Handle(AIS_InteractiveContext)& context() const { return m_context; }
  const Handle(V3d_View)& view() const { return m_view; }
  const Handle(V3d_Viewer)& v3dViewer() const { return m_viewer; }
  void fitAll();
  void redraw(); // after changing displayed objects

  void setInputHandler(InputHandler* handler) { m_handler = handler; }
  // Widget position -> plane-local (u, v). False if the view is edge-on to the plane.
  bool pickOnPlane(const QPointF& pos, const gp_Ax3& plane, double& u, double& v) const;
  double pixelsToWorld(double pixels) const;
  // Looks straight at the plane, `size` model units across.
  void lookAt(const gp_Ax3& plane, double size);
  void setStandardView(V3d_TypeOfOrientation orientation); // animated

protected:
  void initializeGL() override;
  void paintGL() override;

  void mousePressEvent(QMouseEvent*) override;
  void mouseReleaseEvent(QMouseEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void wheelEvent(QWheelEvent*) override;
  void keyPressEvent(QKeyEvent*) override;

  void handleViewRedraw(const Handle(AIS_InteractiveContext)& ctx,
                        const Handle(V3d_View)& view) override;

private:
  void bindWindow();
  void release();
  Graphic3d_Vec2i toPixels(const QPointF& p) const;

  Handle(V3d_Viewer) m_viewer;
  Handle(V3d_View) m_view;
  Handle(AIS_InteractiveContext) m_context;
  Handle(AIS_ViewCube) m_viewCube;
  bool m_fitPending = false;
  InputHandler* m_handler = nullptr;
};
