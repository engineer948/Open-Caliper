#include "app/MainWindow.h"
#include "app/Icons.h"
#include "app/LicensesDialog.h"
#include "app/ProjectFile.h"
#include "io/Exchange.h"
#include "view/Viewer.h"

#include <BRepAdaptor_Surface.hxx>
#include <Prs3d_Drawer.hxx>
#include <Prs3d_LineAspect.hxx>
#include <TopoDS.hxx>

#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QShortcut>
#include <QStatusBar>
#include <QToolBar>

#include <utility>

using K = sketch::ConstraintKind;
using namespace model;

namespace {

constexpr size_t kMaxUndo = 100;

// Small form dialog: label/widget rows plus OK/Cancel.
class FormDialog : public QDialog {
public:
  FormDialog(QWidget* parent, const QString& title) : QDialog(parent), m_form(new QFormLayout(this)) {
    setWindowTitle(title);
  }
  QDoubleSpinBox* number(const QString& label, double value, double min, double max, const QString& suffix) {
    auto* box = new QDoubleSpinBox(this);
    box->setRange(min, max);
    box->setDecimals(3);
    box->setValue(value);
    box->setSuffix(suffix);
    box->selectAll();
    m_form->addRow(label, box);
    return box;
  }
  QComboBox* choice(const QString& label, const QStringList& items, int current = 0) {
    auto* box = new QComboBox(this);
    box->addItems(items);
    box->setCurrentIndex(current);
    m_form->addRow(label, box);
    return box;
  }
  bool run() {
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    m_form->addRow(buttons);
    return exec() == QDialog::Accepted;
  }

private:
  QFormLayout* m_form;
};

QComboBox* operationChoice(FormDialog& dlg, bool hasBody, Operation current) {
  QComboBox* op = dlg.choice(QObject::tr("Operation:"), {QObject::tr("Join"), QObject::tr("Cut"), QObject::tr("Intersect")},
                             int(current));
  if (!hasBody) { // the first solid can only be a new body
    op->setCurrentIndex(0);
    op->setEnabled(false);
  }
  return op;
}

QString opName(Operation op) {
  switch (op) {
  case Operation::Join: return QObject::tr("Join");
  case Operation::Cut: return QObject::tr("Cut");
  case Operation::Intersect: return QObject::tr("Intersect");
  }
  return {};
}

QString num(double v) { return QString::number(v, 'g', 6); }

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  resize(1400, 860);
  setWindowIcon(makeIcon("app"));

  m_viewer = new Viewer(this);
  setCentralWidget(m_viewer);
  m_editor = new SketchEditor(m_viewer, this);
  connect(m_editor, &SketchEditor::message, this, [this](const QString& t) { statusBar()->showMessage(t); });

  m_hint = new QLabel(this);
  statusBar()->addPermanentWidget(m_hint);

  buildFileMenu();
  buildEditMenu();
  buildViewMenu();
  buildSketchUi();
  buildModelUi();
  buildTimeline();
  buildHelpMenu();

  setProjectPath({});
  refreshTimeline();
  updateActions();
}

// --- UI construction ------------------------------------------------------------

QAction* MainWindow::action(const QString& text, const QString& icon, const QKeySequence& key, const QString& tip) {
  QAction* a = new QAction(text, this);
  if (!icon.isEmpty()) a->setIcon(makeIcon(icon));
  if (!key.isEmpty()) a->setShortcut(key);
  QString t = tip.isEmpty() ? text : text + "\n" + tip;
  if (!key.isEmpty()) t += QString(" (%1)").arg(key.toString(QKeySequence::NativeText));
  a->setToolTip(t);
  return a;
}

void MainWindow::buildFileMenu() {
  QMenu* m = menuBar()->addMenu(tr("&File"));
  auto add = [&](QAction* a, auto slot) {
    connect(a, &QAction::triggered, this, slot);
    m->addAction(a);
    m_modelActions << a;
  };
  add(action(tr("&New"), "", QKeySequence::New), &MainWindow::newProject);
  add(action(tr("&Open..."), "", QKeySequence::Open), &MainWindow::openProjectDialog);
  add(action(tr("&Save"), "", QKeySequence::Save), &MainWindow::save);
  add(action(tr("Save &As..."), "", QKeySequence::SaveAs), &MainWindow::saveAs);
  m->addSeparator();
  add(action(tr("&Import STEP / IGES / BREP..."), "import", QKeySequence("Ctrl+I")), &MainWindow::importFile);
  add(action(tr("&Export STEP / IGES / STL / OBJ / BREP..."), "", QKeySequence("Ctrl+E")), &MainWindow::exportFile);
  m->addSeparator();
  QAction* quit = action(tr("&Quit"), "", QKeySequence::Quit);
  connect(quit, &QAction::triggered, this, &QWidget::close);
  m->addAction(quit);
}

