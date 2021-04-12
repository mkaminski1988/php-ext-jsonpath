#ifndef PARSER_H
#define PARSER_H 1

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <Zend/zend_smart_str.h> /* todo where is zval defined? */

#define PARSE_BUF_LEN 50

typedef enum {
    TYPE_OPERAND,
    TYPE_OPERATOR,
    TYPE_PAREN,
} operator_type;

enum ast_type {
    AST_AND,
    AST_BOOL,
    AST_EQ,
    AST_EXPR_END,
    AST_EXPR_START,
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
        bool child_scope; /* @.selector if true, else $.selector */
    } d_selector;
    struct {
        char value[PARSE_BUF_LEN];
        bool value_bool;
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

typedef struct {
    char msg[PARSE_BUF_LEN];
} parse_error;

typedef bool(*compare_cb) (struct ast_node*, struct ast_node*);

bool evaluate_postfix_expression(zval* arr, struct ast_node* tok);
compare_cb exec_cb_by_token(enum ast_type);
operator_type get_token_type(enum ast_type);

bool compare_lt(struct ast_node* lh, struct ast_node* rh);
bool compare_lte(struct ast_node* lh, struct ast_node* rh);
bool compare_gt(struct ast_node* lh, struct ast_node* rh);
bool compare_gte(struct ast_node* lh, struct ast_node* rh);
bool compare_and(struct ast_node* lh, struct ast_node* rh);
bool compare_or(struct ast_node* lh, struct ast_node* rh);
bool compare_eq(struct ast_node* lh, struct ast_node* rh);
bool compare_neq(struct ast_node* lh, struct ast_node* rh);
bool compare_isset(struct ast_node* lh, struct ast_node* rh);	// lh = rh
bool compare_rgxp(struct ast_node* lh, struct ast_node* rh);

bool build_parse_tree(
	lex_token lex_tok[PARSE_BUF_LEN],
	char lex_tok_values[][PARSE_BUF_LEN],
	int* lex_idx,
	int lex_tok_count,
	struct ast_node* head,
	parse_error* err
);

void parse_filter_list(
	lex_token lex_tok[PARSE_BUF_LEN],
	char lex_tok_values[][PARSE_BUF_LEN],
	int* start,
	int lex_tok_count,
	struct ast_node* tok
);

void* jpath_malloc(size_t size);

#endif				/* PARSER_H */
