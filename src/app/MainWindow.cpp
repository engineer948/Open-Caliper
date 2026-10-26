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
#include <QSpinBox>
#include <QInputDialog>
#include <QStatusBar>
#include <QToolBar>

#include <algorithm>
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
  QSpinBox* integer(const QString& label, int value, int min, int max) {
    auto* box = new QSpinBox(this);
    box->setRange(min, max);
    box->setValue(value);
    m_form->addRow(label, box);
    return box;
  }
  QLabel* text(const QString& label, const QString& value) {
    auto* l = new QLabel(value, this);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_form->addRow(label, l);
    return l;
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
  m->addSeparator();
  QAction* dims = action(tr("Show Dimensions"), "dimension", {}, tr("Hole diameters, sketch sizes and extrude depths.\nDouble-click one to change it."));
  dims->setCheckable(true);
  dims->setChecked(m_showDimensions);
  connect(dims, &QAction::toggled, this, [this](bool on) {
    m_showDimensions = on;
    refreshScene();
  });
  m->addAction(dims);
  bar->addAction(dims);
  connect(m_viewer, &Viewer::doubleClicked, this, &MainWindow::editDimension);
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
      {tr("Rectangle"), "rectangle", "R", SketchEditor::Tool::Rectangle},
      {tr("Circle"), "circle", "C", SketchEditor::Tool::Circle},
      {tr("Arc"), "arc", "A", SketchEditor::Tool::Arc},
      {tr("Polygon"), "polygon", "G", SketchEditor::Tool::Polygon},
      {tr("Slot"), "slot", "K", SketchEditor::Tool::Slot}};
  for (const auto& [text, icon, key, t] : toolList) {
    QAction* a = action(text, icon, QKeySequence(key));
    a->setCheckable(true);
    const SketchEditor::Tool tool = t;
    connect(a, &QAction::triggered, this, [this, tool] {
      if (tool == SketchEditor::Tool::Polygon) {
        bool ok = false;
        const int sides = QInputDialog::getInt(this, tr("Polygon"), tr("Number of sides:"), m_editor->polygonSides(), 3, 64, 1, &ok);
        if (!ok) {
          m_toolActions[SketchEditor::Tool::Select]->setChecked(true); // keep the previous tool shown
          m_editor->setTool(SketchEditor::Tool::Select);
          return;
        }
        m_editor->setPolygonSides(sides);
      }
      m_editor->setTool(tool);
    });
    tools->addAction(a);
    menu->addAction(a);
    m_drawBar->addAction(a);
    m_toolActions[t] = a;
    m_sketchActions << a;
  }
  connect(m_editor, &SketchEditor::toolChanged, this, [this](SketchEditor::Tool t) { m_toolActions[t]->setChecked(true); });
  connect(m_editor, &SketchEditor::historyChanged, this, &MainWindow::updateActions);
  QAction* construction = action(tr("Construction"), "construction", QKeySequence("X"),
                                 tr("Turn the selected curves into dashed helper geometry that never becomes a solid"));
  connect(construction, &QAction::triggered, this, [this] { m_editor->toggleConstruction(); });
  menu->addAction(construction);
  m_drawBar->addSeparator();
  m_drawBar->addAction(construction);
  m_sketchActions << construction;
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
  add(action(tr("Shell"), "shell", QKeySequence("Shift+H"), tr("Hollow the part out, leaving the selected faces open")),
      [this] { shell(); });
  add(action(tr("Pattern"), "pattern", QKeySequence("Shift+P"), tr("Repeat an extrude or revolve in a row or a circle")),
      [this] { pattern(); });
  bar->addSeparator();
  menu->addSeparator();
  add(action(tr("Measure"), "measure", QKeySequence("M"), tr("Click one or two points, edges or faces first")),
      [this] { measure(); });
  add(action(tr("Properties"), "properties", QKeySequence("Shift+M"), tr("Volume, size and weight of the part")),
      [this] { showProperties(); });
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
           "<a href=\"https://github.com/open-workshop-tools/Open-Caliper\">github.com/open-workshop-tools/Open-Caliper</a></p>"
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
  clearDimensions();
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

std::vector<int> MainWindow::selectedFaces() const {
  std::vector<int> faces;
  for (const TopoDS_Shape& s : selectedShapes())
    if (s.ShapeType() == TopAbs_FACE)
      if (int i = faceIndex(m_doc.body(), s); i > 0) faces.push_back(i);
  return faces;
}

