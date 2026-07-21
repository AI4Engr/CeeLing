#include <iostream>
#include <string>
#include <memory>

// Simple test without Tree-sitter for now
// We'll create a mock AST to test the evaluator

#include "core/cst_to_ast/ast.h"
#include "core/generator/evaluator.h"

using namespace cling;

int main() {
    std::cout << "Testing CLing Core Components..." << std::endl;

    try {
        // Create mock AST for: x1 := 42; std::cout << x1 << std::endl;
        auto program = std::make_unique<Program>();

        // x1 := 42;
        auto number42 = std::make_unique<Number>("42");
        auto assignment = std::make_unique<AssignmentStatement>("x1", std::move(number42));
        program->addStatement(std::move(assignment));

        // std::cout << x1 << std::endl;  (built as a `<<` chain expression statement)
        auto coutX1 = std::make_unique<BinaryExpression>(
            std::make_unique<ScopedIdentifier>("std::cout"),
            "<<",
            std::make_unique<Identifier>("x1"));
        auto chain = std::make_unique<BinaryExpression>(
            std::move(coutX1), "<<", std::make_unique<ScopedIdentifier>("std::endl"));
        program->addStatement(std::make_unique<ExpressionStatement>(std::move(chain)));

        std::cout << "AST created successfully" << std::endl;
        std::cout << "Program: " << program->toString() << std::endl;

        // Test evaluator
        Evaluator evaluator;
        auto eval_result = evaluator.evaluate(*program);

        if (eval_result.success) {
            std::cout << "Evaluation successful!" << std::endl;
            for (const auto& output : eval_result.outputs) {
                std::cout << "Output: " << output << std::endl;
            }
        } else {
            std::cout << "Evaluation failed: " << eval_result.error << std::endl;
        }

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}