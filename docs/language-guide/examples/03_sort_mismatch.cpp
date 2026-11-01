//@ cpp: compiles
//@ warns: none
// Do not run it: the two iterators belong to different vectors, so the range [a.begin(), b.end()) does not exist.
#include <algorithm>
#include <vector>

int main() {
    std::vector<int> a = {3, 1, 2};
    std::vector<int> b = {9, 8, 7};
    std::sort(a.begin(), b.end());
    return 0;
}
