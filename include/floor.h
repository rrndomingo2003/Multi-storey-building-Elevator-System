#ifndef FLOOR_H
#define FLOOR_H
#include <stdbool.h>

bool parse_floor(const char *s, bool *is_basement, int *num);
bool floor_in_range(bool is_basement, int num, int min_floor, int max_floor);

#endif
