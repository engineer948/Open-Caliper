#include "app/LicensesDialog.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QListWidget>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace {

QString readResource(const QString& path) {
  QFile f(path);
  return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

} // namespace

void showLicenses(QWidget* parent) {
  struct Entry {
    QString title, path;
  };
  const Entry entries[] = {
      {QObject::tr("OpenCaliper (MIT)"), ":/licenses/LICENSE"},
      {QObject::tr("Third-party notices"), ":/licenses/THIRD-PARTY-NOTICES.md"},
      {QObject::tr("Open CASCADE: LGPL 2.1"), ":/licenses/LICENSES/LGPL-2.1-only.txt"},
      {QObject::tr("Open CASCADE: exception"), ":/licenses/LICENSES/OCCT-exception-1.0.txt"},
      {QObject::tr("Qt: LGPL 3.0"), ":/licenses/LICENSES/LGPL-3.0-only.txt"},
      {QObject::tr("GPL 3.0 (part of LGPL 3.0)"), ":/licenses/LICENSES/GPL-3.0-only.txt"},
      {QObject::tr("FreeType License"), ":/licenses/LICENSES/FTL.txt"},
  };

  QDialog dlg(parent);
  dlg.setWindowTitle(QObject::tr("Licenses"));
  dlg.resize(900, 600);
  auto* list = new QListWidget(&dlg);
  list->setMaximumWidth(240);
  auto* text = new QTextBrowser(&dlg);
  text->setOpenExternalLinks(true);
  for (const Entry& e : entries) list->addItem(e.title);

  QObject::connect(list, &QListWidget::currentRowChanged, &dlg, [&](int row) {
    if (row < 0) return;
    const QString content = readResource(entries[row].path);
    if (entries[row].path.endsWith(".md")) {
      text->setFont(QFont());
      text->setMarkdown(content);
    } else {
      text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
      text->setPlainText(content);
    }
  });
  list->setCurrentRow(0);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  auto* row = new QHBoxLayout;
  row->addWidget(list);
  row->addWidget(text, 1);
  auto* layout = new QVBoxLayout(&dlg);
  layout->addLayout(row);
  layout->addWidget(buttons);
  dlg.exec();
}
