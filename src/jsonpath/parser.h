#ifndef PARSER_H
#define PARSER_H 1

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <Zend/zend_smart_str.h>

#define MAX_NODE_DEPTH 5
#define PARSE_BUF_LEN 50


/* AST stuff */

enum ast_type {
    AST_AND,
    AST_BOOL,
    AST_EQ,
    AST_EXPR_END,
    AST_EXPR_START,
    AST_FILTER,
    AST_GT,
    AST_GTE,
    AST_INDEX_LIST,
    AST_INDEX_SLICE,
    AST_ISSET,
    AST_LITERAL_BOOL,
    AST_LITERAL,
    AST_LT,
    AST_LTE,
    AST_NE,
    AST_OR,
    AST_PAREN_LEFT,
    AST_PAREN_RIGHT,
    AST_RECURSE,
    AST_RGXP,
    AST_ROOT,
    AST_SELECTOR,
    AST_WILD_CARD
};

union ast_node_data {
    struct {
        int count;
        union ast_node_data* names[3];
        int indexes[3];
    } d_list;
    struct {
        char value[PARSE_BUF_LEN];
    } d_selector;
    struct {
        char value[PARSE_BUF_LEN];
    } d_literal;
    struct {
        struct ast_node* node;
    } d_expression;
};

struct ast_node {
    struct ast_node* next;
    const char* type_s;
    enum ast_type type;
    union ast_node_data data;
};

/* AST stuff */

typedef struct {
    expr_op_type type;
    char value[PARSE_BUF_LEN];
    bool value_bool;
    char* label[MAX_NODE_DEPTH];
    int label_count;
} expr_operator;

typedef struct {
    operator_type type;
    char* node_value;
    int node_value_len;
    filter_type filter_type;
    int index_count;
    int indexes[PARSE_BUF_LEN];
    expr_operator* expressions;
    int expression_count;
} operator;

typedef struct {
    char msg[PARSE_BUF_LEN];
} parse_error;

bool tokenize(char** input, operator * tok);

typedef bool(*compare_cb) (expr_operator*, expr_operator*);

void convert_to_postfix(expr_operator* expr_in, int in_count, expr_operator* expr_out, int* out_count);
bool evaluate_postfix_expression(expr_operator* expr, int count);
compare_cb exec_cb_by_token(expr_op_type);
operator_type get_token_type(expr_op_type);

bool compare_lt(expr_operator* lh, expr_operator* rh);
bool compare_lte(expr_operator* lh, expr_operator* rh);
bool compare_gt(expr_operator* lh, expr_operator* rh);
bool compare_gte(expr_operator* lh, expr_operator* rh);
bool compare_and(expr_operator* lh, expr_operator* rh);
bool compare_or(expr_operator* lh, expr_operator* rh);
bool compare_eq(expr_operator* lh, expr_operator* rh);
bool compare_neq(expr_operator* lh, expr_operator* rh);
bool compare_isset(expr_operator* lh, expr_operator* rh);	// lh = rh
bool compare_rgxp(expr_operator* lh, expr_operator* rh);

bool build_parse_tree(
	lex_token lex_tok[PARSE_BUF_LEN],
	char lex_tok_values[][PARSE_BUF_LEN],
	int* start,
	int lex_tok_count,
	struct ast_node* head,
	parse_error* err
);

void parseFilterList(
	lex_token lex_tok[PARSE_BUF_LEN],
	char lex_tok_values[][PARSE_BUF_LEN],
	int* start,
	int lex_tok_count,
	struct ast_node* tok
);

void* jpath_malloc(size_t size);

#endif				/* PARSER_H */
