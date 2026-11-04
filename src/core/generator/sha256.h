#pragma once

#include <string>

namespace cling {

// SHA-256 of `bytes`, as 64 lowercase hex digits. Used to bind a fact pack to the exact headers it was scanned from (`@header` lines);
// no other use. Plain FIPS 180-4, no dependency.
std::string sha256Hex(const std::string& bytes);

}  // namespace cling
