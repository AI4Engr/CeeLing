#include "passes/struct_json.h"

#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

#include "syntax/ast_builder.h"
#include "syntax/ast_walk.h"
#include "facts/library_summary.h"

namespace cling {

namespace {

using astwalk::NodeFn;

[[noreturn]] void fail(const std::string& code, const std::string& msg, const std::string& help) {
    throw std::runtime_error("error[semantic/" + code + "]: " + msg + ". " + help);
}

std::string normalized(const std::string& type) {
    return type == "std::string" ? "string" : type;
}

bool isDecodablePrimitive(const std::string& type) {
    static const std::set<std::string> kKnown = {"int", "int64", "double", "float", "bool", "string", "byte"};
    return kKnown.count(normalized(type)) > 0;
}

struct Field {
    std::string type;
    std::string name;
};

struct StructInfo {
    const ClassDeclaration* cls = nullptr;
    std::vector<Field> fields;
    // The struct has a constructor with no parameters: every field is then OPTIONAL. The decoder starts from `new X()` and overwrites the fields that
    // are present (and not null); a missing field keeps the value the constructor gave it. Without one, every field is required and the decoder builds
    // the struct with the constructor that takes all of them.
    bool optional = false;
};

std::string quote(const std::string& s) { return "\"" + s + "\""; }

class Generator {
public:
    Generator(Program& program, bool included) : program_(program), included_(included) {}

    void run() {
        indexProgram();
        std::set<std::string> triggered = findTriggers();
        if (triggered.empty()) return;
        // Everything that must have a decodeXAt: the triggered structs and, transitively, the structs they hold.
        std::set<std::string> needed;
        for (const auto& name : triggered) collectNeeded(name, needed);
        bool anyOptional = false;
        for (const auto& name : needed) anyOptional = anyOptional || infoFor(name).optional;
        if (!included_) requireJsonApi(anyOptional);

        std::vector<StatementPtr> out;
        for (auto& stmt : program_.statements) {
            auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get());
            std::string origin = stmt->originFile;
            out.push_back(std::move(stmt));
            if (cls == nullptr || cls->isExternalDeclaration || needed.count(cls->name) == 0) continue;
            std::string text = atFunction(infoFor(cls->name));
            if (triggered.count(cls->name) > 0) {
                text += cls->name + " decode" + cls->name + "(Item j) {\n    return decode" + cls->name + "At(j, \"\");\n}\n";
            }
            appendParsed(text, origin, out);
        }
        program_.statements = std::move(out);
    }

private:
    Program& program_;
    bool included_;
    std::map<std::string, const ClassDeclaration*> classes_;       // every class/struct visible here, external ones included
    std::set<std::string> functions_;                               // every free function visible here
    std::map<std::string, StructInfo> infos_;

    void indexProgram() {
        for (const auto& stmt : program_.statements) {
            if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) classes_[cls->name] = cls;
            if (auto f = dynamic_cast<const FunctionDeclaration*>(stmt.get())) functions_.insert(f->name);
            if (auto f = dynamic_cast<const ExternFunctionDeclaration*>(stmt.get())) functions_.insert(f->name);
        }
    }

    // Names X for which this file calls an undeclared free function `decodeX` and X is a struct.
    std::set<std::string> findTriggers() {
        std::set<std::string> called;
        for (auto& stmt : program_.statements) {
            if (stmt->isExternalDeclaration) continue;
            astwalk::walkStatement(stmt.get(), [&](ASTNode& node) {
                if (auto call = dynamic_cast<FunctionCall*>(&node)) {
                    if (call->callee == nullptr && call->templateArg.empty() && call->function_name.size() > 6 &&
                        call->function_name.compare(0, 6, "decode") == 0) {
                        called.insert(call->function_name.substr(6));
                    }
                }
            });
        }
        std::set<std::string> triggered;
        for (const auto& name : called) {
            if (functions_.count("decode" + name) > 0) continue;          // somebody wrote it
            auto it = classes_.find(name);
            if (it == classes_.end() || !it->second->isValueType) continue;   // not a struct: an ordinary unknown call
            if (it->second->isExternalDeclaration) {
                fail("struct-json-external", "`decode" + name + "` is called, but struct `" + name + "` is declared in another file",
                     "the decoder is generated in the file that declares the struct (call decode" + name + " there, or "
                     "declare a function `" + name + " decode" + name + "(Item j)` that calls it)");
            }
            triggered.insert(name);
        }
        return triggered;
    }

