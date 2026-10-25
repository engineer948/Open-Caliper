#include "sketch/SketchEditor.h"
#include "sketch/SketchShape.h"

#include <Aspect_Grid.hxx>
#include <BRep_Builder.hxx>
#include <Prs3d_Drawer.hxx>
#include <Prs3d_PointAspect.hxx>
#include <V3d_Viewer.hxx>

#include <QInputDialog>
#include <QKeyEvent>
#include <QMouseEvent>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

using namespace sketch;
using K = ConstraintKind;

namespace {

constexpr double kSnapPixels = 8;

Handle(AIS_Shape) makeLayer(const Handle(AIS_InteractiveContext)& ctx, Quantity_NameOfColor color, double width) {
  TopoDS_Compound empty;
  BRep_Builder().MakeCompound(empty);
  Handle(AIS_Shape) ais = new AIS_Shape(empty);
  ais->SetColor(color);
  ais->SetWidth(width);
  ais->Attributes()->SetPointAspect(new Prs3d_PointAspect(Aspect_TOM_O_POINT, color, 1.5));
  ais->SetZLayer(Graphic3d_ZLayerId_Top);
  ctx->Display(ais, AIS_WireFrame, -1 /* not selectable */, false);
  return ais;
}

double segmentDistance(double px, double py, double ax, double ay, double bx, double by) {
  const double dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
  double t = len2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0;
  t = std::clamp(t, 0.0, 1.0);
  return std::hypot(px - (ax + t * dx), py - (ay + t * dy));
}

} // namespace

SketchEditor::SketchEditor(Viewer* viewer, QObject* parent) : QObject(parent), m_viewer(viewer) {}

void SketchEditor::begin(const gp_Ax3& plane, Sketch initial) {
  m_sketch = std::move(initial);
  m_plane = plane;
  m_picks.clear();
  m_selection.clear();
  m_active = true;

  const Handle(AIS_InteractiveContext)& ctx = m_viewer->context();
  m_aisNormal = makeLayer(ctx, Quantity_NOC_BLUE3, 2.0);
  m_aisSelected = makeLayer(ctx, Quantity_NOC_ORANGE, 3.0);
  m_aisPreview = makeLayer(ctx, Quantity_NOC_GRAY30, 1.0);

  const Handle(V3d_Viewer)& v3d = m_viewer->v3dViewer();
  v3d->SetPrivilegedPlane(plane);
  v3d->SetRectangularGridValues(0, 0, 10, 10, 0);
  v3d->SetRectangularGridGraphicValues(1000, 1000, 0);
  v3d->ActivateGrid(Aspect_GT_Rectangular, Aspect_GDM_Lines);

  m_viewer->setInputHandler(this);
  m_viewer->lookAt(plane, 200);
  setTool(m_sketch.entities.empty() ? Tool::Line : Tool::Select);
  redraw();
  reportState();
}

Sketch SketchEditor::finish() {
  m_active = false;
  m_picks.clear();
  m_selection.clear();
  m_dragPoint = -1;
  m_viewer->setInputHandler(nullptr);
  m_viewer->unsetCursor();
  m_viewer->v3dViewer()->DeactivateGrid();
  const Handle(AIS_InteractiveContext)& ctx = m_viewer->context();
  for (Handle(AIS_Shape)* ais : {&m_aisNormal, &m_aisSelected, &m_aisPreview}) {
    ctx->Remove(*ais, false);
    ais->Nullify();
  }
  m_viewer->redraw();
  return std::exchange(m_sketch, {});
}

void SketchEditor::setTool(Tool tool) {
  cancelOperation();
  m_tool = tool;
  if (tool == Tool::Select) m_viewer->unsetCursor();
  else m_viewer->setCursor(Qt::CrossCursor);
  emit toolChanged(tool);
}

// --- picking ----------------------------------------------------------------

bool SketchEditor::pickAt(const QPointF& pos, bool snap, Pick& out) const {
  out = {};
  if (!m_viewer->pickOnPlane(pos, m_plane, out.u, out.v)) return false;
  if (!snap) return true;

  // Existing points win, then the grid. The grid step follows the zoom.
  double best = m_viewer->pixelsToWorld(kSnapPixels);
  for (int p : usedPoints()) {
    const double d = std::hypot(m_sketch.x(p) - out.u, m_sketch.y(p) - out.v);
    if (d < best) {
      best = d;
      out.point = p;
    }
  }
  if (out.point >= 0) {
    out.u = m_sketch.x(out.point);
    out.v = m_sketch.y(out.point);
    return true;
  }
  // Largest power of 10 below ~10 px, so snapping is never coarser than that.
  const double step = std::pow(10.0, std::floor(std::log10(m_viewer->pixelsToWorld(10))));
  out.u = std::round(out.u / step) * step;
  out.v = std::round(out.v / step) * step;
  for (int p : usedPoints()) // landed exactly on a point: share it, don't duplicate
    if (std::hypot(m_sketch.x(p) - out.u, m_sketch.y(p) - out.v) < 1e-9) out.point = p;
  return true;
}

