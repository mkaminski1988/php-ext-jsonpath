
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
void execRecursiveArrayWalk(zval* arr, struct ast_node* tok, zval* return_value, int xy);
void executeSlice(zval* arr, struct ast_node* tok, zval* return_value);
zval* resolvePropertySelectorValue(zval* arr, struct ast_node* tok);
void resolveIssetSelector(zval* arr, struct ast_node* node);
struct ast_node* execSelectorChain(zval* arr, struct ast_node* tok, zval* return_value, int xy);
void execWildcard(zval* arr, struct ast_node* tok, zval* return_value);
bool is_scalar(zval* arg);
void copyToReturnResult(zval* arr, zval* return_value);

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

    /* assemble an array of query execution instructions from parsed tokens */

    parse_error p_err;
    struct ast_node head;
    int i = 0;

    if (!build_parse_tree(lex_tok, lex_tok_literals, &i, lex_tok_count, &head, &p_err)) {
        zend_throw_exception(spl_ce_RuntimeException, p_err.msg, 0);
    }

    // print_ast(head.next, 0);

    /* execute the JSON-path query instructions against the search target (PHP object/array) */

    array_init(return_value);

    struct ast_node* next = head.next;

    // printf("Evaluating...\n");
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

        // printf("\t➔ %s\n", visible[cur_tok]);

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

    return true;
}