void MainWindow::buildEditMenu() {
  QMenu* m = menuBar()->addMenu(tr("&Edit"));
  m_undoAction = action(tr("&Undo"), "", QKeySequence::Undo);
  m_redoAction = action(tr("&Redo"), "", QKeySequence::Redo);
  m_redoAction->setShortcuts({QKeySequence::Redo, QKeySequence("Ctrl+Y")});
  connect(m_undoAction, &QAction::triggered, this, &MainWindow::undo);
  connect(m_redoAction, &QAction::triggered, this, &MainWindow::redo);
  m->addAction(m_undoAction);
  m->addAction(m_redoAction);
}

void MainWindow::buildViewMenu() {
  QMenu* m = menuBar()->addMenu(tr("&View"));
  QToolBar* bar = addToolBar(tr("View"));
  bar->setObjectName("view");
  QAction* fit = action(tr("Fit All"), "fit", QKeySequence("F"));
  connect(fit, &QAction::triggered, this, [this] { m_viewer->fitAll(); });
  m->addAction(fit);
  bar->addAction(fit);
  const std::pair<QString, V3d_TypeOfOrientation> views[] = {
      {tr("Isometric"), V3d_XposYnegZpos}, {tr("Front"), V3d_Yneg}, {tr("Top"), V3d_Zpos}, {tr("Right"), V3d_Xpos}};
  const char* keys[] = {"0", "1", "2", "3"};
  for (int i = 0; i < 4; ++i) {
    QAction* a = action(views[i].first, i == 0 ? "iso" : "", QKeySequence(keys[i]));
    const V3d_TypeOfOrientation o = views[i].second;
    connect(a, &QAction::triggered, this, [this, o] { m_viewer->setStandardView(o); });
    m->addAction(a);
    if (i == 0) bar->addAction(a);
  }
}