    void requireJsonApi(bool needIsNull) {
        static const char* kNeeded[] = {"isObject", "isString", "isNumber", "isBool", "isTrue",
                                        "isArray", "getObjectItem", "getArraySize", "getArrayItem"};
        std::string missing;
        for (const char* n : kNeeded) {
            if (functions_.count(n) == 0) missing += std::string(missing.empty() ? "" : ", ") + n;
        }
        if (needIsNull && functions_.count("isNull") == 0) missing += std::string(missing.empty() ? "" : ", ") + "isNull";
        if (classes_.count("Item") == 0) missing += std::string(missing.empty() ? "" : ", ") + "class Item";
        if (!missing.empty()) {
            fail("struct-json-needs-cjson", "generated struct decoders need cjson-cling, which is not visible here (missing: " + missing + ")",
                 "add #include \"cjson_api.clh\" (and cjson_item.clh)");
        }
    }

    const StructInfo& infoFor(const std::string& name) {
        auto found = infos_.find(name);
        if (found != infos_.end()) return found->second;
        const ClassDeclaration* cls = classes_.at(name);
        StructInfo info;
        info.cls = cls;
        if (!cls->namespaceName.empty()) {
            fail("struct-json-namespace", "struct `" + name + "` is declared inside a namespace", "decoders are generated for top-level structs only");
        }
        if (!cls->baseClass.empty()) {
            fail("struct-json-base", "struct `" + name + "` has a base struct", "decoders are generated for structs without a base");
        }
        for (const auto& member : cls->members) {
            if (auto v = dynamic_cast<const VariableDeclaration*>(member.get())) {
                if (v->isObserver) fail("struct-json-field", "field `" + v->identifier + "` of `" + name + "` is a reference", "not decodable");
                info.fields.push_back({normalized(v->type), v->identifier});
            } else if (auto c = dynamic_cast<const ContainerDeclaration*>(member.get())) {
                info.fields.push_back({c->type, c->identifier});
            }
        }
        if (info.fields.empty()) {
            fail("struct-json-empty", "struct `" + name + "` has no fields", "there is nothing to decode");
        }
        for (const auto& f : info.fields) checkFieldType(name, f);
        for (const auto& member : cls->members) {
            auto ctor = dynamic_cast<const ConstructorDeclaration*>(member.get());
            if (ctor != nullptr && ctor->parameters.empty()) info.optional = true;
        }
        if (!info.optional) checkConstructor(info);
        return infos_[name] = info;
    }

    bool isOwnStruct(const std::string& type) {
        auto it = classes_.find(type);
        return it != classes_.end() && it->second->isValueType;
    }

    void checkFieldType(const std::string& structName, const Field& f) {
        std::string type = f.type;
        bool inVector = false;
        if (isVectorType(type)) {
            type = normalized(vectorElementType(type));
            inVector = true;
        }
        if (isDecodablePrimitive(type)) return;
        if (isOwnStruct(type)) return;
        fail("struct-json-field",
             "field `" + f.name + "` of struct `" + structName + "` has type `" + f.type + "`, which a generated decoder cannot read",
             std::string("supported: int, int64, double, float, bool, string, byte, a struct, and std::vector of ") +
                 (inVector ? "those (not nested vectors)" : "those") +
                 "; read other data by hand from the Item instead");
    }

