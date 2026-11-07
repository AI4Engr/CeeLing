#pragma once

#include "syntax/ast.h"

namespace cling {

// Validates every use of the built-in `Channel<T>` type:
//   - T must be a plain value, the same kind a `spawn` may capture;
//   - a Channel may not be nested in another generic (vector, map, another Channel);
//   - a Channel may not be a `ref` observer or appear in an `ext_fn` signature.
// Throws std::runtime_error with the usual "error[semantic/...]" text on the first violation.
// Runs after desugarStructJson and before desugarClosures (closure parameters are checked too).
void checkChannels(Program& program);

// True if some declaration, parameter or return type is a Channel; the emitter then adds the
// runtime template (src/emit/runtime_helpers) to the generated file or header.
bool programUsesChannel(const Program& program);

// Same for `span<T>`: the emitter then adds cling::span.
bool programUsesSpan(const Program& program);

}  // namespace cling