int SketchEditor::resolve(const Pick& p) {
  return p.point >= 0 ? p.point : m_sketch.addPoint(p.u, p.v);
}

std::vector<int> SketchEditor::usedPoints() const {
  std::vector<bool> used(m_sketch.points.size(), false);
  for (const Entity& e : m_sketch.entities)
    for (int p : e.p)
      if (p >= 0) used[p] = true;
  std::vector<int> result;
  for (int i = 0; i < int(used.size()); ++i)
    if (used[i]) result.push_back(i);
  return result;
}

SketchEditor::Item SketchEditor::hitTest(double u, double v) const {
  const double tol = m_viewer->pixelsToWorld(kSnapPixels);
  for (int p : usedPoints())
    if (std::hypot(m_sketch.x(p) - u, m_sketch.y(p) - v) < tol) return {true, p};

  Item best;
  double bestDist = tol;
  for (int i = 0; i < int(m_sketch.entities.size()); ++i) {
    const Entity& e = m_sketch.entities[i];
    const double cx = m_sketch.x(e.p[0]), cy = m_sketch.y(e.p[0]);
    double d = 0;
    if (e.kind == EntityKind::Line) {
      d = segmentDistance(u, v, cx, cy, m_sketch.x(e.p[1]), m_sketch.y(e.p[1]));
    } else {
      d = std::abs(std::hypot(u - cx, v - cy) - m_sketch.radius(i));
      if (e.kind == EntityKind::Arc) { // outside the swept angle: distance to the ends
        const double a = std::atan2(v - cy, u - cx);
        const double a1 = std::atan2(m_sketch.y(e.p[1]) - cy, m_sketch.x(e.p[1]) - cx);
        const double a2 = std::atan2(m_sketch.y(e.p[2]) - cy, m_sketch.x(e.p[2]) - cx);
        constexpr double pi = std::numbers::pi;
        const auto ccw = [](double from, double to) { return std::remainder(to - from - pi, 2 * pi) + pi; };
        if (ccw(a1, a) > ccw(a1, a2))
          d = std::min(std::hypot(u - m_sketch.x(e.p[1]), v - m_sketch.y(e.p[1])),
                       std::hypot(u - m_sketch.x(e.p[2]), v - m_sketch.y(e.p[2])));
      }
    }
    if (d < bestDist) {
      bestDist = d;
      best = {false, i};
    }
  }
  return best;
}

SketchEditor::Selected SketchEditor::selected() const {
  Selected s;
  for (const Item& it : m_selection) {
    if (it.isPoint) s.points.push_back(it.index);
    else if (m_sketch.isCircular(it.index)) s.circles.push_back(it.index);
    else s.lines.push_back(it.index);
  }
  return s;
}

// --- drawing tools ------------------------------------------------------------

void SketchEditor::toolClick(const Pick& pick) {
  m_picks.push_back(pick);
  const std::vector<Pick>& k = m_picks;

  switch (m_tool) {
  case Tool::Line: {
    if (k.size() < 2) return;
    if (std::hypot(k[1].u - k[0].u, k[1].v - k[0].v) < 1e-9) {
      m_picks.pop_back();
      return;
    }
    const bool closesChain = k[1].point >= 0; // ended on an existing point
    const int a = resolve(k[0]), b = resolve(k[1]);
    const int line = m_sketch.addLine(a, b);
    // Like Fusion: exactly axis-aligned lines get a constraint for free.
    if (std::abs(k[1].v - k[0].v) < 1e-9) m_sketch.addConstraint({K::Horizontal, line});
    else if (std::abs(k[1].u - k[0].u) < 1e-9) m_sketch.addConstraint({K::Vertical, line});
    m_picks = closesChain ? std::vector<Pick>{} : std::vector<Pick>{Pick{m_sketch.x(b), m_sketch.y(b), b}};
    break;
  }
  case Tool::Circle: {
    if (k.size() < 2) return;
    const double r = std::hypot(k[1].u - k[0].u, k[1].v - k[0].v);
    if (r > 1e-9) m_sketch.addCircle(resolve(k[0]), r);
    m_picks.clear();
    break;
  }
  case Tool::Arc: { // center, start, end
    if (k.size() < 3) return;
    const double r = std::hypot(k[1].u - k[0].u, k[1].v - k[0].v);
    if (r > 1e-9) {
      const int c = resolve(k[0]), s = resolve(k[1]);
      int e = k[2].point;
      if (e < 0) { // put a free end exactly on the circle
        const double a = std::atan2(k[2].v - k[0].v, k[2].u - k[0].u);
        e = m_sketch.addPoint(k[0].u + r * std::cos(a), k[0].v + r * std::sin(a));
      }
      m_sketch.addArc(c, s, e);
      m_sketch.solve();
    }
    m_picks.clear();
    break;
  }
  case Tool::Select:
    m_picks.clear();
    return;
  }
  reportState();
  redraw();
}