void MainWindow::buildSketchUi() {
  QMenu* menu = menuBar()->addMenu(tr("&Sketch"));
  QToolBar* bar = addToolBar(tr("Sketch"));
  bar->setObjectName("sketch");
  bar->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);

  // Plane axes: origin, normal, X direction (Y = normal x X).
  const gp_Pnt o(0, 0, 0);
  const std::pair<QString, gp_Ax3> planes[] = {{tr("Sketch XY"), gp_Ax3(o, gp::DZ(), gp::DX())},
                                               {tr("Sketch XZ"), gp_Ax3(o, -gp::DY(), gp::DX())},
                                               {tr("Sketch YZ"), gp_Ax3(o, gp::DX(), gp::DY())}};
  for (const auto& [text, plane] : planes) {
    QAction* a = action(text, "sketch", {}, tr("Start a sketch on this origin plane"));
    connect(a, &QAction::triggered, this, [this, plane] { startSketch(plane); });
    menu->addAction(a);
    bar->addAction(a);
    m_modelActions << a;
  }
  QAction* onFace = action(tr("On Face"), "face-sketch", QKeySequence("Shift+S"), tr("Select a flat face of the body first"));
  connect(onFace, &QAction::triggered, this, &MainWindow::sketchOnFace);
  menu->addAction(onFace);
  bar->addAction(onFace);
  m_modelActions << onFace;

  QAction* finish = action(tr("Finish"), "finish", QKeySequence("Ctrl+Return"), tr("Finish the sketch"));
  connect(finish, &QAction::triggered, this, &MainWindow::finishSketch);
  menu->addAction(finish);
  bar->addAction(finish);
  m_sketchActions << finish;
  menu->addSeparator();

  // Drawing tools and constraints only show while sketching (like Fusion).
  m_drawBar = addToolBar(tr("Draw"));
  m_drawBar->setObjectName("draw");
  m_drawBar->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
  auto* tools = new QActionGroup(this);
  const std::tuple<QString, QString, QString, SketchEditor::Tool> toolList[] = {
      {tr("Select"), "select", "S", SketchEditor::Tool::Select},
      {tr("Line"), "line", "L", SketchEditor::Tool::Line},
      {tr("Circle"), "circle", "C", SketchEditor::Tool::Circle},
      {tr("Arc"), "arc", "A", SketchEditor::Tool::Arc}};
  for (const auto& [text, icon, key, t] : toolList) {
    QAction* a = action(text, icon, QKeySequence(key));
    a->setCheckable(true);
    const SketchEditor::Tool tool = t;
    connect(a, &QAction::triggered, this, [this, tool] { m_editor->setTool(tool); });
    tools->addAction(a);
    menu->addAction(a);
    m_drawBar->addAction(a);
    m_toolActions[t] = a;
    m_sketchActions << a;
  }
  connect(m_editor, &SketchEditor::toolChanged, this, [this](SketchEditor::Tool t) { m_toolActions[t]->setChecked(true); });
  menu->addSeparator();

  m_constraintBar = addToolBar(tr("Constraints"));
  m_constraintBar->setObjectName("constraints");
  const std::tuple<QString, QString, QString, QString, K> constraints[] = {
      {tr("Coincident"), "coincident", "I", tr("Two points, or a point and a curve"), K::Coincident},
      {tr("Horizontal"), "horizontal", "H", tr("One or more lines"), K::Horizontal},
      {tr("Vertical"), "vertical", "V", tr("One or more lines"), K::Vertical},
      {tr("Parallel"), "parallel", "P", tr("Two lines"), K::Parallel},
      {tr("Perpendicular"), "perpendicular", "", tr("Two lines"), K::Perpendicular},
      {tr("Tangent"), "tangent", "T", tr("A line and a circle/arc"), K::Tangent},
      {tr("Equal"), "equal", "E", tr("Two lines or two circles/arcs"), K::Equal}};
  for (const auto& [text, icon, key, tip, k] : constraints) {
    QAction* a = action(text, icon, QKeySequence(key), tip);
    const K kind = k;
    connect(a, &QAction::triggered, this, [this, kind] { m_editor->constrain(kind); });
    menu->addAction(a);
    m_constraintBar->addAction(a);
    m_sketchActions << a;
  }
  QAction* dim = action(tr("Dimension"), "dimension", QKeySequence("D"), tr("A line, a circle/arc or two points"));
  connect(dim, &QAction::triggered, this, [this] { m_editor->dimension(); });
  QAction* fix = action(tr("Fix"), "fix", {}, tr("Lock the selected points in place"));
  connect(fix, &QAction::triggered, this, [this] { m_editor->toggleFix(); });
  for (QAction* a : {dim, fix}) {
    menu->addAction(a);
    m_constraintBar->addAction(a);
    m_sketchActions << a;
  }
}

void MainWindow::buildModelUi() {
  QMenu* menu = menuBar()->addMenu(tr("&Model"));
  QToolBar* bar = addToolBar(tr("Model"));
  bar->setObjectName("model");
  bar->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
  auto add = [&](QAction* a, auto slot) {
    connect(a, &QAction::triggered, this, slot);
    menu->addAction(a);
    bar->addAction(a);
    m_modelActions << a;
  };
  add(action(tr("Extrude"), "extrude", QKeySequence("Shift+E"), tr("Pull a closed sketch into a solid")),
      [this] { extrude(); });
  add(action(tr("Revolve"), "revolve", QKeySequence("Shift+R"), tr("Spin a closed sketch around an axis")),
      [this] { revolve(); });
  add(action(tr("Fillet"), "fillet", QKeySequence("Shift+F"), tr("Round the selected edges")),
      [this] { filletOrChamfer(true); });
  add(action(tr("Chamfer"), "chamfer", QKeySequence("Shift+C"), tr("Bevel the selected edges")),
      [this] { filletOrChamfer(false); });
}

void MainWindow::buildTimeline() {
  auto* dock = new QDockWidget(tr("Timeline"), this);
  dock->setObjectName("timeline");
  dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
  m_timeline = new QListWidget(dock);
  m_timeline->setIconSize(QSize(22, 22));
  m_timeline->setContextMenuPolicy(Qt::CustomContextMenu);
  m_timeline->setToolTip(tr("Double-click a step to change it"));
  dock->setWidget(m_timeline);
  addDockWidget(Qt::LeftDockWidgetArea, dock);
  resizeDocks({dock}, {230}, Qt::Horizontal);

  connect(m_timeline, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) {
    editFeature(m_timeline->row(it));
  });
  connect(m_timeline, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
    const int row = m_timeline->row(m_timeline->itemAt(pos));
    if (row < 0) return;
    QMenu menu;
    menu.addAction(tr("Edit..."), [this, row] { editFeature(row); });
    menu.addAction(tr("Delete"), [this, row] { deleteFeature(row); });
    menu.exec(m_timeline->viewport()->mapToGlobal(pos));
  });
  auto* del = new QShortcut(QKeySequence::Delete, m_timeline, nullptr, nullptr, Qt::WidgetShortcut);
  connect(del, &QShortcut::activated, this, [this] {
    if (m_timeline->currentRow() >= 0) deleteFeature(m_timeline->currentRow());
  });
}

