#ifndef WINETK_SCENE_H
#define WINETK_SCENE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct wt_scene {
    uint32_t width, height;
    uint64_t frame, text_count;
    int32_t pointer_x, pointer_y, box_x, box_y;
    bool pointer_inside, dragging, focused, paused;
    char last_event[96];
};

void wt_scene_render(const struct wt_scene *scene, uint8_t *rgba);
/* Native size is copied exactly; enlargement uses bilinear pixel-center
 * sampling, while shrink sampling preserves the existing nearest contract. */
void wt_scale_rgba(const uint8_t *source, uint32_t source_width, uint32_t source_height,
                   uint8_t *destination, uint32_t width, uint32_t height);

#endif
