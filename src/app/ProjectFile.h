#pragma once

#include "model/Document.h"

#include <QString>

// Project files (.ocal): the feature timeline as JSON. Bodies are not stored,
// they are rebuilt on load. Imported shapes are embedded as BREP text.
namespace project {

inline constexpr const char* kExtension = "ocal";

// Throw std::runtime_error with a user-facing message.
void save(const model::Document& doc, const QString& path);
model::Document load(const QString& path);

} // namespace project
