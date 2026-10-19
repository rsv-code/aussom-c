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

#ifndef AUSSOM_ACCESS_TYPE_H
#define AUSSOM_ACCESS_TYPE_H

/* Mirrors com.aussom.ast.AccessType. */
typedef enum access_type {
    ACCESS_PRIVATE = 0,
    ACCESS_PUBLIC
} access_type;

const char *access_type_name(access_type a);

#endif /* AUSSOM_ACCESS_TYPE_H */
