// cling-scan-extract: the LibTooling front end of cling-scan.
//
// It parses a translation unit that includes the library header (and, for --type, instantiates a template), finds the class, and prints what
// cling_scan.py's analysis reads: the public methods as CANONICAL type spellings, the enum types, the members it skipped, the non-system
// headers the translation unit includes, and clang's version. The verdict rules (fresh / borrow / mutator / function) stay in Python and are
// shared with the other two backends; this program only reads declarations, through clang's C++ AST (Sema's instantiated classes included,
// which is what the libclang C API cannot show).
//
//   cling-scan-extract (--class Name | --type TypeExpr) --header H [-I dir]... [-std c++17] [--clang-arg A]... [--resource-dir D]
//
// Output, one record per line, fields separated by TAB:
//   version V | class NAME COUNT | dep PATH | enum SPELLING | skipped TEXT | method NAME STATIC CONST RET NPARAMS PARAM...
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/Version.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/Tooling.h"

using namespace clang;

namespace {

struct Method {
    std::string name, ret;
    bool isStatic = false, isConst = false;
    std::vector<std::string> params;
};

struct Result {
    std::string cls;
    int classCount = 0;
    std::vector<Method> methods;
    std::vector<std::string> skipped, deps;
    std::set<std::string> enums;
    std::string error;
};

class Consumer : public ASTConsumer {
public:
    Consumer(Result& r, std::string cls, std::string typ) : result_(r), cls_(std::move(cls)), typ_(std::move(typ)) {}

    void HandleTranslationUnit(ASTContext& ctx) override {
        ctx_ = &ctx;
        SourceManager& sm = ctx.getSourceManager();
        TranslationUnitDecl* tu = ctx.getTranslationUnitDecl();
        const CXXRecordDecl* root = nullptr;
        if (!typ_.empty()) {
            for (Decl* d : tu->decls()) {
                if (auto* vd = dyn_cast<VarDecl>(d); vd != nullptr && vd->getName() == "cling_scan_probe") {
                    root = vd->getType().getCanonicalType()->getAsCXXRecordDecl();
                    break;
                }
            }
            if (root == nullptr || root->getDefinition() == nullptr) {
                result_.error = "could not instantiate `" + typ_ + "` (is it a complete type after including the header?)";
                return;
            }
            root = root->getDefinition();
            result_.cls = typ_;
            visit(root);
        } else {
            root = findClass(tu, sm);
            if (root == nullptr) {
                result_.error = "no definition of a class or struct named `" + cls_ + "` found (for a template instantiation use --type)";
                return;
            }
            result_.cls = cls_;
            visit(root);
        }
        for (auto it = sm.fileinfo_begin(); it != sm.fileinfo_end(); ++it) {
            const FileEntry* fe = it->first;
            if (fe == nullptr) continue;
            std::string name = fe->getName().str();
            if (name.size() >= 5 && name.compare(name.size() - 5, 5, "tu.cc") == 0) continue;
            SourceLocation loc = sm.translateFileLineCol(fe, 1, 1);
            if (loc.isValid() && !sm.isInSystemHeader(loc)) result_.deps.push_back(name);
        }
        std::sort(result_.deps.begin(), result_.deps.end());
        result_.deps.erase(std::unique(result_.deps.begin(), result_.deps.end()), result_.deps.end());
    }

private:
    Result& result_;
    std::string cls_, typ_;
    ASTContext* ctx_ = nullptr;
    std::set<const CXXRecordDecl*> seen_;

    const CXXRecordDecl* findClass(const DeclContext* dc, const SourceManager& sm) {
        for (const Decl* d : dc->decls()) {
            if (auto* rd = dyn_cast<CXXRecordDecl>(d)) {
                if (sm.isInSystemHeader(rd->getLocation())) continue;
                if (rd->isCompleteDefinition() && !rd->getDescribedClassTemplate() && !isa<ClassTemplateSpecializationDecl>(rd)) {
                    const std::string q = rd->getQualifiedNameAsString();
                    if (q == cls_ || (q.size() >= cls_.size() + 2 && q.compare(q.size() - cls_.size() - 2, 2, "::") == 0 &&
                                      q.compare(q.size() - cls_.size(), cls_.size(), cls_) == 0)) {
                        return rd;
                    }
                }
                if (const CXXRecordDecl* inner = findClass(rd, sm)) return inner;
            } else if (auto* ns = dyn_cast<NamespaceDecl>(d)) {
                if (sm.isInSystemHeader(ns->getLocation())) continue;
                if (const CXXRecordDecl* inner = findClass(ns, sm)) return inner;
            }
        }
        return nullptr;
    }

    std::string spell(QualType t) const {
        PrintingPolicy policy = ctx_->getPrintingPolicy();
        return t.getCanonicalType().getAsString(policy);
    }

