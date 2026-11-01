//@ cpp: companion
// The hand-written side. Its signature below hides that it keeps the address.
#include <vector>

static const std::vector<int>* kept = nullptr;

int remember(const std::vector<int>& data) { kept = &data; return 0; }
int firstRemembered() { return (*kept)[0]; }
