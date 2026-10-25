#include "app/ProjectFile.h"

#include <BRepTools.hxx>
#include <BRep_Builder.hxx>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <sstream>
#include <stdexcept>

using namespace model;

namespace project {
namespace {

constexpr int kVersion = 1;

[[noreturn]] void fail(const QString& message) { throw std::runtime_error(message.toStdString()); }

// Enums are stored by name so reordering them never breaks old files.
template <class E> struct Names;
template <> struct Names<Operation> {
  static constexpr const char* list[] = {"join", "cut", "intersect"};
};
template <> struct Names<RevolveFeature::Axis> {
  static constexpr const char* list[] = {"sketch-x", "sketch-y"};
};
template <> struct Names<sketch::EntityKind> {
  static constexpr const char* list[] = {"line", "circle", "arc"};
};
template <> struct Names<sketch::ConstraintKind> {
  static constexpr const char* list[] = {"coincident",     "horizontal", "vertical",       "parallel",
                                         "perpendicular",  "equal",      "tangent",        "point-on-line",
                                         "point-on-circle", "distance",  "length",         "radius"};
};

template <class E> QString nameOf(E value) { return Names<E>::list[int(value)]; }
template <class E> E valueOf(const QJsonValue& v) {
  const QString name = v.toString();
  int i = 0;
  for (const char* n : Names<E>::list) {
    if (name == n) return E(i);
    ++i;
  }
  fail(QObject::tr("Unknown value '%1' in project file.").arg(name));
}

QJsonArray vec(double x, double y, double z) { return {x, y, z}; }
gp_XYZ xyz(const QJsonValue& v) {
  const QJsonArray a = v.toArray();
  return {a[0].toDouble(), a[1].toDouble(), a[2].toDouble()};
}

QJsonArray intList(const std::vector<int>& v) {
  QJsonArray a;
  for (int i : v) a.append(i);
  return a;
}
std::vector<int> intList(const QJsonValue& v) {
  std::vector<int> r;
  for (const QJsonValue& i : v.toArray()) r.push_back(i.toInt());
  return r;
}

QJsonObject writeSketch(const SketchFeature& f) {
  const gp_Ax3& p = f.plane;
  QJsonObject plane{{"origin", vec(p.Location().X(), p.Location().Y(), p.Location().Z())},
                    {"normal", vec(p.Direction().X(), p.Direction().Y(), p.Direction().Z())},
                    {"xdir", vec(p.XDirection().X(), p.XDirection().Y(), p.XDirection().Z())}};
  const sketch::Sketch& s = f.sketch;
  QJsonArray params, points, entities, constraints;
  for (double v : s.params) params.append(v);
  for (const sketch::Point& pt : s.points) points.append(QJsonObject{{"x", pt.x}, {"y", pt.y}, {"fixed", pt.fixed}});
  for (const sketch::Entity& e : s.entities)
    entities.append(QJsonObject{{"kind", nameOf(e.kind)}, {"p", QJsonArray{e.p[0], e.p[1], e.p[2]}}, {"r", e.r}});
  for (const sketch::Constraint& c : s.constraints)
    constraints.append(QJsonObject{{"kind", nameOf(c.kind)}, {"a", c.a}, {"b", c.b}, {"value", c.value}});
  return {{"type", "sketch"}, {"plane", plane},          {"params", params},
          {"points", points}, {"entities", entities}, {"constraints", constraints}};
}

std::unique_ptr<SketchFeature> readSketch(const QJsonObject& o) {
  auto f = std::make_unique<SketchFeature>();
  const QJsonObject plane = o["plane"].toObject();
  f->plane = gp_Ax3(gp_Pnt(xyz(plane["origin"])), gp_Dir(xyz(plane["normal"])), gp_Dir(xyz(plane["xdir"])));
  sketch::Sketch& s = f->sketch;
  for (const QJsonValue& v : o["params"].toArray()) s.params.push_back(v.toDouble());
  for (const QJsonValue& v : o["points"].toArray()) {
    const QJsonObject p = v.toObject();
    s.points.push_back({p["x"].toInt(), p["y"].toInt(), p["fixed"].toBool()});
  }
  for (const QJsonValue& v : o["entities"].toArray()) {
    const QJsonObject e = v.toObject();
    sketch::Entity en{valueOf<sketch::EntityKind>(e["kind"])};
    const QJsonArray p = e["p"].toArray();
    for (int i = 0; i < 3; ++i) en.p[i] = p[i].toInt();
    en.r = e["r"].toInt();
    s.entities.push_back(en);
  }
  for (const QJsonValue& v : o["constraints"].toArray()) {
    const QJsonObject c = v.toObject();
    s.constraints.push_back({valueOf<sketch::ConstraintKind>(c["kind"]), c["a"].toInt(), c["b"].toInt(), c["value"].toDouble()});
  }
  if (!s.valid()) fail(QObject::tr("Damaged sketch in project file.")); // would crash the solver
  return f;
}

std::string brepText(const TopoDS_Shape& shape) {
  std::ostringstream out;
  BRepTools::Write(shape, out);
  return out.str();
}

TopoDS_Shape brepShape(const std::string& text) {
  std::istringstream in(text);
  TopoDS_Shape shape;
  BRepTools::Read(shape, in, BRep_Builder());
  return shape;
}

QJsonObject writeFeature(const Feature& f) {
  if (auto* s = dynamic_cast<const SketchFeature*>(&f)) return writeSketch(*s);
  if (auto* e = dynamic_cast<const ExtrudeFeature*>(&f))
    return {{"type", "extrude"}, {"sketch", e->sketch}, {"distance", e->distance}, {"op", nameOf(e->op)}};
  if (auto* r = dynamic_cast<const RevolveFeature*>(&f))
    return {{"type", "revolve"}, {"sketch", r->sketch}, {"axis", nameOf(r->axis)}, {"angle", r->angle}, {"op", nameOf(r->op)}};
  if (auto* fi = dynamic_cast<const FilletFeature*>(&f))
    return {{"type", "fillet"}, {"edges", intList(fi->edges)}, {"radius", fi->radius}};
  if (auto* c = dynamic_cast<const ChamferFeature*>(&f))
    return {{"type", "chamfer"}, {"edges", intList(c->edges)}, {"distance", c->distance}};
  if (auto* i = dynamic_cast<const ImportFeature*>(&f))
    return {{"type", "import"}, {"source", QString::fromStdString(i->source)}, {"brep", QString::fromStdString(brepText(i->shape))}};
  fail(QObject::tr("Cannot save feature '%1'.").arg(QString::fromStdString(f.name())));
}

std::unique_ptr<Feature> readFeature(const QJsonObject& o) {
  const QString type = o["type"].toString();
  if (type == "sketch") return readSketch(o);
  if (type == "extrude") {
    auto e = std::make_unique<ExtrudeFeature>();
    e->sketch = o["sketch"].toInt();
    e->distance = o["distance"].toDouble();
    e->op = valueOf<Operation>(o["op"]);
    return e;
  }
  if (type == "revolve") {
    auto r = std::make_unique<RevolveFeature>();
    r->sketch = o["sketch"].toInt();
    r->axis = valueOf<RevolveFeature::Axis>(o["axis"]);
    r->angle = o["angle"].toDouble();
    r->op = valueOf<Operation>(o["op"]);
    return r;
  }
  if (type == "fillet") {
    auto f = std::make_unique<FilletFeature>();
    f->edges = intList(o["edges"]);
    f->radius = o["radius"].toDouble();
    return f;
  }
  if (type == "chamfer") {
    auto c = std::make_unique<ChamferFeature>();
    c->edges = intList(o["edges"]);
    c->distance = o["distance"].toDouble();
    return c;
  }
  if (type == "import") {
    auto i = std::make_unique<ImportFeature>();
    i->source = o["source"].toString().toStdString();
    i->shape = brepShape(o["brep"].toString().toStdString());
    return i;
  }
  fail(QObject::tr("Unknown feature type '%1' in project file.").arg(type));
}

} // namespace

void save(const Document& doc, const QString& path) {
  QJsonArray features;
  for (size_t i = 0; i < doc.size(); ++i) features.append(writeFeature(doc.feature(i)));
  const QJsonObject root{{"format", "opencaliper-project"}, {"version", kVersion}, {"units", "mm"}, {"features", features}};
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(QJsonDocument(root).toJson()) < 0)
    fail(QObject::tr("Cannot write %1").arg(path));
}

Document load(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) fail(QObject::tr("Cannot open %1").arg(path));
  QJsonParseError err;
  const QJsonDocument json = QJsonDocument::fromJson(file.readAll(), &err);
  if (json.isNull()) fail(QObject::tr("Not a valid project file: %1").arg(err.errorString()));
  const QJsonObject root = json.object();
  if (root["format"].toString() != "opencaliper-project") fail(QObject::tr("Not a project file."));
  if (root["version"].toInt() > kVersion) fail(QObject::tr("This project was saved by a newer version."));

  Document doc;
  for (const QJsonValue& v : root["features"].toArray()) doc.add(readFeature(v.toObject()));
  return doc;
}

} // namespace project
