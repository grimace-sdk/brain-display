#include <stdio.h>
#include <stdint.h>

#include "nightlight.h"

static int failures;

static void check_strength(const char *name, const brain_night_light_config *cfg, uint32_t now,
                           uint32_t expected) {
    uint32_t got = brain_night_light_schedule_strength(cfg, now);
    if (got != expected) {
        printf("FAIL %s: now=%u expected=%u got=%u\n", name, now, expected, got);
        failures++;
    }
}

static void check_gain(const char *name, uint32_t strength, uint32_t neutral, uint32_t warm,
                       uint32_t expect) {
    uint32_t got = brain_night_light_gain(strength, neutral, warm);
    if (got != expect) {
        printf("FAIL %s: strength=%u neutral=%u warm=%u expected=%u got=%u\n",
               name, strength, neutral, warm, expect, got);
        failures++;
    }
}

int main(void) {
    brain_night_light_config disabled = {0, 1200, 360, 30, 80, 0x18, 0x00};
    check_strength("disabled", &disabled, 1300, 0);

    /* Daytime window 20:00-21:00, no transition. */
    brain_night_light_config box = {1, 1200, 1260, 0, 80, 0x18, 0x00};
    check_strength("box-before", &box, 1199, 0);
    check_strength("box-start", &box, 1200, 80);
    check_strength("box-mid", &box, 1230, 80);
    check_strength("box-end", &box, 1260, 0);
    check_strength("box-after", &box, 1300, 0);

    /* Overnight window 20:00-06:00, no transition. */
    brain_night_light_config night = {1, 1200, 360, 0, 50, 0x18, 0x00};
    check_strength("night-evening", &night, 1300, 50);
    check_strength("night-midnight", &night, 0, 50);
    check_strength("night-early", &night, 300, 50);
    check_strength("night-day", &night, 720, 0);
    check_strength("night-end", &night, 360, 0);

    /* 60-minute window with 30-minute ramps. */
    brain_night_light_config ramp = {1, 1200, 1260, 30, 100, 0x18, 0x00};
    check_strength("ramp-start", &ramp, 1200, 0);
    check_strength("ramp-up-half", &ramp, 1215, 50);
    check_strength("ramp-peak", &ramp, 1230, 100);
    check_strength("ramp-down-half", &ramp, 1245, 50);
    check_strength("ramp-end", &ramp, 1260, 0);

    /* Narrow window: ramps overlap, peak is clipped below target. */
    brain_night_light_config narrow = {1, 1200, 1240, 30, 100, 0x18, 0x00};
    check_strength("narrow-peak", &narrow, 1220, 66);

    /* Gain: the table's factory neutral at strength 0, warm at 100, between. */
    check_gain("gain-green-off", 0, 0x29, 0x18, 0x29);
    check_gain("gain-green-full", 100, 0x29, 0x18, 0x18);
    check_gain("gain-green-half", 50, 0x29, 0x18, 0x21);
    check_gain("gain-blue-off", 0, 0x2A, 0x00, 0x2A);
    check_gain("gain-blue-full", 100, 0x2A, 0x00, 0x00);
    check_gain("gain-blue-half", 50, 0x2A, 0x00, 0x15);
    check_gain("gain-clamp", 200, 0x29, 0x18, 0x18);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("nightlight: all checks passed\n");
    return 0;
}
