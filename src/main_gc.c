/* main_gc.c — test driver for the garbage collector.
 *
 * M0 — prove the collector links against the explicit allocator and can
 * see where the heap lives.
 */

#include <stdio.h>
#include "memlib.h"
#include "mm.h"
#include "gc.h"

static void section(const char *name)
{
    printf("\n========================================\n");
    printf(" %s\n", name);
    printf("========================================\n");
}

int main(void)
{
    mem_init(); /* create the heap                                  */
    mm_init();  /* lay down prologue, epilogue, first free block    */
    gc_init();  /* record where all of that lives                   */

    section("M0: heap bounds");

    char *lo = (char *)gc_heap_lo();
    char *hi = (char *)gc_heap_hi();

    printf("heap low       : %p\n", (void *)lo);
    printf("heap high      : %p   (last valid byte, inclusive)\n", (void *)hi);
    printf("heap size      : %ld bytes\n", (long)(hi - lo + 1));
    printf("sizeof(void *) : %zu bytes\n", sizeof(void *));

    gc_collect(); /* stub — proves it links */

    return 0;
}
