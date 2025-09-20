#include "core/cst_to_ast/ast.h"
#include "core/cst_to_ast/ast_builder.h"
#include "core/generator/cpp_generator.h"
#include <emscripten.h>
#include <string>
#include <memory>

using namespace cling;

// Global instances for WASM interface
static std::unique_ptr<ASTBuilder> g_ast_builder;

extern "C" {

// Initialize CLing WASM module
EMSCRIPTEN_KEEPALIVE
int init_cling() {
    try {
        g_ast_builder = std::make_unique<ASTBuilder>();
        return 1; // Success
    } catch (...) {
        return 0; // Failure
    }
}

// Parse CLing source code and return AST (as string)
EMSCRIPTEN_KEEPALIVE
char* parse_cling(const char* source) {
    try {
        if (!g_ast_builder) {
            init_cling();
        }

        // Use Tree-sitter parser to build AST from source
        auto program = g_ast_builder->buildProgram(std::string(source));
        if (!program) {
            return nullptr;
        }

        std::string result = program->toString();

        // Allocate memory for result (caller must free)
        char* output = new char[result.length() + 1];
        strcpy(output, result.c_str());
        return output;
    } catch (...) {
        return nullptr;
    }
}

// Generate C++ code from CLing source and return result
EMSCRIPTEN_KEEPALIVE
char* evaluate_cling(const char* source) {
    try {
        if (!g_ast_builder) {
            init_cling();
        }

        // Use Tree-sitter parser to build AST from source
        auto program = g_ast_builder->buildProgram(std::string(source));
        if (!program) {
            return nullptr;
        }

        CppGenerator generator;
        std::string result = generator.generateCpp(*program, std::string(source));

        // Create JSON response
        std::string json = "{\"success\":true";
        json += ",\"cpp_code\":\"";

        // Escape quotes and newlines in the C++ code
        for (char c : result) {
            if (c == '"') json += "\\\"";
            else if (c == '\n') json += "\\n";
            else if (c == '\r') json += "\\r";
            else if (c == '\\') json += "\\\\";
            else json += c;
        }

        json += "\",\"error\":\"\"}";

        char* output = new char[json.length() + 1];
        strcpy(output, json.c_str());
        return output;
    } catch (...) {
        return nullptr;
    }
}

// Free memory allocated by WASM functions
EMSCRIPTEN_KEEPALIVE
void free_result(char* ptr) {
    delete[] ptr;
}

} // extern "C"