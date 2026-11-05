#include "library_summary.h"

#include <set>

#include "fact_table.h"

namespace cling {

namespace {
// "std::vector<Node>" or "vector<Node>" -- accept both the qualified and bare form,
// matching how `type` strings are captured verbatim from source text (a user could
// plausibly write either, though every fixture uses the qualified form).
constexpr const char* kQualifiedPrefix = "std::vector<";
constexpr const char* kBarePrefix = "vector<";

constexpr const char* kMapQualifiedPrefix = "std::map<";
constexpr const char* kMapBarePrefix = "map<";

// Strips a prefix/trailing '>' the same way vectorElementType does, returning the
// raw (unsplit) argument list text, e.g. "int, string". Empty if `type` isn't a
// map<...> shape.
std::string mapArgsText(const std::string& type) {
    size_t prefixLen = 0;
    if (type.rfind(kMapQualifiedPrefix, 0) == 0) {
        prefixLen = std::string(kMapQualifiedPrefix).size();
    } else if (type.rfind(kMapBarePrefix, 0) == 0) {
        prefixLen = std::string(kMapBarePrefix).size();
    } else {
        return {};
    }
    if (type.empty() || type.back() != '>') return {};
    return type.substr(prefixLen, type.size() - prefixLen - 1);
}

// Splits "K, V" on the top-level comma (not one nested inside a further '<...>',
// though nested generics as map arguments are rejected downstream by
// isPrimitiveType regardless -- this split just needs to not misparse them into
// garbage key/value strings before that rejection happens), trims surrounding
// whitespace. Returns {"", ""} if there isn't exactly one top-level comma.
std::pair<std::string, std::string> splitMapArgs(const std::string& argsText) {
    int depth = 0;
    size_t commaPos = std::string::npos;
    for (size_t i = 0; i < argsText.size(); ++i) {
        const char c = argsText[i];
        if (c == '<') ++depth;
        else if (c == '>') --depth;
        else if (c == ',' && depth == 0) {
            if (commaPos != std::string::npos) return {};  // more than one top-level comma
            commaPos = i;
        }
    }
    if (commaPos == std::string::npos) return {};

    auto trim = [](std::string s) {
        size_t start = s.find_first_not_of(" \t");
        size_t end = s.find_last_not_of(" \t");
        if (start == std::string::npos) return std::string{};
        return s.substr(start, end - start + 1);
    };
    return {trim(argsText.substr(0, commaPos)), trim(argsText.substr(commaPos + 1))};
}
} // namespace

bool isVectorType(const std::string& type) {
    return !vectorElementType(type).empty();
}

std::string vectorElementType(const std::string& type) {
    size_t prefixLen = 0;
    if (type.rfind(kQualifiedPrefix, 0) == 0) {
        prefixLen = std::string(kQualifiedPrefix).size();
    } else if (type.rfind(kBarePrefix, 0) == 0) {
        prefixLen = std::string(kBarePrefix).size();
    } else {
        return {};
    }
    if (type.empty() || type.back() != '>') return {};
    return type.substr(prefixLen, type.size() - prefixLen - 1);
}

namespace {

// ADMISSION BY RULE (2026-11-02). There is no list of admitted std::vector / std::string / std::map method names. The scan of
// the library's own declarations (facts/std-vector.clfacts, std-string.clfacts, std-map.clfacts) holds one fact per SIGNATURE:
// what it returns, whether it changes the receiver, and the kind of each parameter. A name is admitted when every signature
// CeeLing code could actually call returns a shape the checkers can honour. "Honour" means: after the call, the analysis still
// knows what the result may alias, so admitting it does not open a hole.
//   void, scalar      nothing to alias.
//   string            a fresh std::string (a value type).
//   self-reference    `append` & co return *this; CeeLing binds no observer to a call result, so it is used as a statement
//                     or a temporary.
// Everything else stays out until the checker can follow it, and the reason is reported with the rejected call:
//   raw-pointer       sealed: a raw pointer has nowhere legal to go (data(), c_str()).
//   iterator-view     an iterator dangles when the container grows; iterators are not tracked.
//   element-reference at()/front()/back(): only `v[i]` is tracked as an observer of the container.
//   view              string_view conversion: views are not tracked.
//   value             a type CeeLing has no shape for (std::allocator, a pair, a comparison object).
//   unresolved        the scan could not determine the result.
// A signature is CALLABLE unless a parameter is of a kind CeeLing has no value of: an iterator, an initializer_list, an
// allocator, a pair or map node, a pointer to writable memory (`vector::erase(const_iterator)` is not callable, `map::erase(key)`
// is). Every call that returns an iterator is refused, a string literal is the only `const char *`, and CeeLing has no pairs,
// so such an overload can never be written; leaving it out is exact, and it is what lets `map::erase(key)` and
// `string::insert(pos, text)` in while `erase(iterator)` stays out. A parameter pack (`emplace_back(args...)`) is callable.
// A callable signature with a non-const reference parameter (`swap(other)`, `merge(other)`) is out: CeeLing moves a container
// argument by default, which would empty the other operand.
bool shapeHonoured(const std::string& shape, const std::string& cls = "") {
    // A `view` result (std::string_view::substr) is a view of the SAME owner as the receiver: the checker follows it, but only for a string_view receiver.
    if (shape == "view" && cls == "string_view") return true;
    // An optional holds a number, a bool, a char or a string (nothing else is allowed, see isOptionalElementAllowed). A REFERENCE to that element
    // (`value()`) can only be consumed as a value here: CeeLing cannot bind a reference to a primitive or a string, and a call result is not a valid
    // origin for a view. So the reference is a copy by the time anything can see it, and the element type itself (`value_or` returns it by value) is fresh.
    if (cls == "optional" && (shape == "element-reference" || shape == "value")) return true;
    return shape == "void" || shape == "scalar" || shape == "string" || shape == "self-reference";
}

std::string shapeRefusal(const std::string& shape) {
    if (shape == "raw-pointer") return "it returns a raw pointer, and raw pointers have nowhere legal to go";
    if (shape == "iterator-view") return "it returns an iterator, and iterators are not tracked yet, so nothing would stop one from dangling";
    if (shape == "element-reference") return "it returns a reference to an element, and only `v[i]` is tracked as an observer so far";
    if (shape == "view") return "it returns a view (string_view), and views are not tracked yet";
    if (shape == "value") return "it returns a type CeeLing has no shape for";
    return "the scan of the standard library could not determine what it returns";
}

// The parameter kinds of a signature fact, from its subject `name/const|nonconst/kind+kind`.
std::vector<std::string> signatureKinds(const std::string& subject) {
    size_t second = subject.find('/', subject.find('/') + 1);
    std::vector<std::string> kinds;
    if (second == std::string::npos) return kinds;
    std::string rest = subject.substr(second + 1);
    size_t start = 0;
    while (start <= rest.size()) {
        size_t plus = rest.find('+', start);
        std::string k = rest.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        if (!k.empty() && k != "-") kinds.push_back(k);
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    return kinds;
}

bool signatureCallable(const std::vector<std::string>& kinds) {
    for (const auto& k : kinds) {
        if (k == "iterator" || k == "initlist" || k == "allocator" || k == "pair" || k == "ptr-w") return false;
    }
    return true;
}

struct MethodVerdict {
    bool admitted = false;
    std::string reason;   // why not, when !admitted
};

MethodVerdict methodVerdict(const std::string& cls, const std::string& name) {
    const FactTable& facts = builtinFacts();
    if (facts.has(cls + ".own", name)) return {true, ""};   // a method CeeLing itself defines (copyWithin)
    std::vector<const FactTable::Fact*> sigs = facts.withPrefix(cls + ".sig", name + "/");
    if (sigs.empty()) return {false, "the standard library has no such member"};
    bool anyCallable = false;
    for (const FactTable::Fact* f : sigs) {
        std::vector<std::string> kinds = signatureKinds(f->subject);
        if (!signatureCallable(kinds)) continue;
        anyCallable = true;
        for (const auto& k : kinds) {
            if (k == "mref") return {false, "an overload takes a non-const reference parameter, and CeeLing would move the argument out"};
        }
        const std::string& results = f->attrs.at("result");
        size_t start = 0;
        while (start <= results.size()) {
            size_t comma = results.find(',', start);
            std::string shape = results.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if (!shapeHonoured(shape, cls)) return {false, shapeRefusal(shape)};
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    if (!anyCallable) {
        return {false, "every overload needs an argument CeeLing cannot write (an iterator, a pair, an initializer_list, a pointer to writable memory)"};
    }
    return {true, ""};
}

// Does any signature of `name` change the receiver (so every observer of the container dies)? Counted over ALL signatures, callable or not.
bool methodMutates(const std::string& cls, const std::string& name) {
    for (const FactTable::Fact* f : builtinFacts().withPrefix(cls + ".sig", name + "/")) {
        if (f->attrs.at("mutates") == "yes") return true;
    }
    return false;
}

}  // namespace

bool isKnownVectorMethod(const std::string& methodName) {
    return methodVerdict("vector", methodName).admitted;
}

std::string vectorMethodRefusal(const std::string& methodName) {
    return methodVerdict("vector", methodName).reason;
}

std::string stringMethodRefusal(const std::string& methodName) {
    return methodVerdict("string", methodName).reason;
}

bool vectorMethodSharesArg(const std::string& methodName, size_t argIndex) {
    // push_back(x) links x into the container (like a field store), the same way
    // `a.b = obj;` does not consume `obj` -- see dangling_checker.cpp's member-
    // target-assignment handling for the precedent this follows.
    return argIndex == 0 && builtinFacts().hasFlag("vector.note", methodName, "shares-arg0");
}

bool vectorMethodInvalidatesReferences(const std::string& methodName) {
    // pop_back doesn't reallocate (capacity is unchanged), but it DOES destroy
    // the popped-back element itself, so any existing reference/observer of
    // that element is dangling afterward -- conservatively treated the same
    // as push_back's whole-container invalidation, since this codebase's
    // model tracks invalidation at the container level, not per-element (see
    // Chapter 18 Step 14's design note on this granularity choice).
    // Derived, not listed: a method that may change the receiver (a non-const member with no const twin, found by the scan,
    // `mutates=yes` in facts/std-vector.clfacts) invalidates. That is push_back, pop_back, resize, reserve, assign, clear,
    // shrink_to_fit, ... and nothing a name list could forget.
    return methodMutates("vector", methodName);
}

bool stringMethodInvalidatesReferences(const std::string& methodName) {
    return methodMutates("string", methodName);
}

bool isStringType(const std::string& type) {
    return type == "string" || type == "std::string";
}

bool isStringViewType(const std::string& type) {
    return type == "string_view" || type == "std::string_view";
}

bool isKnownStringViewMethod(const std::string& methodName) {
    return methodVerdict("string_view", methodName).admitted;
}

std::string stringViewMethodRefusal(const std::string& methodName) {
    return methodVerdict("string_view", methodName).reason;
}

// Does this method of a string_view return another view of the SAME owner (substr)? Read from the scan, not listed.
bool stringViewMethodReturnsView(const std::string& methodName) {
    for (const FactTable::Fact* f : builtinFacts().withPrefix("string_view.sig", methodName + "/")) {
        const std::string& r = f->attrs.at("result");
        if (r == "view" || r.find("view,") == 0 || r.find(",view") != std::string::npos) return true;
    }
    return false;
}

namespace {
std::string stripStdPrefix(const std::string& type) {
    return type.rfind("std::", 0) == 0 ? type.substr(5) : type;
}
}  // namespace

bool isOptionalType(const std::string& type) {
    const std::string t = stripStdPrefix(type);
    return t.size() > 10 && t.rfind("optional<", 0) == 0 && t.back() == '>';
}

std::string optionalElementType(const std::string& type) {
    if (!isOptionalType(type)) return {};
    const std::string t = stripStdPrefix(type);
    std::string elem = t.substr(9, t.size() - 10);
    size_t a = elem.find_first_not_of(" 	"), b = elem.find_last_not_of(" 	");
    return a == std::string::npos ? std::string{} : elem.substr(a, b - a + 1);
}

bool isOptionalElementAllowed(const std::string& elem) {
    return isPrimitiveType(elem);        // int float double bool char byte int64 string (isPrimitiveType includes string)
}

bool isKnownOptionalMethod(const std::string& methodName) {
    return methodVerdict("optional", methodName).admitted;
}

std::string optionalMethodRefusal(const std::string& methodName) {
    return methodVerdict("optional", methodName).reason;
}

bool isKnownMapMethod(const std::string& methodName) {
    return methodVerdict("map", methodName).admitted;
}

std::string mapMethodRefusal(const std::string& methodName) {
    return methodVerdict("map", methodName).reason;
}

bool mapMethodInvalidatesReferences(const std::string& methodName) {
    // Coarser than the standard (insert does not invalidate references into a map, erase only those of the erased element):
    // any method that changes the map kills its observers. Conservative, and no map observer can be written today (a map holds
    // primitives only, and `ref` to a primitive does not compile), so nothing is lost.
    return methodMutates("map", methodName);
}

bool isKnownStringMethod(const std::string& methodName) {
    return methodVerdict("string", methodName).admitted;
}

bool isOstreamType(const std::string& type) {
    return type == "ostream";
}

bool isMapType(const std::string& type) {
    const auto [key, value] = splitMapArgs(mapArgsText(type));
    return !key.empty() && !value.empty();
}

std::string mapKeyType(const std::string& type) {
    return splitMapArgs(mapArgsText(type)).first;
}

std::string mapValueType(const std::string& type) {
    return splitMapArgs(mapArgsText(type)).second;
}

bool isPrimitiveType(const std::string& type) {
    static const std::set<std::string> kKnown = {
        "int", "float", "double", "bool", "string", "std::string", "char", "byte", "int64"};
    return kKnown.count(type) > 0;
}

bool isPrimitiveConversionName(const std::string& name) {
    static const std::set<std::string> kKnown = {"int", "float", "double", "char", "byte", "int64"};
    return kKnown.count(name) > 0;
}

bool isCharType(const std::string& type) {
    return type == "char";
}

bool isKnownQualifiedFreeFunction(const std::string& qualifiedName) {
    // std::stoi / stod / to_string, the project shims (facts/shims.clfacts) and Eigen's static factory (facts/eigen.clfacts).
    // The check is a bare string match on the called name, whether or not a namespace qualifier is present.
    return builtinFacts().has("function", qualifiedName);
}

bool isKnownExternalClassType(const std::string& type) {
    return builtinFacts().has("class", type) || builtinFacts().has("opaque", type);
}

bool isOpaqueExternalClass(const std::string& type) {
    return builtinFacts().has("opaque", type);
}

bool isConservativeOpaqueCall(const std::string& className, const std::string& methodName) {
    const FactTable& f = builtinFacts();
    if (!f.has("opaque", className)) return false;
    const std::string key = className + "::" + methodName;
    return !f.has("borrow", key) && !f.has("fresh", key);
}

bool isKnownExternalOperator(const std::string& type, const std::string& operatorStr) {
    return builtinFacts().has("operator", type + "|" + operatorStr);
}

bool isKnownBorrowingMethod(const std::string& className, const std::string& methodName) {
    // Not exhaustive -- extend by consulting Eigen's own Block/Transpose/Ref/Map/Diagonal/Reverse expression-template
    // family (see this function's header doc comment), never by guessing. Each entry gets REAL dangling tracking
    // (DanglingChecker::resolveBorrowingMethodReceiver), unlike isKnownExternalOperator's set.
    // An opaque class's unlisted method is assumed to borrow the receiver (see isOpaqueExternalClass).
    return builtinFacts().has("borrow", className + "::" + methodName) || isConservativeOpaqueCall(className, methodName);
}

bool isKnownFreshMethod(const std::string& className, const std::string& methodName) {
    return builtinFacts().has("fresh", className + "::" + methodName);
}

bool isKnownMutatorMethod(const std::string& className, const std::string& methodName) {
    return builtinFacts().has("mutator", className + "::" + methodName);
}

bool callKillsReceiverObservers(const std::string& className, const std::string& methodName) {
    return isConservativeOpaqueCall(className, methodName) || isKnownMutatorMethod(className, methodName);
}

std::vector<std::string> knownExternalClassTypes() {
    std::vector<std::string> out;
    for (const auto& f : builtinFacts().facts()) {
        if (f.kind == "class" || f.kind == "opaque") out.push_back(f.subject);
    }
    return out;
}

bool isKnownRuntimeHelper(const std::string& qualifiedName) {
    // cling::spawn is produced only by closure_desugar's `spawn(closure ...)` rewrite (a hand-written call is
    // rejected there): it is what makes the sendable-capture check unavoidable.
    static const std::set<std::string> kKnown = {"cling::readFileToString", "cling::fileExists",
                                                  "cling::readFileToBytes", "cling::spawn",
                                                  "cling::setJobFailure", "cling::clearJobFailure",
                                                  "cling::setWorkerLimit", "cling::workerLimit", "cling::joinAllWorkers",
                                                  "cling::setExitGraceMs"};
    return kKnown.count(qualifiedName) > 0;
}

std::string requiredRuntimeHelperDefinitions(const std::set<std::string>& used) {
    std::string out;
    bool needNamespace = used.count("cling::readFileToString") > 0 ||
                          used.count("cling::fileExists") > 0 ||
                          used.count("cling::readFileToBytes") > 0;
    if (needNamespace) out += "namespace cling {\n";
    if (used.count("cling::fileExists") > 0) {
        out +=
            "inline bool fileExists(const std::string& path) {\n"
            "    std::ifstream fin(path);\n"
            "    return static_cast<bool>(fin);\n"
            "}\n";
    }
    if (used.count("cling::readFileToString") > 0) {
        out +=
            "inline std::string readFileToString(const std::string& path) {\n"
            "    std::ifstream fin(path);\n"
            "    if (!fin) { return std::string(); }\n"
            "    std::stringstream ss;\n"
            "    ss << fin.rdbuf();\n"
            "    return ss.str();\n"
            "}\n";
    }
    if (used.count("cling::readFileToBytes") > 0) {
        out +=
            "inline std::vector<int> readFileToBytes(const std::string& path) {\n"
            "    std::ifstream fin(path, std::ios::binary);\n"
            "    if (!fin) { return std::vector<int>(); }\n"
            "    std::vector<int> bytes;\n"
            "    char c;\n"
            "    while (fin.get(c)) {\n"
            "        bytes.push_back(static_cast<int>(static_cast<unsigned char>(c)));\n"
            "    }\n"
            "    return bytes;\n"
            "}\n";
    }
    if (needNamespace) out += "} // namespace cling\n";
    if (used.count("cling::spawn") > 0 || used.count("cling::setJobFailure") > 0 ||
        used.count("cling::clearJobFailure") > 0 || used.count("cling::setWorkerLimit") > 0 ||
        used.count("cling::workerLimit") > 0 || used.count("cling::joinAllWorkers") > 0 ||
        used.count("cling::setExitGraceMs") > 0) {
        // The spawn runtime: a lazily grown, capped worker pool (see the comment inside the emitted text).
        out +=
            "#ifndef CLING_SPAWN_DEFINED\n"
            "#define CLING_SPAWN_DEFINED\n"
            "#include <chrono>\n"
            "#include <condition_variable>\n"
            "#include <cstdio>\n"
            "#include <cstdlib>\n"
            "#include <deque>\n"
            "#include <exception>\n"
            "#include <functional>\n"
            "#include <memory>\n"
            "#include <mutex>\n"
            "#include <thread>\n"
            "#include <vector>\n"
            "// Upper bound on worker threads. 0 = max(4, hardware_concurrency()). Override with -DCLING_MAX_WORKERS=N.\n"
            "#ifndef CLING_MAX_WORKERS\n"
            "#define CLING_MAX_WORKERS 0\n"
            "#endif\n"
            "namespace cling {\n"
            "// A capped pool, started lazily: a spawn queues its job (FIFO) and a new worker thread is created only while\n"
            "// there are more queued jobs than idle workers and the limit has not been reached; at most \"limit\" jobs run at once. A job never blocks the thread\n"
            "// that spawned it; when every worker is busy it waits in the queue. On process exit the pool finishes every queued\n"
            "// job and joins its threads (it is a function-local static, destroyed before anything constructed earlier).\n"
            "class WorkerPool {\n"
            "public:\n"
            "    ~WorkerPool() {\n"
            "        int grace;\n"
            "        { std::lock_guard<std::mutex> lock(m_); done_ = true; grace = graceMs_; }\n"
            "        cv_.notify_all();\n"
            "        if (grace > 0) {   // setExitGraceMs: do not wait for a job that never ends\n"
            "            std::unique_lock<std::mutex> lock(m_);\n"
            "            if (!idleCv_.wait_for(lock, std::chrono::milliseconds(grace), [this] { return queue_.empty() && busy_ == 0; })) {\n"
            "                const size_t unfinished = busy_ + queue_.size();\n"
            "                lock.unlock();\n"
            "                std::fprintf(stderr, \"cling: %zu job(s) still unfinished %d ms after the program ended; exiting without them\\n\", unfinished, grace);\n"
            "                std::fflush(stdout); std::fflush(stderr);\n"
            "                std::_Exit(0);   // the jobs are abandoned where they stand: no destructors run, nothing is unwound\n"
            "            }\n"
            "        }\n"
            "        for (auto& t : threads_) t.join();\n"
            "    }\n"
            "    // 0 (the default) waits for every job however long it takes; n > 0 gives them n milliseconds, then ends the process.\n"
            "    void setGrace(int ms) { std::lock_guard<std::mutex> lock(m_); graceMs_ = ms < 0 ? 0 : ms; }\n"
            "    void submit(std::function<void()> job) {\n"
            "        std::lock_guard<std::mutex> lock(m_);\n"
            "        queue_.push_back(std::move(job));\n"
            "        if (queue_.size() > idle_ && threads_.size() < limit()) threads_.emplace_back([this] { loop(); });\n"
            "        cv_.notify_one();\n"
            "    }\n"
            "    // Waits until the queue is empty and no job is running (the threads stay alive).\n"
            "    void waitIdle() {\n"
            "        std::unique_lock<std::mutex> lock(m_);\n"
            "        idleCv_.wait(lock, [this] { return queue_.empty() && busy_ == 0; });\n"
            "    }\n"
            "    // 0 restores the default. The limit caps how many jobs run at once, whatever number of threads exist.\n"
            "    void setLimit(size_t n) {\n"
            "        { std::lock_guard<std::mutex> lock(m_); limit_ = n; }\n"
            "        cv_.notify_all();\n"
            "    }\n"
            "    size_t limit() const {\n"
            "        if (limit_ > 0) return limit_;\n"
            "        if (CLING_MAX_WORKERS > 0) return static_cast<size_t>(CLING_MAX_WORKERS);\n"
            "        unsigned n = std::thread::hardware_concurrency();\n"
            "        return n < 4 ? 4 : n;\n"
            "    }\n"
            "    size_t threadCount() {\n"
            "        std::lock_guard<std::mutex> lock(m_);\n"
            "        return threads_.size();\n"
            "    }\n"
            "private:\n"
            "    void loop() {\n"
            "        std::unique_lock<std::mutex> lock(m_);\n"
            "        for (;;) {\n"
            "            ++idle_;\n"
            "            cv_.wait(lock, [this] { return (done_ && queue_.empty()) || (!queue_.empty() && busy_ < limit()); });\n"
            "            --idle_;\n"
            "            if (queue_.empty()) return;   // shutting down and nothing left to do\n"
            "            std::function<void()> job = std::move(queue_.front());\n"
            "            queue_.pop_front();\n"
            "            ++busy_;\n"
            "            lock.unlock();\n"
            "            job();\n"
            "            lock.lock();\n"
            "            --busy_;\n"
            "            if (queue_.empty() && busy_ == 0) idleCv_.notify_all();\n"
            "            cv_.notify_all();   // a slot freed: a worker waiting on the limit may take the next job\n"
            "        }\n"
            "    }\n"
            "    std::mutex m_;\n"
            "    std::condition_variable cv_, idleCv_;\n"
            "    std::deque<std::function<void()>> queue_;\n"
            "    std::vector<std::thread> threads_;\n"
            "    size_t idle_ = 0, busy_ = 0, limit_ = 0;\n"
            "    int graceMs_ = 0;\n"
            "    bool done_ = false;\n"
            "};\n"
            "inline WorkerPool& workers() { static WorkerPool p; return p; }\n"
            "inline void joinWorkers() { workers().waitIdle(); }\n"
            "inline int setWorkerLimit(int n) { workers().setLimit(n < 0 ? 0 : static_cast<size_t>(n)); return 1; }\n"
            "inline int workerLimit() { return static_cast<int>(workers().limit()); }\n"
            "inline int setExitGraceMs(int ms) { workers().setGrace(ms); return 1; }\n"
            "inline int joinAllWorkers() { workers().waitIdle(); return 1; }\n"
            "// Runs `body`; any exception becomes a message handed to `onError`. A job never reaches std::terminate.\n"
            "template <typename Body, typename OnError>\n"
            "inline void runGuarded(Body&& body, OnError&& onError) {\n"
            "    try { body(); }\n"
            "    catch (const std::string& e) { onError(e); }\n"
            "    catch (const std::exception& e) { onError(std::string(e.what())); }\n"
            "    catch (...) { onError(std::string(\"unknown error\")); }\n"
            "}\n"
            "// The failure handler registered on the CALLING thread (onJobFailure): inherited by every spawn made from this thread\n"
            "// that has no handler of its own.\n"
            "inline std::function<void(const std::string&)>& threadJobFailure() {\n"
            "    thread_local std::function<void(const std::string&)> hook;\n"
            "    return hook;\n"
            "}\n"
            "template <typename H>\n"
            "inline void setJobFailure(H handler) {\n"
            "    auto kept = std::make_shared<H>(std::move(handler));\n"
            "    threadJobFailure() = [kept](const std::string& message) { (*kept)->fail(message); };\n"
            "}\n"
            "inline int clearJobFailure() { threadJobFailure() = nullptr; return 1; }\n"
            "template <typename J>\n"
            "inline void spawn(J job) {\n"
            "    auto holder = std::make_shared<J>(std::move(job));\n"
            "    std::function<void(const std::string&)> hook = threadJobFailure();\n"
            "    workers().submit([holder, hook] {\n"
            "        runGuarded([&] { (*holder)->run(); }, [&](const std::string& message) {\n"
            "            if (!hook) {\n"
            "                std::fprintf(stderr, \"cling: unhandled error in spawned job: %s\\n\", message.c_str());\n"
            "                return;\n"
            "            }\n"
            "            runGuarded([&] { hook(message); }, [&](const std::string& inner) {\n"
            "                std::fprintf(stderr, \"cling: spawned job failed (%s) and so did its failure handler (%s)\\n\",\n"
            "                             message.c_str(), inner.c_str());\n"
            "            });\n"
            "        });\n"
            "    });\n"
            "}\n"
            "// With a failure handler of its own: when the job throws, `handler->fail(message)` runs on the same worker thread.\n"
            "template <typename J, typename H>\n"
            "inline void spawn(J job, H handler) {\n"
            "    auto holder = std::make_shared<J>(std::move(job));\n"
            "    auto onFail = std::make_shared<H>(std::move(handler));\n"
            "    workers().submit([holder, onFail] {\n"
            "        runGuarded([&] { (*holder)->run(); }, [&](const std::string& message) {\n"
            "            runGuarded([&] { (*onFail)->fail(message); }, [&](const std::string& inner) {\n"
            "                std::fprintf(stderr, \"cling: spawned job failed (%s) and so did its failure handler (%s)\\n\",\n"
            "                             message.c_str(), inner.c_str());\n"
            "            });\n"
            "        });\n"
            "    });\n"
            "}\n"
            "} // namespace cling\n"
            "#endif\n";
    }
    return out;
}

bool isChannelType(const std::string& type) {
    return !channelElementType(type).empty();
}

std::string channelElementType(const std::string& type) {
    static const std::string kPrefix = "Channel<";
    if (type.rfind(kPrefix, 0) != 0 || type.empty() || type.back() != '>') return {};
    std::string inner = type.substr(kPrefix.size(), type.size() - kPrefix.size() - 1);
    const size_t first = inner.find_first_not_of(" \t");
    const size_t last = inner.find_last_not_of(" \t");
    if (first == std::string::npos) return {};
    return inner.substr(first, last - first + 1);
}

bool isKnownChannelMethod(const std::string& methodName, size_t argCount) {
    if (methodName == "send" || methodName == "wait") return argCount == 1;
    return (methodName == "take" || methodName == "size" || methodName == "close" || methodName == "isClosed") && argCount == 0;
}

std::string channelHelperDefinition() {
    return
        "#ifndef CLING_CHANNEL_DEFINED\n"
        "#define CLING_CHANNEL_DEFINED\n"
        "#include <chrono>\n"
        "#include <condition_variable>\n"
        "#include <deque>\n"
        "#include <memory>\n"
        "#include <mutex>\n"
        "#include <string>\n"
        "namespace cling {\n"
        "// A handle to a queue of plain values shared by threads. Copying the handle gives another handle to the same queue; the\n"
        "// move operations are deliberately NOT declared, so std::move(handle) is a copy and a creator's handle never becomes empty.\n"
        "// send never blocks: it fails when the channel is closed or holds `capacity` values (default 1024).\n"
        "template <typename T>\n"
        "class Channel {\n"
        "    struct State {\n"
        "        std::mutex m;\n"
        "        std::condition_variable cv;\n"
        "        std::deque<T> items;\n"
        "        bool closed = false;\n"
        "        size_t capacity = 1024;\n"
        "    };\n"
        "    std::shared_ptr<State> s_;\n"
        "public:\n"
        "    Channel() : s_(std::make_shared<State>()) {}\n"
        "    Channel(const Channel&) = default;\n"
        "    Channel& operator=(const Channel&) = default;\n"
        "    bool send(T value) {\n"
        "        {\n"
        "            std::lock_guard<std::mutex> lock(s_->m);\n"
        "            if (s_->closed || s_->items.size() >= s_->capacity) return false;\n"
        "            s_->items.push_back(std::move(value));\n"
        "        }\n"
        "        s_->cv.notify_one();\n"
        "        return true;\n"
        "    }\n"
        "    // true once a value can be taken. ms < 0 waits as long as it takes, 0 does not wait; false on timeout or when the channel\n"
        "    // is closed and empty.\n"
        "    bool wait(int ms) {\n"
        "        std::unique_lock<std::mutex> lock(s_->m);\n"
        "        auto ready = [this] { return !s_->items.empty() || s_->closed; };\n"
        "        if (ms < 0) {\n"
        "            s_->cv.wait(lock, ready);\n"
        "        } else if (ms > 0) {\n"
        "#ifdef __SANITIZE_THREAD__\n"
        "            s_->cv.wait_until(lock, std::chrono::system_clock::now() + std::chrono::milliseconds(ms), ready);\n"
        "#else\n"
        "            s_->cv.wait_for(lock, std::chrono::milliseconds(ms), ready);\n"
        "#endif\n"
        "        }\n"
        "        return !s_->items.empty();\n"
        "    }\n"
        "    T take() {\n"
        "        std::lock_guard<std::mutex> lock(s_->m);\n"
        "        if (s_->items.empty()) throw std::string(\"channel empty\");\n"
        "        T value = std::move(s_->items.front());\n"
        "        s_->items.pop_front();\n"
        "        return value;\n"
        "    }\n"
        "    int size() {\n"
        "        std::lock_guard<std::mutex> lock(s_->m);\n"
        "        return static_cast<int>(s_->items.size());\n"
        "    }\n"
        "    void close() {\n"
        "        {\n"
        "            std::lock_guard<std::mutex> lock(s_->m);\n"
        "            s_->closed = true;\n"
        "        }\n"
        "        s_->cv.notify_all();\n"
        "    }\n"
        "    bool isClosed() {\n"
        "        std::lock_guard<std::mutex> lock(s_->m);\n"
        "        return s_->closed;\n"
        "    }\n"
        "};\n"
        "}  // namespace cling\n"
        "#endif\n";
}

std::string copyWithinHelperDefinition() {
    // See the header's doc comment for why this is a helper rather than a
    // bare std::memmove call, and why the SSE2 fast path's final chunk no
    // longer preserves bytes past dst+len (removed -- see git history).
    return
        "#ifndef CLING_COPY_WITHIN_DEFINED\n"
        "#define CLING_COPY_WITHIN_DEFINED\n"
        "#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)\n"
        "#include <emmintrin.h>\n"
        "#define CLING_COPY_WITHIN_SSE2 1\n"
        "#endif\n"
        "namespace cling {\n"
        "template <typename T>\n"
        "inline void copyWithin(std::vector<T>& v, long long dst, long long src, long long len) {\n"
        "    unsigned char* base = reinterpret_cast<unsigned char*>(v.data());\n"
        "    const size_t n = static_cast<size_t>(len) * sizeof(T);\n"
        "    const size_t d = static_cast<size_t>(dst) * sizeof(T);\n"
        "    const size_t s = static_cast<size_t>(src) * sizeof(T);\n"
        "#ifdef CLING_COPY_WITHIN_SSE2\n"
        "    const size_t total = v.size() * sizeof(T);\n"
        "    const size_t span = (n + 15) & ~static_cast<size_t>(15);\n"
        "    if (n > 0 && d + span <= total && s + span <= total && (n <= 16 || d >= s + n || s >= d + n)) {\n"
        "        size_t i = 0;\n"
        "        while (n - i > 16) {\n"
        "            _mm_storeu_si128(reinterpret_cast<__m128i*>(base + d + i),\n"
        "                             _mm_loadu_si128(reinterpret_cast<const __m128i*>(base + s + i)));\n"
        "            i += 16;\n"
        "        }\n"
        "        // Unconditional 16-byte overwrite of the tail chunk: bytes in\n"
        "        // [d+i+(n-i), d+i+16) -- past the real end of the copy but inside\n"
        "        // this same SIMD store -- are NOT preserved (see header doc\n"
        "        // comment). Safe because d+span<=total/s+span<=total above already\n"
        "        // guarantee this 16-byte load and store both stay inside v.\n"
        "        _mm_storeu_si128(reinterpret_cast<__m128i*>(base + d + i),\n"
        "                         _mm_loadu_si128(reinterpret_cast<const __m128i*>(base + s + i)));\n"
        "        return;\n"
        "    }\n"
        "#endif\n"
        "    std::memmove(base + d, base + s, n);\n"
        "}\n"
        "} // namespace cling\n"
        "#endif\n";
}

std::string boundsCheckHelperDefinition() {
    // The index is compared as unsigned so one test rejects both `i >= size` and `i < 0`.
    // `if constexpr` picks the map-like branch at compile time: a type with a key_type is indexed
    // by KEY (and inserts), so a size comparison would be wrong for it.
    return
        "#ifndef CLING_BOUNDS_CHECK_DEFINED\n"
        "#define CLING_BOUNDS_CHECK_DEFINED\n"
        "#include <cstdio>\n"
        "#include <cstdlib>\n"
        "#include <type_traits>\n"
        "namespace cling {\n"
        "template <typename C, typename = void> struct has_key_type : std::false_type {};\n"
        "template <typename C> struct has_key_type<C, std::void_t<typename C::key_type>> : std::true_type {};\n"
        "template <typename C, typename I>\n"
        "inline decltype(auto) at(C& c, I i, int line) {\n"
        "    if constexpr (has_key_type<std::remove_const_t<C>>::value) {\n"
        "        return c[i];\n"
        "    } else {\n"
        "        if (static_cast<unsigned long long>(i) >= static_cast<unsigned long long>(c.size())) {\n"
        "            std::fprintf(stderr, \"cling: index out of bounds at line %d (index %lld, size %lld)\\n\",\n"
        "                         line, static_cast<long long>(i), static_cast<long long>(c.size()));\n"
        "            std::abort();\n"
        "        }\n"
        "        return c[static_cast<typename std::remove_const_t<C>::size_type>(i)];\n"
        "    }\n"
        "}\n"
        "} // namespace cling\n"
        "#endif\n";
}

std::string poolAllocatorHelperDefinition() {
    // A free-list allocator for the control-block-plus-object that std::allocate_shared requests.
    // Blocks of one size share one free list PER THREAD (two threads building their own trees cannot
    // corrupt each other; a block freed on another thread simply joins that thread's list). The list
    // head is a plain pointer, so there is no destructor to run at thread or process exit while
    // objects are still being released. Chunks are never returned to the system: the point is that a
    // node costs a pointer pop instead of a malloc.
    return
        "#ifndef CLING_POOL_DEFINED\n"
        "#define CLING_POOL_DEFINED\n"
        "#include <cstddef>\n"
        "#include <memory>\n"
        "#include <new>\n"
        "namespace cling {\n"
        "struct PoolNode { PoolNode* next; };\n"
        "template <std::size_t Size, std::size_t Align>\n"
        "struct PoolFor {\n"
        "    // blocks are aligned to what the OBJECT needs (8 for a node of pointers and doubles), not to\n"
        "    // max_align_t (16): rounding every node up to 16 wasted up to 15 bytes of each one\n"
        "    static constexpr std::size_t kAlign = Align < alignof(PoolNode) ? alignof(PoolNode) : Align;\n"
        "    static constexpr std::size_t kRaw = Size < sizeof(PoolNode) ? sizeof(PoolNode) : Size;\n"
        "    static constexpr std::size_t kBlock = (kRaw + kAlign - 1) / kAlign * kAlign;\n"
        "    static inline thread_local PoolNode* head = nullptr;\n"
        "    static void* take() {\n"
        "        if (head != nullptr) { PoolNode* n = head; head = n->next; return n; }\n"
        "        return refill();\n"
        "    }\n"
        "    static void* refill() {\n"
        "        constexpr std::size_t kPer = 65536 / kBlock > 0 ? 65536 / kBlock : 1;\n"
        "        char* chunk = static_cast<char*>(::operator new(kPer * kBlock));\n"
        "        for (std::size_t i = kPer; i-- > 1;) {\n"
        "            PoolNode* n = reinterpret_cast<PoolNode*>(chunk + i * kBlock);\n"
        "            n->next = head;\n"
        "            head = n;\n"
        "        }\n"
        "        return chunk;\n"
        "    }\n"
        "    static void give(void* p) noexcept { PoolNode* n = static_cast<PoolNode*>(p); n->next = head; head = n; }\n"
        "};\n"
        "template <class T>\n"
        "struct PoolAllocator {\n"
        "    using value_type = T;\n"
        "    PoolAllocator() noexcept = default;\n"
        "    template <class U> PoolAllocator(const PoolAllocator<U>&) noexcept {}\n"
        "    T* allocate(std::size_t n) {\n"
        "        if (n == 1 && alignof(T) <= alignof(std::max_align_t)) return static_cast<T*>(PoolFor<sizeof(T), alignof(T)>::take());\n"
        "        return static_cast<T*>(::operator new(n * sizeof(T)));\n"
        "    }\n"
        "    void deallocate(T* p, std::size_t n) noexcept {\n"
        "        if (n == 1 && alignof(T) <= alignof(std::max_align_t)) PoolFor<sizeof(T), alignof(T)>::give(p);\n"
        "        else ::operator delete(p);\n"
        "    }\n"
        "};\n"
        "template <class A, class B> bool operator==(const PoolAllocator<A>&, const PoolAllocator<B>&) noexcept { return true; }\n"
        "template <class A, class B> bool operator!=(const PoolAllocator<A>&, const PoolAllocator<B>&) noexcept { return false; }\n"
        "} // namespace cling\n"
        "#endif\n";
}

std::string nullCheckHelperDefinition() {
    // cling::nn(p, where) passes p through unchanged when it is non-null, otherwise reports `where`
    // (the .clg file:line of the dereference) on stderr and aborts. A forwarding reference in and
    // out: an lvalue handle comes back as the same lvalue (no shared_ptr copy, so no reference-count
    // traffic per dereference) and a temporary (a call result, weak_ptr::lock()) stays alive for the
    // `->` that follows it in the same full-expression.
    return
        "#ifndef CLING_NULL_CHECK_DEFINED\n"
        "#define CLING_NULL_CHECK_DEFINED\n"
        "#include <cstdio>\n"
        "#include <cstdlib>\n"
        "#include <utility>\n"
        "namespace cling {\n"
        "template <typename P>\n"
        "inline P&& nn(P&& p, const char* where) {\n"
        "    if (!p) {\n"
        "        std::fprintf(stderr, \"cling: null dereference at %s\\n\", where);\n"
        "        std::abort();\n"
        "    }\n"
        "    return std::forward<P>(p);\n"
        "}\n"
        "} // namespace cling\n"
        "#endif\n";
}

} // namespace cling
