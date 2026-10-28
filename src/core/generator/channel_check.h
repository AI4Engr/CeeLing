#pragma once

#include "../cst_to_ast/ast.h"

namespace cling {

// Validates every use of the built-in `Channel<T>` type before any other pass sees it:
//   - T must be a plain value (primitive, string, std::vector of those, or a struct made only of those): the same values a `spawn`
//     may capture, because a channel is how threads hand each other COPIES of such values;
//   - a Channel may not be nested inside another generic (std::vector<Channel<int>>, std::map, Channel<Channel<int>>);
//   - a Channel may not be a `ref` observer and may not cross an `ext_fn` signature.
// Throws std::runtime_error with the compiler's "error[semantic/...]" text on the first violation. Run after desugarStructJson and
// before desugarClosures (closure parameters are still ClosureExpression nodes, and are checked too).
void checkChannels(Program& program);

// True if some declaration, parameter or return type in the program (included files' declarations too) is a Channel. The emitter
// uses it to put the runtime template (library_summary.h: channelHelperDefinition) into the generated file or header.
bool programUsesChannel(const Program& program);

}  // namespace cling
