/* vm.c: Glulxe code related to the VM overall. Also miscellaneous stuff.
    Designed by Andrew Plotkin <erkyrath@netcom.com>
    http://www.eblong.com/zarf/glulx/index.html
*/

#include "glk.h"
#include "glulxe.h"

/* The memory blocks which contain VM main memory and the stack. */
unsigned char *memmap = NULL;
unsigned char *stack = NULL;

/* Various memory addresses which are useful. These are loaded in from
   the game file header. */
glui32 ramstart;
glui32 endgamefile;
glui32 origendmem;
glui32 stacksize;
glui32 startfuncaddr;
glui32 origstringtable;
glui32 checksum;

/* The VM registers. */
glui32 stackptr;
glui32 frameptr;
glui32 pc;
glui32 stringtable;
glui32 valstackbase;
glui32 localsbase;
glui32 endmem;
glui32 protectstart, protectend;

/* setup_vm():
   Read in the game file and build the machine, allocating all the memory
   necessary.
*/
void setup_vm()
{
  unsigned char buf[4 * 7];
  int res;

  pc = 0; /* Clear this, so that error messages are cleaner. */

  /* Read in all the size constants from the game file header. */

  glk_stream_set_position(gamefile, 8, seekmode_Start);
  res = glk_get_buffer_stream(gamefile, (char *)buf, 4 * 7);
  
  ramstart = Read4(buf+0);
  endgamefile = Read4(buf+4);
  origendmem = Read4(buf+8);
  stacksize = Read4(buf+12);
  startfuncaddr = Read4(buf+16);
  origstringtable = Read4(buf+20);
  checksum = Read4(buf+24);

  /* Set the protection range to (0, 0), meaning "off". */
  protectstart = 0;
  protectend = 0;

  /* Do a few sanity checks. */

  if ((ramstart & 0xFF)
    || (endgamefile & 0xFF) 
    || (origendmem & 0xFF)
    || (stacksize & 0xFF)) {
    nonfatal_warning("One of the segment boundaries in the header is not "
      "256-byte aligned.");
  }

  if (ramstart < 0x100 || endgamefile < ramstart || origendmem < endgamefile) {
    fatal_error("The segment boundaries in the header are in an impossible "
      "order.");
  }
  if (stacksize < 0x100) {
    fatal_error("The stack size in the header is too small.");
  }
  
  /* Allocate main memory and the stack. This is where memory allocation
     errors are most likely to occur. */
  endmem = origendmem;
  memmap = (unsigned char *)glulx_malloc(origendmem);
  if (!memmap) {
    fatal_error("Unable to allocate Glulx memory space.");
  }
  stack = (unsigned char *)glulx_malloc(stacksize);
  if (!stack) {
    glulx_free(memmap);
    memmap = NULL;
    fatal_error("Unable to allocate Glulx stack space.");
  }
  stringtable = 0;

  /* Initialize various other things in the terp. */
  init_operands(); 
  init_serial();

  /* Set up the initial machine state. */
  vm_restart();
}

/* finalize_vm():
   Deallocate all the memory and shut down the machine.
*/
void finalize_vm()
{
  if (memmap) {
    glulx_free(memmap);
    memmap = NULL;
  }
  if (stack) {
    glulx_free(stack);
    stack = NULL;
  }
}

/* vm_restart(): 
   Put the VM into a state where it's ready to begin executing the
   game. This is called both at startup time, and when the machine
   performs a "restart" opcode. 
*/
void vm_restart()
{
  glui32 lx;
  int res;

  /* Reset memory to the original size. */
  lx = change_memsize(origendmem);
  if (lx)
    fatal_error("Memory could not be reset to its original size.");

  /* Load in all of main memory */
  glk_stream_set_position(gamefile, 0, seekmode_Start);
  for (lx=0; lx<endgamefile; lx++) {
    res = glk_get_char_stream(gamefile);
    if (res == -1) {
      fatal_error("The game file ended unexpectedly.");
    }
    if (lx >= protectstart && lx < protectend)
      continue;
    memmap[lx] = res;
  }
  for (lx=endgamefile; lx<origendmem; lx++) {
    memmap[lx] = 0;
  }

  /* Reset all the registers */
  stackptr = 0;
  frameptr = 0;
  pc = 0;
  stream_set_table(origstringtable);
  valstackbase = 0;
  localsbase = 0;

  /* Note that we do not reset the protection range. */

  /* Push the first function call. (No arguments.) */
  enter_function(startfuncaddr, 0, NULL);

  /* We're now ready to execute. */
}

/* change_memsize():
   Change the size of the memory map. This may not be available at
   all; #define FIXED_MEMSIZE if you want the interpreter to
   unconditionally refuse.
   Returns 0 for success; otherwise, the operation failed.
*/
glui32 change_memsize(glui32 newlen)
{
  long lx;
  unsigned char *newmemmap;

  if (newlen == endmem)
    return 0;

#ifdef FIXED_MEMSIZE
  return 1;
#else /* FIXED_MEMSIZE */

  if (newlen < origendmem)
    fatal_error("Cannot resize Glulx memory space smaller than it started.");

  if (newlen & 0xFF)
    fatal_error("Can only resize Glulx memory space to a 256-byte boundary.");
  
  newmemmap = (unsigned char *)glulx_realloc(memmap, newlen);
  if (!newmemmap) {
    /* The old block is still in place, unchanged. */
    return 1;
  }
  memmap = newmemmap;

  if (newlen > endmem) {
    for (lx=endmem; lx<newlen; lx++) {
      memmap[lx] = 0;
    }
  }

  endmem = newlen;

  return 0;

#endif /* FIXED_MEMSIZE */
}

/* pop_arguments():
   If addr is 0, pop N arguments off the stack, and put them in an array. 
   If non-0, take N arguments from that main memory address instead.
   This has to dynamically allocate if there are more than 32 arguments,
   but that shouldn't be a problem.
*/
glui32 *pop_arguments(glui32 count, glui32 addr)
{
  int ix;
  glui32 argptr;
  glui32 *array;

  #define MAXARGS (32)
  static glui32 statarray[MAXARGS];
  static glui32 *dynarray = NULL;
  static glui32 dynarray_size = 0;

  if (count == 0)
    return NULL;

  if (count <= MAXARGS) {
    /* Store in the static array. */
    array = statarray;
  }
  else {
    if (!dynarray) {
      dynarray_size = count+8;
      dynarray = glulx_malloc(sizeof(glui32) * dynarray_size);
      if (!dynarray)
	fatal_error("Unable to allocate function arguments.");
      array = dynarray;
    }
    else {
      if (dynarray_size >= count) {
	/* It fits. */
	array = dynarray;
      }
      else {
	dynarray_size = count+8;
	dynarray = glulx_realloc(dynarray, sizeof(glui32) * dynarray_size);
	if (!dynarray)
	  fatal_error("Unable to reallocate function arguments.");
	array = dynarray;
      }
    }
  }

  if (!addr) {
    if (stackptr < valstackbase+4*count) 
      fatal_error("Stack underflow in arguments.");
    stackptr -= 4*count;
    for (ix=0; ix<count; ix++) {
      argptr = stackptr+4*((count-1)-ix);
      array[ix] = Stk4(argptr);
    }
  }
  else {
    for (ix=0; ix<count; ix++) {
      array[ix] = Mem4(addr);
      addr += 4;
    }
  }

  return array;
}

