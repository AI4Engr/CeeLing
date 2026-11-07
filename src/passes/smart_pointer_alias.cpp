// SmartPointerAnalyzer: container element kinds, alias-returning functions and aliased locals
#include "passes/smart_pointer_analyzer.h"
#include "passes/smart_pointer_util.h"
#include "syntax/ast_walk.h"

#include "syntax/class_hierarchy.h"
#include "facts/library_summary.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>


namespace cling {

void SmartPointerAnalyzer::scanContainerElementKinds(const Program& program) {
    pushedArgs_.clear();
    scanContainerElementKindsImpl(program);
    for (const std::string& name : demandedElements_) {
        // only a container of objects this program declares (containers are told apart by name alone)
        auto type = containerFieldTypes_.find(name);
        if (type != containerFieldTypes_.end() && isVectorType(type->second) && classNames.count(vectorElementType(type->second)) > 0) {
            containerElementShared_[name] = true;
        }
    }
    // A value pushed into a container whose elements are shared must itself be shared; decided once every push and element copy has been seen.
    for (const auto& p : pushedArgs_) {
        auto it = containerElementShared_.find(p[3]);
        if (it == containerElementShared_.end() || !it->second) continue;
        auto dec = pointerDecisions.find(buildKey(p[0], p[1], p[2]));
        if (dec != pointerDecisions.end() && dec->second == PointerType::UNIQUE) {
            dec->second = PointerType::SHARED;
            pointerReasons[dec->first] = "pushed into a container whose elements are shared";
        }
    }
}

namespace {
void collectDeclaredNames(const std::vector<StatementPtr>& body, std::set<std::string>& names) {
    for (const auto& st : body) {
        if (auto d = dynamic_cast<const VariableDeclaration*>(st.get())) {
            names.insert(d->identifier);
        } else if (auto c = dynamic_cast<const ContainerDeclaration*>(st.get())) {
            names.insert(c->identifier);
        } else if (auto i = dynamic_cast<const IfStatement*>(st.get())) {
            collectDeclaredNames(i->body, names);
            collectDeclaredNames(i->elseBody, names);
        } else if (auto w = dynamic_cast<const WhileStatement*>(st.get())) {
            collectDeclaredNames(w->body, names);
        } else if (auto f = dynamic_cast<const ForStatement*>(st.get())) {
            names.insert(f->loopVarName);
            collectDeclaredNames(f->body, names);
        } else if (auto t = dynamic_cast<const TryStatement*>(st.get())) {
            names.insert(t->catchIdentifier);
            collectDeclaredNames(t->body, names);
            collectDeclaredNames(t->catchBody, names);
        }
    }
}
}  // namespace

void SmartPointerAnalyzer::scanContainerElementKindsImpl(const Program& program) {
    containerFieldTypes_.clear();
    for (const auto& stmt : program.statements) {
        if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            for (const auto& member : cls->members) {
                if (auto field = dynamic_cast<const ContainerDeclaration*>(member.get())) {
                    containerFieldTypes_[field->identifier] = field->type;
                } else if (auto plain = dynamic_cast<const VariableDeclaration*>(member.get())) {
                    if (isVectorType(plain->type)) containerFieldTypes_[plain->identifier] = plain->type;     // (a field `std::vector<Cmd> items;`)
                }
            }
        }
    }
    auto scanBody = [this](const std::vector<StatementPtr>& body,
                           const std::vector<std::pair<std::string, std::string>>& params, const std::string& scope) {
        scanLocals_.clear();
        collectDeclaredNames(body, scanLocals_);
        for (const auto& p : params) scanLocals_.insert(p.second);
        for (const auto& inner : body) scanContainerElementKindsInStatement(*inner, scope);
    };
    for (const auto& stmt : program.statements) {
        if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
            scanBody(fn->body, fn->parameters, fn->name);
        } else if (auto mn = dynamic_cast<const MainFunction*>(stmt.get())) {
            scanBody(mn->body, {}, "main");
        } else if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            for (const auto& member : cls->members) {
                if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                    scanBody(method->body, method->parameters, cls->name + "::" + method->name);
                } else if (auto ctor = dynamic_cast<const ConstructorDeclaration*>(member.get())) {
                    scanBody(ctor->body, ctor->parameters, cls->name + "::" + ctor->name);
                }
            }
        }
    }
    scanLocals_.clear();
}

namespace {
// The identifier named by `expr`, if it is a bare identifier (the shape `container.push_back(sourceVar)` needs).
const Identifier* asIdentifier(const Expression* expr) {
    return dynamic_cast<const Identifier*>(expr);
}
} // namespace

