//@ cpp: compiles
//@ warns: none
// The same mistake without CeeLing: nothing in C++ says that remember() keeps the address it was given.
// (Not run by the checker: it reads a destroyed vector.)
#include <vector>

static const std::vector<int>* kept = nullptr;

int remember(const std::vector<int>& data) { kept = &data; return 0; }
int firstRemembered() { return (*kept)[0]; }

void fill() {
    std::vector<int> v;
    v.push_back(7);
    remember(v);
}                                    // v is destroyed here

int main() {
    fill();
    return firstRemembered();        // undefined behaviour
}