void SketchEditor::cancelOperation() {
  m_picks.clear();
  if (m_active) redrawPreview();
}

// --- constraints --------------------------------------------------------------

bool SketchEditor::tryConstraints(const std::vector<Constraint>& cs) {
  const size_t before = m_sketch.constraints.size();
  for (const Constraint& c : cs) m_sketch.addConstraint(c);
  if (m_sketch.solve() == SolveResult::Failed) {
    m_sketch.constraints.resize(before);
    emit message(tr("Conflicting constraint, not added."));
    return false;
  }
  reportState();
  return true;
}

void SketchEditor::constrain(ConstraintKind kind) {
  const Selected s = selected();
  std::vector<Constraint> cs;
  switch (kind) {
  case K::Coincident:
    if (s.only(2, 0, 0)) cs = {{K::Coincident, s.points[0], s.points[1]}};
    else if (s.only(1, 1, 0)) cs = {{K::PointOnLine, s.points[0], s.lines[0]}};
    else if (s.only(1, 0, 1)) cs = {{K::PointOnCircle, s.points[0], s.circles[0]}};
    break;
  case K::Horizontal:
  case K::Vertical:
    if (!s.lines.empty() && s.points.empty() && s.circles.empty())
      for (int l : s.lines) cs.push_back({kind, l});
    break;
  case K::Parallel:
  case K::Perpendicular:
    if (s.only(0, 2, 0)) cs = {{kind, s.lines[0], s.lines[1]}};
    break;
  case K::Equal:
    if (s.only(0, 2, 0)) cs = {{kind, s.lines[0], s.lines[1]}};
    else if (s.only(0, 0, 2)) cs = {{kind, s.circles[0], s.circles[1]}};
    break;
  case K::Tangent:
    if (s.only(0, 1, 1)) cs = {{kind, s.lines[0], s.circles[0]}};
    break;
  default:
    break;
  }
  if (cs.empty()) {
    emit message(tr("This constraint does not fit the selection."));
    return;
  }
  if (tryConstraints(cs)) m_selection.clear();
  redraw();
}

void SketchEditor::dimension() {
  const Selected s = selected();
  Constraint c{K::Length};
  if (s.only(0, 1, 0)) {
    c = {K::Length, s.lines[0], -1, m_sketch.length(s.lines[0])};
  } else if (s.only(0, 0, 1)) {
    c = {K::Radius, s.circles[0], -1, m_sketch.radius(s.circles[0])};
  } else if (s.only(2, 0, 0)) {
    c = {K::Distance, s.points[0], s.points[1], m_sketch.distance(s.points[0], s.points[1])};
  } else {
    emit message(tr("Dimension: select a line, a circle/arc or two points."));
    return;
  }
  bool ok = false;
  const double value = QInputDialog::getDouble(m_viewer, tr("Dimension"),
                                               c.kind == K::Radius ? tr("Radius:") : tr("Length:"),
                                               c.value, 1e-6, 1e9, 3, &ok);
  if (!ok) return;
  c.value = value;
  if (tryConstraints({c})) m_selection.clear();
  redraw();
}

void SketchEditor::toggleFix() {
  std::vector<int> pts;
  for (const Item& it : m_selection) {
    if (it.isPoint) pts.push_back(it.index);
    else
      for (int p : m_sketch.entities[it.index].p)
        if (p >= 0) pts.push_back(p);
  }
  if (pts.empty()) {
    emit message(tr("Fix: select points or curves."));
    return;
  }
  const bool allFixed = std::all_of(pts.begin(), pts.end(), [&](int p) { return m_sketch.points[p].fixed; });
  for (int p : pts) m_sketch.points[p].fixed = !allFixed;
  reportState();
}

void SketchEditor::deleteSelection() {
  std::vector<int> ents;
  for (const Item& it : m_selection)
    if (!it.isPoint) ents.push_back(it.index);
  std::sort(ents.rbegin(), ents.rend()); // highest first keeps the other indices valid
  for (int e : ents) m_sketch.removeEntity(e);
  m_selection.clear();
  m_sketch.solve();
  reportState();
  redraw();
}

// --- input --------------------------------------------------------------------