void SmartPointerAnalyzer::scanContainerElementKindsInStatement(const Statement& stmt,
                                                                  const std::string& scopeName) {
    // Find every `<container>.push_back(<arg>)`, wherever it appears.
    auto visitExpr = [this, &scopeName](const Expression* expr) {
        auto call = dynamic_cast<const FunctionCall*>(expr);
        if (call == nullptr || !call->callee) return;
        auto member = dynamic_cast<const MemberAccess*>(call->callee.get());
        if (member == nullptr || member->member != "push_back" || call->arguments.empty()) return;
        const Identifier* recvId = asIdentifier(member->object.get());
        const Identifier* arg = asIdentifier(call->arguments[0].get());
        // the container is a variable (`v.push_back(x)`) or a field reached through an object (`obj.items.push_back(x)`, keyed by the field's name)
        const MemberAccess* recvField = dynamic_cast<const MemberAccess*>(member->object.get());
        if ((recvId == nullptr && recvField == nullptr) || arg == nullptr) return;
        const std::string recvName = recvId != nullptr ? recvId->name : recvField->member;

        // The argument's already-decided pointer kind, looked up by its symbol type (precise scope first, then the global fallback).
        auto typeIt = symbolTypesByScope.find(makeSymbolKey(scopeName, arg->name));
        if (typeIt == symbolTypesByScope.end() && scanLocals_.count(arg->name) == 0) {
            typeIt = symbolTypesByScope.find(makeSymbolKey(kGlobalScope, arg->name));
        }
        if (typeIt == symbolTypesByScope.end()) return;
        // Only an object argument says anything about the element kind; the not-found fallback is shared, so a string or int would wrongly make a vector of shared objects.
        if (classNames.count(typeIt->second) == 0) return;
        pushedArgs_.push_back({scopeName, typeIt->second, arg->name, recvName});
        if (getPointerType(typeIt->second, arg->name) == PointerType::SHARED) {
            containerElementShared_[recvName] = true;
        } else if (containerElementShared_.find(recvName) == containerElementShared_.end()) {
            containerElementShared_[recvName] = false;
        }
    };

    if (auto exprStmt = dynamic_cast<const ExpressionStatement*>(&stmt)) {
        visitExpr(exprStmt->expression.get());
    } else if (auto decl = dynamic_cast<const VariableDeclaration*>(&stmt)) {
        if (decl->expression) visitExpr(decl->expression.get());
        // `Cmd c = items[i];`: two owners of one object, so the elements must be shared.
        if (!decl->isObserver && classNames.count(decl->type) > 0) {
            if (auto sub = dynamic_cast<const SubscriptExpression*>(decl->expression.get())) {
                if (auto cid = dynamic_cast<const Identifier*>(sub->object.get())) {
                    containerElementShared_[cid->name] = true;
                } else if (auto cmem = dynamic_cast<const MemberAccess*>(sub->object.get())) {
                    containerElementShared_[cmem->member] = true;   // `obj.items[i]`: the container is the field `items`
                }
            }
        }
    } else if (auto assign = dynamic_cast<const AssignmentStatement*>(&stmt)) {
        if (assign->expression) visitExpr(assign->expression.get());
        // `v[i] = v[j];` or `x = v[i];`: two owners of one object, so shared elements. The container is known to hold objects from its declared type, not from a push_back.
        if (auto from = dynamic_cast<const SubscriptExpression*>(assign->expression.get())) {
            auto nameOf = [](const Expression* object) -> std::string {
                if (auto id = dynamic_cast<const Identifier*>(object)) return id->name;
                if (auto field = dynamic_cast<const MemberAccess*>(object)) return field->member;
                return "";
            };
            auto holdsObjects = [this](const std::string& container) {
                auto type = containerFieldTypes_.find(container);
                return type != containerFieldTypes_.end() && isVectorType(type->second) && classNames.count(vectorElementType(type->second)) > 0;
            };
            const std::string source = nameOf(from->object.get());
            if (!source.empty() && holdsObjects(source)) containerElementShared_[source] = true;
            if (auto to = dynamic_cast<const SubscriptExpression*>(assign->target.get())) {
                const std::string destination = nameOf(to->object.get());
                if (!destination.empty() && holdsObjects(destination)) containerElementShared_[destination] = true;
            }
        }
    } else if (auto ifs = dynamic_cast<const IfStatement*>(&stmt)) {
        for (const auto& inner : ifs->body) scanContainerElementKindsInStatement(*inner, scopeName);
        for (const auto& inner : ifs->elseBody) scanContainerElementKindsInStatement(*inner, scopeName);
    } else if (auto whiles = dynamic_cast<const WhileStatement*>(&stmt)) {
        for (const auto& inner : whiles->body) scanContainerElementKindsInStatement(*inner, scopeName);
    } else if (auto fors = dynamic_cast<const ForStatement*>(&stmt)) {
        for (const auto& inner : fors->body) scanContainerElementKindsInStatement(*inner, scopeName);
    } else if (auto trys = dynamic_cast<const TryStatement*>(&stmt)) {
        for (const auto& inner : trys->body) scanContainerElementKindsInStatement(*inner, scopeName);
        for (const auto& inner : trys->catchBody) scanContainerElementKindsInStatement(*inner, scopeName);
    }
}

namespace {
// Every `return <expr>;` reachable from `body`, retested on each fixed-point round below.
void collectReturnExpressions(const std::vector<StatementPtr>& body,
                              std::vector<const Expression*>& out) {
    for (const auto& stmtPtr : body) {
        if (auto ret = dynamic_cast<const ReturnStatement*>(stmtPtr.get())) {
            out.push_back(ret->expression.get());
        } else if (auto ifs = dynamic_cast<const IfStatement*>(stmtPtr.get())) {
            collectReturnExpressions(ifs->body, out);
            collectReturnExpressions(ifs->elseBody, out);
        } else if (auto whiles = dynamic_cast<const WhileStatement*>(stmtPtr.get())) {
            collectReturnExpressions(whiles->body, out);
        } else if (auto fors = dynamic_cast<const ForStatement*>(stmtPtr.get())) {
            collectReturnExpressions(fors->body, out);
        } else if (auto trys = dynamic_cast<const TryStatement*>(stmtPtr.get())) {
            collectReturnExpressions(trys->body, out);
            collectReturnExpressions(trys->catchBody, out);
        }
    }
}

// True if `this` appears in `body` as a call argument, a declaration RHS or an assignment RHS, at any nesting depth: the three positions AstEmitter::emitMovedArg
// lowers to shared_from_this(). `return this;` is covered by the anyReturnsThis scan.
bool methodPassesThisAsArgument(const std::vector<StatementPtr>& body) {
    std::function<bool(const Expression*)> exprHasThisArg = [&](const Expression* expr) -> bool {
        if (expr == nullptr) return false;
        if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
            for (const auto& arg : call->arguments) {
                if (dynamic_cast<const ThisExpression*>(arg.get()) != nullptr) return true;
            }
            for (const auto& arg : call->arguments) {
                if (exprHasThisArg(arg.get())) return true;
            }
            return call->callee && exprHasThisArg(call->callee.get());
        }
        if (auto neww = dynamic_cast<const NewExpression*>(expr)) {
            for (const auto& arg : neww->arguments) {
                if (dynamic_cast<const ThisExpression*>(arg.get()) != nullptr) return true;
                if (exprHasThisArg(arg.get())) return true;
            }
            return false;
        }
        if (auto bin = dynamic_cast<const BinaryExpression*>(expr)) {
            return exprHasThisArg(bin->left.get()) || exprHasThisArg(bin->right.get());
        }
        if (auto un = dynamic_cast<const UnaryExpression*>(expr)) {
            return exprHasThisArg(un->operand.get());
        }
        if (auto member = dynamic_cast<const MemberAccess*>(expr)) {
            return exprHasThisArg(member->object.get());
        }
        if (auto sub = dynamic_cast<const SubscriptExpression*>(expr)) {
            return exprHasThisArg(sub->object.get()) || exprHasThisArg(sub->index.get());
        }
        return false;
    };

