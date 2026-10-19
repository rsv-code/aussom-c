/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_STDLIB_TZDB_H
#define AUSSOM_STDLIB_TZDB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Named time zones for Date.parse: the region IDs java.time's
 * ZoneId.getAvailableZoneIds() offers, their rules from the IANA time
 * zone database (third_party/tzdata/tzdata.zi), and the en_US zone
 * names the zone text patterns (z, v) read. See tzdb.c. */

/* The zone ID or name at the start of t[0..n), read the way java.time
 * reads one (see tzdb.c), as the index of its region, or -1. *len is
 * its length. `names` is 0 for IDs only (VV), 1 for IDs and the short
 * names (z, v) and 2 for IDs and the long names (zzzz, vvvv). *type is
 * the kind of name: 0 standard, 1 daylight, 2 generic, or -1 for an ID.
 * Case-sensitive, as java.time's is. */
int tz_match(int names, const uint16_t *t, size_t n, size_t *len, int *type);

/* The region with this ID, or -1. */
int tz_region_named(const char *id);

/* The ID of a region, as java.time prints it. */
const char *tz_region_id(int region);

typedef enum {
    TZ_NORMAL,     /* one valid offset: *earlier == *later */
    TZ_GAP,        /* none: *earlier is the offset before, *later after */
    TZ_OVERLAP     /* two: *earlier is the one in effect first */
} tz_local_kind;

/* The offsets, in seconds, that local time `local` (seconds from
 * 1970-01-01T00:00 local) has in a region. */
tz_local_kind tz_local_offsets(int region, int64_t local, int32_t *earlier, int32_t *later);

#endif /* AUSSOM_STDLIB_TZDB_H */