bool SketchEditor::mousePress(QMouseEvent* e) {
  if (e->button() == Qt::RightButton && !m_picks.empty()) {
    cancelOperation();
    return true;
  }
  if (e->button() != Qt::LeftButton) return false;

  if (m_tool != Tool::Select) {
    Pick p;
    if (pickAt(e->position(), true, p)) toolClick(p);
    return true;
  }

  Pick p;
  if (!pickAt(e->position(), false, p)) return true;
  const Item hit = hitTest(p.u, p.v);
  const bool add = e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier);
  if (!add) m_selection.clear();
  if (hit.index >= 0) {
    const auto it = std::find(m_selection.begin(), m_selection.end(), hit);
    if (it == m_selection.end()) m_selection.push_back(hit);
    else m_selection.erase(it);
    if (hit.isPoint && !m_sketch.points[hit.index].fixed) m_dragPoint = hit.index;
  }
  redraw();
  return true;
}

bool SketchEditor::mouseMove(QMouseEvent* e) {
  if (m_dragPoint >= 0) {
    Pick p;
    if (!pickAt(e->position(), false, p)) return true;
    // Hold the dragged point where the mouse is and let the rest follow.
    const std::vector<double> saved = m_sketch.params;
    m_sketch.points[m_dragPoint].fixed = true;
    m_sketch.setPoint(m_dragPoint, p.u, p.v);
    if (m_sketch.solve() == SolveResult::Failed) m_sketch.params = saved;
    m_sketch.points[m_dragPoint].fixed = false;
    redraw();
    return true;
  }
  if (m_tool != Tool::Select) {
    m_hoverValid = pickAt(e->position(), true, m_hover);
    redrawPreview();
  }
  return false; // the camera still sees moves (middle-drag pan)
}

bool SketchEditor::mouseRelease(QMouseEvent* e) {
  if (e->button() == Qt::LeftButton) {
    if (m_dragPoint >= 0) {
      m_dragPoint = -1;
      reportState();
    }
    return true;
  }
  return false;
}

bool SketchEditor::keyPress(QKeyEvent* e) {
  switch (e->key()) {
  case Qt::Key_Escape:
    if (!m_picks.empty()) cancelOperation();
    else if (m_tool != Tool::Select) setTool(Tool::Select);
    else {
      m_selection.clear();
      redraw();
    }
    return true;
  case Qt::Key_Delete:
  case Qt::Key_Backspace:
    deleteSelection();
    return true;
  default:
    return false;
  }
}

// --- display ------------------------------------------------------------------

void SketchEditor::reportState() {
  const int dof = m_sketch.dof();
  emit message(dof == 0 ? tr("Sketch fully constrained") : tr("Sketch: %1 degrees of freedom left").arg(dof));
}

void SketchEditor::redraw() {
  std::vector<int> normalE, selectedE, normalP, selectedP;
  for (int i = 0; i < int(m_sketch.entities.size()); ++i)
    (std::count(m_selection.begin(), m_selection.end(), Item{false, i}) ? selectedE : normalE).push_back(i);
  for (int p : usedPoints())
    (std::count(m_selection.begin(), m_selection.end(), Item{true, p}) ? selectedP : normalP).push_back(p);

  const Handle(AIS_InteractiveContext)& ctx = m_viewer->context();
  m_aisNormal->SetShape(makeShape(m_sketch, m_plane, normalE, normalP));
  ctx->Redisplay(m_aisNormal, false);
  m_aisSelected->SetShape(makeShape(m_sketch, m_plane, selectedE, selectedP));
  ctx->Redisplay(m_aisSelected, false);
  redrawPreview();
}

void SketchEditor::redrawPreview() {
  BRep_Builder b;
  TopoDS_Compound preview;
  b.MakeCompound(preview);
  auto add = [&](const TopoDS_Shape& s) {
    if (!s.IsNull()) b.Add(preview, s);
  };

  if (m_hoverValid && m_tool != Tool::Select) {
    const Pick& h = m_hover;
    const std::vector<Pick>& k = m_picks;
    Sketch marker; // the snap position as a dot
    marker.addPoint(h.u, h.v);
    add(makeShape(marker, m_plane, {}, {0}));
    if (m_tool == Tool::Line && k.size() == 1) add(makeSegment(m_plane, k[0].u, k[0].v, h.u, h.v));
    if (m_tool == Tool::Circle && k.size() == 1) add(makeCircle(m_plane, k[0].u, k[0].v, std::hypot(h.u - k[0].u, h.v - k[0].v)));
    if (m_tool == Tool::Arc && k.size() == 1) add(makeSegment(m_plane, k[0].u, k[0].v, h.u, h.v));
    if (m_tool == Tool::Arc && k.size() == 2) add(makeArc(m_plane, k[0].u, k[0].v, k[1].u, k[1].v, h.u, h.v));
  }
  m_aisPreview->SetShape(preview);
  m_viewer->context()->Redisplay(m_aisPreview, false);
  m_viewer->redraw();
}