    void checkConstructor(const StructInfo& info) {
        for (const auto& member : info.cls->members) {
            auto ctor = dynamic_cast<const ConstructorDeclaration*>(member.get());
            if (ctor == nullptr || ctor->parameters.size() != info.fields.size()) continue;
            bool same = true;
            for (size_t i = 0; i < info.fields.size(); ++i) {
                if (normalized(ctor->parameters[i].first) != info.fields[i].type) same = false;
            }
            if (same) return;
        }
        std::string sig = info.cls->name + "(";
        for (size_t i = 0; i < info.fields.size(); ++i) {
            sig += (i ? ", " : "") + info.fields[i].type + " " + info.fields[i].name;
        }
        sig += ")";
        fail("struct-json-constructor",
             "struct `" + info.cls->name + "` has no constructor taking its fields in declaration order",
             "the generated decoder builds it with `new " + info.cls->name + "(...)`; add a constructor " + sig);
    }

    void collectNeeded(const std::string& name, std::set<std::string>& needed) {
        if (!needed.insert(name).second) return;
        for (const auto& f : infoFor(name).fields) {
            std::string type = isVectorType(f.type) ? normalized(vectorElementType(f.type)) : f.type;
            if (!isOwnStruct(type)) continue;
            const ClassDeclaration* cls = classes_.at(type);
            if (cls->isExternalDeclaration) {
                // declared in another file: its decoder must exist there
                if (functions_.count("decode" + type + "At") == 0) {
                    fail("struct-json-external", "field `" + f.name + "` holds struct `" + type + "`, declared in another file, whose decoder `decode" + type + "At` is not visible",
                         "call decode" + type + " in that file so it generates one, and include it with its .clh");
                }
                continue;
            }
            collectNeeded(type, needed);
        }
    }

    // ---- text of the generated functions ----

    int counter_ = 0;

    std::string atFunction(const StructInfo& info) {
        counter_ = 0;
        std::ostringstream out;
        const std::string& name = info.cls->name;
        out << name << " decode" << name << "At(Item j, string path) {\n";
        out << "    if (!isObject(j)) {\n"
               "        string m = \"invalid_argument: expected an object\";\n"
               "        if (path.length() > 0) {\n"
               "            m = \"invalid_argument: `\" + path + \"` must be an object\";\n"
               "        }\n"
               "        throw m;\n"
               "    }\n"
               "    string base = \"\";\n"
               "    if (path.length() > 0) {\n"
               "        base = path + \".\";\n"
               "    }\n";
        if (info.optional) {
            // every field optional: start from the constructor's values, overwrite what the object has (a JSON null counts as absent)
            out << "    " << name << " r = new " << name << "();\n";
            for (const auto& f : info.fields) {
                std::string item = "f" + std::to_string(counter_++);
                out << "    Item " << item << " = getObjectItem(j, " << quote(f.name) << ");\n";
                out << "    if (" << item << " != nullptr && !isNull(" << item << ")) {\n";
                std::string var = decodeValue(out, 2, f.type, item, "base + " + quote(f.name));
                out << "        r." << f.name << " = " << var << ";\n";
                out << "    }\n";
            }
            out << "    return r;\n}\n";
            return out.str();
        }
        std::vector<std::string> vars;
        for (const auto& f : info.fields) {
            std::string item = "f" + std::to_string(counter_++);
            out << "    Item " << item << " = getObjectItem(j, " << quote(f.name) << ");\n";
            vars.push_back(decodeValue(out, 1, f.type, item, "base + " + quote(f.name)));
        }
        out << "    return new " << name << "(";
        for (size_t i = 0; i < vars.size(); ++i) out << (i ? ", " : "") << vars[i];
        out << ");\n}\n";
        return out.str();
    }

    static std::string indent(int level) { return std::string(static_cast<size_t>(level) * 4, ' '); }