    std::function<bool(const std::vector<StatementPtr>&)> walkBody =
        [&](const std::vector<StatementPtr>& stmts) -> bool {
        for (const auto& stmtPtr : stmts) {
            const Statement* stmt = stmtPtr.get();
            if (auto decl = dynamic_cast<const VariableDeclaration*>(stmt)) {
                if (dynamic_cast<const ThisExpression*>(decl->expression.get()) != nullptr) return true;
                if (exprHasThisArg(decl->expression.get())) return true;
            } else if (auto assign = dynamic_cast<const AssignmentStatement*>(stmt)) {
                if (dynamic_cast<const ThisExpression*>(assign->expression.get()) != nullptr) return true;
                if (exprHasThisArg(assign->expression.get())) return true;
            } else if (auto exprStmt = dynamic_cast<const ExpressionStatement*>(stmt)) {
                if (exprHasThisArg(exprStmt->expression.get())) return true;
            } else if (auto ret = dynamic_cast<const ReturnStatement*>(stmt)) {
                // `return this;` is the anyReturnsThis scan's job; this only catches `this` nested in a larger returned expression.
                if (exprHasThisArg(ret->expression.get())) return true;
            } else if (auto ifs = dynamic_cast<const IfStatement*>(stmt)) {
                if (exprHasThisArg(ifs->condition.get())) return true;
                if (walkBody(ifs->body) || walkBody(ifs->elseBody)) return true;
            } else if (auto whiles = dynamic_cast<const WhileStatement*>(stmt)) {
                if (exprHasThisArg(whiles->condition.get())) return true;
                if (walkBody(whiles->body)) return true;
            } else if (auto fors = dynamic_cast<const ForStatement*>(stmt)) {
                if (exprHasThisArg(fors->container.get())) return true;
                if (walkBody(fors->body)) return true;
            } else if (auto trys = dynamic_cast<const TryStatement*>(stmt)) {
                if (walkBody(trys->body) || walkBody(trys->catchBody)) return true;
            }
        }
        return false;
    };

    return walkBody(body);
}
} // namespace

namespace {
// Resolves an expression inside `new SameClass(<expr>)` (case (d) of inferAliasReturningMethods) to the (declaredType, fieldName) it reads, crossing classes via
// classMembersByClass_. Handles a bare field/`this->field`, or `<chain>.<method>(...)` with the method already alias-returning on the chain's class. nullopt for
// anything else (fails closed).
std::optional<std::pair<std::string, std::string>> resolveInnerFieldChain(
    const Expression* expr,
    const std::string& enclosingClass,
    const std::map<std::string, std::map<std::string, std::string>>& classMembersByClass,
    const std::map<std::string, PointerType>& methodReturnKinds) {
    if (auto id = dynamic_cast<const Identifier*>(expr)) {
        auto clsIt = classMembersByClass.find(enclosingClass);
        if (clsIt == classMembersByClass.end()) return std::nullopt;
        auto fIt = clsIt->second.find(id->name);
        if (fIt == clsIt->second.end()) return std::nullopt;
        return std::make_pair(fIt->second, id->name);
    }
    if (auto member = dynamic_cast<const MemberAccess*>(expr)) {
        // `this->field` / `recv.field`: resolve the receiver's class (bare identifier receiver only), then look up the member in that class.
        std::string receiverClass;
        if (dynamic_cast<const ThisExpression*>(member->object.get()) != nullptr) {
            receiverClass = enclosingClass;
        } else if (auto recv = dynamic_cast<const Identifier*>(member->object.get())) {
            auto clsIt = classMembersByClass.find(enclosingClass);
            if (clsIt != classMembersByClass.end()) {
                if (auto fIt = clsIt->second.find(recv->name); fIt != clsIt->second.end()) {
                    receiverClass = fIt->second;
                }
            }
        } else if (auto innerMember = dynamic_cast<const MemberAccess*>(member->object.get())) {
            // `a.b.c`: resolve `a.b`'s class first.
            const auto inner = resolveInnerFieldChain(innerMember, enclosingClass, classMembersByClass,
                                                        methodReturnKinds);
            if (!inner.has_value()) return std::nullopt;
            receiverClass = inner->first;
        }
        if (receiverClass.empty()) return std::nullopt;
        auto clsIt = classMembersByClass.find(receiverClass);
        if (clsIt == classMembersByClass.end()) return std::nullopt;
        auto fIt = clsIt->second.find(member->member);
        if (fIt == clsIt->second.end()) return std::nullopt;
        return std::make_pair(fIt->second, member->member);
    }
    if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
        auto callMember = dynamic_cast<const MemberAccess*>(call->callee.get());
        if (callMember == nullptr) return std::nullopt;
        // Resolve the receiver chain's class; the method must already be alias-returning on it.
        std::string receiverClass;
        if (dynamic_cast<const ThisExpression*>(callMember->object.get()) != nullptr) {
            receiverClass = enclosingClass;
        } else if (auto recv = dynamic_cast<const Identifier*>(callMember->object.get())) {
            auto clsIt = classMembersByClass.find(enclosingClass);
            if (clsIt != classMembersByClass.end()) {
                if (auto fIt = clsIt->second.find(recv->name); fIt != clsIt->second.end()) {
                    receiverClass = fIt->second;
                }
            }
        } else {
            const auto inner = resolveInnerFieldChain(callMember->object.get(), enclosingClass,
                                                        classMembersByClass, methodReturnKinds);
            if (!inner.has_value()) return std::nullopt;
            receiverClass = inner->first;
        }
        if (receiverClass.empty()) return std::nullopt;
        const std::string key = makeMemberKey(receiverClass, callMember->member);
        auto kindIt = methodReturnKinds.find(key);
        if (kindIt == methodReturnKinds.end() || kindIt->second != PointerType::SHARED) return std::nullopt;
        // The result aliases the receiver's own resolved field, so recurse into the receiver.
        return resolveInnerFieldChain(callMember->object.get(), enclosingClass, classMembersByClass,
                                       methodReturnKinds);
    }
    return std::nullopt;
}
} // namespace

