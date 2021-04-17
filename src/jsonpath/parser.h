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
    AST_EXPR,
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
    AST_WILD_CARD,
    AST_HEAD
};

extern const char* AST_STR[];

union ast_node_data {
    struct {
        int count;
        union ast_node_data* names[3];
        int indexes[10]; /* todo check for max */
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
        struct ast_node* head;
    } d_expression;
};

struct ast_node {
    struct ast_node* next;
    enum ast_type type;
    union ast_node_data data;
};

typedef struct {
    char msg[PARSE_BUF_LEN];
} parse_error;

bool evaluate_postfix_expression(zval* arr, struct ast_node* tok);
operator_type get_token_type(enum ast_type);

bool compare_lt(zval* lh, zval* rh);
bool compare_lte(zval* lh, zval* rh);
bool compare_gt(zval* lh, zval* rh);
bool compare_gte(zval* lh, zval* rh);
bool compare_and(zval* lh, zval* rh);
bool compare_or(zval* lh, zval* rh);
bool compare_eq(zval* lh, zval* rh);
bool compare_neq(zval* lh, zval* rh);
bool compare_isset(zval* lh, zval* rh);	// lh = rh
bool compare_rgxp(zval* lh, zval* rh);

bool evaluate_subexpression(
    zval* array,
    enum ast_type operator_type,
    struct ast_node* lh_operand, 
    struct ast_node* rh_operand);

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

bool check_parens_balance(lex_token lex_tok[], int lex_tok_count);

#endif				/* PARSER_H */
