// SPDX-License-Identifier: MIT
#ifndef CANFORGE_DBC_DIAGNOSTIC_HPP
#define CANFORGE_DBC_DIAGNOSTIC_HPP

/// The diagnostic machinery lives in the text layer now that the simulator
/// needs it too; these aliases keep the dbc API unchanged.

#include "canforge/text/Diagnostic.hpp"

namespace canforge::dbc {
using text::Diagnostic;
using text::DiagnosticSink;
using text::Severity;
using text::SourceLocation;
}  // namespace canforge::dbc

#endif  // CANFORGE_DBC_DIAGNOSTIC_HPP
