
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "src/jsonpath/lexer.h"
#include "src/jsonpath/parser.h"
#include "php_jsonpath.h"
#include "zend_operators.h"
#include <limits.h>
#include <stdbool.h>
#include "zend_exceptions.h"
#include <ext/spl/spl_exceptions.h>
#include <ext/pcre/php_pcre.h>

/* True global resources - no need for thread safety here */
static int le_jsonpath;
bool scanTokens(char* json_path, lex_token tok[], char tok_literals[][PARSE_BUF_LEN], int* tok_count);
void evaluateAST(zval* arr, struct ast_node* tok, zval* return_value);
void executeExpression(zval* arr, struct ast_node* tok, zval* return_value);
void executeIndexFilter(zval* arr, struct ast_node* tok, zval* return_value);
void execRecursiveArrayWalk(zval* arr, struct ast_node* tok, zval* return_value);
void executeSlice(zval* arr, struct ast_node* tok, zval* return_value);
zval* resolvePropertySelectorValue(zval* arr, struct ast_node* tok);
void execSelectorChain(zval* arr, struct ast_node* tok, zval* return_value);
void execWildcard(zval* arr, struct ast_node* tok, zval* return_value);
bool is_scalar(zval* arg);
void copyToReturnResult(zval* arr, zval* return_value);
#ifdef JSONPATH_DEBUG
void print_lex_tokens(
    lex_token lex_tok[PARSE_BUF_LEN],
    char lex_tok_literals[][PARSE_BUF_LEN],
    int lex_tok_count,
	const char* m);
#endif

zend_class_entry* jsonpath_ce;

#if PHP_VERSION_ID < 80000
#include "jsonpath_legacy_arginfo.h"
#else
#include "jsonpath_arginfo.h"
#endif

PHP_METHOD(JsonPath, find)
{
    /* parse php method parameters */

    char* j_path;
    size_t j_path_len;
    zval* search_target;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "as", &search_target, &j_path, &j_path_len) == FAILURE) {
        return;
    }

    /* tokenize JSON-path string */

    lex_token lex_tok[PARSE_BUF_LEN];
    char lex_tok_literals[PARSE_BUF_LEN][PARSE_BUF_LEN];
    int lex_tok_count = 0;

    if (!scanTokens(j_path, lex_tok, lex_tok_literals, &lex_tok_count)) {
        return;
    }

    #ifdef JSONPATH_DEBUG
    print_lex_tokens(lex_tok, lex_tok_literals, lex_tok_count, "Lexer - Processed tokens");
    #endif

    /* assemble an array of query execution instructions from parsed tokens */

    parse_error p_err;
    struct ast_node head;
    int i = 0;

    if (!build_parse_tree(lex_tok, lex_tok_literals, &i, lex_tok_count, &head, &p_err)) {
        zend_throw_exception(spl_ce_RuntimeException, p_err.msg, 0);
    }

    #ifdef JSONPATH_DEBUG
    print_ast(head.next, "Parser - AST sent to interpreter", 0);
    #endif

    /* execute the JSON-path query instructions against the search target (PHP object/array) */

    array_init(return_value);

    struct ast_node* next = head.next;

    evaluateAST(search_target, next, return_value);

    // /* free the memory allocated for filter expressions */

    // operator * fr = tok_ptr_start;

    // while (fr <= tok_ptr_end) {
    //     if (fr->filter_type == FLTR_EXPR) {
    //         efree((void*)fr->expressions);
    //     }
    //     fr++;
    // }

    // /* return false if no results were found by the JSON-path query */

    if (zend_hash_num_elements(HASH_OF(return_value)) == 0) {
        convert_to_boolean(return_value);
        RETURN_FALSE;
    }
}

bool scanTokens(char* json_path, lex_token tok[], char tok_literals[][PARSE_BUF_LEN], int* tok_count)
{
    lex_token cur_tok;
    char* p = json_path;
    char buffer[PARSE_BUF_LEN];
    lex_error err;

    int i = 0;

    while ((cur_tok = scan(&p, buffer, sizeof(buffer), &err)) != LEX_NOT_FOUND) {

        if (i >= PARSE_BUF_LEN) {
            zend_throw_exception(spl_ce_RuntimeException,
                "The query is too long. Token count exceeds PARSE_BUF_LEN.", 0);
            return false;
        }

        switch (cur_tok) {
        case LEX_NODE:
        case LEX_LITERAL:
        case LEX_LITERAL_BOOL:
            strcpy(tok_literals[i], buffer);
            break;
        case LEX_ERR:
            snprintf(err.msg, sizeof(err.msg), "%s at position %ld", err.msg, (err.pos - json_path));
            zend_throw_exception(spl_ce_RuntimeException, err.msg, 0);
            return false;
        default:
            tok_literals[i][0] = '\0';
            break;
        }
        
        tok[i] = cur_tok;
        i++;
    }

    *tok_count = i;

    return check_parens_balance(tok, *tok_count);
}

