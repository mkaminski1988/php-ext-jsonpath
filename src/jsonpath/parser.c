#include "lexer.h"
#include "parser.h"
#include "stack.h"
#include <limits.h>
#include <stdio.h>
#include "safe_string.h"

bool is_unary(enum ast_type);
int get_operator_precedence(enum ast_type);

struct ast_node* ast_alloc_node(enum ast_type type, const char* type_s) {
	// printf("Allocating %s\n", type_s);
	struct ast_node* ptr = emalloc(sizeof(struct ast_node));
	ptr->next = NULL;
	ptr->type = type;
	ptr->type_s = type_s;
	return ptr;
}

struct ast_node* push_operator(stack* s, struct ast_node* cur) {

	if (!s->size || (*stack_top(s)).type == AST_PAREN_LEFT) {
		stack_push(s, cur);
		return NULL;
	}

	struct ast_node* tmp = stack_top(s);

	//TODO compare macro or assign to var?
	if (get_operator_precedence(cur->type) > get_operator_precedence(tmp->type)) {
		stack_push(s, cur);
		return NULL;
	}

	if (get_operator_precedence(cur->type) < get_operator_precedence(tmp->type)) {
		tmp = stack_top(s);
		stack_pop(s);
	}
	else {
		tmp = stack_top(s);
		stack_pop(s);
		stack_push(s, cur);
	}

	return tmp;
}