    // `label` is a CeeLing string expression naming the value in messages ("path + \"root\"").
    void check(std::ostringstream& out, int level, const std::string& condition, const std::string& label, const std::string& wanted) {
        out << indent(level) << "if (" << condition << ") {\n"
            << indent(level + 1) << "string m = \"invalid_argument: `\" + " << label << " + \"` must be " << wanted << "\";\n"
            << indent(level + 1) << "throw m;\n"
            << indent(level) << "}\n";
    }

    // Emits code that turns the Item variable `item` into a new local, and returns the local's name.
    std::string decodeValue(std::ostringstream& out, int level, const std::string& type, const std::string& item, const std::string& label) {
        const std::string id = std::to_string(counter_++);
        const std::string var = "v" + id;
        if (type == "string") {
            check(out, level, "!isString(" + item + ")", label, "a string");
            out << indent(level) << "string " << var << " = " << item << ".valuestring;\n";
        } else if (type == "bool") {
            check(out, level, "!isBool(" + item + ")", label, "true or false");
            out << indent(level) << "bool " << var << " = isTrue(" << item << ");\n";
        } else if (type == "double" || type == "float") {
            check(out, level, "!isNumber(" + item + ")", label, "a number");
            out << indent(level) << type << " " << var << " = " << (type == "float" ? "float(" + item + ".valuedouble)" : item + ".valuedouble") << ";\n";
        } else if (type == "int" || type == "int64" || type == "byte") {
            check(out, level, "!isNumber(" + item + ")", label, "a number");
            const std::string d = "d" + id;
            out << indent(level) << "double " << d << " = " << item << ".valuedouble;\n";
            std::string lo = "-2147483648.0", hi = "2147483647.0";
            if (type == "int64") { lo = "-9007199254740991.0"; hi = "9007199254740991.0"; }
            if (type == "byte") { lo = "0.0"; hi = "255.0"; }
            check(out, level, d + " != " + d + " || " + d + " < " + lo + " || " + d + " > " + hi + " || " + d + " != double(" + type + "(" + d + "))",
                  label, type == "byte" ? "a whole number from 0 to 255" : "a whole number in range");
            out << indent(level) << type << " " << var << " = " << type << "(" << d << ");\n";
        } else if (isVectorType(type)) {
            const std::string element = normalized(vectorElementType(type));
            const std::string n = "n" + id, i = "i" + id, e = "e" + id;
            check(out, level, "!isArray(" + item + ")", label, "an array");
            out << indent(level) << type << " " << var << ";\n";
            out << indent(level) << "int " << n << " = getArraySize(" << item << ");\n";
            out << indent(level) << "int " << i << " = 0;\n";
            out << indent(level) << "while (" << i << " < " << n << ") {\n";
            out << indent(level + 1) << "Item " << e << " = getArrayItem(" << item << ", " << i << ");\n";
            std::string inner = decodeValue(out, level + 1, element, e, label + " + \"[\" + std::to_string(" + i + ") + \"]\"");
            out << indent(level + 1) << var << ".push_back(" << inner << ");\n";
            out << indent(level + 1) << i << " = " << i << " + 1;\n";
            out << indent(level) << "}\n";
        } else {
            // a struct (checkFieldType has already refused everything else)
            out << indent(level) << type << " " << var << " = decode" << type << "At(" << item << ", " << label << ");\n";
        }
        return var;
    }

    // Parses generated source and appends its declarations to `out`, marked as coming from `origin`.
    void appendParsed(const std::string& text, const std::string& origin, std::vector<StatementPtr>& out) {
        ASTBuilder builder;
        ProgramPtr parsed = builder.buildProgram(text);
        for (auto& stmt : parsed->statements) {
            astwalk::walkStatement(stmt.get(), [](ASTNode& node) { node.loc = SourceLoc(); });   // (positions in generated text mean nothing)
            stmt->originFile = origin;
            out.push_back(std::move(stmt));
        }
    }
};

}  // namespace

void desugarStructJson(Program& program, bool included) {
    Generator(program, included).run();
}

}  // namespace cling