void SmartPointerAnalyzer::inferAliasReturningMethods(const Program& program) {
    // A method is alias-returning (returns shared_ptr<T>) iff every return in its body is one of:
    //   (a) `return this;`
    //   (b) `return <field>;` naming an object-typed field of the same class
    //   (c) `return <recv>.<method>(...);` where the method is already alias-returning
    //   (d) `return new SameClass(<inner>);` (see below)
    // Case (c) makes this a fixed point: leaf methods (a/b) are classified first. Classification only goes from undecided to decided, so it terminates.
    for (const auto& stmt : program.statements) {
        auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get());
        if (cls == nullptr) continue;

        // className.fieldName -> is this field object-typed (case (b) needs an object-typed field).
        std::map<std::string, std::string> fieldTypes;
        for (const auto& member : cls->members) {
            if (auto var = dynamic_cast<const VariableDeclaration*>(member.get())) {
                fieldTypes[var->identifier] = var->type;
            }
        }

        std::vector<const MethodDeclaration*> methods;
        for (const auto& member : cls->members) {
            if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                // Specializations are skipped: the sets and maps here are keyed by bare method name, so a specialization sharing a name would corrupt the primary's
                // classification. Their return lowering is handled elsewhere (AstEmitter::currentSpecializationValueType_).
                if (!method->specializationArg.empty()) continue;
                methods.push_back(method);
            }
        }

        std::set<std::string> aliasReturning;     // method names decided ALIAS_RETURNING this round
        std::set<std::string> notAliasReturning;  // method names decided NOT this round
        std::set<std::string> fieldsNeedingShared;
        // Case (d)'s fields carry their own declared type (possibly of another class), so they are kept apart from fieldsNeedingShared.
        std::vector<std::pair<std::string, std::string>> fieldsNeedingSharedTyped;

        bool progressed = true;
        while (progressed) {
            progressed = false;
            for (const auto* method : methods) {
                if (aliasReturning.count(method->name) || notAliasReturning.count(method->name)) {
                    continue;  // already classified
                }
                std::vector<const Expression*> returns;
                collectReturnExpressions(method->body, returns);
                if (returns.empty()) {
                    notAliasReturning.insert(method->name);
                    progressed = true;
                    continue;
                }

                bool allResolved = true;   // every return matches a known case
                bool anyUndecided = false; // at least one return depends on a
                                           // not-yet-classified method (retry later)
                std::set<std::string> fieldsThisMethod;
                for (const Expression* expr : returns) {
                    if (dynamic_cast<const ThisExpression*>(expr) != nullptr) continue;  // case (a)
                    if (auto id = dynamic_cast<const Identifier*>(expr)) {
                        auto fIt = fieldTypes.find(id->name);
                        if (fIt != fieldTypes.end() && classNames.count(fIt->second) > 0) {
                            fieldsThisMethod.insert(id->name);  // case (b), bare
                            continue;
                        }
                        allResolved = false;
                        break;
                    }
                    if (auto member = dynamic_cast<const MemberAccess*>(expr)) {
                        // `this->field` / `this.field` (a field access, not the whole return value: that is case (a))
                        if (dynamic_cast<const ThisExpression*>(member->object.get()) != nullptr) {
                            auto fIt = fieldTypes.find(member->member);
                            if (fIt != fieldTypes.end() && classNames.count(fIt->second) > 0) {
                                fieldsThisMethod.insert(member->member);  // case (b)
                                continue;
                            }
                        }
                        allResolved = false;
                        break;
                    }
                    if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
                        // case (c): `<recv>.<method>(...)` or a bare `<method>(...)` with an implicit `this`; either way the called method's classification decides.
                        std::string calledName;
                        if (auto callMember = dynamic_cast<const MemberAccess*>(call->callee.get())) {
                            calledName = callMember->member;
                        } else if (!call->callee) {
                            calledName = call->function_name;
                        }
                        // Self-recursion cannot block its own classification: if every other return matches, the recursive call is consistent with it. Otherwise a method
                        // that only calls itself would wait forever and default to not alias-returning.
                        if (!calledName.empty() && calledName == method->name) {
                            continue;
                        }
                        if (!calledName.empty() && aliasReturning.count(calledName) > 0) {
                            continue;
                        }
                        if (!calledName.empty() && notAliasReturning.count(calledName) > 0) {
                            allResolved = false;
                            break;
                        }
                        // Callee not yet classified: retry on a later round if it names a same-class method; otherwise this return can never resolve to alias.
                        if (!calledName.empty()) {
                            bool calleeIsSameClassMethod = false;
                            for (const auto* m : methods) {
                                if (m->name == calledName) { calleeIsSameClassMethod = true; break; }
                            }
                            if (calleeIsSameClassMethod) { anyUndecided = true; continue; }
                        }
                        allResolved = false;
                        break;
                    }
                    // case (d): `return new T(<inner>);` where T is the method's own class (a handle wrapping a payload, e.g. `Node::operator[]`): the aliased field belongs to
                    // another class's chain (resolveInnerFieldChain). It is recorded in fieldsNeedingShared under its own declared type, which is how explicitHints is keyed.
                    if (auto neww = dynamic_cast<const NewExpression*>(expr)) {
                        if (neww->className == cls->name && neww->arguments.size() == 1) {
                            const auto resolved = resolveInnerFieldChain(
                                neww->arguments[0].get(), cls->name, classMembersByClass_, methodReturnKinds_);
                            if (resolved.has_value()) {
                                fieldsNeedingSharedTyped.push_back(*resolved);
                                continue;
                            }
                        }
                        allResolved = false;
                        break;
                    }
                    // any other expression shape -> not alias-returning.
                    allResolved = false;
                    break;
                }

                if (!allResolved) {
                    notAliasReturning.insert(method->name);
                    progressed = true;
                } else if (!anyUndecided) {
                    aliasReturning.insert(method->name);
                    fieldsNeedingShared.insert(fieldsThisMethod.begin(), fieldsThisMethod.end());
                    progressed = true;
                }
                // still depends on an undecided sibling method: leave unclassified, retry next round.
            }
        }
        // Anything still unclassified after the loop stabilizes (a cycle of never-resolving methods) defaults to not alias-returning.

        // Also register under the qualified name (`YAML::Node` as well as `Node`): lookups in AstEmitter/DanglingChecker key by the receiver's declared type string.
        std::vector<std::string> classKeys = {cls->name};
        if (!cls->namespaceName.empty()) {
            classKeys.push_back(cls->namespaceName + "::" + cls->name);
        }
        for (const auto& name : aliasReturning) {
            for (const auto& classKey : classKeys) {
                methodReturnKinds_[makeMemberKey(classKey, name)] = PointerType::SHARED;
            }
        }
        // A field returned by an alias-returning method must be shared like any other field lookup (getPointerType consults explicitHints). The key is
        // "<declared type of the field>.<field name>", not the containing class: that is what secondPassDeterminePointerTypes queries. explicitHintLines is not
        // touched, so inferred entries never trigger the "hint didn't match" diagnostic.
        for (const auto& fieldName : fieldsNeedingShared) {
            explicitHints[makeMemberKey(fieldTypes[fieldName], fieldName)] = PointerType::SHARED;
        }
        // Case (d)'s fields already carry their (declaredType, fieldName), possibly of another class, so write them directly.
        for (const auto& [fieldType, fieldName] : fieldsNeedingSharedTyped) {
            explicitHints[makeMemberKey(fieldType, fieldName)] = PointerType::SHARED;
        }
        for (const auto& name : aliasReturning) {
            const bool anyReturnsThis = [&] {
                for (const auto* method : methods) {
                    if (method->name != name) continue;
                    std::vector<const Expression*> returns;
                    collectReturnExpressions(method->body, returns);
                    for (const Expression* expr : returns) {
                        if (dynamic_cast<const ThisExpression*>(expr) != nullptr) {
                            return true;
                        }
                    }
                }
                return false;
            }();
            // A value-type (struct) class's `return this;` is lowered to `return *this;` and needs no shared_ptr machinery.
            if (anyReturnsThis && !isValueTypeClass(cls->name)) {
                // A class returning `this` as a shared_ptr must always be constructed as shared_ptr (shared_from_this() needs it); reuses classesNeedingSharedRoots.
                classesNeedingSharedFromThis_.insert(cls->name);
                classesNeedingSharedRoots.insert(cls->name);
            }
        }
        // Separate scan over every method: passing `this` as a call argument or storing it (`Node y = this;`) is lowered to shared_from_this() by
        // emitMovedArg, so it needs the same enable_shared_from_this and shared-root machinery. Value-type classes are excluded as above.
        if (!isValueTypeClass(cls->name)) {
            for (const auto* method : methods) {
                if (methodPassesThisAsArgument(method->body)) {
                    classesNeedingSharedFromThis_.insert(cls->name);
                    classesNeedingSharedRoots.insert(cls->name);
                }
            }
        }
    }
}