void evaluateAST(zval* arr, struct ast_node* tok, zval* return_value)
{
    while (tok != NULL) {
        switch (tok->type) {
        case AST_INDEX_LIST:
            executeIndexFilter(arr, tok, return_value);
            tok = tok->next;
            break;
        case AST_INDEX_SLICE:
            executeSlice(arr, tok, return_value);
            tok = tok->next;
            break;
        case AST_ROOT:
            tok = tok->next;
            break;
        case AST_RECURSE:
            tok = tok->next;
            execRecursiveArrayWalk(arr, tok, return_value, 0);
            return;
        case AST_SELECTOR:
            execSelectorChain(arr, tok, return_value, 0);
            return;
        case AST_WILD_CARD:
            execWildcard(arr, tok, return_value);
            return;
        case AST_EXPR:
            executeExpression(arr, tok, return_value);
            tok = tok->next;
            break;
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

struct ast_node* execSelectorChain(zval* arr, struct ast_node* tok, zval* return_value, int xy)
{
    if (Z_TYPE_P(arr) != IS_ARRAY) {
        return NULL;
    }

    if ((arr = zend_hash_str_find(HASH_OF(arr), tok->data.d_selector.value, strlen(tok->data.d_selector.value))) == NULL) {
        return NULL;
    }

    if (tok->next != NULL) {
        evaluateAST(arr, tok->next, return_value);
    } else {
        copyToReturnResult(arr, return_value);
        return tok;
    }   
}


void execWildcard(zval* arr, struct ast_node* tok, zval* return_value)
{
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

void execRecursiveArrayWalk(zval* arr, struct ast_node* tok, zval* return_value, int xy)
{
    if (arr == NULL || Z_TYPE_P(arr) != IS_ARRAY) {
        return;
    }

    zval* data;
    zval* zv_dest;
    zend_string* key;
    zend_ulong num_key;

    execSelectorChain(arr, tok, return_value, xy+1);

    ZEND_HASH_FOREACH_KEY_VAL(HASH_OF(arr), num_key, key, data) {
        execRecursiveArrayWalk(data, tok, return_value, xy+1);
    }
    ZEND_HASH_FOREACH_END();
}

void executeIndexFilter(zval* arr, struct ast_node* tok, zval* return_value)
{
    zval* data;

    for (int i = 0; i < tok->data.d_list.count; i++) {
        if (tok->data.d_list.indexes[i] < 0) {
            tok->data.d_list.indexes[i] = zend_hash_num_elements(HASH_OF(arr)) - abs(tok->data.d_list.indexes[i]);
        }
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

/* populate the expression operator with the array value that */
/* corresponds to the JSON-path object selector. */
/* e.g. $.path.to.val -> $[path][to][val] */
zval* resolvePropertySelectorValue(zval* arr, struct ast_node* tok)
{
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

/* assign the isset() operator a boolean value based on whether there is an */
/* array key for the corresponding JSON-path object selector. */
void resolveIssetSelector(zval* arr, struct ast_node* node)
{
    // zval* data;

    // for (int i = 0; i < node->label_count; i++) {
    //     if ((data = zend_hash_str_find(HASH_OF(arr), node->label[i], strlen(node->label[i]))) == NULL) {
    //         node->value_bool = false;
    //         break;
    //     }
    //     else {
    //         node->value_bool = true;
    //         arr = data;
    //     }
    // }
}

bool compare_rgxp(zval* lh, zval* rh)
{
    // /* obtain the string that will get regexed */

    // zval* s_lh_ptr = resolvePropertySelectorValue(arr, lh);

    // printf("\nTarget: %s\n", Z_STRVAL_P(s_lh_ptr));

    // /* obtain and compile the regex expression */

    // pcre_cache_entry* pce;

    // zend_string* pattern = zend_string_init((*rh).data.d_literal.value, strlen((*rh).data.d_literal.value), 0);


    // if ((pce = pcre_get_compiled_regex_cache(pattern)) == NULL) {
    //     zval_ptr_dtor(&pattern);
    //     return false;
    // }

    // /* execute the regex expression */

    // zval retval;
    // zval subpats;

    // ZVAL_NULL(&retval);
    // ZVAL_NULL(&subpats);

    // zend_string *new_str = zend_string_init(ZSTR_VAL_P(s_lh_ptr), ZSTR_LEN_P(s_lh_ptr), 0);

    // php_pcre_match_impl(pce, new_str, &retval, &subpats, 0, 0, 0, 0);

    // zval_ptr_dtor(&subpats);
    // zval_ptr_dtor(&pattern);

    // // printf("Result: %ld\n", Z_LVAL(retval));

    // return Z_LVAL(retval) > 0;
    return false;
}

int compare(zval* lh, zval* rh)
{
    zval result;
    ZVAL_NULL(&result);

    compare_function(&result, lh, rh);
    return (int) Z_LVAL(result);
}

bool execute_operator_callback(enum ast_type type, zval* arr, struct ast_node* lh, struct ast_node* rh)
{
    zval a, b, result;

    zval* a_ptr = &a;
    zval* b_ptr = &b;

    if (lh->type == AST_SELECTOR) {
        a_ptr = resolvePropertySelectorValue(arr, lh);
		if (a_ptr == NULL) {
			return false;
		}
    } else {
        ZVAL_STRING(&a, (*lh).data.d_literal.value);
    }

    if (rh->type == AST_SELECTOR) {
        b_ptr = resolvePropertySelectorValue(arr, rh);
		if (b_ptr == NULL) {
			return false;
		}
    } else {
        ZVAL_STRING(&b, (*rh).data.d_literal.value);
    }

    bool ret;

	switch (type) {
	case AST_EQ:
		ret = compare(a_ptr, b_ptr) == 0;
        break;
	case AST_NE:
		ret = compare(a_ptr, b_ptr) != 0;
        break;
	case AST_LT:
		ret = compare(a_ptr, b_ptr) < 0;
        break;
	case AST_LTE:
		ret = compare(a_ptr, b_ptr) <= 0;
        break;
	case AST_GT:
		ret = compare(a_ptr, b_ptr) > 0;
        break;
	case AST_GTE:
		ret = compare(a_ptr, b_ptr) >= 0;
        break;
	case AST_ISSET:
		ret = lh->data.d_literal.value_bool && rh->data.d_literal.value_bool;
        break;
	case AST_OR:
		ret = lh->data.d_literal.value_bool || rh->data.d_literal.value_bool;
        break;
	case AST_AND:
		ret = lh->data.d_literal.value_bool && rh->data.d_literal.value_bool;
        break;
	case AST_RGXP:
		ret = false;
        break;
		// return compare_rgxp;
	}

    zval_ptr_dtor(a_ptr);
    zval_ptr_dtor(b_ptr);

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

void* jpath_malloc(size_t size) {
    return emalloc(size);
}

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
