// A "private library" for the test of --facts: a C++ class CeeLing has never seen. Nothing about it is known to the compiler
// except what grid.clfacts says.
#pragma once
#include <vector>

class Grid {
public:
    Grid(int w, int h) : w_(w), h_(h), cells_(static_cast<size_t>(w) * h, 1.0) {}
    int width() const { return w_; }
    int height() const { return h_; }
    double sum() const { double s = 0; for (double c : cells_) s += c; return s; }
    // a non-owning window onto this grid: it refers to this grid's storage and must not outlive it
    Grid window(int w, int h) const { Grid g(0, 0); g.w_ = w; g.h_ = h; g.borrowed_ = cells_.data(); return g; }
    int windowWidth() const { return w_; }
    // changes the storage in place
    void shrink() { cells_.clear(); w_ = h_ = 0; }
private:
    int w_, h_;
    std::vector<double> cells_;
    const double* borrowed_ = nullptr;
};