void MainWindow::buildHelpMenu() {
  QMenu* m = menuBar()->addMenu(tr("&Help"));
  m->addAction(tr("&About OpenCaliper..."), this, [this] {
    // The Open CASCADE LGPL exception requires this notice in the documentation.
    QMessageBox::about(
        this, tr("About OpenCaliper"),
        tr("<h3>OpenCaliper %1</h3>"
           "<p>A simple parametric 3D CAD program.<br>"
           "<a href=\"https://github.com/engineer948/Open-Caliper\">github.com/engineer948/Open-Caliper</a></p>"
           "<p>Copyright &copy; 2026 engineer948 and contributors. Released under the MIT license.</p>"
           "<p>This software makes use of, and is based on facilities provided by, the "
           "<b>Open CASCADE Technology</b> software (LGPL 2.1 with the Open CASCADE exception).<br>"
           "Built with <b>Qt</b> (LGPL 3).<br>"
           "Portions of this software are copyright &copy; The FreeType Project (https://freetype.org). "
           "All rights reserved.</p>"
           "<p>Help &gt; Licenses shows the full license texts.</p>")
            .arg(QApplication::applicationVersion()));
  });
  m->addAction(tr("&Licenses..."), this, [this] { showLicenses(this); });
  m->addAction(tr("About &Qt..."), qApp, &QApplication::aboutQt);
}

// --- sketches -----------------------------------------------------------------

void MainWindow::startSketch(const gp_Ax3& plane) {
  m_editingSketch = -1;
  m_viewer->context()->ClearSelected(false); // the picked face would stay highlighted
  m_editor->begin(plane);
  updateActions();
}

void MainWindow::sketchOnFace() {
  const TopoDS_Face face = selectedFace();
  if (face.IsNull()) {
    hint(tr("Sketch on face: click a flat face of the body first."));
    return;
  }
  BRepAdaptor_Surface surface(face);
  if (surface.GetType() != GeomAbs_Plane) {
    hint(tr("Sketch on face: the selected face is not flat."));
    return;
  }
  const gp_Pln pln = surface.Plane();
  gp_Dir normal = pln.Axis().Direction();
  if (face.Orientation() == TopAbs_REVERSED) normal.Reverse(); // point out of the material
  // Origin: the world origin projected onto the face plane, so sketch
  // coordinates line up with the model axes where possible.
  const gp_Pnt loc = pln.Location();
  const gp_Vec toOrigin(loc, gp::Origin());
  const gp_Pnt origin = loc.Translated(toOrigin - gp_Vec(normal) * toOrigin.Dot(gp_Vec(normal)));
  gp_Dir xdir = pln.Position().XDirection();
  if (std::abs(normal.Dot(gp::DZ())) < 0.99) xdir = gp::DZ().Crossed(normal); // keep "up" as world Z on walls
  startSketch(gp_Ax3(origin, normal, xdir));
}

void MainWindow::finishSketch() {
  auto feature = std::make_unique<SketchFeature>();
  feature->plane = m_editor->plane();
  feature->sketch = m_editor->finish();
  const int editing = std::exchange(m_editingSketch, -1);
  statusBar()->clearMessage();
  updateActions();
  if (editing >= 0) {
    replaceFeature(editing, std::move(feature));
  } else if (!feature->sketch.entities.empty()) {
    if (addFeature(std::move(feature))) hint(tr("Sketch finished. Use Extrude or Revolve on it."));
  } else {
    refreshScene();
  }
}

int MainWindow::chosenSketch() const {
  const Handle(AIS_InteractiveContext)& ctx = m_viewer->context();
  for (ctx->InitSelected(); ctx->MoreSelected(); ctx->NextSelected())
    for (const auto& [ais, index] : m_sketchAis)
      if (ctx->SelectedInteractive() == ais) return index;
  for (int i = int(m_doc.size()) - 1; i >= 0; --i)
    if (m_doc.as<SketchFeature>(i) && !m_doc.isConsumed(i)) return i;
  return -1;
}

