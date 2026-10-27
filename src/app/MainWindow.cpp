#include "app/MainWindow.h"
#include "app/Icons.h"
#include "app/LicensesDialog.h"
#include "app/ProjectFile.h"
#include "io/Exchange.h"
#include "view/Viewer.h"
#ifdef OPENCALIPER_MCP
#include "app/McpLink.h"
#include "app/ModelApi.h"
#endif

#include <BRepAdaptor_Surface.hxx>
#include <Graphic3d_Camera.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <Prs3d_Drawer.hxx>
#include <Prs3d_LineAspect.hxx>
#include <TopoDS.hxx>

#include <QActionGroup>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QLockFile>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QShortcut>
#include <QSpinBox>
#include <QInputDialog>
#include <QStatusBar>
#include <QToolBar>

#include <algorithm>
#include <cmath>
#include <utility>

using K = sketch::ConstraintKind;
using namespace model;

namespace {

constexpr size_t kMaxUndo = 100;

QString num(double v) { return QString::number(v, 'g', 6); }

// Size entry that also takes formulas with parameters ("width / 2"); the
// result is shown next to it as you type.
class SizeField : public QWidget {
public:
  SizeField(QWidget* parent, const Variables& vars, double value, const std::string& expr, double min, double max,
            const QString& suffix, bool integer)
      : QWidget(parent), m_vars(vars), m_min(min), m_max(max), m_suffix(suffix), m_integer(integer) {
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    m_edit = new QLineEdit(expr.empty() ? num(value) : QString::fromStdString(expr), this);
    m_edit->setToolTip(QObject::tr("A number, or a formula with parameters such as width / 2"));
    m_result = new QLabel(this);
    m_result->setMinimumWidth(100);
    row->addWidget(m_edit, 1);
    row->addWidget(m_result);
    QObject::connect(m_edit, &QLineEdit::textChanged, this, [this] {
      QString error;
      check(error);
    });
    QString error;
    check(error);
    m_edit->selectAll();
  }
  // Evaluates the text; false with `error` set when it is not a valid value.
  bool check(QString& error) {
    const std::string text = m_edit->text().trimmed().toStdString();
    try {
      double v = model::evaluate(text, m_vars);
      if (m_integer) v = std::round(v);
      if (v < m_min || v > m_max)
        throw std::runtime_error(QObject::tr("The value must be between %1 and %2.").arg(num(m_min), num(m_max)).toStdString());
      m_value = v;
      m_expr = model::isPlainNumber(text) ? std::string() : text;
      m_result->setText(m_expr.empty() ? m_suffix.trimmed() : "= " + num(v) + m_suffix);
      m_result->setStyleSheet({});
      return true;
    } catch (const std::exception& e) {
      error = QString::fromStdString(e.what());
      m_result->setText(QObject::tr("not valid"));
      m_result->setStyleSheet("color: #c8281e");
      return false;
    }
  }
  double value() const { return m_value; }
  const std::string& expression() const { return m_expr; } // empty for a plain number
  void setValue(double v) { m_edit->setText(num(v)); }
  QLineEdit* edit() const { return m_edit; }

private:
  const Variables& m_vars;
  double m_min, m_max;
  QString m_suffix;
  bool m_integer;
  QLineEdit* m_edit;
  QLabel* m_result;
  double m_value = 0;
  std::string m_expr;
};

// Small form dialog: label/widget rows plus OK/Cancel.
class FormDialog : public QDialog {
public:
  FormDialog(QWidget* parent, const QString& title, const Variables& vars = {})
      : QDialog(parent), m_form(new QFormLayout(this)), m_vars(vars) {
    setWindowTitle(title);
  }
  SizeField* size(const QString& label, double value, const std::string& expr, double min, double max,
                  const QString& suffix, bool integer = false) {
    auto* field = new SizeField(this, m_vars, value, expr, min, max, suffix, integer);
    m_form->addRow(label, field);
    m_sizes.push_back(field);
    if (m_sizes.size() == 1) field->edit()->setFocus();
    return field;
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
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
      for (SizeField* f : m_sizes) {
        QString error;
        if (f->isEnabled() && !f->check(error)) {
          QMessageBox::warning(this, windowTitle(), error);
          f->edit()->setFocus();
          return;
        }
      }
      accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    m_form->addRow(buttons);
    return exec() == QDialog::Accepted;
  }

private:
  QFormLayout* m_form;
  Variables m_vars; // a copy: the fields refer to it
  std::vector<SizeField*> m_sizes;
};

// The formula stored for `field` of `f` (empty when it is a plain number or f is null).
std::string exprOf(const Feature* f, const std::string& field) {
  if (!f) return {};
  const auto it = f->expressions.find(field);
  return it == f->expressions.end() ? std::string() : it->second;
}

void setExpr(Feature& f, const std::string& field, const SizeField* size) {
  if (size->expression().empty()) f.expressions.erase(field);
  else f.expressions[field] = size->expression();
}

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
#ifdef OPENCALIPER_MCP
  buildToolsMenu();
#endif
  buildHelpMenu();

