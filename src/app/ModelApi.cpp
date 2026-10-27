#include "app/ModelApi.h"
#include "app/ProjectFile.h"
#include "io/Exchange.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <numbers>
#include <stdexcept>

using namespace model;
using K = sketch::ConstraintKind;

namespace mcp {
namespace {

[[noreturn]] void fail(const QString& message) { throw std::runtime_error(message.toStdString()); }

// Short numbers in replies: 4 decimals are plenty for mm.
double r4(double v) { return std::round(v * 1e4) / 1e4 + 0.0; } // + 0.0: no "-0"
QJsonArray vec(const gp_XYZ& v) { return {r4(v.X()), r4(v.Y()), r4(v.Z())}; }

gp_XYZ xyzArg(const QJsonValue& v, const char* what) {
  const QJsonArray a = v.toArray();
  if (a.size() != 3 || !a[0].isDouble() || !a[1].isDouble() || !a[2].isDouble())
    fail(QString("%1: expected [x, y, z]").arg(what));
  return {a[0].toDouble(), a[1].toDouble(), a[2].toDouble()};
}

struct XY {
  double x, y;
};
XY xyArg(const QJsonValue& v, const char* what) {
  const QJsonArray a = v.toArray();
  if (a.size() != 2 || !a[0].isDouble() || !a[1].isDouble()) fail(QString("%1: expected [x, y]").arg(what));
  return {a[0].toDouble(), a[1].toDouble()};
}

// A size given as a number or as a formula with parameters ("width / 2").
struct Size {
  double value = 0;
  std::string expr; // empty for a plain number
};
Size sizeArg(const Document& doc, const QJsonValue& v, const QString& what) {
  if (v.isDouble()) return {v.toDouble(), {}};
  if (!v.isString()) fail(what + ": expected a number or a formula such as \"width / 2\"");
  const std::string text = v.toString().trimmed().toStdString();
  try {
    return {doc.evaluate(text), isPlainNumber(text) ? std::string() : text};
  } catch (const std::exception& e) {
    fail(what + ": " + QString::fromStdString(e.what()));
  }
}

int intArg(const QJsonObject& args, const char* key) {
  const QJsonValue v = args[key];
  if (!v.isDouble() || v.toDouble() != std::floor(v.toDouble())) fail(QString("'%1': expected a whole number").arg(key));
  return v.toInt();
}

int featureIndex(const Document& doc, const QJsonObject& args) {
  const int i = intArg(args, "index");
  if (i < 0 || i >= int(doc.size()))
    fail(QString("No step %1: the timeline has steps 0 to %2.").arg(i).arg(int(doc.size()) - 1));
  return i;
}

QString json(const QJsonValue& v) {
  return QString::fromUtf8(v.isArray() ? QJsonDocument(v.toArray()).toJson(QJsonDocument::Indented)
                                       : QJsonDocument(v.toObject()).toJson(QJsonDocument::Indented));
}

// --- reading the model ------------------------------------------------------------

const char* const kConstraintNames[] = {"coincident",      "horizontal", "vertical", "parallel", "perpendicular",
                                        "equal",           "tangent",    "point-on-line", "point-on-circle",
                                        "distance",        "length",     "radius",   "symmetric"};

QJsonObject bodyInfo(const TopoDS_Shape& body) {
  if (body.IsNull()) return {};
  const MassProperties m = massProperties(body);
  Bnd_Box box;
  BRepBndLib::AddOptimal(body, box, false);
  double x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
  if (!box.IsVoid()) box.Get(x0, y0, z0, x1, y1, z1);
  TopTools_IndexedMapOfShape faces, edges;
  TopExp::MapShapes(body, TopAbs_FACE, faces);
  TopExp::MapShapes(body, TopAbs_EDGE, edges);
  return {{"volume_mm3", r4(m.volume)},
          {"area_mm2", r4(m.area)},
          {"size", vec(m.size)},
          {"min", vec(gp_XYZ(x0, y0, z0))},
          {"max", vec(gp_XYZ(x1, y1, z1))},
          {"center_of_mass", vec(m.center.XYZ())},
          {"faces", faces.Extent()},
          {"edges", edges.Extent()}};
}

// A sketch with point coordinates instead of the solver's parameter indices.
QJsonObject sketchInfo(const SketchFeature& f) {
  QJsonObject o = project::featureToJson(f);
  o.remove("params");
  const sketch::Sketch& s = f.sketch;
  QJsonArray points, curves, constraints;
  for (size_t i = 0; i < s.points.size(); ++i) {
    QJsonObject p{{"index", int(i)}, {"x", r4(s.x(int(i)))}, {"y", r4(s.y(int(i)))}};
    if (s.points[i].fixed) p["fixed"] = true;
    points.append(p);
  }
  for (size_t i = 0; i < s.entities.size(); ++i) {
    const sketch::Entity& e = s.entities[i];
    QJsonObject c{{"index", int(i)}};
    if (e.kind == sketch::EntityKind::Line) {
      c["kind"] = "line";
      c["points"] = QJsonArray{e.p[0], e.p[1]};
      c["length"] = r4(s.length(int(i)));
    } else {
      c["kind"] = e.kind == sketch::EntityKind::Circle ? "circle" : "arc";
      c["points"] = e.kind == sketch::EntityKind::Circle ? QJsonArray{e.p[0]} : QJsonArray{e.p[0], e.p[1], e.p[2]};
      c["radius"] = r4(s.radius(int(i)));
    }
    if (e.construction) c["construction"] = true;
    curves.append(c);
  }
  for (size_t i = 0; i < s.constraints.size(); ++i) {
    const sketch::Constraint& c = s.constraints[i];
    QJsonObject j{{"index", int(i)}, {"kind", kConstraintNames[int(c.kind)]}, {"a", c.a}};
    if (c.b >= 0) j["b"] = c.b;
    if (c.c >= 0) j["c"] = c.c;
    if (c.kind == K::Distance || c.kind == K::Length || c.kind == K::Radius) j["value"] = r4(c.value);
    if (!c.expr.empty()) j["expr"] = QString::fromStdString(c.expr);
    constraints.append(j);
  }
  o["points"] = points;
  o["curves"] = curves;
  o["constraints"] = constraints;
  o.remove("entities");
  o["degrees_of_freedom"] = s.dof();
  bool closed = true;
  try {
    f.profile();
  } catch (...) {
    closed = false;
  }
  o["closed_profile"] = closed;
  return o;
}

QJsonObject featureInfo(const Document& doc, size_t i) {
  QJsonObject o;
  if (const auto* s = doc.as<SketchFeature>(i)) {
    o = sketchInfo(*s);
    o["used_by_later_step"] = doc.isConsumed(i);
  } else {
    o = project::featureToJson(doc.feature(i));
    o.remove("keys"); // internal: where picked edges/faces were
    o.remove("key");
    o.remove("brep");
  }
  QJsonObject r{{"index", int(i)}};
  for (auto it = o.begin(); it != o.end(); ++it) r[it.key()] = it.value();
  if (!doc.error(i).empty()) r["error"] = QString::fromStdString(doc.error(i));
  return r;
}

QJsonObject getModel(Host& host) {
  const Document& doc = host.document();
  QJsonArray params, features;
  for (const Parameter& p : doc.parameters()) {
    QJsonObject o{{"name", QString::fromStdString(p.name)}, {"expr", QString::fromStdString(p.expression)}};
    const auto it = doc.values().find(p.name);
    if (it != doc.values().end()) o["value"] = r4(it->second);
    params.append(o);
  }
  for (size_t i = 0; i < doc.size(); ++i) features.append(featureInfo(doc, i));
  QJsonObject o{{"mode", host.live() ? "live: the open OpenCaliper window" : "file: no OpenCaliper window is open"},
                {"project", host.projectPath().isEmpty() ? QJsonValue() : QJsonValue(host.projectPath())},
                {"parameters", params},
                {"steps", features},
                {"body", doc.body().IsNull() ? QJsonValue() : QJsonValue(bodyInfo(doc.body()))}};
  if (!doc.parameterError().empty()) o["parameter_error"] = QString::fromStdString(doc.parameterError());
  return o;
}

const char* curveName(GeomAbs_CurveType t) {
  switch (t) {
  case GeomAbs_Line: return "line";
  case GeomAbs_Circle: return "circle";
  case GeomAbs_Ellipse: return "ellipse";
  case GeomAbs_BSplineCurve: return "bspline";
  default: return "other";
  }
}

const char* surfaceName(GeomAbs_SurfaceType t) {
  switch (t) {
  case GeomAbs_Plane: return "plane";
  case GeomAbs_Cylinder: return "cylinder";
  case GeomAbs_Cone: return "cone";
  case GeomAbs_Sphere: return "sphere";
  case GeomAbs_Torus: return "torus";
  default: return "other";
  }
}

QJsonObject findEdges(const Document& doc, const QJsonObject& args) {
  const TopoDS_Shape body = doc.body();
  if (body.IsNull()) fail("There is no body yet.");
  TopTools_IndexedMapOfShape edges, faces;
  TopExp::MapShapes(body, TopAbs_EDGE, edges);
  TopExp::MapShapes(body, TopAbs_FACE, faces);
  TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
  TopExp::MapShapesAndAncestors(body, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
  const QString type = args["type"].toString();
  const bool hasNear = args.contains("near");
  const gp_XYZ near = hasNear ? xyzArg(args["near"], "near") : gp_XYZ();

  std::vector<std::pair<double, QJsonObject>> found;
  for (int i = 1; i <= edges.Extent(); ++i) {
    const TopoDS_Edge& e = TopoDS::Edge(edges(i));
    if (BRep_Tool::Degenerated(e)) continue;
    BRepAdaptor_Curve c(e);
    const char* name = curveName(c.GetType());
    if (!type.isEmpty() && type != name) continue;
    const gp_Pnt mid = c.Value((c.FirstParameter() + c.LastParameter()) / 2);
    QJsonObject o{{"index", i}, {"type", name}, {"length", r4(GCPnts_AbscissaPoint::Length(c))}, {"midpoint", vec(mid.XYZ())}};
    if (c.GetType() == GeomAbs_Line) {
      o["direction"] = vec(c.Line().Direction().XYZ());
    } else if (c.GetType() == GeomAbs_Circle) {
      o["center"] = vec(c.Circle().Location().XYZ());
      o["radius"] = r4(c.Circle().Radius());
      o["axis"] = vec(c.Circle().Axis().Direction().XYZ());
    }
    QJsonArray adjacent;
    if (const TopTools_ListOfShape* fs = edgeFaces.Seek(e))
      for (const TopoDS_Shape& f : *fs)
        if (!adjacent.contains(faces.FindIndex(f))) adjacent.append(faces.FindIndex(f));
    o["faces"] = adjacent;
    found.emplace_back(hasNear ? (mid.XYZ() - near).Modulus() : double(i), o);
  }
  std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  const int limit = args.contains("limit") ? std::max(1, intArg(args, "limit")) : 100;
  QJsonArray list;
  for (int i = 0; i < int(found.size()) && i < limit; ++i) list.append(found[i].second);
  return {{"edges", list}, {"matching", int(found.size())}, {"total_edges", edges.Extent()}};
}

QJsonObject findFaces(const Document& doc, const QJsonObject& args) {
  const TopoDS_Shape body = doc.body();
  if (body.IsNull()) fail("There is no body yet.");
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(body, TopAbs_FACE, faces);
  const QString type = args["type"].toString();
  const bool hasNormal = args.contains("normal");
  const gp_XYZ wanted = hasNormal ? xyzArg(args["normal"], "normal") : gp_XYZ();
  if (hasNormal && wanted.Modulus() < 1e-9) fail("normal: must not be [0, 0, 0]");
  const bool hasNear = args.contains("near");
  const gp_XYZ near = hasNear ? xyzArg(args["near"], "near") : gp_XYZ();

  std::vector<std::pair<double, QJsonObject>> found;
  for (int i = 1; i <= faces.Extent(); ++i) {
    const TopoDS_Face& f = TopoDS::Face(faces(i));
    BRepAdaptor_Surface s(f);
    const char* name = surfaceName(s.GetType());
    if (!type.isEmpty() && type != name) continue;
    GProp_GProps props;
    BRepGProp::SurfaceProperties(f, props);
    QJsonObject o{{"index", i}, {"type", name}, {"area", r4(props.Mass())}, {"center", vec(props.CentreOfMass().XYZ())}};
    if (s.GetType() == GeomAbs_Plane) {
      gp_Dir n = s.Plane().Axis().Direction();
      if (f.Orientation() == TopAbs_REVERSED) n.Reverse();
      o["normal"] = vec(n.XYZ()); // out of the material
      if (hasNormal && n.XYZ().Dot(wanted.Normalized()) < 0.999) continue;
    } else {
      if (hasNormal) continue;
      if (s.GetType() == GeomAbs_Cylinder) {
        o["axis"] = vec(s.Cylinder().Axis().Direction().XYZ());
        o["axis_point"] = vec(s.Cylinder().Location().XYZ());
        o["radius"] = r4(s.Cylinder().Radius());
      }
    }
    found.emplace_back(hasNear ? (props.CentreOfMass().XYZ() - near).Modulus() : double(i), o);
  }
  std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  const int limit = args.contains("limit") ? std::max(1, intArg(args, "limit")) : 100;
  QJsonArray list;
  for (int i = 0; i < int(found.size()) && i < limit; ++i) list.append(found[i].second);
  return {{"faces", list}, {"matching", int(found.size())}, {"total_faces", faces.Extent()}};
}

// --- changing the model -----------------------------------------------------------

// Applies `next` if `index` built without an error; returns the reply.
QJsonObject commit(Host& host, Document next, int index, const QString& what) {
  if (index >= 0 && !next.error(size_t(index)).empty())
    fail(QString("%1 failed: %2").arg(what, QString::fromStdString(next.error(size_t(index)))));
  const bool laterErrors = next.hasErrors();
  if (const QString err = host.apply(std::move(next), what); !err.isEmpty()) fail(err);
  const Document& doc = host.document();
  QJsonObject o{{"done", what}};
  if (index >= 0 && index < int(doc.size())) o["step"] = featureInfo(doc, size_t(index));
  o["body"] = doc.body().IsNull() ? QJsonValue() : QJsonValue(bodyInfo(doc.body()));
  if (laterErrors) {
    QJsonArray broken;
    for (size_t i = 0; i < doc.size(); ++i)
      if (!doc.error(i).empty()) broken.append(QJsonObject{{"index", int(i)}, {"error", QString::fromStdString(doc.error(i))}});
    o["steps_with_errors"] = broken;
  }
  return o;
}

QJsonObject setParameters(Host& host, const QJsonObject& args) {
  std::vector<Parameter> params = host.document().parameters();
  for (const QJsonValue& v : args["remove"].toArray()) {
    const std::string name = v.toString().toStdString();
    std::erase_if(params, [&](const Parameter& p) { return p.name == name; });
  }
  for (const QJsonValue& v : args["parameters"].toArray()) {
    const std::string name = v["name"].toString().trimmed().toStdString();
    QString expr = v["expr"].toString().trimmed();
    if (v["expr"].isDouble()) expr = QString::number(v["expr"].toDouble(), 'g', 12);
    if (!isValidName(name)) fail(QString("'%1' is not a valid parameter name (letters, digits, _).").arg(QString::fromStdString(name)));
    if (expr.isEmpty()) fail(QString("Parameter '%1' needs an expr.").arg(QString::fromStdString(name)));
    auto it = std::find_if(params.begin(), params.end(), [&](const Parameter& p) { return p.name == name; });
    if (it != params.end()) it->expression = expr.toStdString();
    else params.push_back({name, expr.toStdString()});
  }
  Document next = host.document();
  next.setParameters(params);
  if (!next.parameterError().empty()) fail(QString::fromStdString(next.parameterError()));
  return commit(host, std::move(next), -1, "Parameters changed");
}

// Builds a sketch from simple curve descriptions; equal points are shared, so
// lines that meet form closed loops.
class SketchBuilder {
public:
  explicit SketchBuilder(const Document& doc) : m_doc(doc) {}
  sketch::Sketch s;

  QJsonObject addCurve(const QJsonObject& c) {
    const QString type = c["type"].toString();
    const int first = int(s.entities.size());
    if (type == "line") {
      const XY a = xyArg(c["from"], "line.from"), b = xyArg(c["to"], "line.to");
      line(a, b);
    } else if (type == "rect") {
      const Size w = size(c, "width"), h = size(c, "height");
      XY p = c.contains("center") ? xyArg(c["center"], "rect.center") : xyArg(c["corner"], "rect.corner");
      if (c.contains("center")) p = {p.x - w.value / 2, p.y - h.value / 2};
      const int a = point(p.x, p.y), b = point(p.x + w.value, p.y), cc = point(p.x + w.value, p.y + h.value),
                d = point(p.x, p.y + h.value);
      const int bottom = s.addLine(a, b), right = s.addLine(b, cc), top = s.addLine(cc, d), left = s.addLine(d, a);
      s.addConstraint({K::Horizontal, bottom});
      s.addConstraint({K::Horizontal, top});
      s.addConstraint({K::Vertical, left});
      s.addConstraint({K::Vertical, right});
      if (c["width"].isString()) s.addConstraint({K::Length, bottom, -1, w.value, w.expr});
      if (c["height"].isString()) s.addConstraint({K::Length, left, -1, h.value, h.expr});
    } else if (type == "circle") {
      const XY m = xyArg(c["center"], "circle.center");
      Size r;
      if (c.contains("diameter")) {
        r = size(c, "diameter");
        r.value /= 2;
        if (!r.expr.empty()) r.expr = "(" + r.expr + ") / 2";
      } else {
        r = size(c, "radius");
      }
      const int e = s.addCircle(point(m.x, m.y), r.value);
      if (c["radius"].isString() || c["diameter"].isString()) s.addConstraint({K::Radius, e, -1, r.value, r.expr});
    } else if (type == "arc") {
      const XY m = xyArg(c["center"], "arc.center"), a = xyArg(c["start"], "arc.start"), b = xyArg(c["end"], "arc.end");
      const double r = std::hypot(a.x - m.x, a.y - m.y), rb = std::hypot(b.x - m.x, b.y - m.y);
      if (r < 1e-9 || rb < 1e-9) fail("arc: start and end must not be at the center");
      const XY end{m.x + (b.x - m.x) * r / rb, m.y + (b.y - m.y) * r / rb}; // on the circle
      s.addArc(point(m.x, m.y), point(a.x, a.y), point(end.x, end.y));
    } else if (type == "polygon") {
      const XY m = xyArg(c["center"], "polygon.center");
      const double r = size(c, "radius").value;
      const int sides = c["sides"].toInt(6);
      if (sides < 3 || sides > 64) fail("polygon: sides must be 3 to 64");
      const double start = c["rotation"].toDouble(0) * std::numbers::pi / 180;
      std::vector<int> pts;
      for (int i = 0; i < sides; ++i) {
        const double a = start + 2 * std::numbers::pi * i / sides;
        pts.push_back(point(m.x + r * std::cos(a), m.y + r * std::sin(a)));
      }
      for (int i = 0; i < sides; ++i) s.addLine(pts[i], pts[(i + 1) % sides]);
    } else if (type == "slot") {
      const XY a = xyArg(c["from"], "slot.from"), b = xyArg(c["to"], "slot.to");
      const double r = size(c, "width").value / 2, len = std::hypot(b.x - a.x, b.y - a.y);
      if (len < 1e-9 || r <= 0) fail("slot: needs two different points and a positive width");
      const XY d{(b.x - a.x) / len, (b.y - a.y) / len}, n{-d.y, d.x}; // along, left
      const int pa = point(a.x + n.x * r, a.y + n.y * r), pb = point(b.x + n.x * r, b.y + n.y * r);
      const int pc = point(b.x - n.x * r, b.y - n.y * r), pd = point(a.x - n.x * r, a.y - n.y * r);
      s.addLine(pd, pc);
      s.addArc(point(b.x, b.y), pc, pb);
      s.addLine(pb, pa);
      s.addArc(point(a.x, a.y), pa, pd);
    } else {
      fail(QString("Unknown curve type '%1' (line, rect, circle, arc, polygon, slot).").arg(type));
    }
    const bool construction = c["construction"].toBool();
    QJsonArray made;
    for (int i = first; i < int(s.entities.size()); ++i) {
      s.entities[i].construction = construction;
      made.append(i);
    }
    return {{"type", type}, {"curves", made}};
  }

  void addConstraint(const QJsonObject& c) {
    const QString kindName = c["kind"].toString();
    int k = 0;
    while (k < int(std::size(kConstraintNames)) && kindName != kConstraintNames[k]) ++k;
    if (k == int(std::size(kConstraintNames))) fail(QString("Unknown constraint kind '%1'.").arg(kindName));
    const K kind = K(k);
    // Which references are points (the rest are curves).
    const bool aPoint = kind == K::Coincident || kind == K::PointOnLine || kind == K::PointOnCircle ||
                        kind == K::Distance || kind == K::Symmetric;
    const bool bPoint = kind == K::Coincident || kind == K::Distance || kind == K::Symmetric;
    sketch::Constraint con{kind};
    con.a = ref(c["a"], aPoint, "a");
    if (c.contains("b")) con.b = ref(c["b"], bPoint, "b");
    if (c.contains("c")) con.c = ref(c["c"], false, "c");
    if (kind == K::Distance || kind == K::Length || kind == K::Radius) {
      const Size v = size(c, "value");
      con.value = v.value;
      con.expr = v.expr;
    }
    s.addConstraint(con);
  }

private:
  Size size(const QJsonObject& c, const char* key) {
    if (!c.contains(key)) fail(QString("'%1' is missing").arg(key));
    return sizeArg(m_doc, c[key], key);
  }

  int point(double x, double y) {
    for (int i = 0; i < int(s.points.size()); ++i)
      if (std::abs(s.x(i) - x) < 1e-7 && std::abs(s.y(i) - y) < 1e-7) return i;
    return s.addPoint(x, y);
  }

  void line(XY a, XY b) {
    const int p = point(a.x, a.y), q = point(b.x, b.y);
    if (p == q) fail("line: from and to are the same point");
    s.addLine(p, q);
  }

  // A point or curve: its index, or [x, y] for the nearest one.
  int ref(const QJsonValue& v, bool isPoint, const char* what) {
    if (v.isDouble()) {
      const int i = v.toInt();
      if (i < 0 || i >= int(isPoint ? s.points.size() : s.entities.size()))
        fail(QString("constraint %1: no %2 %3").arg(what, isPoint ? "point" : "curve").arg(i));
      return i;
    }
    const XY p = xyArg(v, what);
    double best = std::numeric_limits<double>::max();
    int found = -1;
    const int n = int(isPoint ? s.points.size() : s.entities.size());
    for (int i = 0; i < n; ++i) {
      const double d = isPoint ? std::hypot(s.x(i) - p.x, s.y(i) - p.y) : curveDistance(i, p);
      if (d < best) best = d, found = i;
    }
    if (found < 0 || best > 0.01)
      fail(QString("constraint %1: no %2 at [%3, %4]").arg(what, isPoint ? "point" : "curve").arg(p.x).arg(p.y));
    return found;
  }

  double curveDistance(int e, XY p) const {
    const sketch::Entity& en = s.entities[e];
    if (en.kind == sketch::EntityKind::Line) {
      const double ax = s.x(en.p[0]), ay = s.y(en.p[0]), bx = s.x(en.p[1]), by = s.y(en.p[1]);
      const double dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
      const double t = len2 > 0 ? std::clamp(((p.x - ax) * dx + (p.y - ay) * dy) / len2, 0.0, 1.0) : 0;
      return std::hypot(ax + t * dx - p.x, ay + t * dy - p.y);
    }
    return std::abs(std::hypot(p.x - s.x(en.p[0]), p.y - s.y(en.p[0])) - s.radius(e));
  }

  const Document& m_doc;
};

gp_Ax3 sketchPlaneArg(const Document& doc, const QJsonValue& v) {
  const gp_Pnt o(0, 0, 0);
  if (v.isString()) {
    const QString name = v.toString().toUpper();
    if (name == "XY") return gp_Ax3(o, gp::DZ(), gp::DX());
    if (name == "XZ") return gp_Ax3(o, -gp::DY(), gp::DX());
    if (name == "YZ") return gp_Ax3(o, gp::DX(), gp::DY());
    fail("plane: \"XY\", \"XZ\", \"YZ\", {\"face\": index} or {\"origin\", \"normal\", \"xdir\"}");
  }
  const QJsonObject p = v.toObject();
  if (p.contains("face")) {
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(doc.body(), TopAbs_FACE, faces);
    const int i = p["face"].toInt();
    if (doc.body().IsNull() || i < 1 || i > faces.Extent()) fail(QString("plane: no face %1 (see find_faces)").arg(i));
    gp_Ax3 plane;
    if (!sketchPlaneOnFace(faces(i), plane)) fail(QString("plane: face %1 is not flat").arg(i));
    return plane;
  }
  const gp_XYZ n = xyzArg(p["normal"], "plane.normal"), x = xyzArg(p["xdir"], "plane.xdir");
  if (n.Modulus() < 1e-9 || x.Modulus() < 1e-9 || n.Crossed(x).Modulus() < 1e-6 * n.Modulus() * x.Modulus())
    fail("plane: normal and xdir must be non-zero and not parallel");
  return gp_Ax3(gp_Pnt(xyzArg(p["origin"], "plane.origin")), gp_Dir(n), gp_Dir(x));
}

QJsonObject createSketch(Host& host, const QJsonObject& args) {
  const Document& doc = host.document();
  auto f = std::make_unique<SketchFeature>();
  f->plane = sketchPlaneArg(doc, args["plane"]);
  if (args.contains("offset")) f->plane.Translate(gp_Vec(f->plane.Direction()) * args["offset"].toDouble());
  SketchBuilder b(doc);
  QJsonArray made;
  for (const QJsonValue& c : args["curves"].toArray()) made.append(b.addCurve(c.toObject()));
  if (b.s.entities.empty()) fail("curves: give at least one curve");
  for (const QJsonValue& c : args["constraints"].toArray()) b.addConstraint(c.toObject());
  if (!b.s.valid()) fail("A constraint refers to the wrong kind of point or curve.");
  if (b.s.solve() == sketch::SolveResult::Failed) fail("The constraints contradict each other.");
  f->sketch = std::move(b.s);

  Document next = doc;
  const int index = next.add(std::move(f));
  QJsonObject o = commit(host, std::move(next), index, "Sketch added");
  o["created"] = made;
  return o;
}

std::unique_ptr<Feature> newFeature(const QString& type) {
  if (type == "extrude") return std::make_unique<ExtrudeFeature>();
  if (type == "revolve") return std::make_unique<RevolveFeature>();
  if (type == "fillet") return std::make_unique<FilletFeature>();
  if (type == "chamfer") return std::make_unique<ChamferFeature>();
  if (type == "shell") return std::make_unique<ShellFeature>();
  if (type == "hole") return std::make_unique<HoleFeature>();
  if (type == "mirror") return std::make_unique<MirrorFeature>();
  if (type == "pattern") return std::make_unique<PatternFeature>();
  if (type == "sketch") fail("Use create_sketch for sketches.");
  fail(QString("Unknown step type '%1' (extrude, revolve, fillet, chamfer, shell, hole, mirror, pattern).").arg(type));
}

std::vector<ShapeKey> keysOf(const TopoDS_Shape& body, const std::vector<int>& indices, TopAbs_ShapeEnum kind) {
  TopTools_IndexedMapOfShape map;
  TopExp::MapShapes(body, kind, map);
  const char* what = kind == TopAbs_EDGE ? "edge" : "face";
  std::vector<ShapeKey> keys;
  for (int i : indices) {
    if (i < 1 || i > map.Extent()) fail(QString("There is no %1 %2 (see find_%1s).").arg(what).arg(i));
    keys.push_back(shapeKey(body, map(i)));
  }
  return keys;
}

// `base` (a feature as in the project file) with `changes` applied. Numbers
// may be formulas; picked edges/faces get keys from `body` so they are found
// again when earlier steps change.
std::unique_ptr<Feature> changed(const Document& doc, const Feature& base, const QJsonObject& changes,
                                 const TopoDS_Shape& body) {
  QJsonObject o = project::featureToJson(base);
  QJsonObject exprs = o["expressions"].toObject();
  for (auto it = changes.begin(); it != changes.end(); ++it) {
    const QString key = it.key();
    if (key == "type") continue;
    if (!o.contains(key) || key == "keys" || key == "key" || key == "expressions") {
      QStringList fields;
      for (auto f = o.begin(); f != o.end(); ++f)
        if (f.key() != "type" && f.key() != "keys" && f.key() != "key" && f.key() != "expressions") fields << f.key();
      fail(QString("%1 has no field '%2'; fields: %3").arg(QString::fromStdString(base.name()), key, fields.join(", ")));
    }
    if (o[key].isDouble()) {
      const Size v = sizeArg(doc, it.value(), key);
      o[key] = v.value;
      if (v.expr.empty()) exprs.remove(key);
      else exprs[key] = QString::fromStdString(v.expr);
    } else {
      o[key] = it.value();
    }
  }
  o["expressions"] = exprs;
  std::unique_ptr<Feature> f = project::featureFromJson(o);
  // New edge/face picks: remember where they are.
  if (changes.contains("edges")) {
    if (auto* fi = dynamic_cast<FilletFeature*>(f.get())) fi->keys = keysOf(body, fi->edges, TopAbs_EDGE);
    if (auto* c = dynamic_cast<ChamferFeature*>(f.get())) c->keys = keysOf(body, c->edges, TopAbs_EDGE);
  }
  if (auto* sh = dynamic_cast<ShellFeature*>(f.get()); sh && changes.contains("faces"))
    sh->keys = keysOf(body, sh->faces, TopAbs_FACE);
  if (auto* m = dynamic_cast<MirrorFeature*>(f.get()); m && changes.contains("face") && m->face >= 1)
    m->key = keysOf(body, {m->face}, TopAbs_FACE)[0];
  return f;
}

QJsonObject addFeature(Host& host, const QJsonObject& args) {
  const Document& doc = host.document();
  const QJsonObject spec = args["feature"].toObject();
  const std::unique_ptr<Feature> base = newFeature(spec["type"].toString());
  std::unique_ptr<Feature> f = changed(doc, *base, spec, doc.body());
  const QString name = QString::fromStdString(f->name());
  Document next = doc;
  const int index = next.add(std::move(f));
  return commit(host, std::move(next), index, name + " added");
}

QJsonObject updateFeature(Host& host, const QJsonObject& args) {
  const Document& doc = host.document();
  const int index = featureIndex(doc, args);
  const QJsonObject changes = args["changes"].toObject();
  std::unique_ptr<Feature> f;
  if (const auto* s = doc.as<SketchFeature>(size_t(index))) {
    // Sketches: change dimension values (constraint index -> number or formula).
    auto copy = std::make_unique<SketchFeature>(*s);
    for (const QJsonValue& v : changes["constraints"].toArray()) {
      const QJsonObject c = v.toObject();
      const int i = intArg(c, "index");
      if (i < 0 || i >= int(copy->sketch.constraints.size())) fail(QString("No constraint %1 in this sketch.").arg(i));
      sketch::Constraint& con = copy->sketch.constraints[size_t(i)];
      if (con.kind != K::Distance && con.kind != K::Length && con.kind != K::Radius)
        fail(QString("Constraint %1 has no value.").arg(i));
      const Size val = sizeArg(doc, c["value"], "value");
      con.value = val.value;
      con.expr = val.expr;
    }
    if (changes.size() != (changes.contains("constraints") ? 1 : 0) || !changes.contains("constraints"))
      fail("A sketch only takes {\"constraints\": [{\"index\": i, \"value\": number or formula}]}. "
           "For other changes delete it and create a new one.");
    if (copy->sketch.solve() == sketch::SolveResult::Failed) fail("The sketch has no solution with these values.");
    f = std::move(copy);
  } else {
    f = changed(doc, doc.feature(size_t(index)), changes, doc.bodyBefore(size_t(index)));
  }
  const QString name = QString::fromStdString(f->name());
  Document next = doc;
  next.replace(size_t(index), std::move(f));
  return commit(host, std::move(next), index, name + " changed");
}

QJsonObject deleteFeature(Host& host, const QJsonObject& args) {
  Document next = host.document();
  const int index = featureIndex(next, args);
  const QString name = QString::fromStdString(next.feature(size_t(index)).name());
  const int removed = next.removeWithDependents(size_t(index));
  QJsonObject o = commit(host, std::move(next), -1, name + " deleted");
  o["steps_removed"] = removed;
  return o;
}

QJsonObject exportBody(Host& host, const QJsonObject& args) {
  const QString path = args["path"].toString();
  if (!QDir::isAbsolutePath(path)) fail("path: give a full path, such as /home/me/part.step");
  if (host.document().body().IsNull()) fail("There is no body yet.");
  const QString q = args["quality"].toString("normal");
  const io::MeshQuality quality = q == "coarse" ? io::MeshQuality::Coarse
                                  : q == "fine" ? io::MeshQuality::Fine
                                                : io::MeshQuality::Normal;
  io::exportShape(host.document().body(), QDir::cleanPath(path).toStdString(), quality);
  return {{"exported", QDir::cleanPath(path)}};
}

QJsonObject ok(const QString& error, const QString& done) {
  if (!error.isEmpty()) fail(error);
  return {{"done", done}};
}

// --- tool definitions ---------------------------------------------------------------

QJsonObject schema(const QJsonObject& properties, const QStringList& required = {}) {
  QJsonObject s{{"type", "object"}, {"properties", properties}};
  if (!required.isEmpty()) s["required"] = QJsonArray::fromStringList(required);
  return s;
}
QJsonObject prop(const char* type, const QString& description) { return {{"type", type}, {"description", description}}; }
QJsonObject anyProp(const QString& description) { return {{"description", description}}; }

const char* kConventions =
    "Units are mm and degrees. Step indices are 0-based (the timeline shows index + 1). "
    "Sizes can be numbers or formulas with parameters (\"width / 2 + 3\"); formulas update when parameters change.";

} // namespace

QJsonObject textResult(const QString& text, bool error) {
  return {{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", text}}}}, {"isError", error}};
}

QJsonArray tools() {
  const QJsonObject index = prop("integer", "Step index (0-based), from get_model");
  const QJsonObject xyz = prop("array", "[x, y, z] in mm");
  return {
      QJsonObject{
          {"name", "get_model"},
          {"description", QString("The open project: parameters, every step of the timeline with its settings and "
                                  "errors, sketches with their points, curves and constraints, and the body (volume, "
                                  "size, bounding box). Call this first. ") +
                              kConventions},
          {"inputSchema", schema({})}},
      QJsonObject{
          {"name", "set_parameters"},
          {"description", "Add or change named parameters (\"wall\" = \"2\", \"width\" = \"length / 2\"), or remove "
                          "some. Everything that uses them rebuilds."},
          {"inputSchema",
           schema({{"parameters", QJsonObject{{"type", "array"},
                                              {"description", "[{\"name\": \"wall\", \"expr\": \"2\"}, ...]"},
                                              {"items", schema({{"name", prop("string", "letters, digits, _")},
                                                                {"expr", prop("string", "number or formula")}},
                                                               {"name", "expr"})}}},
                   {"remove", QJsonObject{{"type", "array"}, {"items", QJsonObject{{"type", "string"}}},
                                          {"description", "names to remove"}}}})}},
      QJsonObject{
          {"name", "create_sketch"},
          {"description",
           QString("Add a 2D sketch on a plane. Planes: \"XY\" (normal +Z; sketch x,y = world X,Y), \"XZ\" (normal -Y; "
                   "sketch x = world X, y = world Z), \"YZ\" (normal +X; sketch x = world Y, y = world Z), "
                   "{\"face\": n} for a flat face of the body (normal points out of the part; see find_faces; the "
                   "reply's step.plane gives origin and axes), or {\"origin\", \"normal\", \"xdir\"}. "
                   "Curves (sketch coordinates): {\"type\":\"line\",\"from\":[x,y],\"to\":[x,y]}, "
                   "{\"type\":\"rect\",\"corner\":[x,y] or \"center\":[x,y],\"width\",\"height\"}, "
                   "{\"type\":\"circle\",\"center\":[x,y],\"radius\" or \"diameter\"}, "
                   "{\"type\":\"arc\",\"center\",\"start\",\"end\"} (counter-clockwise), "
                   "{\"type\":\"polygon\",\"center\",\"radius\",\"sides\",\"rotation\"}, "
                   "{\"type\":\"slot\",\"from\",\"to\",\"width\"}; add \"construction\": true for helper geometry. "
                   "Points at the same coordinates are joined, so lines that meet form closed shapes; loops inside "
                   "loops become holes. A rect makes 4 lines (bottom, right, top, left), a polygon one line per side, "
                   "a slot 2 lines and 2 arcs. A rect width/height or circle radius/diameter given as a string (\"w\", "
                   "\"30\") also adds a dimension that parameters can drive. Optional constraints: {\"kind\": "
                   "coincident|horizontal|vertical|parallel|perpendicular|equal|tangent|point-on-line|point-on-circle|"
                   "distance|length|radius|symmetric, \"a\", \"b\", \"c\", \"value\"}; references are curve or point "
                   "indices or [x, y] for the nearest one. Circles of a sketch are where a hole step drills. ") +
               kConventions},
          {"inputSchema",
           schema({{"plane", anyProp("\"XY\", \"XZ\", \"YZ\", {\"face\": n} or {\"origin\", \"normal\", \"xdir\"}")},
                   {"offset", prop("number", "move the plane along its normal (mm)")},
                   {"curves", QJsonObject{{"type", "array"}, {"items", QJsonObject{{"type", "object"}}}}},
                   {"constraints", QJsonObject{{"type", "array"}, {"items", QJsonObject{{"type", "object"}}}}}},
                  {"plane", "curves"})}},
      QJsonObject{
          {"name", "add_feature"},
          {"description",
           QString("Add a step after the last one. feature = {\"type\": ..., fields}; fields left out keep their "
                   "defaults. extrude {sketch, distance (negative = other way), op: join|cut|intersect}; "
                   "revolve {sketch, axis: sketch-x|sketch-y, angle, op}; fillet {edges: [..], radius}; "
                   "chamfer {edges, distance}; shell {faces: open faces, thickness}; "
                   "hole {sketch (its circles are the hole centers; drilled against the sketch normal unless flip), "
                   "kind: simple|counterbore|countersink, diameter, depth (0 = through), bore-diameter, bore-depth, "
                   "sink-diameter, sink-angle, flip}; mirror {plane: xy|xz|yz|face, face}; "
                   "pattern {source: extrude/revolve/hole step, kind: linear|circular, axis: x|y|z, count (total, "
                   "original included), spacing, angle}. Edges and faces are 1-based indices from find_edges / "
                   "find_faces. ") +
               kConventions},
          {"inputSchema", schema({{"feature", prop("object", "{\"type\": \"extrude\", \"sketch\": 0, \"distance\": 10}")}},
                                 {"feature"})}},
      QJsonObject{
          {"name", "update_feature"},
          {"description", "Change fields of an existing step (same fields as add_feature); later steps rebuild. For a "
                          "sketch: {\"constraints\": [{\"index\": i, \"value\": number or formula}]} changes dimension "
                          "values (constraint indices from get_model)."},
          {"inputSchema", schema({{"index", index}, {"changes", prop("object", "fields to change")}}, {"index", "changes"})}},
      QJsonObject{
          {"name", "delete_feature"},
          {"description", "Delete a step and every step built from it."},
          {"inputSchema", schema({{"index", index}}, {"index"})}},
      QJsonObject{
          {"name", "find_edges"},
          {"description", "Edges of the body with 1-based index, type, length, midpoint, direction (lines) or "
                          "center/radius/axis (circles) and the faces they touch. Filter by type, sort by distance "
                          "to a point. Use the indices for fillet and chamfer."},
          {"inputSchema", schema({{"type", prop("string", "line, circle, ellipse, bspline or other")},
                                  {"near", xyz},
                                  {"limit", prop("integer", "at most this many (default 100)")}})}},
      QJsonObject{
          {"name", "find_faces"},
          {"description", "Faces of the body with 1-based index, type, area, center and outward normal (planes) or "
                          "axis/radius (cylinders). Filter by type or normal ([0,0,1] = facing up), sort by distance "
                          "to a point. Use the indices for shell, mirror and sketches on a face."},
          {"inputSchema", schema({{"type", prop("string", "plane, cylinder, cone, sphere, torus or other")},
                                  {"normal", xyz},
                                  {"near", xyz},
                                  {"limit", prop("integer", "at most this many (default 100)")}})}},
      QJsonObject{
          {"name", "screenshot"},
          {"description", "Picture of the part in the OpenCaliper window (only when the window is open)."},
          {"inputSchema", schema({{"view", prop("string", "iso (default), front, back, top, bottom, left, right or current")}})}},
      QJsonObject{
          {"name", "undo"},
          {"description", "Undo the last change (the user can also press Ctrl+Z in OpenCaliper)."},
          {"inputSchema", schema({})}},
      QJsonObject{{"name", "redo"}, {"description", "Redo the last undone change."}, {"inputSchema", schema({})}},
      QJsonObject{
          {"name", "export"},
          {"description", "Write the body to a file; the extension picks the format: .step .iges .stl .obj .3mf .brep. "
                          "Ask the user before overwriting files."},
          {"inputSchema", schema({{"path", prop("string", "full path")},
                                  {"quality", prop("string", "coarse, normal or fine (STL, OBJ, 3MF)")}},
                                 {"path"})}},
      QJsonObject{
          {"name", "new_project"},
          {"description", "Start an empty project (refused if the open one has unsaved changes)."},
          {"inputSchema", schema({})}},
      QJsonObject{
          {"name", "open_project"},
          {"description", "Open a .ocal project (refused if the open one has unsaved changes)."},
          {"inputSchema", schema({{"path", prop("string", "full path of a .ocal file")}}, {"path"})}},
      QJsonObject{
          {"name", "save_project"},
          {"description", "Save the project as .ocal, to its current file or to path."},
          {"inputSchema", schema({{"path", prop("string", "full path; optional if the project has a file")}})}},
  };
}

QJsonObject callTool(Host& host, const QString& name, const QJsonObject& args) {
  try {
    QJsonObject r;
    if (name == "get_model") r = getModel(host);
    else if (name == "set_parameters") r = setParameters(host, args);
    else if (name == "create_sketch") r = createSketch(host, args);
    else if (name == "add_feature") r = addFeature(host, args);
    else if (name == "update_feature") r = updateFeature(host, args);
    else if (name == "delete_feature") r = deleteFeature(host, args);
    else if (name == "find_edges") r = findEdges(host.document(), args);
    else if (name == "find_faces") r = findFaces(host.document(), args);
    else if (name == "undo") r = ok(host.undo(), "Undone");
    else if (name == "redo") r = ok(host.redo(), "Redone");
    else if (name == "export") r = exportBody(host, args);
    else if (name == "new_project") r = ok(host.newProject(), "New project");
    else if (name == "open_project") r = ok(host.openProject(args["path"].toString()), "Opened");
    else if (name == "save_project") r = ok(host.saveProject(args["path"].toString()), "Saved");
    else if (name == "screenshot") {
      QByteArray png;
      if (const QString err = host.screenshot(args["view"].toString("iso"), png); !err.isEmpty()) fail(err);
      return {{"content", QJsonArray{QJsonObject{{"type", "image"},
                                                 {"data", QString::fromLatin1(png.toBase64())},
                                                 {"mimeType", "image/png"}}}},
              {"isError", false}};
    } else {
      return textResult(QString("Unknown tool '%1'.").arg(name), true);
    }
    if (name != "get_model" && name != "find_edges" && name != "find_faces" && !name.endsWith("_project") &&
        name != "export" && !r.contains("body")) {
      const Document& doc = host.document();
      r["body"] = doc.body().IsNull() ? QJsonValue() : QJsonValue(bodyInfo(doc.body()));
      r["steps"] = int(doc.size());
    }
    return textResult(json(r));
  } catch (const Standard_Failure& e) { // first: in OCCT 8 it is a std::exception
    return textResult(QString("Geometry kernel error: %1").arg(e.GetMessageString()), true);
  } catch (const std::exception& e) {
    return textResult(QString::fromStdString(e.what()), true);
  }
}

// --- FileHost -------------------------------------------------------------------------

QString FileHost::apply(Document doc, const QString&) {
  m_undo.push_back(std::move(m_doc));
  if (m_undo.size() > 100) m_undo.erase(m_undo.begin());
  m_redo.clear();
  m_doc = std::move(doc);
  return {};
}

QString FileHost::undo() {
  if (m_undo.empty()) return "Nothing to undo.";
  m_redo.push_back(std::move(m_doc));
  m_doc = std::move(m_undo.back());
  m_undo.pop_back();
  return {};
}

QString FileHost::redo() {
  if (m_redo.empty()) return "Nothing to redo.";
  m_undo.push_back(std::move(m_doc));
  m_doc = std::move(m_redo.back());
  m_redo.pop_back();
  return {};
}

QString FileHost::newProject() {
  m_doc = {};
  m_undo.clear();
  m_redo.clear();
  m_path.clear();
  return {};
}

QString FileHost::openProject(const QString& path) {
  try {
    m_doc = project::load(path);
  } catch (const std::exception& e) {
    return QString::fromStdString(e.what());
  }
  m_undo.clear();
  m_redo.clear();
  m_path = QFileInfo(path).absoluteFilePath();
  return {};
}

QString FileHost::saveProject(const QString& path) {
  const QString target = path.isEmpty() ? m_path : path;
  if (target.isEmpty()) return "The project has no file yet: give a path.";
  if (!QDir::isAbsolutePath(target)) return "path: give a full path, such as /home/me/box.ocal";
  try {
    project::save(m_doc, target);
  } catch (const std::exception& e) {
    return QString::fromStdString(e.what());
  }
  m_path = target;
  return {};
}

QString FileHost::screenshot(const QString&, QByteArray&) {
  return "Pictures need the OpenCaliper window: ask the user to open OpenCaliper and turn on "
         "Tools > Claude (MCP). Changes made without the window are not in it; save them with save_project first.";
}

} // namespace mcp
