#ifndef STACK_H
#define STACK_H 1

#include "lexer.h"
#include "parser.h"

// todo: enforce stack max check
#define STACK_MAX 100

struct stack {
    struct ast_node *data[STACK_MAX];
    int size;
};

typedef struct stack stack;

void stack_init(stack *);
struct ast_node* stack_top(stack*);
void stack_push(stack*, struct ast_node*);
void stack_pop(stack*);

#endif				/* STACK_H */
