// A class with one method of every shape cling-scan has to decide on. See run_scan_test.sh for the expected verdicts.
#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

class Shapes {
public:
    Shapes() {}
    int count() const { return 0; }                                 // fresh: const, returns a number
    std::size_t big() const { return 0; }                           // fresh: std::size_t is a number too (yaml-cpp found this)
    bool empty() const { return true; }                             // fresh
    std::string name() const { return ""; }                         // fresh: a string by value
    std::vector<double> data() const { return {}; }                 // fresh: a vector of numbers by value
    const double& at(int) const { static double d; return d; }      // borrow: a reference into the receiver
    std::string_view view() const { return ""; }                    // borrow: a view
    const double* raw() const { return nullptr; }                   // borrow: a pointer
    void clear() {}                                                 // mutator: non-const, returns nothing
    int pop() { return 0; }                                         // mutator: non-const, returns a number
    double& ref() { static double d; return d; }                    // no fact: non-const and returns a reference
    static int version() { return 1; }                              // function: static, returns a number
    static Shapes make(int) { return Shapes(); }                    // function: static, takes only numbers: nothing for the result to refer to
    Shapes copy() const { return Shapes(); }                        // no fact: a class by value may be a view
    template <class T> T as() const { return T(); }                 // skipped: member template
    Shapes& operator+=(const Shapes&) { return *this; }             // skipped: operator
    int over(int) const { return 0; }                               // overloads: const scalar ...
    int over(double) { return 0; }                                  // ... and non-const scalar disagree -> no fact
private:
    int hidden() const { return 0; }                                // private: ignored
};

struct Plain {
    int x;
    int getX() const { return x; }
    void setX(int v) { x = v; }
};

// A second class whose name CONTAINS the first one's: clang's name filter prints it as the next top-level node right after Shapes.
class ShapesIterator {
public:
    int viewCount() const { return 0; }                             // must NOT become a fact of Shapes
};
