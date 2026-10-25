#ifdef _WIN32
#include <windows.h>
#endif

#include "view/Viewer.h"

#include <Aspect_DisplayConnection.hxx>
#include <Aspect_ScrollDelta.hxx>
#include <Aspect_VKeyFlags.hxx>
#include <Graphic3d_TransformPers.hxx>
#include <Aspect_NeutralWindow.hxx>
#include <Font_FontMgr.hxx>
#include <Message.hxx>
#include <Message_Messenger.hxx>
#include <OpenGl_Context.hxx>
#include <OpenGl_FrameBuffer.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <Prs3d_Drawer.hxx>
#include <V3d_Viewer.hxx>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>

#include <numbers>

namespace {

// Wraps the FBO that QOpenGLWidget renders into, so OCCT draws there
// and keeps sRGB conversion enabled on it.
class QtFrameBuffer : public OpenGl_FrameBuffer {
  DEFINE_STANDARD_RTTI_INLINE(QtFrameBuffer, OpenGl_FrameBuffer)
public:
  void BindBuffer(const Handle(OpenGl_Context)& gl) override {
    OpenGl_FrameBuffer::BindBuffer(gl);
    gl->SetFrameBufferSRGB(true, false);
  }
  void BindDrawBuffer(const Handle(OpenGl_Context)& gl) override {
    OpenGl_FrameBuffer::BindDrawBuffer(gl);
    gl->SetFrameBufferSRGB(true, false);
  }
};

Aspect_VKeyMouse qtButtons(Qt::MouseButtons b) {
  Aspect_VKeyMouse r = Aspect_VKeyMouse_NONE;
  if (b & Qt::LeftButton) r |= Aspect_VKeyMouse_LeftButton;
  if (b & Qt::MiddleButton) r |= Aspect_VKeyMouse_MiddleButton;
  if (b & Qt::RightButton) r |= Aspect_VKeyMouse_RightButton;
  return r;
}

Aspect_VKeyFlags qtModifiers(Qt::KeyboardModifiers m) {
  Aspect_VKeyFlags r = Aspect_VKeyFlags_NONE;
  if (m & Qt::ShiftModifier) r |= Aspect_VKeyFlags_SHIFT;
  if (m & Qt::ControlModifier) r |= Aspect_VKeyFlags_CTRL;
  if (m & Qt::AltModifier) r |= Aspect_VKeyFlags_ALT;
  return r;
}

} // namespace

Viewer::Viewer(QWidget* parent) : QOpenGLWidget(parent) {
  // Axis labels use "Courier", which many Linux installs lack; OCCT takes the first
  // existing font of the alias list.
  Handle(Font_FontMgr) fonts = Font_FontMgr::GetInstance();
  for (const char* f : {"liberation mono", "dejavu sans mono", "noto sans mono", "adwaita mono"})
    fonts->AddFontAlias("courier", f);
  // The view cube asks for generic families.
  for (const char* f : {"liberation sans", "dejavu sans", "noto sans", "adwaita sans", "cantarell"})
    fonts->AddFontAlias("sans-serif", f);
  for (const char* f : {"liberation serif", "dejavu serif", "noto serif"})
    fonts->AddFontAlias("serif", f);

#if defined(_WIN32)
  Handle(Aspect_DisplayConnection) display = new Aspect_DisplayConnection();
#else
  // Share Qt's X connection: Qt's GLX context is only valid on that Display.
  auto* x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
  Handle(Aspect_DisplayConnection) display =
      new Aspect_DisplayConnection((Aspect_XDisplay*)x11->display());
#endif
  Handle(OpenGl_GraphicDriver) driver = new OpenGl_GraphicDriver(display, false);
  // Qt swaps buffers and owns the window; OCCT only renders.
  driver->ChangeOptions().buffersNoSwap = true;
  driver->ChangeOptions().buffersOpaqueAlpha = true;
  driver->ChangeOptions().useSystemBuffer = false;

  m_viewer = new V3d_Viewer(driver);
  m_viewer->SetDefaultLights();
  m_viewer->SetLightOn();

  m_context = new AIS_InteractiveContext(m_viewer);
  m_context->SetDisplayMode(AIS_Shaded, false);
  m_context->DefaultDrawer()->SetFaceBoundaryDraw(true); // black edges on shaded faces
  // Hover in light blue, selection in orange (whole objects and sub-shapes).
  m_context->HighlightStyle(Prs3d_TypeOfHighlight_LocalDynamic)->SetColor(Quantity_NOC_DEEPSKYBLUE1);
  m_context->HighlightStyle(Prs3d_TypeOfHighlight_Dynamic)->SetColor(Quantity_NOC_DEEPSKYBLUE1);
  m_context->HighlightStyle(Prs3d_TypeOfHighlight_LocalSelected)->SetColor(Quantity_NOC_ORANGE);
  m_context->HighlightStyle(Prs3d_TypeOfHighlight_Selected)->SetColor(Quantity_NOC_ORANGE);
  m_context->DefaultDrawer()->SetDeviationCoefficient(0.0005); // finer display mesh:
  m_context->DefaultDrawer()->SetDeviationAngle(6.0 * std::numbers::pi / 180.0); // smooth round faces

  m_view = m_viewer->CreateView();
  m_view->SetImmediateUpdate(false);
  m_view->SetBgGradientColors(Quantity_Color(0.20, 0.22, 0.25, Quantity_TOC_sRGB),
                              Quantity_Color(0.55, 0.58, 0.62, Quantity_TOC_sRGB),
                              Aspect_GradientFillMethod_Vertical);

  // Clickable navigation cube in the top right corner (also shows the axes).
  m_viewCube = new AIS_ViewCube();
  m_viewCube->SetSize(55);
  m_viewCube->SetBoxColor(Quantity_Color(0.82, 0.84, 0.87, Quantity_TOC_sRGB));
  m_viewCube->SetTransformPersistence(
      new Graphic3d_TransformPers(Graphic3d_TMF_TriedronPers, Aspect_TOTP_RIGHT_UPPER, Graphic3d_Vec2i(85, 85)));
  m_viewCube->SetViewAnimation(myViewAnimation);
  m_viewCube->SetFixedAnimationLoop(false);
  m_viewCube->SetAutoStartAnimation(true);
  m_context->Display(m_viewCube, 0, 0, false);
  m_view->SetProj(V3d_XposYnegZpos);
  m_view->ChangeRenderingParams().IsAntialiasingEnabled = true;
  m_view->ChangeRenderingParams().NbMsaaSamples = 4;

  setMouseTracking(true);
  setFocusPolicy(Qt::StrongFocus);
  setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
  setMinimumSize(400, 300);

  // Release OCCT while the top-level window still exists; in the destructor
  // it is already gone and OCCT cannot make its context current any more.
  connect(qApp, &QCoreApplication::aboutToQuit, this, &Viewer::release);
}