/* See http://csis.pace.edu/~wolf/CS122/infix-postfix.htm */
bool build_parse_tree(
	lex_token lex_tok[PARSE_BUF_LEN],
	char lex_tok_values[][PARSE_BUF_LEN],
	int* lex_idx,
	int lex_tok_count,
	struct ast_node* head,
	parse_error* err
) {

	struct ast_node* cur = head;
	struct ast_node* tmp;
	stack s;
	stack_init(&s);

	int expr_start_count = 0;

	for (; *lex_idx < lex_tok_count; (*lex_idx)++) {

		switch (lex_tok[*lex_idx]) {
		case LEX_WILD_CARD:
			// todo : create a macro here
			cur->next = ast_alloc_node(AST_WILD_CARD, "AST_WILD_CARD");
			cur = cur->next;
			break;
		case LEX_ROOT:
			cur->next = ast_alloc_node(AST_ROOT, "AST_ROOT");
			cur = cur->next;
			break;
		case LEX_DEEP_SCAN:
			cur->next = ast_alloc_node(AST_RECURSE, "AST_RECURSE");
			cur = cur->next;
			break;
		case LEX_NODE:
			// fall-through
		case LEX_CUR_NODE:
			cur->next = ast_alloc_node(AST_SELECTOR, "AST_SELECTOR");
			cur = cur->next;
			if (lex_tok[*lex_idx] == LEX_CUR_NODE) {
				cur->data.d_selector.child_scope = true;
			} else {
				cur->data.d_selector.child_scope = false;
			}
			strcpy(cur->data.d_selector.value, lex_tok_values[*lex_idx]);
			break;
		case LEX_FILTER_START:
			if (lex_tok[(*lex_idx)+1] == LEX_LITERAL || lex_tok[(*lex_idx)+1] == LEX_SLICE) {
				(*lex_idx)++;
				cur->next = ast_alloc_node(AST_INDEX_SLICE, "AST_INDEX_SLICE");
				cur = cur->next;
				parse_filter_list(lex_tok, lex_tok_values, lex_idx, lex_tok_count, cur);
			}
			// else we should have a wildcard
			break;
		case LEX_EXPR_START:
			cur->next = ast_alloc_node(AST_EXPR_START, "AST_EXPR_START");
			cur = cur->next;
			expr_start_count++;
			break;
		case LEX_PAREN_OPEN:
			stack_push(&s, ast_alloc_node(AST_PAREN_LEFT, "AST_PAREN_LEFT"));
			break;
		case LEX_PAREN_CLOSE:
			if (cur->type == AST_SELECTOR) {
				cur->next = ast_alloc_node(AST_ISSET, "AST_ISSET");
				cur = cur->next;
			} else {
				// TODO move to function
				while (s.size) {
					tmp = stack_top(&s);
					stack_pop(&s);
					if (tmp->type == AST_PAREN_LEFT) {
						break;
					}
					cur->next = tmp;
					cur = cur->next;
				}
			}
			break;
		case LEX_LITERAL:
			cur->next = ast_alloc_node(AST_LITERAL, "AST_LITERAL");
			cur = cur->next;

			if (jp_str_cpy(cur->data.d_literal.value, PARSE_BUF_LEN, lex_tok_values[*lex_idx], strlen(lex_tok_values[*lex_idx])) > 0) {
				strncpy(err->msg, "Buffer size exceeded", sizeof(err->msg));
				return false;
			}
			break;
		case LEX_LITERAL_BOOL:
			cur->next = ast_alloc_node(AST_LITERAL_BOOL, "AST_LITERAL_BOOL");
			cur = cur->next;

			if (jp_str_cpy(cur->data.d_literal.value, PARSE_BUF_LEN, lex_tok_values[*lex_idx], strlen(lex_tok_values[*lex_idx])) > 0) {
				strncpy(err->msg, "Buffer size exceeded", sizeof(err->msg));
				return false;
			}

			if (strcmp(cur->data.d_literal.value, "true") == 0) {
				jp_str_cpy(cur->data.d_literal.value, PARSE_BUF_LEN, "JP_LITERAL_TRUE", 15);
			}
			else if (strcmp(cur->data.d_literal.value, "false") == 0) {
				jp_str_cpy(cur->data.d_literal.value, PARSE_BUF_LEN, "JP_LITERAL_FALSE", 16);
			}
			break;
		case LEX_LT:
			tmp = push_operator(&s, ast_alloc_node(AST_LT, "AST_LT"));
			if (tmp != NULL) {
				cur->next = tmp;
				cur = cur->next;
			}
			break;
		case LEX_LTE:
			tmp = push_operator(&s, ast_alloc_node(AST_LTE, "AST_LTE"));
			if (tmp != NULL) {
				cur->next = tmp;
				cur = cur->next;
			}
			break;
		case LEX_GT:
			tmp = push_operator(&s, ast_alloc_node(AST_GT, "AST_GT"));
			if (tmp != NULL) {
				cur->next = tmp;
				cur = cur->next;
			}
			break;
		case LEX_GTE:
			tmp = push_operator(&s, ast_alloc_node(AST_GTE, "AST_GTE"));
			if (tmp != NULL) {
				cur->next = tmp;
				cur = cur->next;
			}
			break;
		case LEX_NEQ:
			tmp = push_operator(&s, ast_alloc_node(AST_NE, "AST_NE"));
			if (tmp != NULL) {
				cur->next = tmp;
				cur = cur->next;
			}
			break;
		case LEX_EQ:
			tmp = push_operator(&s, ast_alloc_node(AST_EQ, "AST_EQ"));
			if (tmp != NULL) {
				cur->next = tmp;
				cur = cur->next;
			}
			break;
		case LEX_OR:
			tmp = push_operator(&s, ast_alloc_node(AST_OR, "AST_OR"));
			if (tmp != NULL) {
				cur->next = tmp;
				cur = cur->next;
			}
			break;
		case LEX_AND:
			tmp = push_operator(&s, ast_alloc_node(AST_AND, "AST_AND"));
			if (tmp != NULL) {
				cur->next = tmp;
				cur = cur->next;
			}
			break;
		case LEX_RGXP:
			tmp = push_operator(&s, ast_alloc_node(AST_RGXP, "AST_RGXP"));
			if (tmp != NULL) {
				cur->next = tmp;
				cur = cur->next;
			}
			break;
		case LEX_EXPR_END:
			cur->next = ast_alloc_node(AST_EXPR_END, "AST_EXPR_END");
			cur = cur->next;

			/* Remove remaining elements */
			while (s.size > 0) {
				cur->next = stack_top(&s);
				cur = cur->next;
				stack_pop(&s);
			}

			expr_start_count--;
			break;
		default:
			// printf("build_parse_tree: default\n");
			break;
		}
	}

	if (expr_start_count != 0) {
		/* we made it to the end without finding an expression terminator */
		strncpy(err->msg, "Missing filter end ]", sizeof(err->msg));
		return false;
	}

	return true;
}

void parse_filter_list(
	lex_token lex_tok[PARSE_BUF_LEN],
	char lex_tok_values[][PARSE_BUF_LEN],
	int* lex_idx,
	int lex_tok_count,
	struct ast_node* tok
) {
	int slice_count = 0;

	for (; *lex_idx < lex_tok_count; (*lex_idx)++) {
		if (lex_tok[*lex_idx] == LEX_EXPR_END) {
			return;
		}
		else if (lex_tok[*lex_idx] == LEX_CHILD_SEP) {
			tok->type = AST_INDEX_LIST;
			tok->type_s = "AST_INDEX_LIST";
		}
		else if (lex_tok[*lex_idx] == LEX_SLICE) {
			tok->type = AST_INDEX_SLICE;
			tok->type_s = "AST_INDEX_SLICE";

			slice_count++;
			// [:a] => [0:a]
			// [a::] => [a:0:]
			if (slice_count > tok->data.d_list.count) {
				if (slice_count == 1) {
					tok->data.d_list.indexes[tok->data.d_list.count] = INT_MAX;
				}
				else if (slice_count == 2) {
					tok->data.d_list.indexes[tok->data.d_list.count] = INT_MAX;
				}
				tok->data.d_list.count++;
			}
		}
		else if (lex_tok[*lex_idx] == LEX_LITERAL) {
			// printf("parseList: got a literal %s\n", lex_tok_values[*lex_idx]);
			tok->data.d_list.indexes[tok->data.d_list.count] = atoi(lex_tok_values[*lex_idx]);
			tok->data.d_list.count++;
		} else {
			// printf("parse_filter_list, got %s", visible[lex_tok[*lex_idx]]);
			// return;
		}
	}
}

