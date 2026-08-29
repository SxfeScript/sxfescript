#include "sxfe.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    const char *source =
        "interface Point { x: f32; y: f32; }\n"
        "const move = (point: &mut Point, dx: f32): void => { point.x += dx; };\n"
        "let mut point: Point = { x: 1.0, y: 2.0 };\n"
        "unsafe { move(&mut point, 3.0); }\n"
        "safe const origin: Point = point;\n"
        "safe let mut count: i32 = 0; count += 1;\n"
        "console.log(point.x);\n";
    SxfeCompileResult result;
    assert(sxfe_compile(source, strlen(source), &result) == 0);
    assert(strstr(result.javascript, "interface") == NULL);
    assert(strstr(result.javascript, "let mut") == NULL);
    assert(strstr(result.javascript, "&mut") == NULL);
    assert(strstr(result.javascript, "safe") == NULL);
    assert(strstr(result.javascript, "origin") != NULL);
    assert(strstr(result.javascript, "point.x += dx") != NULL);
    sxfe_compile_result_free(&result);

    const char *ffi_source = "unsafe extern add(i32, i32): i32 from \"add.dylib\";\n";
    assert(sxfe_compile(ffi_source, strlen(ffi_source), &result) == 0);
    assert(strstr(result.javascript, "Sxn.ffi(\"add.dylib\", \"add\"") != NULL);
    sxfe_compile_result_free(&result);

    const char *invalid_safe = "safe var value = 1;\n";
    assert(sxfe_compile(invalid_safe, strlen(invalid_safe), &result) == 1);
    assert(result.diagnostic_count == 1);
    assert(result.diagnostics[0].code == SX1002_INVALID_TYPE);
    sxfe_compile_result_free(&result);

    SxfeLayoutField fields[] = {{"x", SXFE_F32, 0}, {"enabled", SXFE_BOOL, 0}, {"value", SXFE_F64, 0}};
    SxfeLayout layout = {"Example", fields, 3, 0, 0};
    assert(sxfe_layout_finalize(&layout) == 0);
    assert(layout.alignment == 8 && layout.size == 16 && fields[2].offset == 8);

    SxfeArena arena;
    assert(sxfe_arena_init(&arena, 16) == 0);
    size_t mark = 0;
    void *value = sxfe_arena_alloc(&arena, layout.size, layout.alignment, &mark);
    assert(value != NULL && arena.stack_pointer >= layout.size);
    sxfe_arena_release(&arena, mark);
    assert(arena.stack_pointer == mark);
    sxfe_arena_destroy(&arena);
    puts("sxfe frontend tests passed");
    return 0;
}