  // Crash backup, written every minute while there are unsaved changes. The
  // lock file tells a later start whether this window is still running.
  const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/autosave";
  QDir().mkpath(dir);
  const QString id = QUuid::createUuid().toString(QUuid::Id128).left(12);
  m_autosavePath = dir + "/" + id + "." + project::kExtension;
  m_autosaveLock = std::make_unique<QLockFile>(dir + "/" + id + ".lock");
  m_autosaveLock->tryLock(0);
  auto* timer = new QTimer(this);
  connect(timer, &QTimer::timeout, this, &MainWindow::autosave);
  timer->start(60 * 1000);

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
  m_recentMenu = m->addMenu(tr("Open &Recent"));
  connect(m_recentMenu, &QMenu::aboutToShow, this, &MainWindow::fillRecentMenu);
  m_modelActions << m_recentMenu->menuAction();
  add(action(tr("&Save"), "", QKeySequence::Save), &MainWindow::save);
  add(action(tr("Save &As..."), "", QKeySequence::SaveAs), &MainWindow::saveAs);
  m->addSeparator();
  add(action(tr("&Import STEP / IGES / BREP..."), "import", QKeySequence("Ctrl+I")), &MainWindow::importFile);
  add(action(tr("&Export STEP / IGES / STL / OBJ / 3MF / BREP..."), "", QKeySequence("Ctrl+E")), &MainWindow::exportFile);
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
      {tr("Slot"), "slot", "K", SketchEditor::Tool::Slot},
      {tr("Trim"), "trim", "W", SketchEditor::Tool::Trim},
      {tr("Fillet"), "sketch-fillet", "N", SketchEditor::Tool::Fillet}};
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
  QAction* offset = action(tr("Offset"), "offset", QKeySequence("O"),
                           tr("Parallel copy of the selected curves; a closed shape grows or shrinks"));
  connect(offset, &QAction::triggered, this, [this] { m_editor->offsetSelection(); });
  QAction* mirrorCurves = action(tr("Mirror"), "mirror", QKeySequence("U"),
                                 tr("Mirror the selected curves across the line selected last"));
  connect(mirrorCurves, &QAction::triggered, this, [this] { m_editor->mirrorSelection(); });
  m_drawBar->addSeparator();
  for (QAction* a : {offset, mirrorCurves, construction}) {
    menu->addAction(a);
    m_drawBar->addAction(a);
    m_sketchActions << a;
  }
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
      {tr("Equal"), "equal", "E", tr("Two lines or two circles/arcs"), K::Equal},
      {tr("Symmetric"), "symmetric", "Y", tr("Two points and the line they mirror across"), K::Symmetric}};
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
  add(action(tr("Pattern"), "pattern", QKeySequence("Shift+P"), tr("Repeat an extrude, revolve or hole in a row or a circle")),
      [this] { pattern(); });
  add(action(tr("Hole"), "hole", QKeySequence("Shift+D"),
             tr("Screw holes at the circles of a sketch: simple, counterbored or countersunk")),
      [this] { hole(); });
  add(action(tr("Mirror"), "mirror", QKeySequence("Shift+U"), tr("Mirror the part across a plane or a face")),
      [this] { mirror(); });
  bar->addSeparator();
  menu->addSeparator();
  add(action(tr("Parameters"), "parameters", QKeySequence("Ctrl+P"), tr("Named sizes and formulas that drive the model")),
      [this] { editParameters(); });
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
  m_editor->setVariables(m_doc.values());
  m_editor->begin(plane);
  updateActions();
}