void evaluateAST(zval* arr, struct ast_node* tok, zval* return_value)
{
    while (tok != NULL) {
        switch (tok->type) {
        case AST_INDEX_LIST:
            executeIndexFilter(arr, tok, return_value);
            return;
        case AST_INDEX_SLICE:
            executeSlice(arr, tok, return_value);
            return;
        case AST_ROOT:
            tok = tok->next;
            break;
        case AST_RECURSE:
            tok = tok->next;
            execRecursiveArrayWalk(arr, tok, return_value);
            return;
        case AST_SELECTOR:
            execSelectorChain(arr, tok, return_value);
            return;
        case AST_WILD_CARD:
            execWildcard(arr, tok, return_value);
            return;
        case AST_EXPR:
            executeExpression(arr, tok, return_value);
            return;
        }
    }
}

void copyToReturnResult(zval* arr, zval* return_value)
{
    zval tmp;
    ZVAL_COPY_VALUE(&tmp, arr);
    zval_copy_ctor(&tmp);
    add_next_index_zval(return_value, &tmp);
}

void execSelectorChain(zval* arr, struct ast_node* tok, zval* return_value)
{
    if (arr == NULL || Z_TYPE_P(arr) != IS_ARRAY) {
        return;
    }

    if ((arr = zend_hash_str_find(HASH_OF(arr), tok->data.d_selector.value, strlen(tok->data.d_selector.value))) == NULL) {
        return;
    }

    if (tok->next != NULL) {
        evaluateAST(arr, tok->next, return_value);
    } else {
        copyToReturnResult(arr, return_value);
    }   
}

void execWildcard(zval* arr, struct ast_node* tok, zval* return_value)
{
    if (arr == NULL || Z_TYPE_P(arr) != IS_ARRAY) {
        return;
    }

    zval* data;
    zval* zv_dest;
    zend_string* key;
    zend_ulong num_key;

    ZEND_HASH_FOREACH_KEY_VAL(HASH_OF(arr), num_key, key, data) {
        if (tok->next == NULL) {
            copyToReturnResult(data, return_value);
        }
        else {
            evaluateAST(data, tok->next, return_value);
        }
    }
    ZEND_HASH_FOREACH_END();
}

void execRecursiveArrayWalk(zval* arr, struct ast_node* tok, zval* return_value)
{
    if (arr == NULL || Z_TYPE_P(arr) != IS_ARRAY) {
        return;
    }

    zval* data;
    zval* zv_dest;
    zend_string* key;
    zend_ulong num_key;

    evaluateAST(arr, tok, return_value);

    ZEND_HASH_FOREACH_KEY_VAL(HASH_OF(arr), num_key, key, data) {
        execRecursiveArrayWalk(data, tok, return_value);
    }
    ZEND_HASH_FOREACH_END();
}

void executeIndexFilter(zval* arr, struct ast_node* tok, zval* return_value)
{
    for (int i = 0; i < tok->data.d_list.count; i++) {
        if (tok->data.d_list.indexes[i] < 0) {
            tok->data.d_list.indexes[i] = zend_hash_num_elements(HASH_OF(arr)) - abs(tok->data.d_list.indexes[i]);
        }
        zval* data;
        if ((data = zend_hash_index_find(HASH_OF(arr), tok->data.d_list.indexes[i])) != NULL) {
            if (tok->next == NULL) {
                copyToReturnResult(data, return_value);
            }
            else {
                evaluateAST(data, tok->next, return_value);
            }
        }
    }
}

