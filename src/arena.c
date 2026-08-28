#include "sxfe.h"

#include <stdlib.h>
#include <string.h>

static size_t primitive_size(SxfePrimitive primitive) {
    switch (primitive) {
        case SXFE_BOOL: return 1;
        case SXFE_I32:
        case SXFE_F32: return 4;
        case SXFE_F64: return 8;
    }
    return 0;
}

int sxfe_layout_finalize(SxfeLayout *layout) {
    size_t offset = 0;
    size_t maximum_alignment = 1;
    if (!layout || (!layout->fields && layout->field_count)) return -1;
    for (size_t i = 0; i < layout->field_count; ++i) {
        size_t alignment = primitive_size(layout->fields[i].primitive);
        if (!alignment) return -1;
        if (alignment > maximum_alignment) maximum_alignment = alignment;
        offset = (offset + alignment - 1) & ~(alignment - 1);
        layout->fields[i].offset = offset;
        offset += alignment;
    }
    layout->alignment = maximum_alignment;
    layout->size = (offset + maximum_alignment - 1) & ~(maximum_alignment - 1);
    return 0;
}

int sxfe_arena_init(SxfeArena *arena, size_t initial_capacity) {
    if (!arena) return -1;
    if (initial_capacity < 256) initial_capacity = 256;
    arena->data = malloc(initial_capacity);
    if (!arena->data) return -1;
    arena->capacity = initial_capacity;
    arena->stack_pointer = 0;
    return 0;
}

void sxfe_arena_destroy(SxfeArena *arena) {
    if (!arena) return;
#ifdef SXN_ENABLE_POISON
    if (arena->data) memset(arena->data, 0xdd, arena->capacity);
#endif
    free(arena->data);
    memset(arena, 0, sizeof(*arena));
}

void *sxfe_arena_alloc(SxfeArena *arena, size_t size, size_t alignment, size_t *mark) {
    if (!arena || !arena->data || !alignment || (alignment & (alignment - 1))) return NULL;
    size_t aligned = (arena->stack_pointer + alignment - 1) & ~(alignment - 1);
    if (aligned > SIZE_MAX - size) return NULL;
    size_t needed = aligned + size;
    if (needed > arena->capacity) {
        size_t capacity = arena->capacity;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2) return NULL;
            capacity *= 2;
        }
        uint8_t *replacement = realloc(arena->data, capacity);
        if (!replacement) return NULL;
        arena->data = replacement;
        arena->capacity = capacity;
    }
    if (mark) *mark = arena->stack_pointer;
    arena->stack_pointer = needed;
    memset(arena->data + aligned, 0, size);
    return arena->data + aligned;
}

void sxfe_arena_release(SxfeArena *arena, size_t mark) {
    if (!arena || mark > arena->stack_pointer) return;
#ifdef SXN_ENABLE_POISON
    memset(arena->data + mark, 0xdd, arena->stack_pointer - mark);
#endif
    arena->stack_pointer = mark;
}

int sxfe_arena_move(void *destination, void *source, size_t size) {
    if (!destination || !source) return -1;
    memmove(destination, source, size);
#ifdef SXN_ENABLE_POISON
    memset(source, 0xa5, size);
#endif
    return 0;
}

