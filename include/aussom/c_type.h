/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef AUSSOM_C_TYPE_H
#define AUSSOM_C_TYPE_H

/* Mirrors com.aussom.types.cType. The runtime value type tag. AST
 * nodes carry a primativeType field of this tag so the parser can
 * record declared types for typed slots ("int x = 5"). The full
 * runtime AussomType hierarchy is not yet ported; this enum is enough
 * for the AST layer. */
typedef enum c_type {
    C_TYPE_UNDEF = 0,
    C_TYPE_NULL,
    C_TYPE_BOOL,
    C_TYPE_INT,
    C_TYPE_DOUBLE,
    C_TYPE_STRING,
    C_TYPE_OBJECT,
    C_TYPE_CALLBACK,
    C_TYPE_MAP,
    C_TYPE_LIST,
    C_TYPE_EXCEPTION,
    C_TYPE_REF,
    C_TYPE_BREAK,
    C_TYPE_RETURN
} c_type;

const char *c_type_name(c_type t);

#endif /* AUSSOM_C_TYPE_H */
