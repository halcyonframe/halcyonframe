#pragma once
// POSIX TZ string for an IANA zone name ("America/Chicago" -> "CST6CDT,M3.2.0,M11.1.0"); NULL if unknown.
const char *tzdb_posix(const char *iana);
