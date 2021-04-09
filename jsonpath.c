
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
void execRecursiveArrayWalk(zval* arr, struct ast_node* tok, zval* return_value, int xy);
void resolvePropertySelectorValue(zval* arr, expr_operator* node);
void resolveIssetSelector(zval* arr, expr_operator* node);
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

    operator tok[PARSE_BUF_LEN];
    parse_error p_err;

    struct ast_node head;

    int i = 0;

    if (!build_parse_tree(lex_tok, lex_tok_literals, &i, lex_tok_count, &head, &p_err)) {
        zend_throw_exception(spl_ce_RuntimeException, p_err.msg, 0);
    }

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
        // printf("evaluateAST: %s\n", tok->type_s);
        switch (tok->type) {
        case AST_FILTER:
            evaluateAST(arr, tok->data.d_filter.children, return_value);
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
    // for (int i = 0; i < xy; i++) {
    //     printf("\t");
    // }

    // printf("execSelectorChain Type: %s Value: %s\n", tok->type_s, tok->data.d_selector.value);

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
        else if (Z_TYPE_P(data) == IS_ARRAY) {
            evaluateAST(data, tok->next, return_value);
        }
    }
    ZEND_HASH_FOREACH_END();
}

void execRecursiveArrayWalk(zval* arr, struct ast_node* tok, zval* return_value, int xy)
{
    // for (int i = 0; i < xy; i++) {
    //     printf("\t");
    // }
    // printf("execRecursiveArrayWalk Type: %s Value: %s\n", tok->type_s, tok->data.d_selector.value);

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

/* populate the expression operator with the array value that */
/* corresponds to the JSON-path object selector. */
/* e.g. $.path.to.val -> $[path][to][val] */
void resolvePropertySelectorValue(zval* arr, expr_operator* node)
{
    if (Z_TYPE_P(arr) != IS_ARRAY) {
        return;
    }

    zval* data;

    for (int i = 0; i < node->label_count; i++) {
        if ((data = zend_hash_str_find(HASH_OF(arr), node->label[i], strlen(node->label[i]))) == NULL) {
            return;
        }
        arr = data;
    }

    /* we can't compare strings/numbers to non-scalar values in JSON-path */
    if (!is_scalar(data)) {
        return;
    }

    if (Z_TYPE_P(data) == IS_TRUE) {
        strncpy(node->value, "JP_LITERAL_TRUE", 15);
        node->value[15] = '\0';
    }
    else if (Z_TYPE_P(data) == IS_FALSE) {
        strncpy(node->value, "JP_LITERAL_FALSE", 16);
        node->value[16] = '\0';
    }
    else if (Z_TYPE_P(data) != IS_STRING) {

        zval zcopy;

        int free_zcopy = zend_make_printable_zval(data, &zcopy);
        if (free_zcopy) {
            data = &zcopy;
        }

        size_t s_len = Z_STRLEN_P(data);
        char* s = Z_STRVAL_P(data);

        strncpy(node->value, s, s_len);
        node->value[s_len] = '\0';

        if (free_zcopy) {
            zval_dtor(&zcopy);
        }
    }
    else {
        size_t s_len = Z_STRLEN_P(data);
        char* s = Z_STRVAL_P(data);
        strncpy(node->value, s, s_len);
        node->value[s_len] = '\0';
    }
}

/* assign the isset() operator a boolean value based on whether there is an */
/* array key for the corresponding JSON-path object selector. */
void resolveIssetSelector(zval* arr, expr_operator* node)
{
    zval* data;

    for (int i = 0; i < node->label_count; i++) {
        if ((data = zend_hash_str_find(HASH_OF(arr), node->label[i], strlen(node->label[i]))) == NULL) {
            node->value_bool = false;
            break;
        }
        else {
            node->value_bool = true;
            arr = data;
        }
    }
}

bool compare_lt(expr_operator* lh, expr_operator* rh)
{
    zval a, b, result;

    ZVAL_STRING(&a, (*lh).value);
    ZVAL_STRING(&b, (*rh).value);

    compare_function(&result, &a, &b);
    zval_ptr_dtor(&a);
    zval_ptr_dtor(&b);

    bool res = (Z_LVAL(result) < 0);

    return res;
}

bool compare_gt(expr_operator* lh, expr_operator* rh)
{
    zval a, b, result;

    ZVAL_STRING(&a, (*lh).value);
    ZVAL_STRING(&b, (*rh).value);

    compare_function(&result, &a, &b);
    zval_ptr_dtor(&a);
    zval_ptr_dtor(&b);

    bool res = (Z_LVAL(result) > 0);

    return res;
}

bool compare_lte(expr_operator* lh, expr_operator* rh)
{
    zval a, b, result;

    ZVAL_STRING(&a, (*lh).value);
    ZVAL_STRING(&b, (*rh).value);

    compare_function(&result, &a, &b);
    zval_ptr_dtor(&a);
    zval_ptr_dtor(&b);

    bool res = (Z_LVAL(result) <= 0);

    return res;
}

bool compare_gte(expr_operator* lh, expr_operator* rh)
{
    zval a, b, result;

    ZVAL_STRING(&a, (*lh).value);
    ZVAL_STRING(&b, (*rh).value);

    compare_function(&result, &a, &b);
    zval_ptr_dtor(&a);
    zval_ptr_dtor(&b);

    bool res = (Z_LVAL(result) >= 0);

    return res;
}

bool compare_and(expr_operator* lh, expr_operator* rh)
{
    return (*lh).value_bool && (*rh).value_bool;
}

bool compare_or(expr_operator* lh, expr_operator* rh)
{
    return (*lh).value_bool || (*rh).value_bool;
}

bool compare_eq(expr_operator* lh, expr_operator* rh)
{
    zval a, b, result;

    ZVAL_STRING(&a, (*lh).value);
    ZVAL_STRING(&b, (*rh).value);

    compare_function(&result, &a, &b);
    zval_ptr_dtor(&a);
    zval_ptr_dtor(&b);

    bool res = (Z_LVAL(result) == 0);

    return res;
}

bool compare_neq(expr_operator* lh, expr_operator* rh)
{
    zval a, b, result;

    ZVAL_STRING(&a, (*lh).value);
    ZVAL_STRING(&b, (*rh).value);

    compare_function(&result, &a, &b);
    zval_ptr_dtor(&a);
    zval_ptr_dtor(&b);

    bool res = (Z_LVAL(result) != 0);

    return res;
}

bool compare_isset(expr_operator* lh, expr_operator* rh)
{
    return (*lh).value_bool && (*rh).value_bool;
}

bool compare_rgxp(expr_operator* lh, expr_operator* rh)
{
    zval pattern;
    pcre_cache_entry* pce;

    ZVAL_STRING(&pattern, (*rh).value);

    if ((pce = pcre_get_compiled_regex_cache(Z_STR(pattern))) == NULL) {
        zval_ptr_dtor(&pattern);
        return false;
    }

    zval retval;
    zval subpats;

    ZVAL_NULL(&retval);
    ZVAL_NULL(&subpats);

    zend_string* s_lh = zend_string_init((*lh).value, strlen((*lh).value), 0);

    php_pcre_match_impl(pce, s_lh, &retval, &subpats, 0, 0, 0, 0);

    zend_string_release_ex(s_lh, 0);
    zval_ptr_dtor(&subpats);
    zval_ptr_dtor(&pattern);

    return Z_LVAL(retval) > 0;
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
