//@ cpp: compiles
//@ warns: none
// The C++ side of an ext_fn: the CeeLing file declares `ext_fn double mySqrt(double x);`, this is
// the body, written by hand and linked in. CeeLing never reads it; it believes the declaration.
#include <cmath>

double mySqrt(double x) { return std::sqrt(x); }
