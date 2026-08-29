/* Feasibility spike for replacing the interpreter's computed-goto dispatch
   with musttail chaining (the CPython 3.14 technique).
   Build:  cc -O2 -std=c2x -o dispatch_bench dispatch_bench.c
   Models both styles over the same opcode mix -- the fused loop body of
   `for (let i=0;i<N;i++) x+=1;`. Opcode bodies are identical and trivial, so
   what is measured is dispatch, not work. 200M chained tail calls complete
   without stack growth, which is the proof musttail was honoured.

   Result on Apple clang 21 / arm64: musttail is 47-70% SLOWER than computed
   goto across repeated runs at -O2 and -O3. CPython's reported 10-15% came
   from a baseline that was not using computed goto (and, per Nelhage's
   follow-up analysis, partly from a clang regression in that baseline).
   Against a healthy threaded interpreter the technique does not pay here.
   See spec/IMPLEMENTATION.md. */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>

enum { OP_GETLOC, OP_PUSH, OP_ADD, OP_PUTLOC, OP_CMP, OP_JMP, OP_END };

/* the loop body of `for (let i=0;i<N;i++) x+=1;` after fusion */
static uint8_t prog[] = {
    OP_CMP, OP_PUSH, OP_ADD, OP_PUTLOC, OP_GETLOC, OP_JMP, OP_END
};

typedef struct VM { const uint8_t *pc; int64_t *sp; int64_t stack[64]; int64_t loc[4]; int64_t iter, limit; } VM;

static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1e9+t.tv_nsec; }

static int64_t run_goto(VM *vm) {
    static void * const tab[] = {&&L_GETLOC,&&L_PUSH,&&L_ADD,&&L_PUTLOC,&&L_CMP,&&L_JMP,&&L_END};
    #define NEXT() goto *tab[*vm->pc++]
    NEXT();
L_GETLOC: *vm->sp++ = vm->loc[0]; NEXT();
L_PUSH:   *vm->sp++ = 1; NEXT();
L_ADD:    vm->sp[-2] += vm->sp[-1]; vm->sp--; NEXT();
L_PUTLOC: vm->loc[0] = *--vm->sp; NEXT();
L_CMP:    if (++vm->iter >= vm->limit) { vm->pc = prog + 6; } NEXT();
L_JMP:    vm->pc = prog; NEXT();
L_END:    return vm->loc[0];
    #undef NEXT
}

#define TC __attribute__((preserve_none)) static int64_t
TC tc_getloc(VM*); TC tc_push(VM*); TC tc_add(VM*); TC tc_putloc(VM*); TC tc_cmp(VM*); TC tc_jmp(VM*); TC tc_end(VM*);
typedef int64_t (*tc_fn)(VM*) __attribute__((preserve_none));
static tc_fn const tctab[] = {tc_getloc,tc_push,tc_add,tc_putloc,tc_cmp,tc_jmp,tc_end};
#define TCNEXT() [[clang::musttail]] return tctab[*vm->pc++](vm)
TC tc_getloc(VM *vm){ *vm->sp++ = vm->loc[0]; TCNEXT(); }
TC tc_push  (VM *vm){ *vm->sp++ = 1; TCNEXT(); }
TC tc_add   (VM *vm){ vm->sp[-2] += vm->sp[-1]; vm->sp--; TCNEXT(); }
TC tc_putloc(VM *vm){ vm->loc[0] = *--vm->sp; TCNEXT(); }
TC tc_cmp   (VM *vm){ if (++vm->iter >= vm->limit) { vm->pc = prog + 6; } TCNEXT(); }
TC tc_jmp   (VM *vm){ vm->pc = prog; TCNEXT(); }
TC tc_end   (VM *vm){ return vm->loc[0]; }

int main(void){
    const int64_t N = 200000000;
    double best_g = 1e30, best_t = 1e30;
    for (int r = 0; r < 5; r++) {
        VM vm = {.pc=prog, .loc={0}, .iter=0, .limit=N}; vm.sp = vm.stack;
        double t0 = now(); volatile int64_t rg = run_goto(&vm); double d = (now()-t0)/N; (void)rg;
        if (d < best_g) best_g = d;
        VM vm2 = {.pc=prog, .loc={0}, .iter=0, .limit=N}; vm2.sp = vm2.stack;
        t0 = now(); volatile int64_t rt = tctab[*vm2.pc++](&vm2); d = (now()-t0)/N; (void)rt;
        if (d < best_t) best_t = d;
    }
    printf("computed-goto : %.2f ns/iteration (6 opcodes)\n", best_g);
    printf("musttail      : %.2f ns/iteration (6 opcodes)\n", best_t);
    printf("delta         : %+.1f%%\n", (best_t-best_g)/best_g*100.0);
    return 0;
}
