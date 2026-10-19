/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/c_type.h"

const char *c_type_name(c_type t)
{
    switch (t) {
    case C_TYPE_UNDEF:     return "cUndef";
    case C_TYPE_NULL:      return "cNull";
    case C_TYPE_BOOL:      return "cBool";
    case C_TYPE_INT:       return "cInt";
    case C_TYPE_DOUBLE:    return "cDouble";
    case C_TYPE_STRING:    return "cString";
    case C_TYPE_OBJECT:    return "cObject";
    case C_TYPE_CALLBACK:  return "cCallback";
    case C_TYPE_MAP:       return "cMap";
    case C_TYPE_LIST:      return "cList";
    case C_TYPE_EXCEPTION: return "cException";
    case C_TYPE_REF:       return "cRef";
    case C_TYPE_BREAK:     return "cBreak";
    case C_TYPE_RETURN:    return "cReturn";
    }
    return "cUndef";
}
