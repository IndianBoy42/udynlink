#include <stdio.h>

static int counter = 0;            /* .bss: mutated, must survive relocate        */
volatile int g = 10;              /* .data: mutated, must survive relocate        */
volatile int z;                   /* .bss                                          */

static int sq(volatile int *x) { return (*x) * (*x); }          /* code: static   */
int doub(volatile int *x) { return (*x) + (*x); }              /* code: exported  */

typedef int (*fptr_t)(volatile int*);
volatile fptr_t p_sq = sq;        /* .data code-pointer  (rebased by code_delta)  */
volatile int *p_g = &g;          /* .data data-pointer  (rebased by data_delta)  */

void bump(void) { counter++; g += 5; z = g * 2; }
int get_counter(void) { return counter; }
int check_ptrs(void) { return p_sq(p_g) == (g * g); }   /* uses rebased ptrs      */

int test(void) { printf("Running test 'mod_relocate'\n"); return 1; }
