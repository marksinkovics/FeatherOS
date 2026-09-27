#ifndef FONT_H
#define FONT_H

#include <stdint.h>

extern uint8_t font16_8[];

extern int8_t indexForCharacter(char character, uint8_t* index);

static const uint8_t FONT_HEIGHT = 16;
static const uint8_t FONT_WIDTH = 8;

#endif