void SmartPointerAnalyzer::inferAliasReturningFreeFunctions(const Program& program) {
    // Free-function counterpart of case (c): a free function that only returns a call to an already-classified alias-returning method or function is itself
    // alias-returning. Runs after promoteReturnsOfSharedLocals, because a class method such as parseDocument only becomes shared through that pass.
    std::set<std::string> aliasReturningFreeFunctions;
    // Fixed point: a function that forwards `return other(...)` is classified from `other`, which may be defined later or classified by an earlier pass.
    for (bool changed = true; changed;) {
    changed = false;
    for (const auto& stmt : program.statements) {
        auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get());
        if (fn == nullptr) continue;
        if (classNames.find(fn->returnType) == classNames.end()) continue;
        if (aliasReturningFreeFunctions.count(fn->name) > 0) continue;

        std::vector<const Expression*> returns;
        collectReturnExpressions(fn->body, returns);
        if (returns.empty()) continue;

        bool allAliasCalls = true;
        bool sawCall = false;
        for (const Expression* expr : returns) {
            // `return nullptr;` fits either pointer kind and says nothing about this one.
            if (auto nullId = dynamic_cast<const Identifier*>(expr); nullId != nullptr && nullId->name == "nullptr") {
                continue;
            }
            // `return x.field;` hands out an object somebody else keeps owning: shared.
            if (auto ma = dynamic_cast<const MemberAccess*>(expr)) {
                sawCall = true;
                // The returned field must itself be a shared handle: resolve `base.field` (one level, or a chain) to the field's owning class and mark that field shared.
                std::function<std::string(const Expression*)> classOfExpr = [&](const Expression* e) -> std::string {
                    if (auto id = dynamic_cast<const Identifier*>(e)) {
                        auto t = symbolTypesByScope.find(makeSymbolKey(fn->name, id->name));
                        return t == symbolTypesByScope.end() ? std::string() : t->second;
                    }
                    if (auto m = dynamic_cast<const MemberAccess*>(e)) {
                        const std::string owner = classOfExpr(m->object.get());
                        if (owner.empty()) return "";
                        auto t = symbolTypesByScope.find(makeSymbolKey(owner, m->member));
                        return t == symbolTypesByScope.end() ? std::string() : t->second;
                    }
                    return "";
                };
                const std::string owner = classOfExpr(ma->object.get());
                if (!owner.empty()) {
                    for (auto& [vkey, vref] : variableReferences) {
                        if (vref.containingScope == owner && vref.variableName == ma->member &&
                            !vref.hasExplicitHint && pointerDecisions[vkey] == PointerType::UNIQUE) {
                            pointerDecisions[vkey] = PointerType::SHARED;
                            pointerReasons[vkey] = "returned as an alias by a free function";
                            vref.reason = pointerReasons[vkey];
                        }
                    }
                }
                continue;
            }
            auto call = dynamic_cast<const FunctionCall*>(expr);
            if (call == nullptr) { allAliasCalls = false; break; }
            sawCall = true;
            std::string calledClass;
            std::string calledName;
            if (call->callee) {
                auto member = dynamic_cast<const MemberAccess*>(call->callee.get());
                if (member == nullptr) { allAliasCalls = false; break; }
                if (auto recvId = dynamic_cast<const Identifier*>(member->object.get())) {
                    // Try this function's own scope first: the global fallback entry is last-write-wins, so two functions with a local of the same name and different types collide there.
                    auto typeIt = symbolTypesByScope.find(makeSymbolKey(fn->name, recvId->name));
                    if (typeIt == symbolTypesByScope.end()) {
                        typeIt = symbolTypesByScope.find(makeSymbolKey(kGlobalScope, recvId->name));
                    }
                    if (typeIt != symbolTypesByScope.end()) calledClass = typeIt->second;
                }
                calledName = member->member;
            } else {
                calledName = call->function_name;
            }
            if (calledClass.empty()) {
                if (aliasReturningFreeFunctions.count(calledName) == 0 &&
                    getMethodReturnPointerType("", calledName) != PointerType::SHARED) {
                    allAliasCalls = false;
                    break;
                }
            } else if (getMethodReturnPointerType(calledClass, calledName) != PointerType::SHARED) {
                allAliasCalls = false;
                break;
            }
        }
        if (allAliasCalls && sawCall) {
            changed = true;
            aliasReturningFreeFunctions.insert(fn->name);
            methodReturnKinds_[makeMemberKey("", fn->name)] = PointerType::SHARED;
            // Also register under the qualified name (`YAML::Load`): a call outside the namespace spells it that way, while the definition side queries the bare name.
            if (!fn->namespaceName.empty()) {
                methodReturnKinds_[makeMemberKey("", fn->namespaceName + "::" + fn->name)] =
                    PointerType::SHARED;
            }
        }
    }
    }
}

