#include "channel_check.h"

#include <sstream>
#include <stdexcept>

#include "ast_walk.h"
#include "closure_desugar.h"
#include "library_summary.h"

namespace cling {

namespace {

using TypeFn = std::function<void(const std::string& type, const ASTNode& at, bool observer, bool inExternFn)>;

// Calls fn for every type spelled in a declaration: fields, locals, parameters, return types, loop variables.
void forEachType(Program& program, const TypeFn& fn) {
    for (auto& stmt : program.statements) {
        astwalk::walkStatement(stmt.get(), [&](ASTNode& node) {
            if (auto v = dynamic_cast<VariableDeclaration*>(&node)) {
                fn(v->type, node, v->isObserver, false);
            } else if (auto c = dynamic_cast<ContainerDeclaration*>(&node)) {
                fn(c->type, node, false, false);
            } else if (auto f = dynamic_cast<FunctionDeclaration*>(&node)) {
                fn(f->returnType, node, false, false);
                for (auto& p : f->parameters) fn(p.first, node, false, false);
            } else if (auto m = dynamic_cast<MethodDeclaration*>(&node)) {
                fn(m->returnType, node, false, false);
                for (auto& p : m->parameters) fn(p.first, node, false, false);
            } else if (auto k = dynamic_cast<ConstructorDeclaration*>(&node)) {
                for (auto& p : k->parameters) fn(p.first, node, false, false);
            } else if (auto cl = dynamic_cast<ClosureExpression*>(&node)) {
                for (auto& p : cl->parameters) fn(p.first, node, false, false);
            } else if (auto fo = dynamic_cast<ForStatement*>(&node)) {
                fn(fo->loopVarType, node, false, false);
            } else if (auto e = dynamic_cast<ExternFunctionDeclaration*>(&node)) {
                fn(e->returnType, node, false, true);
                for (auto& p : e->parameters) fn(p.first, node, false, true);
            }
        });
    }
}

[[noreturn]] void reject(const std::string& msg, const std::string& help, const ASTNode& at) {
    std::ostringstream oss;
    oss << "error[semantic/forbidden-construct]: " << msg;
    if (at.loc.line > 0) oss << " (line " << at.loc.line << ", column " << at.loc.column << ")";
    oss << ". " << help;
    throw std::runtime_error(oss.str());
}

bool mentionsChannel(const std::string& type) {
    return type.find("Channel<") != std::string::npos;
}

}  // namespace

void checkChannels(Program& program) {
    forEachType(program, [&](const std::string& type, const ASTNode& at, bool observer, bool inExternFn) {
        if (!mentionsChannel(type)) return;
        if (inExternFn) {
            reject("`" + type + "` appears in an ext_fn signature",
                   "ext_fn parameters and results are primitives and vectors of primitives; a channel cannot cross that boundary", at);
        }
        if (!isChannelType(type)) {
            reject("`" + type + "` holds a Channel inside another type",
                   "a Channel is used as itself (a local, parameter, field or result of type `Channel<T>`); it cannot be an element of a "
                   "std::vector or std::map, or of another Channel", at);
        }
        if (observer) {
            reject("`ref` cannot be applied to a Channel",
                   "a Channel is already a handle: copy it (`Channel<int> d = c;`) instead of observing it", at);
        }
        const std::string element = channelElementType(type);
        if (mentionsChannel(element)) {
            reject("`" + type + "` is a Channel of Channels", "a Channel carries plain values only", at);
        }
        if (!isPlainValueType(element, program)) {
            reject("`" + type + "` would carry `" + element + "`, which is not a plain value",
                   "a Channel carries what a `spawn` may capture: primitives, string, std::vector of those, and `struct`s made only of "
                   "those. They are copied through the queue, so no object is shared between threads. Send a `struct`, or serialize the "
                   "object to a string first", at);
        }
    });
}

bool programUsesChannel(const Program& program) {
    bool found = false;
    forEachType(const_cast<Program&>(program), [&](const std::string& type, const ASTNode&, bool, bool) {
        if (mentionsChannel(type)) found = true;
    });
    return found;
}

bool programUsesSpan(const Program& program) {
    bool found = false;
    forEachType(const_cast<Program&>(program), [&](const std::string& type, const ASTNode&, bool, bool) {
        if (type.find("span<") != std::string::npos) found = true;
    });
    return found;
}

}  // namespace cling