std::vector<int> MainWindow::selectedEdges() const {
  std::vector<int> edges;
  const Handle(AIS_InteractiveContext)& ctx = m_viewer->context();
  if (m_bodyAis.IsNull()) return edges;
  for (ctx->InitSelected(); ctx->MoreSelected(); ctx->NextSelected())
    if (ctx->SelectedInteractive() == m_bodyAis && ctx->SelectedShape().ShapeType() == TopAbs_EDGE)
      if (int i = edgeIndex(m_doc.body(), ctx->SelectedShape()); i > 0) edges.push_back(i);
  return edges;
}

TopoDS_Face MainWindow::selectedFace() const {
  const Handle(AIS_InteractiveContext)& ctx = m_viewer->context();
  if (m_bodyAis.IsNull()) return {};
  for (ctx->InitSelected(); ctx->MoreSelected(); ctx->NextSelected())
    if (ctx->SelectedInteractive() == m_bodyAis && ctx->SelectedShape().ShapeType() == TopAbs_FACE)
      return TopoDS::Face(ctx->SelectedShape());
  return {};
}

// --- features -----------------------------------------------------------------

void MainWindow::extrude(int editIndex) {
  const auto* old = editIndex >= 0 ? m_doc.as<ExtrudeFeature>(editIndex) : nullptr;
  const int sketch = old ? old->sketch : chosenSketch();
  if (sketch < 0) {
    hint(tr("Extrude: draw a closed sketch first."));
    return;
  }
  const bool hasBody = !(old ? m_doc.bodyBefore(editIndex) : m_doc.body()).IsNull();
  FormDialog dlg(this, tr("Extrude"));
  QDoubleSpinBox* distance = dlg.number(tr("Distance:"), old ? old->distance : 10, -1e6, 1e6, " mm");
  QComboBox* op = operationChoice(dlg, hasBody, old ? old->op : Operation::Join);
  if (!dlg.run()) return;

  auto f = std::make_unique<ExtrudeFeature>();
  f->sketch = sketch;
  f->distance = distance->value();
  f->op = Operation(op->currentIndex());
  if (old) replaceFeature(editIndex, std::move(f));
  else addFeature(std::move(f));
}

void MainWindow::revolve(int editIndex) {
  const auto* old = editIndex >= 0 ? m_doc.as<RevolveFeature>(editIndex) : nullptr;
  const int sketch = old ? old->sketch : chosenSketch();
  if (sketch < 0) {
    hint(tr("Revolve: draw a closed sketch first."));
    return;
  }
  const bool hasBody = !(old ? m_doc.bodyBefore(editIndex) : m_doc.body()).IsNull();
  FormDialog dlg(this, tr("Revolve"));
  QComboBox* axis = dlg.choice(tr("Axis:"), {tr("Sketch Y axis (vertical)"), tr("Sketch X axis (horizontal)")},
                               old && old->axis == RevolveFeature::Axis::SketchX ? 1 : 0);
  QDoubleSpinBox* angle = dlg.number(tr("Angle:"), old ? old->angle : 360, 0.001, 360, QString::fromUtf8(" °"));
  QComboBox* op = operationChoice(dlg, hasBody, old ? old->op : Operation::Join);
  if (!dlg.run()) return;

  auto f = std::make_unique<RevolveFeature>();
  f->sketch = sketch;
  f->axis = axis->currentIndex() == 0 ? RevolveFeature::Axis::SketchY : RevolveFeature::Axis::SketchX;
  f->angle = angle->value();
  f->op = Operation(op->currentIndex());
  if (old) replaceFeature(editIndex, std::move(f));
  else addFeature(std::move(f));
}

void MainWindow::filletOrChamfer(bool fillet, int editIndex) {
  const QString title = fillet ? tr("Fillet") : tr("Chamfer");
  std::vector<int> edges;
  double value = 1;
  if (editIndex >= 0) {
    if (const auto* f = m_doc.as<FilletFeature>(editIndex)) edges = f->edges, value = f->radius;
    if (const auto* c = m_doc.as<ChamferFeature>(editIndex)) edges = c->edges, value = c->distance;
  } else {
    edges = selectedEdges();
  }
  if (edges.empty()) {
    hint(tr("%1: click one or more edges of the body first (Shift+click adds).").arg(title));
    return;
  }
  FormDialog dlg(this, title);
  QDoubleSpinBox* size = dlg.number(fillet ? tr("Radius:") : tr("Distance:"), value, 0.001, 1e6, " mm");
  if (!dlg.run()) return;

  std::unique_ptr<Feature> f;
  if (fillet) {
    auto fi = std::make_unique<FilletFeature>();
    fi->edges = edges;
    fi->radius = size->value();
    f = std::move(fi);
  } else {
    auto c = std::make_unique<ChamferFeature>();
    c->edges = edges;
    c->distance = size->value();
    f = std::move(c);
  }
  if (editIndex >= 0) replaceFeature(editIndex, std::move(f));
  else addFeature(std::move(f));
}

