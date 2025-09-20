#include "tree_sitter/api.h"

#ifdef __cplusplus
extern "C" {
#endif

// Tree-sitter parser declaration for CLing
const TSLanguage *tree_sitter_cling();

#ifdef __cplusplus
}
#endif

// Tree-sitter node type constants for CLing
typedef enum {
    TS_CLING_SOURCE_FILE,
    TS_CLING_STATEMENT,
    TS_CLING_ASSIGNMENT_STATEMENT,
    TS_CLING_PRINT_STATEMENT,
    TS_CLING_EXPRESSION,
    TS_CLING_IDENTIFIER,
    TS_CLING_NUMBER
} TSClingNodeType;