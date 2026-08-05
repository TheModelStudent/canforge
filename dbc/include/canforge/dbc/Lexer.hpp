// SPDX-License-Identifier: MIT
#ifndef CANFORGE_DBC_LEXER_HPP
#define CANFORGE_DBC_LEXER_HPP

/// The lexer lives in the text layer now that the simulator config parser
/// needs the same scanner; these aliases keep the dbc API unchanged.

#include "canforge/dbc/Diagnostic.hpp"
#include "canforge/text/Lexer.hpp"

namespace canforge::dbc {
using text::Lexer;
using text::Source;
using text::Token;
using text::TokenKind;
using text::to_string;
}  // namespace canforge::dbc

#endif  // CANFORGE_DBC_LEXER_HPP
