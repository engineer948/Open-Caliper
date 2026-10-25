#pragma once

#include <QIcon>
#include <QString>

// Toolbar icons drawn in code (no image files, no downloads), in the
// palette's text color so they work with light and dark themes.
QIcon makeIcon(const QString& name);
