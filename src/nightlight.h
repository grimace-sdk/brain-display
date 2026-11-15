/*
 * nightlight.h - Night light schedule maths. Pure, host-testable: no registry,
 * no clock, no panel. Turns a schedule plus a time of day into an effective
 * strength, and a strength into per-channel panel gamma gains.
 */
#ifndef BRAIN_NIGHTLIGHT_H
#define BRAIN_NIGHTLIGHT_H

#include <stdint.h>

#define BRAIN_NIGHT_GAIN_MAX 0x3Fu

typedef struct {
    int enabled;
    uint32_t start_min;       /* window start, minutes after midnight */
    uint32_t end_min;         /* window end, minutes after midnight */
    uint32_t transition_min;  /* ramp length at each edge, minutes */
    uint32_t strength;        /* scheduled target strength, 10 to 100 */
    uint32_t warm_green;      /* green gain at full warmth (6-bit) */
    uint32_t warm_blue;       /* blue gain at full warmth (6-bit) */
} brain_night_light_config;

/*
 * Effective strength 0 to 100 for a time of day. Ramps linearly over the
 * transition window at each edge, holds the target between, and returns 0 when
 * disabled or outside the window. Handles overnight spans (start > end) and
 * narrow windows where the two ramps overlap.
 */
uint32_t brain_night_light_schedule_strength(const brain_night_light_config *cfg, uint32_t now_min);

/*
 * One channel's 6-bit gamma gain for an effective strength, interpolating from
 * the channel's factory neutral (strength 0) down to its warm value (strength
 * 100). Neutral is each gamma table's own factory gain, not a fixed maximum, so
 * strength 0 leaves the panel untouched.
 */
uint32_t brain_night_light_gain(uint32_t strength, uint32_t neutral, uint32_t warm);

#endif