namespace {
// The initializer expression of the local variable `name` declared somewhere in `body` (nested blocks included), or nullptr if it is not declared
// with an initializer. The first declaration wins.
const Expression* findLocalInitializer(const std::vector<StatementPtr>& body, const std::string& name) {
    for (const auto& st : body) {
        if (auto d = dynamic_cast<const VariableDeclaration*>(st.get())) {
            if (d->identifier == name) return d->expression.get();
        } else if (auto i = dynamic_cast<const IfStatement*>(st.get())) {
            if (auto found = findLocalInitializer(i->body, name)) return found;
            if (auto found = findLocalInitializer(i->elseBody, name)) return found;
        } else if (auto w = dynamic_cast<const WhileStatement*>(st.get())) {
            if (auto found = findLocalInitializer(w->body, name)) return found;
        } else if (auto f = dynamic_cast<const ForStatement*>(st.get())) {
            if (auto found = findLocalInitializer(f->body, name)) return found;
        } else if (auto t = dynamic_cast<const TryStatement*>(st.get())) {
            if (auto found = findLocalInitializer(t->body, name)) return found;
            if (auto found = findLocalInitializer(t->catchBody, name)) return found;
        }
    }
    return nullptr;
}
}  // namespace

// Is `init` a call whose result is shared, by the classification so far? The same question AstEmitter::isSharedReturningCall answers when it declares
// the local: `recv.method(...)` (the receiver's class from its declared type), or a plain call (a free function, or a method of `ownClass`).
bool SmartPointerAnalyzer::localComesFromSharedCall(const Expression* init, const std::string& ownClass, const std::string& scope) const {
    auto call = dynamic_cast<const FunctionCall*>(init);
    if (call == nullptr || !call->templateArg.empty()) return false;
    if (call->callee) {
        auto member = dynamic_cast<const MemberAccess*>(call->callee.get());
        if (member == nullptr) return false;
        auto recv = dynamic_cast<const Identifier*>(member->object.get());
        if (recv == nullptr) return false;
        auto type = symbolTypesByScope.find(makeSymbolKey(scope, recv->name));
        if (type == symbolTypesByScope.end()) type = symbolTypesByScope.find(makeSymbolKey(kGlobalScope, recv->name));
        if (type == symbolTypesByScope.end()) return false;
        return getMethodReturnPointerType(type->second, member->member) == PointerType::SHARED;
    }
    if (getMethodReturnPointerType("", call->function_name) == PointerType::SHARED) return true;
    return !ownClass.empty() && getMethodReturnPointerType(ownClass, call->function_name) == PointerType::SHARED;
}

void SmartPointerAnalyzer::promoteReturnsOfSharedLocals(const Program& program) {
    // `Item w = get(i); ... return w;`: a local initialised from a call whose result is shared is shared (as AstEmitter::isSharedReturningCall declares it), so the
    // method must return shared too. Which methods return shared depends on this pass itself, so it runs to a fixed point.
    for (bool changed = true; changed;) {
        changed = false;
        promoteReturnsOfSharedLocalsOnce(program, changed);
    }
}

