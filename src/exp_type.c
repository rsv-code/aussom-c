/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/exp_type.h"

const char *exp_type_name(exp_type e)
{
    switch (e) {
    case EXP_UNDEF:      return "UNDEF";
    case EXP_ASSIGNMENT: return "ASSIGNMENT";
    case EXP_ADD:        return "ADD";
    case EXP_SUBTRACT:   return "SUBTRACT";
    case EXP_MULTIPLY:   return "MULTIPLY";
    case EXP_DIVIDE:     return "DIVIDE";
    case EXP_FLOORDIV:   return "FLOORDIV";
    case EXP_MODULUS:    return "MODULUS";
    case EXP_EQEQ:       return "EQEQ";
    case EXP_NOTEQ:      return "NOTEQ";
    case EXP_LT:         return "LT";
    case EXP_GT:         return "GT";
    case EXP_LTEQ:       return "LTEQ";
    case EXP_GTEQ:       return "GTEQ";
    case EXP_NOT:        return "NOT";
    case EXP_MISSNULL:   return "MISSNULL";
    case EXP_AND:        return "AND";
    case EXP_OR:         return "OR";
    case EXP_ANDB:       return "ANDB";
    case EXP_ORB:        return "ORB";
    case EXP_COUNT:      return "COUNT";
    case EXP_INSERT:     return "INSERT";
    case EXP_PLEQ:       return "PLEQ";
    case EXP_MIEQ:       return "MIEQ";
    case EXP_MUEQ:       return "MUEQ";
    case EXP_DIEQ:       return "DIEQ";
    case EXP_PLPL:       return "PLPL";
    case EXP_MIMI:       return "MIMI";
    case EXP_INSTANCEOF: return "INSTANCEOF";
    case EXP_INCLUDE:    return "INCLUDE";
    case EXP_SET:        return "SET";
    }
    return "UNDEF";
}
