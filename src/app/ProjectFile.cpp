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
template <> struct Names<PatternFeature::Kind> {
  static constexpr const char* list[] = {"linear", "circular"};
};
template <> struct Names<PatternFeature::Axis> {
  static constexpr const char* list[] = {"x", "y", "z"};
};
template <> struct Names<HoleFeature::Kind> {
  static constexpr const char* list[] = {"simple", "counterbore", "countersink"};
};
template <> struct Names<MirrorFeature::Plane> {
  static constexpr const char* list[] = {"xy", "xz", "yz", "face"};
};
template <> struct Names<sketch::EntityKind> {
  static constexpr const char* list[] = {"line", "circle", "arc", "spline"};
};
template <> struct Names<sketch::ConstraintKind> {
  static constexpr const char* list[] = {"coincident",     "horizontal", "vertical",       "parallel",
                                         "perpendicular",  "equal",      "tangent",        "point-on-line",
                                         "point-on-circle", "distance",  "length",         "radius",
                                         "symmetric"};
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

QJsonArray vec(const gp_XYZ& v) { return vec(v.X(), v.Y(), v.Z()); }

QJsonObject writeKey(const ShapeKey& k) {
  QJsonArray normals;
  for (const gp_XYZ& n : k.normals) normals.append(vec(n));
  return {{"type", k.type}, {"point", vec(k.point)}, {"dir", vec(k.dir)}, {"size", k.size}, {"normals", normals}};
}
ShapeKey readKey(const QJsonValue& v) {
  const QJsonObject o = v.toObject();
  ShapeKey k;
  if (o.isEmpty()) return k;
  k.type = o["type"].toInt(-1);
  k.point = xyz(o["point"]);
  k.dir = xyz(o["dir"]);
  k.size = o["size"].toDouble();
  for (const QJsonValue& n : o["normals"].toArray()) k.normals.push_back(xyz(n));
  return k;
}
QJsonArray keyList(const std::vector<ShapeKey>& keys) {
  QJsonArray a;
  for (const ShapeKey& k : keys) a.append(writeKey(k));
  return a;
}
std::vector<ShapeKey> keyList(const QJsonValue& v) { // absent in files from 0.3 and older
  std::vector<ShapeKey> keys;
  for (const QJsonValue& k : v.toArray()) keys.push_back(readKey(k));
  return keys;
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
  for (const sketch::Entity& e : s.entities) {
    QJsonObject o{{"kind", nameOf(e.kind)}, {"p", QJsonArray{e.p[0], e.p[1], e.p[2]}}, {"r", e.r},
                  {"construction", e.construction}};
    if (e.kind == sketch::EntityKind::Spline) o["points"] = intList(e.pts);
    entities.append(o);
  }
  for (const sketch::Constraint& c : s.constraints) {
    QJsonObject o{{"kind", nameOf(c.kind)}, {"a", c.a}, {"b", c.b}, {"value", c.value}};
    if (!c.expr.empty()) o["expr"] = QString::fromStdString(c.expr);
    if (c.c >= 0) o["c"] = c.c;
    constraints.append(o);
  }
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
    en.construction = e["construction"].toBool(); // absent in files from 0.1/0.2
    en.pts = intList(e["points"]);                // splines (0.6)
    s.entities.push_back(en);
  }
  for (const QJsonValue& v : o["constraints"].toArray()) {
    const QJsonObject c = v.toObject();
    s.constraints.push_back({valueOf<sketch::ConstraintKind>(c["kind"]), c["a"].toInt(), c["b"].toInt(),
                             c["value"].toDouble(), c["expr"].toString().toStdString(), c["c"].toInt(-1)});
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

QJsonObject writeFeatureData(const Feature& f) {
  if (auto* s = dynamic_cast<const SketchFeature*>(&f)) return writeSketch(*s);
  if (auto* e = dynamic_cast<const ExtrudeFeature*>(&f))
    return {{"type", "extrude"}, {"sketch", e->sketch}, {"distance", e->distance}, {"symmetric", e->symmetric},
            {"op", nameOf(e->op)}};
  if (auto* l = dynamic_cast<const LoftFeature*>(&f))
    return {{"type", "loft"}, {"sketches", intList(l->sketches)}, {"ruled", l->ruled}, {"op", nameOf(l->op)}};
  if (auto* r = dynamic_cast<const RevolveFeature*>(&f))
    return {{"type", "revolve"}, {"sketch", r->sketch}, {"axis", nameOf(r->axis)}, {"angle", r->angle}, {"op", nameOf(r->op)}};
  if (auto* fi = dynamic_cast<const FilletFeature*>(&f))
    return {{"type", "fillet"}, {"edges", intList(fi->edges)}, {"keys", keyList(fi->keys)}, {"radius", fi->radius}};
  if (auto* c = dynamic_cast<const ChamferFeature*>(&f))
    return {{"type", "chamfer"}, {"edges", intList(c->edges)}, {"keys", keyList(c->keys)}, {"distance", c->distance}};
  if (auto* sh = dynamic_cast<const ShellFeature*>(&f))
    return {{"type", "shell"}, {"faces", intList(sh->faces)}, {"keys", keyList(sh->keys)}, {"thickness", sh->thickness}};
  if (auto* h = dynamic_cast<const HoleFeature*>(&f))
    return {{"type", "hole"},          {"sketch", h->sketch},           {"kind", nameOf(h->kind)},
            {"diameter", h->diameter}, {"depth", h->depth},             {"bore-diameter", h->boreDiameter},
            {"bore-depth", h->boreDepth}, {"sink-diameter", h->sinkDiameter}, {"sink-angle", h->sinkAngle},
            {"flip", h->flip}};
  if (auto* m = dynamic_cast<const MirrorFeature*>(&f))
    return {{"type", "mirror"}, {"plane", nameOf(m->plane)}, {"face", m->face}, {"key", writeKey(m->key)}};
  if (auto* p = dynamic_cast<const PatternFeature*>(&f))
    return {{"type", "pattern"}, {"source", p->source}, {"kind", nameOf(p->kind)}, {"axis", nameOf(p->axis)},
            {"count", p->count},   {"spacing", p->spacing}, {"angle", p->angle}};
  if (auto* i = dynamic_cast<const ImportFeature*>(&f))
    return {{"type", "import"}, {"source", QString::fromStdString(i->source)}, {"brep", QString::fromStdString(brepText(i->shape))}};
  fail(QObject::tr("Cannot save feature '%1'.").arg(QString::fromStdString(f.name())));
}

QJsonObject writeFeature(const Feature& f) {
  QJsonObject o = writeFeatureData(f);
  if (!f.expressions.empty()) {
    QJsonObject e;
    for (const auto& [field, expr] : f.expressions) e[QString::fromStdString(field)] = QString::fromStdString(expr);
    o["expressions"] = e;
  }
  return o;
}

std::unique_ptr<Feature> readFeatureData(const QJsonObject& o) {
  const QString type = o["type"].toString();
  if (type == "sketch") return readSketch(o);
  if (type == "extrude") {
    auto e = std::make_unique<ExtrudeFeature>();
    e->sketch = o["sketch"].toInt();
    e->distance = o["distance"].toDouble();
    e->symmetric = o["symmetric"].toBool(); // absent before 0.6
    e->op = valueOf<Operation>(o["op"]);
    return e;
  }
  if (type == "loft") {
    auto l = std::make_unique<LoftFeature>();
    l->sketches = intList(o["sketches"]);
    l->ruled = o["ruled"].toBool();
    l->op = valueOf<Operation>(o["op"]);
    return l;
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
    f->keys = keyList(o["keys"]);
    f->radius = o["radius"].toDouble();
    return f;
  }
  if (type == "chamfer") {
    auto c = std::make_unique<ChamferFeature>();
    c->edges = intList(o["edges"]);
    c->keys = keyList(o["keys"]);
    c->distance = o["distance"].toDouble();
    return c;
  }
  if (type == "shell") {
    auto sh = std::make_unique<ShellFeature>();
    sh->faces = intList(o["faces"]);
    sh->keys = keyList(o["keys"]);
    sh->thickness = o["thickness"].toDouble();
    return sh;
  }
  if (type == "hole") {
    auto h = std::make_unique<HoleFeature>();
    h->sketch = o["sketch"].toInt();
    h->kind = valueOf<HoleFeature::Kind>(o["kind"]);
    h->diameter = o["diameter"].toDouble();
    h->depth = o["depth"].toDouble();
    h->boreDiameter = o["bore-diameter"].toDouble();
    h->boreDepth = o["bore-depth"].toDouble();
    h->sinkDiameter = o["sink-diameter"].toDouble();
    h->sinkAngle = o["sink-angle"].toDouble(90);
    h->flip = o["flip"].toBool();
    return h;
  }
  if (type == "mirror") {
    auto m = std::make_unique<MirrorFeature>();
    m->plane = valueOf<MirrorFeature::Plane>(o["plane"]);
    m->face = o["face"].toInt(-1);
    m->key = readKey(o["key"]);
    return m;
  }
  if (type == "pattern") {
    auto p = std::make_unique<PatternFeature>();
    p->source = o["source"].toInt();
    p->kind = valueOf<PatternFeature::Kind>(o["kind"]);
    p->axis = valueOf<PatternFeature::Axis>(o["axis"]);
    p->count = o["count"].toInt();
    p->spacing = o["spacing"].toDouble();
    p->angle = o["angle"].toDouble();
    return p;
  }
  if (type == "import") {
    auto i = std::make_unique<ImportFeature>();
    i->source = o["source"].toString().toStdString();
    i->shape = brepShape(o["brep"].toString().toStdString());
    return i;
  }
  fail(QObject::tr("Unknown feature type '%1' in project file.").arg(type));
}

std::unique_ptr<Feature> readFeature(const QJsonObject& o) {
  std::unique_ptr<Feature> f = readFeatureData(o);
  const QJsonObject e = o["expressions"].toObject(); // absent in files from 0.3 and older
  for (auto it = e.begin(); it != e.end(); ++it) f->expressions[it.key().toStdString()] = it.value().toString().toStdString();
  return f;
}

} // namespace

QJsonObject featureToJson(const Feature& f) { return writeFeature(f); }
std::unique_ptr<Feature> featureFromJson(const QJsonObject& o) { return readFeature(o); }

void save(const Document& doc, const QString& path) {
  QJsonArray features, parameters;
  for (size_t i = 0; i < doc.size(); ++i) features.append(writeFeature(doc.feature(i)));
  for (const Parameter& p : doc.parameters())
    parameters.append(QJsonObject{{"name", QString::fromStdString(p.name)}, {"expr", QString::fromStdString(p.expression)}});
  const QJsonObject root{{"format", "opencaliper-project"}, {"version", kVersion}, {"units", "mm"},
                         {"parameters", parameters}, {"features", features}};
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
  std::vector<Parameter> params;
  for (const QJsonValue& v : root["parameters"].toArray())
    params.push_back({v["name"].toString().toStdString(), v["expr"].toString().toStdString()});
  doc.setParameters(std::move(params)); // before the features that use them
  for (const QJsonValue& v : root["features"].toArray()) doc.add(readFeature(v.toObject()));
  return doc;
}

} // namespace project
