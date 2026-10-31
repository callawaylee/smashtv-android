#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

/* On-screen twin-stick controls for phones and tablets.
   The game sits in the middle; the left side is a movement stick, the right side a fire stick
   (8 directions), with pause / coin / start buttons in the top corners. */
enum { TOUCH_NONE, TOUCH_PAUSE, TOUCH_COIN, TOUCH_START };

void touch_set_enabled(bool on);
bool touch_enabled(void);
/* Handles finger events while playing. Returns TOUCH_PAUSE when the pause button was tapped. */
int touch_event(const SDL_Event *e, SDL_Renderer *ren, SDL_FRect game);
/* Libretro joypad bits for player 1 (movement, fire directions, coin, start). */
uint16_t touch_mask(void);
void touch_release_all(void);
void touch_render(SDL_Renderer *ren, SDL_FRect game);