void MainWindow::editFeature(int index) {
  if (index < 0 || index >= int(m_doc.size()) || m_editor->isActive()) return;
  if (const auto* s = m_doc.as<SketchFeature>(index)) {
    m_editingSketch = index;
    m_viewer->context()->ClearSelected(false);
    m_editor->begin(s->plane, s->sketch);
    updateActions();
  } else if (m_doc.as<ExtrudeFeature>(index)) {
    extrude(index);
  } else if (m_doc.as<RevolveFeature>(index)) {
    revolve(index);
  } else if (m_doc.as<FilletFeature>(index)) {
    filletOrChamfer(true, index);
  } else if (m_doc.as<ChamferFeature>(index)) {
    filletOrChamfer(false, index);
  } else {
    hint(tr("This step has no settings to change."));
  }
}

void MainWindow::deleteFeature(int index) {
  if (index < 0 || index >= int(m_doc.size()) || m_editor->isActive()) return;
  int dependents = 0;
  for (size_t i = index + 1; i < m_doc.size(); ++i)
    if (m_doc.feature(i).sketchInput() == index) ++dependents;
  if (dependents > 0 &&
      QMessageBox::question(this, tr("Delete"), tr("Steps built from this sketch will be deleted too. Continue?")) !=
          QMessageBox::Yes)
    return;
  pushUndo();
  m_doc.removeWithDependents(index);
  documentChanged();
  if (m_doc.hasErrors()) hint(tr("Some later steps no longer work, see the red marks in the timeline."));
}

// --- document changes ---------------------------------------------------------

void MainWindow::pushUndo() {
  m_undo.push_back(m_doc);
  if (m_undo.size() > kMaxUndo) m_undo.erase(m_undo.begin());
  m_redo.clear();
}

void MainWindow::rollback() {
  m_doc = std::move(m_undo.back());
  m_undo.pop_back();
}

bool MainWindow::addFeature(std::unique_ptr<Feature> feature) {
  const bool firstBody = m_doc.body().IsNull();
  const QString name = QString::fromStdString(feature->name());
  pushUndo();
  const int index = m_doc.add(std::move(feature));
  if (!m_doc.error(index).empty()) {
    const QString why = QString::fromStdString(m_doc.error(index));
    rollback();
    refreshScene();
    QMessageBox::warning(this, name, why);
    return false;
  }
  documentChanged();
  if (firstBody && !m_doc.body().IsNull()) m_viewer->fitAll();
  hint(tr("%1 added. Double-click it in the timeline to change it.").arg(name));
  return true;
}

bool MainWindow::replaceFeature(int index, std::unique_ptr<Feature> feature) {
  const QString name = QString::fromStdString(feature->name());
  pushUndo();
  m_doc.replace(index, std::move(feature));
  if (!m_doc.error(index).empty()) {
    const QString why = QString::fromStdString(m_doc.error(index));
    rollback();
    refreshScene();
    QMessageBox::warning(this, name, why);
    return false;
  }
  documentChanged();
  if (m_doc.hasErrors()) hint(tr("Some later steps no longer work, see the red marks in the timeline."));
  else hint(tr("%1 updated.").arg(name));
  return true;
}

void MainWindow::documentChanged() {
  setWindowModified(true);
  refreshScene();
  refreshTimeline();
  updateActions();
}

void MainWindow::undo() {
  if (m_undo.empty() || m_editor->isActive()) return;
  m_redo.push_back(std::move(m_doc));
  m_doc = std::move(m_undo.back());
  m_undo.pop_back();
  documentChanged();
}

void MainWindow::redo() {
  if (m_redo.empty() || m_editor->isActive()) return;
  m_undo.push_back(std::move(m_doc));
  m_doc = std::move(m_redo.back());
  m_redo.pop_back();
  documentChanged();
}

// --- files --------------------------------------------------------------------

