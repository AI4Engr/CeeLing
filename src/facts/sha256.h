#pragma once

#include <string>

namespace cling {

// SHA-256 of `bytes` as 64 lowercase hex digits; binds a fact pack to the headers it was scanned from (`@header`).
std::string sha256Hex(const std::string& bytes);

}  // namespace cling