    void noteEnum(QualType t) {
        t = t.getCanonicalType().getNonReferenceType();
        if (const auto* et = t->getAs<EnumType>()) {
            PrintingPolicy policy = ctx_->getPrintingPolicy();
            result_.enums.insert(QualType(et, 0).getAsString(policy));
        }
    }

    void visit(const CXXRecordDecl* rd) {
        rd = rd->getDefinition();
        if (rd == nullptr || !seen_.insert(rd).second) return;
        ++result_.classCount;
        for (const Decl* d : rd->decls()) {
            if (d->getAccess() != AS_public) continue;
            if (isa<FunctionTemplateDecl>(d)) {
                result_.skipped.push_back("a member template (needs the arguments it is used with)");
                continue;
            }
            const auto* md = dyn_cast<CXXMethodDecl>(d);
            if (md == nullptr || isa<CXXConstructorDecl>(md) || isa<CXXDestructorDecl>(md) || isa<CXXConversionDecl>(md) || md->isImplicit()) continue;
            const std::string name = md->getNameAsString();
            if (md->isOverloadedOperator()) {
                result_.skipped.push_back(name + ": operators need `operator` facts written by hand");
                continue;
            }
            Method m;
            m.name = name;
            m.isStatic = md->isStatic();
            m.isConst = md->isConst();
            m.ret = spell(md->getReturnType());
            noteEnum(md->getReturnType());
            for (const ParmVarDecl* p : md->parameters()) {
                m.params.push_back(spell(p->getType()));
                noteEnum(p->getType());
            }
            result_.methods.push_back(std::move(m));
        }
        for (const CXXBaseSpecifier& base : rd->bases()) {
            if (base.getAccessSpecifier() != AS_public) continue;
            if (const CXXRecordDecl* b = base.getType().getCanonicalType()->getAsCXXRecordDecl()) visit(b);
        }
    }
};

class Action : public ASTFrontendAction {
public:
    Action(Result& r, std::string cls, std::string typ) : r_(r), cls_(std::move(cls)), typ_(std::move(typ)) {}
    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance&, llvm::StringRef) override {
        return std::make_unique<Consumer>(r_, cls_, typ_);
    }

private:
    Result& r_;
    std::string cls_, typ_;
};

}  // namespace

int main(int argc, char** argv) {
    std::string header, cls, typ, std_ = "c++17", resource;
    std::vector<std::string> includes, extra;
#ifdef CLING_SCAN_RESOURCE_DIR
    resource = CLING_SCAN_RESOURCE_DIR;
#endif
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--header") header = next();
        else if (a == "--class") cls = next();
        else if (a == "--type") typ = next();
        else if (a == "-I") includes.push_back(next());
        else if (a == "-std") std_ = next();
        else if (a == "--clang-arg") extra.push_back(next());
        else if (a == "--resource-dir") resource = next();
        else { std::fprintf(stderr, "cling-scan-extract: unknown argument %s\n", a.c_str()); return 2; }
    }
    if (header.empty() || (cls.empty() && typ.empty())) {
        std::fprintf(stderr, "usage: cling-scan-extract (--class Name | --type TypeExpr) --header H [-I dir]... [-std c++17] [--clang-arg A]... [--resource-dir D]\n");
        return 2;
    }
    std::replace(header.begin(), header.end(), '\\', '/');
    std::string code = "#include \"" + header + "\"\n";
    if (!typ.empty()) code += "static_assert(sizeof(" + typ + ") > 0, \"\");\nextern " + typ + " cling_scan_probe;\n";

    std::vector<std::string> args = {"-std=" + std_, "-fsyntax-only", "-x", "c++"};
    if (!resource.empty()) { args.push_back("-resource-dir"); args.push_back(resource); }
    for (const auto& i : includes) args.push_back("-I" + i);
    for (const auto& e : extra) args.push_back(e);

    Result result;
    tooling::runToolOnCodeWithArgs(std::make_unique<Action>(result, cls, typ), code, args, "tu.cc");
    if (!result.error.empty()) {
        std::fprintf(stderr, "%s\n", result.error.c_str());
        return 1;
    }
    std::ostringstream out;
    out << "version\t" << CLANG_VERSION_STRING << "\n";
    out << "class\t" << result.cls << "\t" << result.classCount << "\n";
    for (const auto& d : result.deps) out << "dep\t" << d << "\n";
    for (const auto& e : result.enums) out << "enum\t" << e << "\n";
    for (const auto& s : result.skipped) out << "skipped\t" << s << "\n";
    for (const auto& m : result.methods) {
        out << "method\t" << m.name << "\t" << (m.isStatic ? 1 : 0) << "\t" << (m.isConst ? 1 : 0) << "\t" << m.ret << "\t" << m.params.size();
        for (const auto& p : m.params) out << "\t" << p;
        out << "\n";
    }
    std::cout << out.str();
    return 0;
}