void executeSlice(zval* arr, struct ast_node* tok, zval* return_value)
{
    zval* data;

    int data_length = zend_hash_num_elements(HASH_OF(arr));

    int range_start = tok->data.d_list.indexes[0];
    int range_end = tok->data.d_list.count > 1 ? tok->data.d_list.indexes[1] : INT_MAX;
    int range_step = tok->data.d_list.count > 2 ? tok->data.d_list.indexes[2] : 1;

    // Zero-steps are not allowed, abort
    if (range_step == 0) {
        return;
    }

    // Replace placeholder with actual value
    if (range_start == INT_MAX) {
        range_start = range_step > 0 ? 0 : data_length - 1;
    }
    // Indexing from the end of the list
    else if (range_start < 0) {
        range_start = data_length - abs(range_start);
    }

    // Replace placeholder with actual value
    if (range_end == INT_MAX) {
        range_end = range_step > 0 ? data_length : -1;
    }
    // Indexing from the end of the list
    else if (range_end < 0) {
        range_end = data_length - abs(range_end);
    }

    // Set suitable boundaries for start index
    range_start = range_start < -1 ? -1 : range_start;
    range_start = range_start > data_length ? data_length : range_start;

    // Set suitable boundaries for end index
    range_end = range_end < -1 ? -1 : range_end;
    range_end = range_end > data_length ? data_length : range_end;

    if (range_step > 0) {
        // Make sure that the range is sane so we don't end up in an infinite loop
        if (range_start >= range_end) {
            return;
        }

        for (int i = range_start; i < range_end; i += range_step) {
            if ((data = zend_hash_index_find(HASH_OF(arr), i)) != NULL) {
                if (tok->next == NULL) {
                    copyToReturnResult(data, return_value);
                }
                else {
                    evaluateAST(data, tok->next, return_value);
                }
            }
        }
    }
    else {
        // Make sure that the range is sane so we don't end up in an infinite loop
        if (range_start <= range_end) {
            return;
        }

        for (int i = range_start; i > range_end; i += range_step) {
            if ((data = zend_hash_index_find(HASH_OF(arr), i)) != NULL) {
                if (tok->next == NULL) {
                    copyToReturnResult(data, return_value);
                }
                else {
                    evaluateAST(data, tok->next, return_value);
                }
            }
        }
    }
}

void executeExpression(zval* arr, struct ast_node* tok, zval* return_value)
{
    zend_ulong num_key;
    zend_string* key;
    zval* data;

    ZEND_HASH_FOREACH_KEY_VAL(HASH_OF(arr), num_key, key, data) {
        if (evaluate_postfix_expression(data, tok->data.d_expression.head)) {
            if (tok->next == NULL) {
                copyToReturnResult(data, return_value);
            }
            else {
                evaluateAST(data, tok->next, return_value);
            }
        }
    }
    ZEND_HASH_FOREACH_END();
}

zval* resolvePropertySelectorValue(zval* arr, struct ast_node* tok)
{
    if (arr == NULL || Z_TYPE_P(arr) != IS_ARRAY) {
        return NULL;
    }

    zval* data;

    while (tok->type == AST_SELECTOR) {
        if ((data = zend_hash_str_find(HASH_OF(arr), tok->data.d_selector.value, strlen(tok->data.d_selector.value))) == NULL) {
            return NULL;
        }
        arr = data;
        tok = tok->next;
    }

    return arr;
}

int compare(zval* lh, zval* rh)
{
    zval result;
    ZVAL_NULL(&result);

    compare_function(&result, lh, rh);
    return (int) Z_LVAL(result);
}

bool compare_rgxp(zval* lh, zval* rh)
{
    pcre_cache_entry* pce;

    if ((pce = pcre_get_compiled_regex_cache(Z_STR_P(rh))) == NULL) {
        zval_ptr_dtor(rh);
        return false;
    }

    zval retval;
    zval subpats;

    ZVAL_NULL(&retval);
    ZVAL_NULL(&subpats);

    zend_string* s_lh = zend_string_copy(Z_STR_P(lh));

    php_pcre_match_impl(pce, s_lh, &retval, &subpats, 0, 0, 0, 0);

    zend_string_release_ex(s_lh, 0);
    zval_ptr_dtor(&subpats);

    return Z_LVAL(retval) > 0;
}

zval* operand_to_zval(struct ast_node* src, zval* tmp_dest, zval* arr)
{
    if (src->type == AST_SELECTOR) {
        return resolvePropertySelectorValue(arr, src);
    } else if (src->type == AST_LITERAL) {
        ZVAL_STRING(tmp_dest, src->data.d_literal.value);
        return tmp_dest;
    } else {
        /* todo: runtime error */
    }
}

bool evaluate_subexpression(
    zval* arr,
    enum ast_type operator_type,
    struct ast_node* lh_operand,
    struct ast_node* rh_operand)
{
    switch (operator_type) {
    case AST_OR:
        return lh_operand->data.d_literal.value_bool || rh_operand->data.d_literal.value_bool;
    case AST_AND:
        return lh_operand->data.d_literal.value_bool && rh_operand->data.d_literal.value_bool;
    case AST_ISSET:
        return resolvePropertySelectorValue(arr, lh_operand) != NULL;
    }

    /* use stack-allocated zvals in order to avoid malloc, if possible */
    zval tmp_lh = {0}, tmp_rh = {0};

