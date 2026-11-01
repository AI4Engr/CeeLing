//@ cpp: compiles
//@ warns: none
// The boilerplate for "read a whole file". The stream is opened in text mode: on Windows that translates CRLF, which corrupts binary data.
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

std::string readFileToString(const std::string& path) {
    std::ifstream fin(path);
    if (!fin) {
        return std::string();
    }
    std::stringstream ss;
    ss << fin.rdbuf();
    return ss.str();
}

int main() {
    std::cout << readFileToString("no_such_file.txt").size() << std::endl;
    return 0;
}