void SmartPointerAnalyzer::promoteReturnsOfSharedLocalsOnce(const Program& program, bool& changed) {
    // inferAliasReturningMethods is structural and does not recognize `return <local>;`. A local can become shared in a later pass (promoteSharedCallArguments),
    // and the method returning it must then return shared. This pass runs after that one and reuses getPointerTypeForVariable.
    for (const auto& stmt : program.statements) {
        auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get());
        if (cls == nullptr) continue;
        for (const auto& member : cls->members) {
            auto method = dynamic_cast<const MethodDeclaration*>(member.get());
            if (method == nullptr) continue;
            // Specializations are skipped: methodReturnKinds_ is keyed by bare method name, so a specialization would corrupt the primary's entry.
            if (!method->specializationArg.empty()) continue;
            // Only for a class-typed return: getPointerTypeForVariable defaults to shared for a name it cannot match, which would misfire on a primitive local.
            if (classNames.find(method->returnType) == classNames.end()) continue;
            const std::string key = makeMemberKey(cls->name, method->name);
            if (methodReturnKinds_.count(key) > 0) continue;  // already SHARED

            std::vector<const Expression*> returns;
            collectReturnExpressions(method->body, returns);
            bool anyLocalIsShared = false;
            // Scoped lookup ("Class::method"): a plain name-only match would collide with a same-named local in an unrelated function.
            const std::string methodScope = cls->name + "::" + method->name;
            for (const Expression* expr : returns) {
                // `return this;` is handled by the anyReturnsThis / classesNeedingSharedFromThis_ mechanism; ThisExpression is not an Identifier, so it falls through here.
                auto id = dynamic_cast<const Identifier*>(expr);
                if (id == nullptr) continue;
                if (getPointerTypeForVariableInScope(methodScope, id->name) == PointerType::SHARED ||
                    localComesFromSharedCall(findLocalInitializer(method->body, id->name), cls->name, methodScope)) {
                    anyLocalIsShared = true;
                    break;
                }
            }
            if (anyLocalIsShared) {
                changed = true;
                methodReturnKinds_[key] = PointerType::SHARED;
                if (!cls->namespaceName.empty()) {
                    methodReturnKinds_[makeMemberKey(cls->namespaceName + "::" + cls->name, method->name)] =
                        PointerType::SHARED;
                }
            }
        }
    }

    // Same check for top-level free functions, keyed under the "" class (as emitFunction queries them).
    for (const auto& stmt : program.statements) {
        auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get());
        if (fn == nullptr) continue;
        if (classNames.find(fn->returnType) == classNames.end()) continue;
        const std::string key = makeMemberKey("", fn->name);
        if (methodReturnKinds_.count(key) > 0) continue;

        std::vector<const Expression*> returns;
        collectReturnExpressions(fn->body, returns);
        bool anyLocalIsShared = false;
        // Scoped lookup (bare fn->name), for the same reason as the class-method loop above.
        for (const Expression* expr : returns) {
            auto id = dynamic_cast<const Identifier*>(expr);
            if (id == nullptr) continue;
            if (getPointerTypeForVariableInScope(fn->name, id->name) == PointerType::SHARED ||
                localComesFromSharedCall(findLocalInitializer(fn->body, id->name), "", fn->name)) {
                anyLocalIsShared = true;
                break;
            }
        }
        if (anyLocalIsShared) {
            changed = true;
            // Keyed under the bare name ("" class + fn->name), as AstEmitter::buildFunctionSignature queries the definition. Also registered under the qualified name
            // (`YAML::Clone`), which is how a caller outside the namespace spells it.
            methodReturnKinds_[key] = PointerType::SHARED;
            if (!fn->namespaceName.empty()) {
                methodReturnKinds_[makeMemberKey("", fn->namespaceName + "::" + fn->name)] =
                    PointerType::SHARED;
            }
        }
    }
}

namespace {
void collectIdentifierNames(const Expression* expr, std::set<std::string>& out) {
    if (expr == nullptr) return;
    if (auto id = dynamic_cast<const Identifier*>(expr)) { out.insert(id->name); return; }
    if (auto m = dynamic_cast<const MemberAccess*>(expr)) { collectIdentifierNames(m->object.get(), out); return; }
    if (auto sub = dynamic_cast<const SubscriptExpression*>(expr)) {
        collectIdentifierNames(sub->object.get(), out);
        collectIdentifierNames(sub->index.get(), out);
        return;
    }
    if (auto bin = dynamic_cast<const BinaryExpression*>(expr)) {
        collectIdentifierNames(bin->left.get(), out);
        collectIdentifierNames(bin->right.get(), out);
        return;
    }
    if (auto un = dynamic_cast<const UnaryExpression*>(expr)) { collectIdentifierNames(un->operand.get(), out); return; }
    if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
        collectIdentifierNames(call->callee.get(), out);
        for (const auto& a : call->arguments) collectIdentifierNames(a.get(), out);
        return;
    }
    if (auto neww = dynamic_cast<const NewExpression*>(expr)) {
        for (const auto& a : neww->arguments) collectIdentifierNames(a.get(), out);
        return;
    }
}

// One entry per statement in execution (pre-)order. `used` holds the names read by the statement's own expressions (a loop's entry holds its condition or container);
// nested bodies get entries of their own.
struct FlatStmt {
    const Statement* stmt = nullptr;
    std::set<std::string> used;
    std::string declared;
    int loopEnd = -1;  // for a while/for: index of its last nested statement
};

void flattenBody(const std::vector<StatementPtr>& body, std::vector<FlatStmt>& out) {
    for (const auto& sp : body) {
        const Statement* st = sp.get();
        FlatStmt fs;
        fs.stmt = st;
        const size_t myIndex = out.size();
        if (auto decl = dynamic_cast<const VariableDeclaration*>(st)) {
            collectIdentifierNames(decl->expression.get(), fs.used);
            fs.declared = decl->identifier;
            out.push_back(fs);
        } else if (auto cdecl = dynamic_cast<const ContainerDeclaration*>(st)) {
            fs.declared = cdecl->identifier;
            out.push_back(fs);
        } else if (auto asg = dynamic_cast<const AssignmentStatement*>(st)) {
            collectIdentifierNames(asg->expression.get(), fs.used);
            if (asg->target) collectIdentifierNames(asg->target.get(), fs.used);
            out.push_back(fs);
        } else if (auto es = dynamic_cast<const ExpressionStatement*>(st)) {
            collectIdentifierNames(es->expression.get(), fs.used);
            out.push_back(fs);
        } else if (auto ret = dynamic_cast<const ReturnStatement*>(st)) {
            collectIdentifierNames(ret->expression.get(), fs.used);
            out.push_back(fs);
        } else if (auto th = dynamic_cast<const ThrowStatement*>(st)) {
            collectIdentifierNames(th->expression.get(), fs.used);
            out.push_back(fs);
        } else if (auto ifs = dynamic_cast<const IfStatement*>(st)) {
            collectIdentifierNames(ifs->condition.get(), fs.used);
            out.push_back(fs);
            flattenBody(ifs->body, out);
            flattenBody(ifs->elseBody, out);
        } else if (auto wh = dynamic_cast<const WhileStatement*>(st)) {
            collectIdentifierNames(wh->condition.get(), fs.used);
            out.push_back(fs);
            flattenBody(wh->body, out);
            out[myIndex].loopEnd = static_cast<int>(out.size()) - 1;
        } else if (auto fo = dynamic_cast<const ForStatement*>(st)) {
            collectIdentifierNames(fo->container.get(), fs.used);
            fs.declared = fo->loopVarName;
            out.push_back(fs);
            flattenBody(fo->body, out);
            out[myIndex].loopEnd = static_cast<int>(out.size()) - 1;
        } else if (auto tr = dynamic_cast<const TryStatement*>(st)) {
            out.push_back(fs);
            flattenBody(tr->body, out);
            flattenBody(tr->catchBody, out);
        } else {
            out.push_back(fs);
        }
    }
}
} // namespace

