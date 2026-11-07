#include "emit/runtime_helpers.h"

#include <set>

// The C++ text of the runtime support that generated programs carry (cling::at, cling::nn, Channel, span, the worker pool, ...).
// Each function returns one self-contained, include-guarded block; AstEmitter puts a block into the output only when the program needs it.

namespace cling {

std::string spanHelperDefinition() {
    return R"CLINGSPAN(#ifndef CLING_SPAN_DEFINED
#define CLING_SPAN_DEFINED
#include <cstdio>
#include <cstdlib>
#include <type_traits>
namespace cling {
// A non-owning view of a vector's elements (CeeLing's span<T>). Every way to narrow it is range-checked; subscripts go through cling::at.
template <typename T>
class span {
public:
    using size_type = size_t;
    span() : p_(nullptr), n_(0) {}
    span(std::vector<std::remove_const_t<T>>& v) : p_(v.data()), n_(v.size()) {}
    // span<const T>: a read-only view may also be made of a vector that is only readable here (an inferred `const&` parameter), and from a span<T>
    template <typename U = T, typename = std::enable_if_t<std::is_const<U>::value>>
    span(const std::vector<std::remove_const_t<T>>& v) : p_(v.data()), n_(v.size()) {}
    template <typename U, typename = std::enable_if_t<std::is_const<T>::value && std::is_same<const U, T>::value && !std::is_const<U>::value>>
    span(const span<U>& o) : p_(o.p_), n_(o.n_) {}
    size_t size() const { return n_; }
    bool empty() const { return n_ == 0; }
    T& operator[](size_t i) const { return p_[i]; }
    span subspan(size_t off, size_t count) const { if (off > n_ || count > n_ - off) fail("subspan", off, count); return span(p_ + off, count); }
    span first(size_t count) const { if (count > n_) fail("first", 0, count); return span(p_, count); }
    span last(size_t count) const { if (count > n_) fail("last", 0, count); return span(p_ + (n_ - count), count); }
    T& front() const { if (n_ == 0) fail("front", 0, 0); return p_[0]; }
    T& back() const { if (n_ == 0) fail("back", 0, 0); return p_[n_ - 1]; }
private:
    span(T* p, size_t n) : p_(p), n_(n) {}
    void fail(const char* what, size_t off, size_t count) const {
        std::fprintf(stderr, "cling: span::%s out of range (offset %zu, count %zu, size %zu)\n", what, off, count, n_);
        std::abort();
    }
    template <typename> friend class span;
    T* p_;
    size_t n_;
};
} // namespace cling
#endif
)CLINGSPAN";
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

std::string vectorAlgorithmHelperDefinition() {
    return R"CLINGALGO(#ifndef CLING_VECTOR_ALGO_DEFINED
#define CLING_VECTOR_ALGO_DEFINED
#include <algorithm>
namespace cling {
template <typename T> inline void vsort(std::vector<T>& v) {
    std::sort(v.begin(), v.end(), [](const T& a, const T& b) { return a < b || (b != b && a == a); });   // NaN last: a strict weak order
}
template <typename T> inline void vreverse(std::vector<T>& v) { std::reverse(v.begin(), v.end()); }
template <typename T, typename U> inline void vfill(std::vector<T>& v, U x) { std::fill(v.begin(), v.end(), static_cast<T>(x)); }
template <typename T, typename U> inline int vindexOf(const std::vector<T>& v, U x) {
    for (size_t i = 0; i < v.size(); ++i) if (v[i] == static_cast<T>(x)) return static_cast<int>(i);
    return -1;
}
template <typename T, typename U> inline bool vcontains(const std::vector<T>& v, U x) { return vindexOf(v, x) >= 0; }
} // namespace cling
#endif
)CLINGALGO";
}

std::string copyWithinHelperDefinition() {
    // A helper rather than a bare memmove: the typical copy is short, where a library call is mostly overhead.
    // The fast path's last chunk does not preserve bytes past dst+len (see the header).
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
    // The index is compared as unsigned, so one test rejects both `i >= size` and `i < 0`.
    // A type with a key_type (map) is indexed by key, so `if constexpr` skips the size test for it.
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
    // A free-list allocator for the block std::allocate_shared requests. Blocks of one size share a free list
    // per thread (a block freed on another thread joins that thread's list). The list head is a plain pointer,
    // so no destructor runs at exit. Chunks are never returned to the system.
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
    // cling::nn(p, where) returns p when it is non-null, otherwise reports `where` (the .clg line) and aborts.
    // It takes and returns a forwarding reference, so an lvalue handle is not copied (no reference-count
    // traffic) and a temporary stays alive for the `->` that follows.
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
