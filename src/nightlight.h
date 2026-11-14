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
 * Map an effective strength to per-channel 6-bit gains, interpolating from
 * neutral (BRAIN_NIGHT_GAIN_MAX, no warmth) to warm_green/warm_blue at 100.
 * Red stays at maximum; warming pulls green and blue down.
 */
void brain_night_light_gains(uint32_t strength, uint32_t warm_green, uint32_t warm_blue,
                             uint32_t *green_gain, uint32_t *blue_gain);

#endif