void SmartPointerAnalyzer::promoteAliasedLocalsInBody(
    const std::vector<StatementPtr>& body,
    const std::vector<std::pair<std::string, std::string>>& params, const std::string& scope) {
    std::vector<FlatStmt> flat;
    flattenBody(body, flat);
    std::map<std::string, std::string> types;  // object-class locals and parameters
    for (const auto& [ptype, pname] : params) {
        if (classNames.count(ptype) > 0) types[pname] = ptype;
    }
    for (const auto& fs : flat) {
        if (auto decl = dynamic_cast<const VariableDeclaration*>(fs.stmt)) {
            if (classNames.count(decl->type) > 0 && !decl->isObserver) types[decl->identifier] = decl->type;
        }
    }
    auto promote = [&](const std::string& name) {
        auto ti = types.find(name);
        if (ti == types.end()) return;
        auto it = pointerDecisions.find(buildKey(scope, ti->second, name));
        if (it == pointerDecisions.end() || it->second == PointerType::WEAK) return;
        // an explicit hint always wins
        auto vr = variableReferences.find(buildKey(scope, ti->second, name));
        if (vr != variableReferences.end() && vr->second.hasExplicitHint) return;
        it->second = PointerType::SHARED;
        pointerReasons[buildKey(scope, ti->second, name)] =
            "assigned to another handle and used again afterwards (two live owners)";
    };
    for (size_t i = 0; i < flat.size(); ++i) {
        std::string target;
        const Expression* rhs = nullptr;
        if (auto decl = dynamic_cast<const VariableDeclaration*>(flat[i].stmt)) {
            if (classNames.count(decl->type) == 0 || decl->isObserver) continue;
            target = decl->identifier;
            rhs = decl->expression.get();
        } else if (auto asg = dynamic_cast<const AssignmentStatement*>(flat[i].stmt)) {
            if (asg->target) continue;  // a field store already shares
            target = asg->variable;
            rhs = asg->expression.get();
        } else {
            continue;
        }
        // `T x = v[i];` / `T x = obj.items[i];` copies a handle out of a container element: x and the element are two owners.
        if (auto sub = dynamic_cast<const SubscriptExpression*>(rhs);
            sub != nullptr && types.find(target) != types.end() &&
            (dynamic_cast<const Identifier*>(sub->object.get()) != nullptr ||
             dynamic_cast<const MemberAccess*>(sub->object.get()) != nullptr)) {
            promote(target);
            continue;
        }
        // `T x = field;` inside a method: the local and the field are two owners from now on (a field copy never moves the field). Both become shared,
        // otherwise the local would be a shared_ptr initialised from a unique_ptr field.
        if (auto fieldId = dynamic_cast<const Identifier*>(rhs);
            fieldId != nullptr && types.find(fieldId->name) == types.end() && types.find(target) != types.end()) {
            const size_t sep = scope.find("::");
            if (sep != std::string::npos) {
                const std::string cls = scope.substr(0, sep);
                auto members = classMembersByClass_.find(cls);
                if (members != classMembersByClass_.end()) {
                    auto fieldType = members->second.find(fieldId->name);
                    if (fieldType != members->second.end() && classNames.count(fieldType->second) > 0) {
                        for (auto& [key, ref] : variableReferences) {
                            if (ref.className != fieldType->second || ref.variableName != fieldId->name) continue;
                            if (ref.containingScope != normalizeScope(cls) || ref.hasExplicitHint) continue;
                            auto dec = pointerDecisions.find(key);
                            if (dec == pointerDecisions.end() || dec->second == PointerType::WEAK) continue;
                            dec->second = PointerType::SHARED;
                            pointerReasons[key] = "copied into a local handle (two live owners)";
                        }
                        promote(target);
                        continue;
                    }
                }
            }
        }
        auto rhsId = dynamic_cast<const Identifier*>(rhs);
        if (rhsId == nullptr || types.find(rhsId->name) == types.end() || types.find(target) == types.end()) {
            continue;
        }
        const std::string& src = rhsId->name;
        bool usedAgain = false;
        for (size_t j = i + 1; j < flat.size() && !usedAgain; ++j) {
            if (flat[j].used.count(src) > 0) usedAgain = true;
        }
        // inside a loop, a use anywhere in the loop is 'after' on the next iteration, unless the source is declared inside that loop
        for (size_t l = 0; l < i && !usedAgain; ++l) {
            if (flat[l].loopEnd < 0 || static_cast<size_t>(flat[l].loopEnd) < i) continue;
            bool declaredInside = false;
            for (size_t k = l; k <= static_cast<size_t>(flat[l].loopEnd); ++k) {
                if (k != l && flat[k].declared == src) declaredInside = true;
            }
            if (declaredInside) continue;
            for (size_t k = l; k <= static_cast<size_t>(flat[l].loopEnd) && !usedAgain; ++k) {
                if (k != i && flat[k].used.count(src) > 0) usedAgain = true;
            }
        }
        if (usedAgain) {
            promote(src);
            promote(target);
        }
    }
}

} // namespace cling
