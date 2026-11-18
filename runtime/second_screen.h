#pragma once
#include <stdbool.h>
void second_screen_publish(bool has_link, float x, float y, float z, float stamina, float hour, int weather,
                           const char *place, const char *region);
void second_screen_frame(void);
