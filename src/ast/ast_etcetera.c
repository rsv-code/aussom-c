/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/ast_etcetera.h"

#include "aussom/ast_dispatch.h"
#include "aussom/aussom_gc.h"

ast_etcetera *ast_etcetera_new(void)
{
    ast_etcetera *n = AUSSOM_ALLOC(sizeof(*n));
    ast_node_init(&n->base);
    n->base.type = AST_TYPE_ETCETERA;
    n->associative = NULL;
    return n;
}

void ast_etcetera_write_body(const ast_node *n, int level, FILE *out)
{
    ast_node_write_tabs(level, out);
    fputs("{\n", out);
    ast_node_write_node_str(n, level + 1, out);
    fputs(",\n", out);
    ast_node_write_tabs(level + 1, out);
    fprintf(out, "\"primType\": %s\n", c_type_name(n->prim_type));
    ast_node_write_tabs(level, out);
    fputc('}', out);
}