    zval* val_lh = operand_to_zval(lh_operand, &tmp_lh, arr);
    if (val_lh == NULL) {
        return false;
    }

    zval* val_rh = operand_to_zval(rh_operand, &tmp_rh, arr);
    if (val_rh == NULL) {
        return false;
    }

    bool ret;

	switch (operator_type) {
	case AST_EQ:
		ret = compare(val_lh, val_rh) == 0;
        break;
	case AST_NE:
		ret = compare(val_lh, val_rh) != 0;
        break;
	case AST_LT:
		ret = compare(val_lh, val_rh) < 0;
        break;
	case AST_LTE:
		ret = compare(val_lh, val_rh) <= 0;
        break;
	case AST_GT:
		ret = compare(val_lh, val_rh) > 0;
        break;
	case AST_GTE:
		ret = compare(val_lh, val_rh) >= 0;
        break;
	case AST_RGXP:
		ret = compare_rgxp(val_lh, val_rh);
        break;
	}

    zval_ptr_dtor(val_lh);
    zval_ptr_dtor(val_rh);

    return ret;
}

bool is_scalar(zval* arg)
{
    switch (Z_TYPE_P(arg)) {
    case IS_FALSE:
    case IS_TRUE:
    case IS_DOUBLE:
    case IS_LONG:
    case IS_STRING:
        return true;
        break;

    default:
        return false;
        break;
    }
}

#ifdef JSONPATH_DEBUG
void print_lex_tokens(
    lex_token lex_tok[PARSE_BUF_LEN],
    char lex_tok_literals[][PARSE_BUF_LEN],
    int lex_tok_count,
	const char* m)
{
	printf("--------------------------------------\n");
	printf("%s\n\n", m);

	for (int i = 0; i < lex_tok_count; i++) {
		printf("\t• %s", LEX_STR[lex_tok[i]]);
        if (strlen(lex_tok_literals[i]) > 0) {
		    printf(" [val=%s]", lex_tok_literals[i]);
        }
		printf("\n");
	}
}
#endif

/* {{{ PHP_MINIT_FUNCTION
 */
PHP_MINIT_FUNCTION(jsonpath)
{
    zend_class_entry jsonpath_class_entry;
    INIT_CLASS_ENTRY(jsonpath_class_entry, "JsonPath", class_JsonPath_methods);

    jsonpath_ce = zend_register_internal_class(&jsonpath_class_entry);

    return SUCCESS;
}

/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
PHP_MSHUTDOWN_FUNCTION(jsonpath)
{
    /* uncomment this line if you have INI entries
       UNREGISTER_INI_ENTRIES();
     */
    return SUCCESS;
}

/* }}} */

/* Remove if there's nothing to do at request start */
/* {{{ PHP_RINIT_FUNCTION
 */
PHP_RINIT_FUNCTION(jsonpath)
{
    return SUCCESS;
}

/* }}} */

/* Remove if there's nothing to do at request end */
/* {{{ PHP_RSHUTDOWN_FUNCTION
 */
PHP_RSHUTDOWN_FUNCTION(jsonpath)
{
    return SUCCESS;
}

/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(jsonpath)
{
    php_info_print_table_start();
    php_info_print_table_row(2, "jsonpath support", "enabled");
    php_info_print_table_row(2, "jsonpath version", PHP_JSONPATH_VERSION);
    php_info_print_table_end();

    /* Remove comments if you have entries in php.ini
       DISPLAY_INI_ENTRIES();
     */
}

/* }}} */

/* {{{ jsonpath_functions[]
 *
 * Every user visible function must have an entry in jsonpath_functions[].
 */
const zend_function_entry jsonpath_functions[] = {
    PHP_FE_END			/* Must be the last line in jsonpath_functions[] */
};

/* }}} */

/* {{{ jsonpath_module_entry
 */
zend_module_entry jsonpath_module_entry = {
    STANDARD_MODULE_HEADER,
    "jsonpath",
    jsonpath_functions,
    PHP_MINIT(jsonpath),
    PHP_MSHUTDOWN(jsonpath),
    PHP_RINIT(jsonpath),	/* Replace with NULL if there's nothing to do at request start */
    PHP_RSHUTDOWN(jsonpath),	/* Replace with NULL if there's nothing to do at request end */
    PHP_MINFO(jsonpath),
    PHP_JSONPATH_VERSION,
    STANDARD_MODULE_PROPERTIES
};

/* }}} */

#ifdef COMPILE_DL_JSONPATH
ZEND_GET_MODULE(jsonpath)
#endif
