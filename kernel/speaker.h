#ifndef FOS_SPEAKER_H
#define FOS_SPEAKER_H

#include "types.h"

void speaker_init(void);
void speaker_tone(u32 hz);
void speaker_off(void);

#endif