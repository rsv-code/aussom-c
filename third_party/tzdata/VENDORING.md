# Vendoring notes: tzdata 2026c

Source: https://data.iana.org/time-zones/releases/tzdata2026c.tar.gz
SHA-256: e4a178a4477f3d0ea77cc31828ff72aa38feff8d61aa13e7e99e142e9d902be4

The IANA time zone database. Its `LICENSE` file says: "Unless specified
below, all files in the tz code and data (including this LICENSE file)
are in the public domain." The BSD-licensed files it names (`date.c`,
`newstrftime.3`, `strftime.c`) are code files that are not in the data
release and are not vendored here.

## Modifications

None. Every vendored file is byte-for-byte upstream. `tzdata.zi` is
generated from them (see below).

## What was taken

- The data files Java's region IDs come from: `africa`, `antarctica`,
  `asia`, `australasia`, `europe`, `northamerica`, `southamerica`,
  `etcetera` and `backward`.
- `ziguard.awk` and `zishrink.awk`, IANA's scripts that build
  `tzdata.zi` from them.
- `LICENSE`, `README` and `version`.

## What was left out, and why

- `factory`: its one zone, Factory, is not a java.time region ID.
- `backzone`: java.time does not use it; its zones are links in
  `backward`.
- The tables (`zone.tab`, `zone1970.tab`, `zonenow.tab`, `iso3166.tab`),
  the leap second files, `calendars`, `theory.html`, `NEWS`,
  `CONTRIBUTING`, `SECURITY` and the check scripts: not read by the
  build.
- IANA's `Makefile`: it builds the zic and zdump programs too, and its
  `version` rule needs their source. The project's `make tzdata-zi`
  runs the same two awk steps it uses for `tzdata.zi`.

## Generated file

`tzdata.zi` is the whole database in one shrunk file, as IANA's own
`make tzdata.zi` builds it, less `factory`. The build embeds it as a C
string (see the Makefile); `src/stdlib/tzdb.c` reads it. To update:

1. Replace the files above with those of the new release.
2. Run `make tzdata-zi`, which needs only awk.
3. Update the version and checksum in this file and in `THIRD-PARTY.md`.
