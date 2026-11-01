// A header-only shim: ordinary C++, written by hand. The .clg file #includes it (the line is copied into the generated C++)
// and declares the function with ext_fn.
#pragma once

inline int clampTo(int x, int lo, int hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}
