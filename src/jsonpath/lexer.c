#include "lexer.h"
#include <string.h>
#ifndef S_SPLINT_S
#include <ctype.h>
#endif
#include "safe_string.h"
#include <stdbool.h>

static bool extract_quoted_literal(char* p, lex_token *tok, lex_error* err);
static void extract_unbounded_literal(char* p, lex_token *tok);
static void extract_unbounded_numeric_literal(char* p, lex_token *tok);
static void extract_boolean_literal(char* p, lex_token *tok);

const char* visible[] = {
	"NOT_FOUND",    /* Token not found */
	"ROOT",         /* $ */
	"CUR_NODE",     /* @ */
	"WILD_CARD",    /* * */
	"DEEP_SCAN",    /* .. */
	"NODE",         /* .child, ['child'] */
	"EXPR_END",     /* ] */
	"SLICE",        /* : */
	"CHILD_SEP",    /* , */
	"EXPR_START",   /* ? */
	"EQ",           /* == */
	"NEQ",          /* != */
	"LT",           /* < */
	"LTE",          /* <= */
	"GT",           /* > */
	"GTE",          /* >= */
	"RGXP",         /* =~ */
	"PAREN_OPEN",   /* ( */
	"PAREN_CLOSE",  /* ) */
	"LITERAL",      /* \"some string\" 'some string' */
	"LITERAL_BOOL", /* true, false */
	"FILTER_START"  /* [ */
};

bool scan(char** p, lex_token* tok, lex_error* err)
{
	char* start = *p;
	tok->type = LEX_NOT_FOUND;

	while (**p != '\0' && tok->type == LEX_NOT_FOUND) {

		switch (**p) {

		case '$':
			tok->type = LEX_ROOT;
			break;
		case '.':

			switch (*(*p + 1)) {
			case '.':
				tok->type = LEX_DEEP_SCAN;
				break;
			case '[':
				break;		/* dot is superfluous in .['node'] */
			case '*':
				break;		/* get in next loop */
			default:
				tok->type = LEX_NODE;
				break;

			}

			if (tok->type == LEX_NODE) {
				(*p)++;

				extract_unbounded_literal(*p, tok);

				*p += tok->len - 1;
			}

			break;
		case '[':
			(*p)++;

			for (; **p != '\0' && **p == ' '; (*p)++);

			switch (**p) {
			case '\'':
				if (!extract_quoted_literal(*p, tok, err)) {
					return false;
				}
				*p += tok->len + 2;

				for (; **p != '\0' && **p == ' '; (*p)++);

				if (**p != ']') {
					err->pos = *p;
					strcpy(err->msg, "Missing closing ] bracket");
					return false;
				}
				tok->type = LEX_NODE;
				break;
			case '"':
				if (!extract_quoted_literal(*p, tok, err)) {
					return false;
				}
				*p += tok->len + 2;

				for (; **p != '\0' && **p == ' '; (*p)++);

				if (**p != ']') {
					err->pos = *p;
					strcpy(err->msg, "Missing closing ] bracket");
					return false;
				}
				tok->type = LEX_NODE;
				break;
			case '?':
				tok->type = LEX_EXPR_START;
				break;
			default:
				/* Pick up start in next iteration, maybe simplify */
				(*p)--;
				tok->type = LEX_FILTER_START;
				break;
			}
			break;
		case ']':
			tok->type = LEX_EXPR_END;
			break;
		case '@':
			tok->type = LEX_CUR_NODE;
			break;
		case ':':
			tok->type = LEX_SLICE;
			break;
		case ',':
			tok->type = LEX_CHILD_SEP;
			break;
		case '=':
			(*p)++;

			if (**p == '=') {
				tok->type = LEX_EQ;
			}
			else if (**p == '~') {
				tok->type = LEX_RGXP;
			}

			break;
		case '!':
			(*p)++;

			if (**p != '=') {
				err->pos = *p;
				strcpy(err->msg, "! operator missing =");
				return false;
			}

			tok->type = LEX_NEQ;
			break;
		case '>':
			if (*(*p + 1) == '=') {
				tok->type = LEX_GTE;
				(*p)++;
			}
			else {
				tok->type = LEX_GT;
			}
			break;
		case '<':
			if (*(*p + 1) == '=') {
				tok->type = LEX_LTE;
				(*p)++;
			}
			else {
				tok->type = LEX_LT;
			}
			break;
		case '&':
			(*p)++;

			if (**p != '&') {
				err->pos = *p;
				strcpy(err->msg, "'And' operator must be double &&");
				return false;
			}

			tok->type = LEX_AND;
			break;
		case '|':
			(*p)++;

			if (**p != '|') {
				err->pos = *p;
				strcpy(err->msg, "'Or' operator must be double ||");
				return false;
			}

			tok->type = LEX_OR;
			break;
		case '(':
			tok->type = LEX_PAREN_OPEN;
			break;
		case ')':
			tok->type = LEX_PAREN_CLOSE;
			break;
		case '\'':
			if (!extract_quoted_literal(*p, tok, err)) {
					return false;
			}
			*p += tok->len + 1;
			tok->type = LEX_LITERAL;
			break;
		case '"':
			if (!extract_quoted_literal(*p, tok, err)) {
				return false;
			}
			*p += tok->len + 1;
			tok->type = LEX_LITERAL;
			break;
		case '*':
			tok->type = LEX_WILD_CARD;
			break;
		case 't':
		case 'f':
			extract_boolean_literal(*p, tok);
			*p += tok->len - 1;
			tok->type = LEX_LITERAL_BOOL;
			break;
		case '-':
			if (!isdigit(*(*p + 1))) {
				return false;
			}
			extract_unbounded_numeric_literal(*p, tok);
			*p += tok->len - 1;
			tok->type = LEX_LITERAL;
			break;
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
			extract_unbounded_numeric_literal(*p, tok);
			*p += tok->len - 1;
			tok->type = LEX_LITERAL;
			break;
		}

		(*p)++;
	}

	return tok->type;
}

