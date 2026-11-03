#include <map>
#include <string>
// two key types: the facts of a template must not depend on its arguments (the "witness" check in gen_pack.py)
template class std::map<int, int>;
template class std::map<std::string, int>;