std::vector<TopoDS_Shape> MainWindow::selectedShapes() const {
  std::vector<TopoDS_Shape> shapes;
  const Handle(AIS_InteractiveContext)& ctx = m_viewer->context();
  if (m_bodyAis.IsNull()) return shapes;
  for (ctx->InitSelected(); ctx->MoreSelected(); ctx->NextSelected())
    if (ctx->SelectedInteractive() == m_bodyAis && ctx->HasSelectedShape()) shapes.push_back(ctx->SelectedShape());
  return shapes;
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

void MainWindow::shell(int editIndex) {
  const auto* old = editIndex >= 0 ? m_doc.as<ShellFeature>(editIndex) : nullptr;
  const std::vector<int> faces = old ? old->faces : selectedFaces();
  if (faces.empty()) {
    hint(tr("Shell: click the face(s) to leave open first, e.g. the top (Shift+click adds)."));
    return;
  }
  FormDialog dlg(this, tr("Shell"));
  QDoubleSpinBox* thickness = dlg.number(tr("Wall thickness:"), old ? old->thickness : 2, 0.01, 1e5, " mm");
  if (!dlg.run()) return;
  auto f = std::make_unique<ShellFeature>();
  f->faces = faces;
  f->thickness = thickness->value();
  if (old) replaceFeature(editIndex, std::move(f));
  else addFeature(std::move(f));
}

void MainWindow::pattern(int editIndex) {
  const auto* old = editIndex >= 0 ? m_doc.as<PatternFeature>(editIndex) : nullptr;
  // Extrudes and revolves before this step can be repeated.
  const size_t end = old ? size_t(editIndex) : m_doc.size();
  std::vector<int> sources;
  QStringList names;
  for (size_t i = 0; i < end; ++i)
    if (m_doc.as<ExtrudeFeature>(i) || m_doc.as<RevolveFeature>(i)) {
      sources.push_back(int(i));
      names << QString("%1. %2").arg(i + 1).arg(describe(i));
    }
  if (sources.empty()) {
    hint(tr("Pattern: make an extrude or revolve first (for example a hole cut), then repeat it."));
    return;
  }
  const int current = old ? int(std::find(sources.begin(), sources.end(), old->source) - sources.begin())
                          : int(sources.size()) - 1;

  FormDialog dlg(this, tr("Pattern"));
  QComboBox* source = dlg.choice(tr("Repeat:"), names, std::clamp(current, 0, int(sources.size()) - 1));
  QComboBox* kind = dlg.choice(tr("Type:"), {tr("In a row (linear)"), tr("In a circle (circular)")},
                               old && old->kind == PatternFeature::Kind::Circular ? 1 : 0);
  QComboBox* axis = dlg.choice(tr("Axis:"), {tr("X"), tr("Y"), tr("Z")}, old ? int(old->axis) : 0);
  QSpinBox* count = dlg.integer(tr("Copies (in total):"), old ? old->count : 3, 2, 500);
  QDoubleSpinBox* spacing = dlg.number(tr("Spacing:"), old ? old->spacing : 20, -1e5, 1e5, " mm");
  QDoubleSpinBox* angle = dlg.number(tr("Total angle:"), old ? old->angle : 360, 1, 360, QString::fromUtf8(" °"));
  auto sync = [&] { // a row needs spacing, a circle an angle
    spacing->setEnabled(kind->currentIndex() == 0);
    angle->setEnabled(kind->currentIndex() == 1);
    if (kind->currentIndex() == 1 && !old) axis->setCurrentIndex(2); // around Z is the usual case
  };
  connect(kind, &QComboBox::currentIndexChanged, &dlg, sync);
  sync();
  if (!dlg.run()) return;

  auto f = std::make_unique<PatternFeature>();
  f->source = sources[source->currentIndex()];
  f->kind = kind->currentIndex() == 0 ? PatternFeature::Kind::Linear : PatternFeature::Kind::Circular;
  f->axis = PatternFeature::Axis(axis->currentIndex());
  f->count = count->value();
  f->spacing = spacing->value();
  f->angle = angle->value();
  if (old) replaceFeature(editIndex, std::move(f));
  else addFeature(std::move(f));
}

void MainWindow::measure() {
  const std::vector<TopoDS_Shape> picked = selectedShapes();
  const std::string result = picked.size() <= 2 ? model::measure(picked) : std::string();
  if (result.empty()) {
    hint(tr("Measure: click one or two points, edges or faces of the part (Shift+click for the second)."));
    return;
  }
  QMessageBox::information(this, tr("Measure"), QString::fromStdString(result));
}

void MainWindow::showProperties() {
  if (m_doc.body().IsNull()) {
    hint(tr("Properties: there is no body yet."));
    return;
  }
  const MassProperties m = massProperties(m_doc.body());
  // Density in g/cm³ of common materials for 3D printing and machining.
  const std::pair<QString, double> materials[] = {
      {tr("PLA"), 1.24},        {tr("PETG"), 1.27},      {tr("ABS"), 1.04},   {tr("Nylon (PA12)"), 1.01},
      {tr("Aluminium"), 2.70}, {tr("Steel"), 7.85},     {tr("Stainless steel"), 8.00},
      {tr("Brass"), 8.50},     {tr("Wood (pine)"), 0.50}};
  QStringList names;
  for (const auto& [name, density] : materials) names << QString("%1  (%2 g/cm³)").arg(name).arg(density);

  FormDialog dlg(this, tr("Properties"));
  dlg.text(tr("Volume:"), tr("%1 cm³  (%2 mm³)").arg(num(m.volume / 1000)).arg(num(m.volume)));
  dlg.text(tr("Surface area:"), tr("%1 cm²").arg(num(m.area / 100)));
  dlg.text(tr("Size (X × Y × Z):"),
           QString::fromUtf8("%1 × %2 × %3 mm").arg(num(m.size.X()), num(m.size.Y()), num(m.size.Z())));
  dlg.text(tr("Centre of mass:"),
           QString("X %1, Y %2, Z %3 mm").arg(num(m.center.X()), num(m.center.Y()), num(m.center.Z())));
  QComboBox* material = dlg.choice(tr("Material:"), names);
  QLabel* weight = dlg.text(tr("Weight:"), {});
  auto update = [&] {
    const double grams = m.volume / 1000 * materials[material->currentIndex()].second;
    weight->setText(tr("<b>%1 g</b>").arg(num(std::round(grams * 10) / 10)));
  };
  connect(material, &QComboBox::currentIndexChanged, &dlg, update);
  update();
  dlg.run();
}

void MainWindow::editFeature(int index) {
  if (index < 0 || index >= int(m_doc.size()) || m_editor->isActive()) return;
  if (const auto* s = m_doc.as<SketchFeature>(index)) {
    m_editingSketch = index;
    m_viewer->context()->ClearSelected(false);
    clearDimensions();
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
  } else if (m_doc.as<ShellFeature>(index)) {
    shell(index);
  } else if (m_doc.as<PatternFeature>(index)) {
    pattern(index);
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
  hint(tr("%1 added. Double-click it in the timeline, or a dimension in the view, to change it.").arg(name));
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
  if (m_editor->isActive()) return m_editor->undo(); // inside a sketch: undo sketch edits
  if (m_undo.empty()) return;
  m_redo.push_back(std::move(m_doc));
  m_doc = std::move(m_undo.back());
  m_undo.pop_back();
  documentChanged();
}

void MainWindow::redo() {
  if (m_editor->isActive()) return m_editor->redo();
  if (m_redo.empty()) return;
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
    ctx->Activate(m_bodyAis, AIS_Shape::SelectionMode(TopAbs_VERTEX)); // for Measure
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

  clearDimensions();
  if (m_showDimensions && !m_editor->isActive()) {
    m_dimensions = buildDimensions(m_doc);
    for (const auto& [dim, ref] : m_dimensions) {
      dim->SetZLayer(Graphic3d_ZLayerId_Topmost); // readable even behind or inside the part
      ctx->Display(dim, 0, 0, false);
    }
  }
  m_viewer->redraw();
}

void MainWindow::clearDimensions() {
  const Handle(AIS_InteractiveContext)& ctx = m_viewer->context();
  for (const auto& [dim, ref] : m_dimensions) ctx->Remove(dim, false);
  m_dimensions.clear();
}

void MainWindow::editDimension() {
  const Handle(AIS_InteractiveContext)& ctx = m_viewer->context();
  if (!ctx->HasDetected() || m_editor->isActive()) return;
  const auto it = std::find_if(m_dimensions.begin(), m_dimensions.end(),
                               [&](const auto& d) { return d.first == ctx->DetectedInteractive(); });
  if (it == m_dimensions.end()) return;
  const DimensionRef ref = it->second; // refreshScene below replaces the list

  QString label = tr("Length:");
  if (ref.kind == DimensionRef::Kind::ExtrudeDepth) label = tr("Depth:");
  else if (ref.kind == DimensionRef::Kind::SketchCurve) label = ref.diameter ? tr("Diameter:") : tr("Radius:");
  FormDialog dlg(this, tr("Change Dimension"));
  QDoubleSpinBox* value = dlg.number(label, ref.value, 0.001, 1e6, " mm");
  if (!dlg.run()) return;

  std::string error;
  std::unique_ptr<Feature> changed = applyDimension(m_doc, ref, value->value(), error);
  if (!changed) {
    QMessageBox::warning(this, tr("Change Dimension"), QString::fromStdString(error));
    return;
  }
  replaceFeature(ref.feature, std::move(changed));
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
  if (const auto* sh = m_doc.as<ShellFeature>(i))
    return tr("Shell %1 mm, %n open face(s)", "", int(sh->faces.size())).arg(num(sh->thickness));
  if (const auto* p = m_doc.as<PatternFeature>(i)) {
    const QString axis = QString("XYZ")[int(p->axis)];
    return p->kind == PatternFeature::Kind::Linear
               ? tr("Pattern %1× along %2, %3 mm apart").arg(p->count).arg(axis, num(p->spacing))
               : tr("Pattern %1× around %2, %3°").arg(p->count).arg(axis, num(p->angle));
  }
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
  m_undoAction->setEnabled(sketching ? m_editor->canUndo() : !m_undo.empty());
  m_redoAction->setEnabled(sketching ? m_editor->canRedo() : !m_redo.empty());
  if (sketching)
    hint(tr("Sketching: L line, R rectangle, C circle, A arc, G polygon, K slot, X construction, S select. "
            "Esc stops, Delete removes, Ctrl+Z undoes. Ctrl+Enter finishes."));
  else if (m_doc.size() == 0)
    hint(tr("Start with a sketch: pick a plane in the Sketch toolbar."));
}

void MainWindow::hint(const QString& text) {
  m_hint->setText(text);
}
