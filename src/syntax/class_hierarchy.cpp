#include "syntax/class_hierarchy.h"

#include <map>
#include <set>

namespace cling {

std::vector<const ClassDeclaration*> classesInBaseFirstOrder(const Program& program) {
    std::map<std::string, const ClassDeclaration*> byName;
    std::vector<const ClassDeclaration*> inputOrder;
    for (const auto& stmt : program.statements) {
        if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            byName[cls->name] = cls;
            inputOrder.push_back(cls);
        }
    }

    // Depth-first emit: a class is emitted only after its base already has been.
    // `emitted` doubles as the cycle guard -- validateInheritance already rejected
    // cycles at parse time, but this stays self-contained rather than trusting that
    // from a different module.
    std::vector<const ClassDeclaration*> ordered;
    std::set<std::string> emitted;

    for (const ClassDeclaration* start : inputOrder) {
        if (emitted.count(start->name) > 0) continue;

        // Walk up to the root, collecting the unemitted chain, then emit it
        // base-first. Bounded by the number of classes, so a (already-rejected)
        // cycle still terminates here rather than spinning.
        std::vector<const ClassDeclaration*> chain;
        std::set<std::string> onChain;
        const ClassDeclaration* cursor = start;
        while (cursor != nullptr && emitted.count(cursor->name) == 0 &&
               onChain.insert(cursor->name).second) {
            chain.push_back(cursor);
            if (cursor->baseClass.empty()) break;
            auto it = byName.find(cursor->baseClass);
            cursor = (it == byName.end()) ? nullptr : it->second;
        }

        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            if (emitted.insert((*it)->name).second) {
                ordered.push_back(*it);
            }
        }
    }

    return ordered;
}

} // namespace cling