Viewer::~Viewer() {
  release();
}

void Viewer::release() {
  if (m_viewer.IsNull()) return;
  makeCurrent();
  Handle(Aspect_DisplayConnection) display = m_viewer->Driver()->GetDisplayConnection();
  m_context->RemoveAll(false);
  m_context.Nullify();
  m_viewCube.Nullify();
  {
    // OCCT always reports one harmless "glXMakeCurrent() has failed" while
    // removing a view bound to Qt's context; mute the console just for that.
    const Handle(Message_Messenger)& msg = Message::DefaultMessenger();
    const Message_SequenceOfPrinters printers = msg->Printers();
    msg->ChangePrinters().Clear();
    m_view->Remove();
    msg->ChangePrinters() = printers;
  }
  m_view.Nullify();
  m_viewer.Nullify();
  display.Nullify();
  doneCurrent();
}

void Viewer::initializeGL() {
  // Nothing here: Qt may call this with a temporary offscreen surface before
  // the window is shown, which OCCT rejects. The view is bound in paintGL.
}

void Viewer::bindWindow() {
  const qreal ratio = devicePixelRatioF();
  const Graphic3d_Vec2i size(int(width() * ratio), int(height() * ratio));

  // OCCT needs a real native window. Qt renders this widget via an offscreen
  // surface (a pbuffer on X11), so hand OCCT the top-level window instead;
  // our own winId() would turn this widget into a native child window.
#if defined(_WIN32)
  const Aspect_Drawable nativeWin = (Aspect_Drawable)WindowFromDC(wglGetCurrentDC());
#else
  const Aspect_Drawable nativeWin = (Aspect_Drawable)window()->winId();
#endif

  // Drop GL errors left by Qt, or OCCT reports them as its own.
  while (QOpenGLWidget::context()->functions()->glGetError() != GL_NO_ERROR) {}

  Handle(OpenGl_Context) gl = new OpenGl_Context();
  if (!gl->Init(false)) {
    Message::SendFail("Viewer: OpenGL context could not be wrapped");
    return;
  }

  Handle(Aspect_NeutralWindow) window = Handle(Aspect_NeutralWindow)::DownCast(m_view->Window());
  if (window.IsNull()) {
    window = new Aspect_NeutralWindow();
    window->SetVirtual(true);
    window->SetNativeHandle(nativeWin);
  }
  window->SetSize(size.x(), size.y());
  try {
    m_view->SetWindow(window, gl->RenderingContext());
  } catch (const Standard_Failure& e) {
    Message::SendFail(TCollection_AsciiString("Viewer: ") + e.GetMessageString());
  }
}