void MainWindow::sketchOnFace() {
  const TopoDS_Face face = selectedFace();
  if (face.IsNull()) {
    hint(tr("Sketch on face: click a flat face of the body first."));
    return;
  }
  gp_Ax3 plane;
  if (!sketchPlaneOnFace(face, plane)) {
    hint(tr("Sketch on face: the selected face is not flat."));
    return;
  }
  startSketch(plane);
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

std::vector<ShapeKey> MainWindow::keysOf(const std::vector<int>& indices, TopAbs_ShapeEnum kind) const {
  TopTools_IndexedMapOfShape map;
  TopExp::MapShapes(m_doc.body(), kind, map);
  std::vector<ShapeKey> keys;
  for (int i : indices)
    if (i >= 1 && i <= map.Extent()) keys.push_back(shapeKey(m_doc.body(), map(i)));
  return keys;
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
  FormDialog dlg(this, tr("Extrude"), m_doc.values());
  SizeField* distance = dlg.size(tr("Distance:"), old ? old->distance : 10, exprOf(old, "distance"), -1e6, 1e6, " mm");
  QComboBox* op = operationChoice(dlg, hasBody, old ? old->op : Operation::Join);
  if (!dlg.run()) return;

  auto f = std::make_unique<ExtrudeFeature>();
  f->sketch = sketch;
  f->distance = distance->value();
  setExpr(*f, "distance", distance);
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
  FormDialog dlg(this, tr("Revolve"), m_doc.values());
  QComboBox* axis = dlg.choice(tr("Axis:"), {tr("Sketch Y axis (vertical)"), tr("Sketch X axis (horizontal)")},
                               old && old->axis == RevolveFeature::Axis::SketchX ? 1 : 0);
  SizeField* angle = dlg.size(tr("Angle:"), old ? old->angle : 360, exprOf(old, "angle"), 0.001, 360, QString::fromUtf8(" °"));
  QComboBox* op = operationChoice(dlg, hasBody, old ? old->op : Operation::Join);
  if (!dlg.run()) return;

  auto f = std::make_unique<RevolveFeature>();
  f->sketch = sketch;
  f->axis = axis->currentIndex() == 0 ? RevolveFeature::Axis::SketchY : RevolveFeature::Axis::SketchX;
  f->angle = angle->value();
  setExpr(*f, "angle", angle);
  f->op = Operation(op->currentIndex());
  if (old) replaceFeature(editIndex, std::move(f));
  else addFeature(std::move(f));
}

void MainWindow::filletOrChamfer(bool fillet, int editIndex) {
  const QString title = fillet ? tr("Fillet") : tr("Chamfer");
  const char* field = fillet ? "radius" : "distance";
  std::vector<int> edges;
  std::vector<ShapeKey> keys;
  double value = 1;
  const Feature* old = editIndex >= 0 ? &m_doc.feature(editIndex) : nullptr;
  if (editIndex >= 0) {
    if (const auto* f = m_doc.as<FilletFeature>(editIndex)) edges = f->edges, keys = f->keys, value = f->radius;
    if (const auto* c = m_doc.as<ChamferFeature>(editIndex)) edges = c->edges, keys = c->keys, value = c->distance;
  } else {
    edges = selectedEdges();
    keys = keysOf(edges, TopAbs_EDGE);
  }
  if (edges.empty()) {
    hint(tr("%1: click one or more edges of the body first (Shift+click adds).").arg(title));
    return;
  }
  FormDialog dlg(this, title, m_doc.values());
  SizeField* size = dlg.size(fillet ? tr("Radius:") : tr("Distance:"), value, exprOf(old, field), 0.001, 1e6, " mm");
  if (!dlg.run()) return;

  std::unique_ptr<Feature> f;
  if (fillet) {
    auto fi = std::make_unique<FilletFeature>();
    fi->edges = edges;
    fi->keys = keys;
    fi->radius = size->value();
    f = std::move(fi);
  } else {
    auto c = std::make_unique<ChamferFeature>();
    c->edges = edges;
    c->keys = keys;
    c->distance = size->value();
    f = std::move(c);
  }
  setExpr(*f, field, size);
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
  FormDialog dlg(this, tr("Shell"), m_doc.values());
  SizeField* thickness = dlg.size(tr("Wall thickness:"), old ? old->thickness : 2, exprOf(old, "thickness"), 0.01, 1e5, " mm");
  if (!dlg.run()) return;
  auto f = std::make_unique<ShellFeature>();
  f->faces = faces;
  f->keys = old ? old->keys : keysOf(faces, TopAbs_FACE);
  f->thickness = thickness->value();
  setExpr(*f, "thickness", thickness);
  if (old) replaceFeature(editIndex, std::move(f));
  else addFeature(std::move(f));
}

void MainWindow::pattern(int editIndex) {
  const auto* old = editIndex >= 0 ? m_doc.as<PatternFeature>(editIndex) : nullptr;
  // Extrudes, revolves and holes before this step can be repeated.
  const size_t end = old ? size_t(editIndex) : m_doc.size();
  std::vector<int> sources;
  QStringList names;
  for (size_t i = 0; i < end; ++i)
    if (m_doc.as<ExtrudeFeature>(i) || m_doc.as<RevolveFeature>(i) || m_doc.as<HoleFeature>(i)) {
      sources.push_back(int(i));
      names << QString("%1. %2").arg(i + 1).arg(describe(i));
    }
  if (sources.empty()) {
    hint(tr("Pattern: make an extrude, revolve or hole first (for example a hole cut), then repeat it."));
    return;
  }
  const int current = old ? int(std::find(sources.begin(), sources.end(), old->source) - sources.begin())
                          : int(sources.size()) - 1;

  FormDialog dlg(this, tr("Pattern"), m_doc.values());
  QComboBox* source = dlg.choice(tr("Repeat:"), names, std::clamp(current, 0, int(sources.size()) - 1));
  QComboBox* kind = dlg.choice(tr("Type:"), {tr("In a row (linear)"), tr("In a circle (circular)")},
                               old && old->kind == PatternFeature::Kind::Circular ? 1 : 0);
  QComboBox* axis = dlg.choice(tr("Axis:"), {tr("X"), tr("Y"), tr("Z")}, old ? int(old->axis) : 0);
  SizeField* count = dlg.size(tr("Copies (in total):"), old ? old->count : 3, exprOf(old, "count"), 2, 500, {}, true);
  SizeField* spacing = dlg.size(tr("Spacing:"), old ? old->spacing : 20, exprOf(old, "spacing"), -1e5, 1e5, " mm");
  SizeField* angle = dlg.size(tr("Total angle:"), old ? old->angle : 360, exprOf(old, "angle"), 1, 360, QString::fromUtf8(" °"));
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
  f->count = int(count->value());
  setExpr(*f, "count", count);
  if (f->kind == PatternFeature::Kind::Linear) {
    f->spacing = spacing->value();
    setExpr(*f, "spacing", spacing);
    f->angle = old ? old->angle : 360;
  } else {
    f->angle = angle->value();
    setExpr(*f, "angle", angle);
    f->spacing = old ? old->spacing : 20;
  }
  if (old) replaceFeature(editIndex, std::move(f));
  else addFeature(std::move(f));
}

void MainWindow::hole(int editIndex) {
  const auto* old = editIndex >= 0 ? m_doc.as<HoleFeature>(editIndex) : nullptr;
  const int sketch = old ? old->sketch : chosenSketch();
  if (!old && (sketch < 0 || m_doc.body().IsNull())) {
    hint(tr("Hole: sketch on a face of the part and draw a circle where each hole goes, then click Hole."));
    return;
  }
  // ISO metric screws: clearance hole (normal fit), counterbore for socket head
  // cap screws, 90° countersink for flat head screws.
  struct Size {
    const char* name;
    double hole, bore, boreDepth, sink;
  };
  static constexpr Size sizes[] = {{"M2", 2.4, 4.4, 2.3, 4.4},     {"M2.5", 2.9, 5.5, 2.8, 5.5},
                                   {"M3", 3.4, 6.5, 3.3, 6.3},     {"M4", 4.5, 8, 4.4, 9.4},
                                   {"M5", 5.5, 10, 5.4, 10.4},     {"M6", 6.6, 11, 6.5, 12.6},
                                   {"M8", 9, 15, 8.6, 17.3},       {"M10", 11, 18, 10.8, 20},
                                   {"M12", 13.5, 20, 13, 24}};
  QStringList sizeNames{tr("Custom")};
  for (const Size& s : sizes) sizeNames << tr("%1 screw").arg(s.name);

  const HoleFeature defaults;
  const HoleFeature& h = old ? *old : defaults;
  FormDialog dlg(this, tr("Hole"), m_doc.values());
  QComboBox* kind = dlg.choice(tr("Type:"), {tr("Simple"), tr("Counterbore (socket head)"), tr("Countersink (flat head)")},
                               int(h.kind));
  QComboBox* standard = dlg.choice(tr("Size:"), sizeNames, old ? 0 : 3);
  SizeField* diameter = dlg.size(tr("Diameter:"), h.diameter, exprOf(old, "diameter"), 0.01, 1e5, " mm");
  SizeField* depth = dlg.size(tr("Depth (0 = through all):"), h.depth, exprOf(old, "depth"), 0, 1e5, " mm");
  SizeField* bore = dlg.size(tr("Counterbore diameter:"), h.boreDiameter, exprOf(old, "bore-diameter"), 0.01, 1e5, " mm");
  SizeField* boreDepth = dlg.size(tr("Counterbore depth:"), h.boreDepth, exprOf(old, "bore-depth"), 0.01, 1e5, " mm");
  SizeField* sink = dlg.size(tr("Countersink diameter:"), h.sinkDiameter, exprOf(old, "sink-diameter"), 0.01, 1e5, " mm");
  SizeField* sinkAngle = dlg.size(tr("Countersink angle:"), h.sinkAngle, exprOf(old, "sink-angle"), 1, 179, QString::fromUtf8(" °"));
  QComboBox* direction = dlg.choice(tr("Direction:"), {tr("Into the face"), tr("Opposite")}, h.flip ? 1 : 0);
  auto sync = [&] {
    bore->setEnabled(kind->currentIndex() == 1);
    boreDepth->setEnabled(kind->currentIndex() == 1);
    sink->setEnabled(kind->currentIndex() == 2);
    sinkAngle->setEnabled(kind->currentIndex() == 2);
  };
  connect(kind, &QComboBox::currentIndexChanged, &dlg, sync);
  connect(standard, &QComboBox::currentIndexChanged, &dlg, [&](int i) {
    if (i <= 0) return;
    const Size& s = sizes[i - 1];
    diameter->setValue(s.hole);
    bore->setValue(s.bore);
    boreDepth->setValue(s.boreDepth);
    sink->setValue(s.sink);
    sinkAngle->setValue(90);
  });
  if (!old) emit standard->currentIndexChanged(standard->currentIndex()); // fill in M3
  sync();
  if (!dlg.run()) return;

  auto f = std::make_unique<HoleFeature>();
  f->sketch = sketch;
  f->kind = HoleFeature::Kind(kind->currentIndex());
  f->flip = direction->currentIndex() == 1;
  const std::pair<const char*, SizeField*> fields[] = {{"diameter", diameter}, {"depth", depth},
                                                       {"bore-diameter", bore}, {"bore-depth", boreDepth},
                                                       {"sink-diameter", sink}, {"sink-angle", sinkAngle}};
  for (const auto& [name, field] : fields) {
    f->setValue(name, field->value());
    setExpr(*f, name, field);
  }
  if (old) replaceFeature(editIndex, std::move(f));
  else addFeature(std::move(f));
}

void MainWindow::mirror(int editIndex) {
  const auto* old = editIndex >= 0 ? m_doc.as<MirrorFeature>(editIndex) : nullptr;
  if (!old && m_doc.body().IsNull()) {
    hint(tr("Mirror: make a body first."));
    return;
  }
  const std::vector<int> faces = old ? std::vector<int>{} : selectedFaces();
  QStringList planes{tr("YZ plane (left / right)"), tr("XZ plane (front / back)"), tr("XY plane (top / bottom)")};
  const MirrorFeature::Plane order[] = {MirrorFeature::Plane::YZ, MirrorFeature::Plane::XZ, MirrorFeature::Plane::XY,
                                        MirrorFeature::Plane::Face};
  const bool canFace = (old && old->plane == MirrorFeature::Plane::Face) || faces.size() == 1;
  if (canFace) planes << tr("The selected face");
  int current = 0;
  if (old) current = int(std::find(std::begin(order), std::end(order), old->plane) - std::begin(order));
  else if (canFace) current = 3;

  FormDialog dlg(this, tr("Mirror"), m_doc.values());
  dlg.text({}, tr("Mirrors the whole part and joins the copy.\nTo mirror across a face of the part, click it first."));
  QComboBox* plane = dlg.choice(tr("Mirror across:"), planes, current);
  if (!dlg.run()) return;

  auto f = std::make_unique<MirrorFeature>();
  f->plane = order[plane->currentIndex()];
  if (f->plane == MirrorFeature::Plane::Face) {
    f->face = old ? old->face : faces[0];
    f->key = old ? old->key : keysOf(faces, TopAbs_FACE)[0];
  }
  if (old) replaceFeature(editIndex, std::move(f));
  else addFeature(std::move(f));
}

// Name / formula table; the values are computed as you type.
void MainWindow::editParameters() {
  QDialog dlg(this);
  dlg.setWindowTitle(tr("Parameters"));
  dlg.resize(560, 420);
  auto* layout = new QVBoxLayout(&dlg);
  auto* intro = new QLabel(tr("Name sizes once and use them anywhere a size is asked for: <i>width / 2 + 5</i>.<br>"
                              "Changing a value here rebuilds every step that uses it."),
                           &dlg);
  intro->setWordWrap(true);
  layout->addWidget(intro);
  auto* table = new QTableWidget(0, 3, &dlg);
  table->setHorizontalHeaderLabels({tr("Name"), tr("Value or formula"), tr("Result")});
  table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  table->verticalHeader()->hide();
  layout->addWidget(table);
  auto* status = new QLabel(&dlg);
  status->setWordWrap(true);
  layout->addWidget(status);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  QPushButton* addButton = buttons->addButton(tr("Add"), QDialogButtonBox::ActionRole);
  QPushButton* removeButton = buttons->addButton(tr("Remove"), QDialogButtonBox::ActionRole);
  layout->addWidget(buttons);

  auto read = [table] {
    std::vector<Parameter> params;
    for (int r = 0; r < table->rowCount(); ++r) {
      const QString name = table->item(r, 0) ? table->item(r, 0)->text().trimmed() : QString();
      const QString expr = table->item(r, 1) ? table->item(r, 1)->text().trimmed() : QString();
      if (!name.isEmpty() || !expr.isEmpty()) params.push_back({name.toStdString(), expr.toStdString()});
    }
    return params;
  };
  bool filling = false;
  auto refresh = [&] { // results column and the error line
    filling = true;
    std::string error;
    Variables values;
    const std::vector<Parameter> params = read();
    try {
      values = model::evaluateParameters(params);
    } catch (const std::exception& e) {
      error = e.what();
      for (size_t pass = 0; pass < params.size(); ++pass) // still show the ones that work
        for (const Parameter& p : params)
          if (!values.count(p.name)) {
            try {
              values[p.name] = model::evaluate(p.expression, values);
            } catch (const std::exception&) {
              // stays "?"
            }
          }
    }
    for (int r = 0; r < table->rowCount(); ++r) {
      const QString name = table->item(r, 0) ? table->item(r, 0)->text().trimmed() : QString();
      const auto it = values.find(name.toStdString());
      auto* result = new QTableWidgetItem(it == values.end() ? QString("?") : num(it->second));
      result->setFlags(Qt::ItemIsEnabled);
      table->setItem(r, 2, result);
    }
    status->setText(error.empty() ? QString() : QString::fromStdString(error));
    status->setStyleSheet("color: #c8281e");
    filling = false;
  };
  auto addRow = [&](const QString& name, const QString& expr) {
    const int r = table->rowCount();
    table->insertRow(r);
    table->setItem(r, 0, new QTableWidgetItem(name));
    table->setItem(r, 1, new QTableWidgetItem(expr));
  };
  for (const Parameter& p : m_doc.parameters())
    addRow(QString::fromStdString(p.name), QString::fromStdString(p.expression));
  refresh();
  connect(table, &QTableWidget::itemChanged, &dlg, [&] {
    if (!filling) refresh();
  });
  connect(addButton, &QPushButton::clicked, &dlg, [&] {
    int n = int(table->rowCount()) + 1; // a free default name
    auto taken = [&](const QString& name) {
      for (int r = 0; r < table->rowCount(); ++r)
        if (table->item(r, 0) && table->item(r, 0)->text() == name) return true;
      return false;
    };
    while (taken(QString("p%1").arg(n))) ++n;
    filling = true;
    addRow(QString("p%1").arg(n), "10");
    filling = false;
    refresh();
    table->setCurrentCell(table->rowCount() - 1, 0);
    table->editItem(table->item(table->rowCount() - 1, 0));
  });
  connect(removeButton, &QPushButton::clicked, &dlg, [&] {
    if (table->currentRow() >= 0) table->removeRow(table->currentRow());
    refresh();
  });
  connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  connect(buttons, &QDialogButtonBox::accepted, &dlg, [&] {
    try {
      model::evaluateParameters(read());
    } catch (const std::exception& e) {
      QMessageBox::warning(&dlg, tr("Parameters"), QString::fromStdString(e.what()));
      return;
    }
    dlg.accept();
  });
  if (dlg.exec() != QDialog::Accepted) return;

  std::vector<Parameter> params = read();
  const auto& now = m_doc.parameters();
  if (params.size() == now.size() &&
      std::equal(params.begin(), params.end(), now.begin(),
                 [](const Parameter& a, const Parameter& b) { return a.name == b.name && a.expression == b.expression; }))
    return;
  pushUndo();
  m_doc.setParameters(std::move(params));
  documentChanged();
  if (m_doc.hasErrors()) hint(tr("Some steps no longer work with these parameters, see the red marks in the timeline."));
  else hint(tr("Parameters updated."));
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
    m_editor->setVariables(m_doc.values());
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
  } else if (m_doc.as<HoleFeature>(index)) {
    hole(index);
  } else if (m_doc.as<MirrorFeature>(index)) {
    mirror(index);
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
  clearAutosave();
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
  if (!path.isEmpty() && openProject(path)) addRecent(path);
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
  clearAutosave();
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
  clearAutosave();
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
  addRecent(path);
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
  const QStringList filters = {tr("STEP (*.step *.stp)"),     tr("IGES (*.iges *.igs)"),
                               tr("STL mesh (*.stl)"),         tr("OBJ mesh (*.obj)"),
                               tr("3MF for 3D printing (*.3mf)"), tr("OpenCASCADE BREP (*.brep)")};
  const char* defaultExt[] = {"step", "iges", "stl", "obj", "3mf", "brep"};
  const QString base = m_path.isEmpty() ? tr("part") : QFileInfo(m_path).completeBaseName();
  QString filter = filters[0];
  QString path = QFileDialog::getSaveFileName(this, tr("Export"), base + ".step", filters.join(";;"), &filter);
  if (path.isEmpty()) return;
  if (QFileInfo(path).suffix().isEmpty()) path += QString(".") + defaultExt[std::max<qsizetype>(0, filters.indexOf(filter))];
  try {
    io::MeshQuality quality = io::MeshQuality::Normal;
    if (io::isMesh(io::formatOf(path.toStdString()))) { // meshes: how smooth should curves be?
      QSettings settings;
      FormDialog dlg(this, tr("Mesh Quality"));
      dlg.text({}, tr("Curved faces are cut into flat triangles.\nFiner looks smoother but makes bigger files."));
      QComboBox* q = dlg.choice(tr("Quality:"), {tr("Coarse (small file)"), tr("Normal"), tr("Fine (smooth curves)")},
                                std::clamp(settings.value("meshQuality", 1).toInt(), 0, 2));
      if (!dlg.run()) return;
      settings.setValue("meshQuality", q->currentIndex());
      quality = io::MeshQuality(q->currentIndex());
    }
    io::exportShape(m_doc.body(), path.toStdString(), quality);
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

void MainWindow::addRecent(const QString& path) {
  QSettings settings;
  QStringList recent = settings.value("recentFiles").toStringList();
  const QString full = QFileInfo(path).absoluteFilePath();
  recent.removeAll(full);
  recent.prepend(full);
  while (recent.size() > 10) recent.removeLast();
  settings.setValue("recentFiles", recent);
}

void MainWindow::fillRecentMenu() {
  m_recentMenu->clear();
  const QStringList recent = QSettings().value("recentFiles").toStringList();
  for (const QString& path : recent) {
    const QFileInfo info(path);
    QAction* a = m_recentMenu->addAction(
        QString("%1   (%2)").arg(info.fileName(), QDir::toNativeSeparators(info.absolutePath())));
    a->setEnabled(info.exists());
    connect(a, &QAction::triggered, this, [this, path] {
      if (confirmDiscard() && openProject(path)) addRecent(path);
    });
  }
  if (recent.isEmpty()) {
    m_recentMenu->addAction(tr("No recent projects"))->setEnabled(false);
    return;
  }
  m_recentMenu->addSeparator();
  m_recentMenu->addAction(tr("Clear List"), this, [] { QSettings().remove("recentFiles"); });
}

void MainWindow::autosave() {
  if (!isWindowModified() || m_doc.size() == 0) return;
  try {
    project::save(m_doc, m_autosavePath);
    QSettings().setValue("autosave/" + QFileInfo(m_autosavePath).completeBaseName(), m_path);
  } catch (const std::exception&) {
    // A failed backup must not interrupt the work; the next minute tries again.
  }
}

void MainWindow::clearAutosave() {
  QFile::remove(m_autosavePath);
  QSettings().remove("autosave/" + QFileInfo(m_autosavePath).completeBaseName());
}

void MainWindow::recoverAutosave() {
  const QDir dir = QFileInfo(m_autosavePath).absoluteDir();
  for (const QFileInfo& backup : dir.entryInfoList({QString("*.") + project::kExtension}, QDir::Files, QDir::Time)) {
    if (backup.absoluteFilePath() == m_autosavePath) continue;
    QLockFile lock(dir.filePath(backup.completeBaseName() + ".lock"));
    if (!lock.tryLock(0)) continue; // another OpenCaliper window is still using it
    const QString key = "autosave/" + backup.completeBaseName();
    const QString original = QSettings().value(key).toString();
    const QString name = original.isEmpty() ? tr("an untitled project") : QFileInfo(original).fileName();
    const auto answer = QMessageBox::question(
        this, tr("Recover Unsaved Work"),
        tr("OpenCaliper was closed without saving %1.\nA backup from %2 was found. Recover it?")
            .arg(name, QLocale().toString(backup.lastModified(), QLocale::ShortFormat)),
        QMessageBox::Yes | QMessageBox::No);
    if (answer == QMessageBox::Yes) {
      if (!openProject(backup.absoluteFilePath())) return; // keep the backup when it cannot be read
      setProjectPath(original);
      setWindowModified(true);
      autosave(); // now this window's backup
      hint(tr("Recovered. Save the project to keep it."));
    }
    QFile::remove(backup.absoluteFilePath());
    QSettings().remove(key);
    if (answer == QMessageBox::Yes) return;
  }
}

void MainWindow::closeEvent(QCloseEvent* e) {
  if (m_editor->isActive()) finishSketch();
  if (confirmDiscard()) {
    clearAutosave();
    e->accept();
  } else {
    e->ignore();
  }
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
  FormDialog dlg(this, tr("Change Dimension"), m_doc.values());
  SizeField* value = dlg.size(label, ref.value, ref.expr, 0.001, 1e6, " mm");
  if (!dlg.run()) return;

  std::string error;
  std::unique_ptr<Feature> changed = applyDimension(m_doc, ref, value->value(), error, value->expression());
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
  if (const auto* h = m_doc.as<HoleFeature>(i)) {
    int holes = 0;
    if (const auto* s = m_doc.as<SketchFeature>(size_t(h->sketch)))
      for (const sketch::Entity& e : s->sketch.entities) holes += e.kind == sketch::EntityKind::Circle && !e.construction;
    const QString kind = h->kind == HoleFeature::Kind::Counterbore ? tr("Counterbored hole")
                         : h->kind == HoleFeature::Kind::Countersink ? tr("Countersunk hole")
                                                                     : tr("Hole");
    const QString depth = h->depth > 0 ? tr("%1 mm deep").arg(num(h->depth)) : tr("through");
    return tr("%1 Ø%2 %3, %n hole(s)", "", holes).arg(kind, num(h->diameter), depth);
  }
  if (const auto* m = m_doc.as<MirrorFeature>(i)) {
    const QString names[] = {"XY", "XZ", "YZ", tr("a face")};
    return tr("Mirror across %1").arg(names[int(m->plane)]);
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
    hint(tr("Sketching: L line, R rectangle, C circle, A arc, G polygon, K slot, W trim, N fillet, O offset, "
            "U mirror, X construction, S select. Esc stops, Delete removes, Ctrl+Z undoes. Ctrl+Enter finishes."));
  else if (m_doc.size() == 0)
    hint(tr("Start with a sketch: pick a plane in the Sketch toolbar."));
}

void MainWindow::hint(const QString& text) {
  m_hint->setText(text);
}

// --- Claude (MCP) -------------------------------------------------------------

#ifdef OPENCALIPER_MCP

// Tool calls from Claude, on this window's document: every change is one undo
// step and shows up at once. Never opens dialogs (nobody would answer them).
class MainWindow::McpHost : public mcp::Host {
public:
  explicit McpHost(MainWindow& w) : w(w) {}

  const Document& document() const override { return w.m_doc; }
  bool live() const override { return true; }

  QString apply(Document doc, const QString& what) override {
    if (const QString why = busy(); !why.isEmpty()) return why;
    const bool firstBody = w.m_doc.body().IsNull();
    w.pushUndo();
    w.m_doc = std::move(doc);
    w.documentChanged();
    if (firstBody && !w.m_doc.body().IsNull()) w.m_viewer->fitAll();
    w.hint(tr("Claude: %1. Ctrl+Z undoes it.").arg(what));
    return {};
  }
  QString undo() override {
    if (const QString why = busy(); !why.isEmpty()) return why;
    if (w.m_undo.empty()) return "Nothing to undo.";
    w.undo();
    return {};
  }
  QString redo() override {
    if (const QString why = busy(); !why.isEmpty()) return why;
    if (w.m_redo.empty()) return "Nothing to redo.";
    w.redo();
    return {};
  }
  QString projectPath() const override { return w.m_path; }
  QString newProject() override {
    if (const QString why = busyOrUnsaved(); !why.isEmpty()) return why;
    w.newProject();
    return {};
  }
  QString openProject(const QString& path) override {
    if (const QString why = busyOrUnsaved(); !why.isEmpty()) return why;
    try {
      project::load(path); // its errors here, not in a message box
    } catch (const std::exception& e) {
      return QString::fromStdString(e.what());
    }
    if (!w.openProject(path)) return "Could not open the project.";
    w.addRecent(path);
    return {};
  }
  QString saveProject(const QString& path) override {
    const QString target = path.isEmpty() ? w.m_path : path;
    if (target.isEmpty()) return "The project has no file yet: give a path.";
    if (!QDir::isAbsolutePath(target)) return "path: give a full path, such as /home/me/box.ocal";
    try {
      project::save(w.m_doc, target);
    } catch (const std::exception& e) {
      return QString::fromStdString(e.what());
    }
    if (target != w.m_path) w.addRecent(target);
    w.setProjectPath(target);
    w.clearAutosave();
    w.hint(tr("Claude saved %1").arg(QFileInfo(target).fileName()));
    return {};
  }
  QString screenshot(const QString& name, QByteArray& png) override {
    const std::pair<const char*, V3d_TypeOfOrientation> views[] = {
        {"iso", V3d_XposYnegZpos}, {"front", V3d_Yneg}, {"back", V3d_Ypos}, {"top", V3d_Zpos},
        {"bottom", V3d_Zneg},      {"left", V3d_Xneg},  {"right", V3d_Xpos}};
    const Handle(V3d_View)& view = w.m_viewer->view();
    if (view.IsNull()) return "The 3D view is not ready yet.";
    // Look from the asked side, take the picture, then put the user's camera back.
    Handle(Graphic3d_Camera) saved = new Graphic3d_Camera();
    saved->Copy(view->Camera());
    if (name != "current") {
      const auto it = std::find_if(std::begin(views), std::end(views), [&](const auto& v) { return name == v.first; });
      if (it == std::end(views)) return "view: iso, front, back, top, bottom, left, right or current";
      view->SetProj(it->second);
      view->FitAll(0.05, false);
      view->ZFitAll();
    }
    QImage image = w.m_viewer->grabFramebuffer();
    view->Camera()->Copy(saved);
    view->Invalidate();
    w.m_viewer->redraw();
    if (image.isNull()) return "Could not read the 3D view.";
    if (image.width() > 1280) image = image.scaledToWidth(1280, Qt::SmoothTransformation);
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return {};
  }

private:
  static QString tr(const char* text) { return MainWindow::tr(text); }
  QString busy() const {
    if (w.m_editor->isActive())
      return "The user is editing a sketch in OpenCaliper. Ask them to finish it (Ctrl+Enter) first.";
    return {};
  }
  QString busyOrUnsaved() const {
    if (const QString why = busy(); !why.isEmpty()) return why;
    if (w.isWindowModified())
      return "The open project has unsaved changes. Save it with save_project, or ask the user what to do.";
    return {};
  }

  MainWindow& w;
};

void MainWindow::buildToolsMenu() {
  QMenu* m = menuBar()->addMenu(tr("&Tools"));
  m->addAction(tr("&Claude (MCP)..."), this, &MainWindow::showClaudeDialog);
  m_mcpHost = std::make_unique<McpHost>(*this);
  m_mcpListener = new mcp::Listener(*m_mcpHost, this);
  m_mcpStatus = new QLabel(this);
  statusBar()->addPermanentWidget(m_mcpStatus);
  connect(m_mcpListener, &mcp::Listener::stateChanged, this, &MainWindow::updateClaudeStatus);
  m_mcpListener->setEnabled(mcp::enabled());
  updateClaudeStatus();
}

void MainWindow::updateClaudeStatus() {
  m_mcpStatus->setVisible(m_mcpListener->listening());
  m_mcpStatus->setText(m_mcpListener->connected() ? tr("Claude: connected") : tr("Claude: on"));
  m_mcpStatus->setStyleSheet(m_mcpListener->connected() ? "color: #2e7d32; font-weight: bold" : "");
}

void MainWindow::showClaudeDialog() {
  QDialog dlg(this);
  dlg.setWindowTitle(tr("Claude (MCP)"));
  dlg.setMinimumWidth(560);
  auto* layout = new QVBoxLayout(&dlg);
  auto* intro = new QLabel(tr("Claude, Anthropic's AI assistant, can read and change the open model through MCP: "
                              "\"make the walls 3 mm and put M3 holes in the corners\". Every change is a normal step "
                              "in the timeline, and Ctrl+Z undoes it."),
                           &dlg);
  intro->setWordWrap(true);
  auto* on = new QCheckBox(tr("Allow Claude to work on the open model"), &dlg);
  on->setChecked(mcp::enabled());
  auto* state = new QLabel(&dlg);
  state->setWordWrap(true);
  auto* howto = new QLabel(tr("<b>Connect Claude Code</b>: run this once in a terminal:"), &dlg);
  auto* command = new QLineEdit("claude mcp add opencaliper -- " + mcp::serverCommand(), &dlg);
  command->setReadOnly(true);
  command->setCursorPosition(0);
  auto* copy = new QPushButton(tr("Copy"), &dlg);
  connect(copy, &QPushButton::clicked, &dlg, [command] { QGuiApplication::clipboard()->setText(command->text()); });
  auto* row = new QHBoxLayout;
  row->addWidget(command, 1);
  row->addWidget(copy);
  auto* note = new QLabel(tr("Then ask Claude for a part, for example: \"In OpenCaliper, make a 60 x 40 x 20 mm box "
                             "with 2 mm walls and an open top.\" When no OpenCaliper window is open, Claude works on "
                             "project files instead."),
                          &dlg);
  note->setWordWrap(true);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
  connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  for (QWidget* widget : std::initializer_list<QWidget*>{intro, on, state, howto}) layout->addWidget(widget);
  layout->addLayout(row);
  layout->addWidget(note);
  layout->addWidget(buttons);

  auto update = [this, state] {
    if (!m_mcpListener->problem().isEmpty()) state->setText(m_mcpListener->problem());
    else if (m_mcpListener->connected()) state->setText(tr("Claude is connected to this window."));
    else if (m_mcpListener->listening()) state->setText(tr("On. Claude connects with its first request."));
    else state->setText(tr("Off: Claude cannot read or change anything."));
  };
  connect(on, &QCheckBox::toggled, &dlg, [this](bool enable) {
    mcp::setEnabled(enable);
    m_mcpListener->setEnabled(enable);
  });
  connect(m_mcpListener, &mcp::Listener::stateChanged, &dlg, update);
  update();
  dlg.exec();
}

MainWindow::~MainWindow() {
  delete m_mcpListener; // it uses m_mcpHost
}

#else

MainWindow::~MainWindow() = default;

#endif
