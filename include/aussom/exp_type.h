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

#ifndef AUSSOM_EXP_TYPE_H
#define AUSSOM_EXP_TYPE_H

/* Mirrors com.aussom.ast.expType. Selects the operation an
 * ast_expression node performs at eval time. */
typedef enum exp_type {
    EXP_UNDEF = 0,
    EXP_ASSIGNMENT,
    EXP_ADD,
    EXP_SUBTRACT,
    EXP_MULTIPLY,
    EXP_DIVIDE,
    EXP_FLOORDIV,
    EXP_MODULUS,
    EXP_EQEQ,
    EXP_NOTEQ,
    EXP_LT,
    EXP_GT,
    EXP_LTEQ,
    EXP_GTEQ,
    EXP_NOT,
    EXP_MISSNULL,
    EXP_AND,
    EXP_OR,
    EXP_ANDB,
    EXP_ORB,
    EXP_COUNT,
    EXP_INSERT,
    EXP_PLEQ,
    EXP_MIEQ,
    EXP_MUEQ,
    EXP_DIEQ,
    EXP_PLPL,
    EXP_MIMI,
    EXP_INSTANCEOF,
    EXP_INCLUDE,
    EXP_SET
} exp_type;

const char *exp_type_name(exp_type e);

#endif /* AUSSOM_EXP_TYPE_H */