void Viewer::paintGL() {
  if (m_view.IsNull()) return;
  if (m_view->Window().IsNull()) bindWindow();
  if (m_view->Window().IsNull()) return;

  Handle(OpenGl_GraphicDriver) driver = Handle(OpenGl_GraphicDriver)::DownCast(m_viewer->Driver());
  const Handle(OpenGl_Context)& gl = driver->GetSharedContext();
  Handle(OpenGl_FrameBuffer) fbo = gl->DefaultFrameBuffer();
  if (fbo.IsNull()) {
    fbo = new QtFrameBuffer();
    gl->SetDefaultFrameBuffer(fbo);
  }
  gl->ResetErrors(false);
  if (!fbo->InitWrapper(gl)) {
    // Happens on the very first paint, before Qt has created its FBO.
    fbo.Nullify();
    return;
  }

  // QOpenGLWidget resizes its FBO itself; follow it.
  Graphic3d_Vec2i oldSize;
  const Graphic3d_Vec2i newSize = fbo->GetVPSize();
  Handle(Aspect_NeutralWindow) window = Handle(Aspect_NeutralWindow)::DownCast(m_view->Window());
  window->Size(oldSize.x(), oldSize.y());
  if (newSize != oldSize) {
    window->SetSize(newSize.x(), newSize.y());
    m_view->MustBeResized();
    m_view->Invalidate();
  }

  if (m_fitPending) {
    m_fitPending = false;
    m_view->FitAll(0.1, false);
    m_view->Invalidate();
  }

  m_view->InvalidateImmediate();
  FlushViewEvents(m_context, m_view, true);
}

void Viewer::fitAll() {
  // Deferred to paintGL: the view needs its final size first.
  m_fitPending = true;
  update();
}

Graphic3d_Vec2i Viewer::toPixels(const QPointF& p) const {
  const qreal r = devicePixelRatioF();
  return Graphic3d_Vec2i(int(p.x() * r), int(p.y() * r));
}

void Viewer::mousePressEvent(QMouseEvent* e) {
  if (m_handler && m_handler->mousePress(e)) return;
  if (PressMouseButton(toPixels(e->position()), qtButtons(e->button()),
                       qtModifiers(e->modifiers()), false))
    update();
}

void Viewer::mouseReleaseEvent(QMouseEvent* e) {
  if (m_handler && m_handler->mouseRelease(e)) return;
  if (ReleaseMouseButton(toPixels(e->position()), qtButtons(e->button()),
                         qtModifiers(e->modifiers()), false))
    update();
}

void Viewer::mouseMoveEvent(QMouseEvent* e) {
  if (m_handler && m_handler->mouseMove(e)) return;
  if (UpdateMousePosition(toPixels(e->position()), qtButtons(e->buttons()),
                          qtModifiers(e->modifiers()), false))
    update();
}

void Viewer::wheelEvent(QWheelEvent* e) {
  const double delta = e->angleDelta().y() / 8.0;
  if (UpdateZoom(Aspect_ScrollDelta(toPixels(e->position()), delta)))
    update();
}

void Viewer::keyPressEvent(QKeyEvent* e) {
  if (m_handler && m_handler->keyPress(e)) return;
  QOpenGLWidget::keyPressEvent(e);
}

void Viewer::redraw() {
  m_view->Invalidate();
  update();
}

bool Viewer::pickOnPlane(const QPointF& pos, const gp_Ax3& plane, double& u, double& v) const {
  const Graphic3d_Vec2i px = toPixels(pos);
  double x, y, z, dx, dy, dz;
  m_view->ConvertWithProj(px.x(), px.y(), x, y, z, dx, dy, dz);
  const gp_Pnt from(x, y, z);
  const gp_Vec ray(dx, dy, dz), normal(plane.Direction());
  const double along = ray.Dot(normal);
  if (std::abs(along) < 1e-12) return false;
  const gp_Pnt hit = from.Translated(ray * (gp_Vec(from, plane.Location()).Dot(normal) / along));
  const gp_Vec local(plane.Location(), hit);
  u = local.Dot(gp_Vec(plane.XDirection()));
  v = local.Dot(gp_Vec(plane.YDirection()));
  return true;
}

double Viewer::pixelsToWorld(double pixels) const {
  return m_view->Convert(int(pixels * devicePixelRatioF()));
}

void Viewer::lookAt(const gp_Ax3& plane, double size) {
  const gp_Pnt& at = plane.Location();
  const gp_Dir& n = plane.Direction();
  const gp_Dir& up = plane.YDirection();
  m_view->SetAt(at.X(), at.Y(), at.Z());
  m_view->SetProj(n.X(), n.Y(), n.Z());
  m_view->SetUp(up.X(), up.Y(), up.Z());
  m_view->SetSize(size);
  redraw();
}

void Viewer::setStandardView(V3d_TypeOfOrientation orientation) {
  m_view->SetProj(orientation);
  m_fitPending = true;
  update();
}

void Viewer::handleViewRedraw(const Handle(AIS_InteractiveContext)& ctx,
                              const Handle(V3d_View)& view) {
  AIS_ViewController::handleViewRedraw(ctx, view);
  if (myToAskNextFrame) update(); // keep animating (e.g. inertia)
}
