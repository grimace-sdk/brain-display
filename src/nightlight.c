/*
 * nightlight.c - Night light schedule maths. See nightlight.h. Pure functions
 * only; the registry read, preview override, timer and panel write live in the
 * driver so this stays host-testable.
 */
#include "nightlight.h"

#define MINUTES_PER_DAY 1440u

static uint32_t clamp_strength(uint32_t strength) {
    return strength > 100u ? 100u : strength;
}

uint32_t brain_night_light_schedule_strength(const brain_night_light_config *cfg, uint32_t now_min) {
    if (!cfg->enabled) {
        return 0;
    }

    uint32_t start = cfg->start_min % MINUTES_PER_DAY;
    uint32_t end = cfg->end_min % MINUTES_PER_DAY;
    uint32_t span = (end + MINUTES_PER_DAY - start) % MINUTES_PER_DAY;
    if (span == 0) {
        return 0;
    }

    uint32_t since = (now_min % MINUTES_PER_DAY + MINUTES_PER_DAY - start) % MINUTES_PER_DAY;
    if (since >= span) {
        return 0;
    }

    uint32_t target = clamp_strength(cfg->strength);
    uint32_t transition = cfg->transition_min;
    if (transition == 0) {
        return target;
    }

    uint32_t remaining = span - since;
    uint32_t rising = (since < transition) ? target * since / transition : target;
    uint32_t falling = (remaining < transition) ? target * remaining / transition : target;
    return rising < falling ? rising : falling;
}

uint32_t brain_night_light_gain(uint32_t strength, uint32_t neutral, uint32_t warm) {
    strength = clamp_strength(strength);
    if (neutral > BRAIN_NIGHT_GAIN_MAX) {
        neutral = BRAIN_NIGHT_GAIN_MAX;
    }
    if (warm > BRAIN_NIGHT_GAIN_MAX) {
        warm = BRAIN_NIGHT_GAIN_MAX;
    }

    int32_t gain = (int32_t)neutral + ((int32_t)warm - (int32_t)neutral) * (int32_t)strength / 100;
    if (gain < 0) {
        gain = 0;
    }
    if (gain > (int32_t)BRAIN_NIGHT_GAIN_MAX) {
        gain = BRAIN_NIGHT_GAIN_MAX;
    }
    return (uint32_t)gain;
}