/* Extract contents of string bounded by either single or double quotes */
static bool extract_quoted_literal(char* p, lex_token *tok, lex_error* err)
{
	char* start;
	char quote_type;
	bool quote_found = false;
	size_t cpy_len;

	for (; *p != '\0' && (*p == '\'' || *p == '"' || *p == ' '); p++) {
		// Find first occurrence
		if (*p == '\'' || *p == '"') {
			quote_found = true;
			quote_type = *p;
			p++;
			break;
		}
	}

	if (quote_found == false) {
		err->pos = p;
		strcpy(err->msg, "Missing opening quote in string literal");
		return false;
	}

	tok->start = p;

	for (; *p != '\0' && *p != quote_type && *(p - 1) != '\\'; p++);

	tok->len = (size_t)(p - tok->len);

	return true;
}

/* Extract literal without clear bounds that ends in non alpha-numeric char */
static void extract_unbounded_literal(char* p, lex_token *tok)
{
	for (; *p != '\0' && *p == ' '; p++);

	tok->start = p;

	for (; *p != '\0' && !isspace(*p) && (*p == '_' || *p == '-' || !ispunct(*p)); p++);

	tok->len = (size_t)(p - tok->len);
}

/* Extract literal without clear bounds that ends in non alpha-numeric char */
static void extract_unbounded_numeric_literal(char* p, lex_token *tok)
{
	for (; *p != '\0' && *p == ' '; p++);

	tok->start = p;

	if (*p == '-') {
		p++;
	}

	for (; isdigit(*p); p++);

	// Optional decimal separator and fraction part
	if (*p != '\0' && *(p + 1) != '\0' && *p == '.' && isdigit(*(p + 1))) {
		p++;

		for (; isdigit(*p); p++);
	}

	tok->len = (size_t)(p - tok->len);
}

/* Extract boolean */
static void extract_boolean_literal(char* p, lex_token *tok)
{
	for (; *p != '\0' && *p == ' '; p++);

	tok->start = p;

	for (; *p != '\0' && !isspace(*p) && (*p == '_' || *p == '-' || !ispunct(*p)); p++);

	tok->len = (size_t)(p - tok->len);
}