bool MainWindow::confirmDiscard() {
  if (!isWindowModified()) return true;
  const auto answer = QMessageBox::question(this, tr("Unsaved changes"), tr("Save the changes to this project?"),
                                            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
  if (answer == QMessageBox::Save) return save();
  return answer == QMessageBox::Discard;
}

void MainWindow::newProject() {
  if (!confirmDiscard()) return;
  m_doc = {};
  m_undo.clear();
  m_redo.clear();
  setProjectPath({});
  refreshScene();
  refreshTimeline();
  updateActions();
}

void MainWindow::openProjectDialog() {
  if (!confirmDiscard()) return;
  const QString path = QFileDialog::getOpenFileName(this, tr("Open Project"), {},
                                                    tr("Projects (*.%1)").arg(project::kExtension));
  if (!path.isEmpty()) openProject(path);
}

bool MainWindow::openProject(const QString& path) {
  try {
    m_doc = project::load(path);
  } catch (const std::exception& e) {
    QMessageBox::warning(this, tr("Open Project"), QString::fromStdString(e.what()));
    return false;
  }
  m_undo.clear();
  m_redo.clear();
  setProjectPath(path);
  refreshScene();
  refreshTimeline();
  updateActions();
  m_viewer->setStandardView(V3d_XposYnegZpos);
  if (m_doc.hasErrors()) hint(tr("Some steps could not be rebuilt, see the red marks in the timeline."));
  return true;
}

bool MainWindow::save() {
  if (m_path.isEmpty()) return saveAs();
  try {
    project::save(m_doc, m_path);
  } catch (const std::exception& e) {
    QMessageBox::warning(this, tr("Save"), QString::fromStdString(e.what()));
    return false;
  }
  setWindowModified(false);
  hint(tr("Saved %1").arg(QFileInfo(m_path).fileName()));
  return true;
}

bool MainWindow::saveAs() {
  QString path = QFileDialog::getSaveFileName(this, tr("Save Project"), m_path,
                                              tr("Projects (*.%1)").arg(project::kExtension));
  if (path.isEmpty()) return false;
  if (QFileInfo(path).suffix().isEmpty()) path += QString(".") + project::kExtension;
  const QString old = m_path;
  m_path = path;
  if (!save()) {
    m_path = old;
    return false;
  }
  setProjectPath(path);
  return true;
}

void MainWindow::importFile() {
  const QString path = QFileDialog::getOpenFileName(this, tr("Import"), {},
                                                    tr("CAD files (*.step *.stp *.iges *.igs *.brep);;All files (*)"));
  if (path.isEmpty()) return;
  auto f = std::make_unique<ImportFeature>();
  try {
    f->shape = io::importShape(path.toStdString());
  } catch (const std::exception& e) {
    QMessageBox::warning(this, tr("Import"), QString::fromStdString(e.what()));
    return;
  }
  f->source = QFileInfo(path).fileName().toStdString();
  if (addFeature(std::move(f))) m_viewer->fitAll();
}

void MainWindow::exportFile() {
  if (m_doc.body().IsNull()) {
    hint(tr("Export: there is no body yet."));
    return;
  }
  const QStringList filters = {tr("STEP (*.step *.stp)"), tr("IGES (*.iges *.igs)"), tr("STL mesh (*.stl)"),
                               tr("OBJ mesh (*.obj)"), tr("OpenCASCADE BREP (*.brep)")};
  const char* defaultExt[] = {"step", "iges", "stl", "obj", "brep"};
  const QString base = m_path.isEmpty() ? tr("part") : QFileInfo(m_path).completeBaseName();
  QString filter = filters[0];
  QString path = QFileDialog::getSaveFileName(this, tr("Export"), base + ".step", filters.join(";;"), &filter);
  if (path.isEmpty()) return;
  if (QFileInfo(path).suffix().isEmpty()) path += QString(".") + defaultExt[std::max<qsizetype>(0, filters.indexOf(filter))];
  try {
    io::exportShape(m_doc.body(), path.toStdString());
  } catch (const std::exception& e) {
    QMessageBox::warning(this, tr("Export"), QString::fromStdString(e.what()));
    return;
  }
  hint(tr("Exported %1").arg(QFileInfo(path).fileName()));
}

void MainWindow::setProjectPath(const QString& path) {
  m_path = path;
  const QString name = path.isEmpty() ? tr("Untitled") : QFileInfo(path).fileName();
  setWindowTitle(name + "[*] - OpenCaliper");
  setWindowModified(false);
}

void MainWindow::closeEvent(QCloseEvent* e) {
  if (m_editor->isActive()) finishSketch();
  if (confirmDiscard()) e->accept();
  else e->ignore();
}

// --- display ------------------------------------------------------------------

void MainWindow::refreshScene() {
  const Handle(AIS_InteractiveContext)& ctx = m_viewer->context();
  ctx->ClearSelected(false);
  if (!m_bodyAis.IsNull()) ctx->Remove(m_bodyAis, false);
  m_bodyAis.Nullify();
  for (const auto& [ais, index] : m_sketchAis) ctx->Remove(ais, false);
  m_sketchAis.clear();

  const TopoDS_Shape body = m_doc.body();
  if (!body.IsNull()) {
    m_bodyAis = new AIS_Shape(body);
    m_bodyAis->SetColor(Quantity_NOC_GOLDENROD);
    // SetColor also tints the edges; keep them black like in CAD tools.
    m_bodyAis->Attributes()->SetFaceBoundaryAspect(new Prs3d_LineAspect(Quantity_NOC_BLACK, Aspect_TOL_SOLID, 1.0));
    // Click picks edges (fillet/chamfer) and faces (sketch on face). The first
    // mode must be given to Display: an object displayed with -1 and activated
    // afterwards is never picked.
    ctx->Display(m_bodyAis, AIS_Shaded, AIS_Shape::SelectionMode(TopAbs_FACE), false);
    ctx->Activate(m_bodyAis, AIS_Shape::SelectionMode(TopAbs_EDGE));
  }
  // Sketches stay visible (and selectable) until a feature uses them, like Fusion.
  for (size_t i = 0; i < m_doc.size(); ++i) {
    const auto* s = m_doc.as<SketchFeature>(i);
    if (!s || m_doc.isConsumed(i) || int(i) == m_editingSketch) continue;
    Handle(AIS_Shape) ais = new AIS_Shape(s->edges());
    ais->SetColor(Quantity_NOC_GRAY20);
    ais->SetWidth(2.0);
    ctx->Display(ais, AIS_WireFrame, 0, false);
    m_sketchAis.emplace_back(ais, int(i));
  }
  m_viewer->redraw();
}

QString MainWindow::describe(size_t i) const {
  if (const auto* s = m_doc.as<SketchFeature>(i))
    return tr("Sketch (%n curve(s))", "", int(s->sketch.entities.size()));
  if (const auto* e = m_doc.as<ExtrudeFeature>(i))
    return tr("Extrude %1 mm, %2").arg(num(e->distance), opName(e->op));
  if (const auto* r = m_doc.as<RevolveFeature>(i))
    return tr("Revolve %1°, %2").arg(num(r->angle), opName(r->op));
  if (const auto* f = m_doc.as<FilletFeature>(i))
    return tr("Fillet R%1, %n edge(s)", "", int(f->edges.size())).arg(num(f->radius));
  if (const auto* c = m_doc.as<ChamferFeature>(i))
    return tr("Chamfer %1 mm, %n edge(s)", "", int(c->edges.size())).arg(num(c->distance));
  if (const auto* im = m_doc.as<ImportFeature>(i))
    return tr("Import %1").arg(QString::fromStdString(im->source));
  return QString::fromStdString(m_doc.feature(i).name());
}

void MainWindow::refreshTimeline() {
  m_timeline->clear();
  for (size_t i = 0; i < m_doc.size(); ++i) {
    const QString icon = QString::fromStdString(m_doc.feature(i).name()).toLower(); // icon per feature type
    auto* item = new QListWidgetItem(QString("%1. %2").arg(i + 1).arg(describe(i)), m_timeline);
    const std::string& err = m_doc.error(i);
    item->setIcon(makeIcon(err.empty() ? icon : "error"));
    if (!err.empty()) {
      item->setForeground(QColor(200, 40, 30));
      item->setToolTip(QString::fromStdString(err));
    }
  }
}

void MainWindow::updateActions() {
  const bool sketching = m_editor->isActive();
  for (QAction* a : m_modelActions) a->setEnabled(!sketching);
  for (QAction* a : m_sketchActions) a->setEnabled(sketching);
  m_drawBar->setVisible(sketching);
  m_constraintBar->setVisible(sketching);
  m_timeline->setEnabled(!sketching);
  m_undoAction->setEnabled(!sketching && !m_undo.empty());
  m_redoAction->setEnabled(!sketching && !m_redo.empty());
  if (sketching)
    hint(tr("Sketching: L line, C circle, A arc, S select. Esc stops, Delete removes. Ctrl+Enter finishes."));
  else if (m_doc.size() == 0)
    hint(tr("Start with a sketch: pick a plane in the Sketch toolbar."));
}

void MainWindow::hint(const QString& text) {
  m_hint->setText(text);
}