operator_type get_token_type(struct ast_node* tok)
{
	switch (tok->type) {
	case AST_EQ:
	case AST_NE:
	case AST_LT:
	case AST_LTE:
	case AST_GT:
	case AST_GTE:
	case AST_OR:
	case AST_AND:
	case AST_ISSET:
	case AST_RGXP:
		return TYPE_OPERATOR;
	case AST_PAREN_LEFT:
	case AST_PAREN_RIGHT:
		return TYPE_PAREN;
	case AST_LITERAL:
	case AST_LITERAL_BOOL:
	case AST_BOOL:
		/* make eval ast return strings? */
		return TYPE_OPERAND;
	}
}

bool evaluate_postfix_expression(zval* arr, struct ast_node* tok)
{
	stack s;
	stack_init(&s);
	struct ast_node* expr_lh;
	struct ast_node* expr_rh;

	/* Temporary operators that store intermediate evaluations */
	struct ast_node op_true;

	op_true.type = AST_BOOL;
	op_true.data.d_literal.value_bool = true;

	struct ast_node op_false;

	op_false.type = AST_BOOL;
	op_true.data.d_literal.value_bool = false;

	while (tok != NULL) {

		if (tok->type == AST_EXPR_END) {
			break;
		} else if (tok->next == NULL) {
			// TODO error about missing expression end
		}

		switch (get_token_type(tok->type)) {
		case TYPE_OPERATOR:

			if (!is_unary(tok->type)) {
				expr_rh = stack_top(&s);
				stack_pop(&s);
				expr_lh = stack_top(&s);
			}
			else {
				expr_rh = stack_top(&s);
				expr_lh = expr_rh;
			}

			stack_pop(&s);

			if (exec_cb_by_token(tok->type) (expr_lh, expr_rh)) {
				stack_push(&s, &op_true);
			}
			else {
				stack_push(&s, &op_false);
			}

			break;
		case TYPE_OPERAND:
			stack_push(&s, tok);
			break;
		}

		tok = tok->next;
	}


	expr_lh = stack_top(&s);

	return expr_lh->data.d_literal.value_bool;
}

compare_cb exec_cb_by_token(enum ast_type type)
{
	switch (type) {
	case AST_EQ:
		return compare_eq;
	case AST_NE:
		return compare_neq;
	case AST_LT:
		return compare_lt;
	case AST_LTE:
		return compare_lte;
	case AST_GT:
		return compare_gt;
	case AST_GTE:
		return compare_gte;
	case AST_ISSET:
		return compare_isset;
	case AST_OR:
		return compare_or;
	case AST_AND:
		return compare_and;
	case AST_RGXP:
		return compare_rgxp;
	case AST_PAREN_LEFT:
	case AST_PAREN_RIGHT:
	case AST_LITERAL:
	case AST_LITERAL_BOOL:
	case AST_BOOL:
	default:
		printf("Error, no callback for token");
		break;
	}
}

bool is_unary(enum ast_type type)
{
	return type == AST_ISSET;
}

//TODO: Distinguish between operator and token?
int get_operator_precedence(enum ast_type type)
{
	switch (type) {
	case AST_ISSET:
		return 10000;
	case AST_LT:
		return 1000;
	case AST_LTE:
		return 1000;
		break;
	case AST_GT:
		return 1000;
	case AST_GTE:
		return 1000;
	case AST_RGXP:
		return 1000;
	case AST_NE:
		return 900;
	case AST_EQ:
		return 900;
	case AST_AND:
		return 800;
	case AST_OR:
		return 700;
	case AST_PAREN_LEFT:
	case AST_PAREN_RIGHT:
	case AST_LITERAL:
	case AST_LITERAL_BOOL:
	case AST_BOOL:
	default:
		printf("Error, no operator precedence for token");
		break;
	}
}
