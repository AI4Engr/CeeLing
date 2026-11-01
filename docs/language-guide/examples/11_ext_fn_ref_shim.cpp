//@ cpp: companion
#include <string>
#include <vector>

int sumAll(const std::vector<int>& data) {
    int total = 0;
    for (int x : data) total += x;
    return total;
}

int splitTens(int value, int& rest) {
    rest = value % 10;
    return value / 10;
}

int fillText(std::string& out) {
    out = "filled by C++";
    return 0;
}
