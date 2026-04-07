#include "floor.h"
#include <ctype.h>
#include <stdlib.h>

static bool parse_uint(const char *s, int *out) {
    if (!s || !*s) return false;
    long v = 0;
    for (const unsigned char *p = (const unsigned char*)s; *p; ++p) {
        if (!isdigit(*p)) return false;
        v = v*10 + (*p - '0');
        if (v > 100000) return false;
    }
    if (v <= 0) return false;
    *out = (int)v;
    return true;
}

bool parse_floor(const char *s, bool *is_basement, int *num) {
    if (!s || !*s) return false;
    if (s[0]=='B' || s[0]=='b') { *is_basement = true;  return parse_uint(s+1, num); }
    else                        { *is_basement = false; return parse_uint(s,   num); }
}

bool floor_in_range(bool is_basement, int num, int min_floor, int max_floor) {
    (void)is_basement; // adjust if basements use different bounds
    return num >= min_floor && num <= max_floor;
}
