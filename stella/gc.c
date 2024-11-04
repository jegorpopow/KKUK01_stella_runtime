#include "gc.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include "runtime.h"

#define STELLA_OBJECT_FIELDS_FOR(object, index, field)                       \
  for (size_t int_stella_object_for_field_count_ =                           \
                  STELLA_OBJECT_HEADER_FIELD_COUNT((object)->object_header), \
              index = 0, int_stella_object_for_flag_ = 1;                    \
       index < int_stella_object_for_field_count_;                           \
       index++, int_stella_object_for_flag_ = 1)                             \
    for (stella_object* field =                                              \
             (stella_object*)(((object)->object_fields[index]));             \
         int_stella_object_for_flag_; int_stella_object_for_flag_ = 0)

/**
  Implements Baker's garbage collecrion algorithm,
  (inceremental copy garbage collection with semi-dfs copying strategy)
*/

/**
  Size of from- and to- space, 4Mb by default

  Should be enough to contain all data that was alive
  during last `flip` call and all data, allocated since that moment,
  so maximal number of bytes alive in one moment may be not enough
*/

/**
  Logging functions
*/

static void gc_debug_log(const char* format, ...) {
#ifdef STELLA_GC_DEBUG
  va_list args;
  va_start(args, format);
  printf("[gc] ");
  vprintf(format, args);
#endif
}

static void gc_debug_log_state(void) {
#ifdef STELLA_GC_DEBUG
  print_gc_state();
#endif
}

#define WORD_SIZE (sizeof(void*))

#ifndef MAX_ALLOC_SIZE
#define MAX_ALLOC_SIZE (4 * 1024 * 1024)
#endif

#define GC_INTERNAL_SPACE_SIZE MAX_ALLOC_SIZE

char first_buffer[GC_INTERNAL_SPACE_SIZE] = {};
char second_buffer[GC_INTERNAL_SPACE_SIZE] = {};

/**
  Some utils for void pointers
  (void pointer arithmetics is gcc extension, wrapping it to fullfil standart)
*/

static void* advance_void(void* base, ptrdiff_t diff) {
  return (void*)((char*)base + diff);
}

static ptrdiff_t diff_void(void* top, void* bottom) {
  return (char*)top - (char*)bottom;
}

static bool belongs_to(void* object, void* space) {
  return diff_void(object, space) >= 0 &&
         diff_void(object, space) < GC_INTERNAL_SPACE_SIZE;
}

/**
  Garbage collector state:

  We pretend that we already completed one cycle of Baker's algorithm
  and moved *zero* bytes from old from-space to to-space
  (like when from-space consists of garbage only)
  so now we allocate new objects from the ending of to-space
*/

#define GC_INTERNAL_INITIAL_TO_SPACE second_buffer
#define GC_INTERNAL_INITIAL_FROM_SPACE first_buffer

void* from_space = GC_INTERNAL_INITIAL_FROM_SPACE;
void* to_space = GC_INTERNAL_INITIAL_TO_SPACE;

/**
  Points to the end of last `shallow copied` object
  Initialized with the beginning of to-space
*/
void* next = GC_INTERNAL_INITIAL_TO_SPACE;

/**
  Points to the end of last `deeply copied` object
  (the objects, all fields of which a already copied)
  Initialized with the beginning of to-space
*/
void* scan = GC_INTERNAL_INITIAL_TO_SPACE;

/**
  Points to the begining of last allocated object
  Initialized with the end of to-space
*/
void* limit = GC_INTERNAL_INITIAL_TO_SPACE + GC_INTERNAL_SPACE_SIZE;

/** Total allocated number of bytes (over the entire duration of the program).
 */
int total_allocated_bytes = 0;

/** Total allocated number of objects (over the entire duration of the program).
 */
int total_allocated_objects = 0;

int max_allocated_bytes = 0;
int max_allocated_objects = 0;

int current_allocated_bytes = 0;
int current_allocated_objects = 0;

int total_reads = 0;
int total_writes = 0;

#define MAX_GC_ROOTS 1024

int gc_roots_max_size = 0;
int gc_roots_top = 0;
void** gc_roots[MAX_GC_ROOTS];

static bool gc_inited = false;

/**
  Prints debug info and terminates the program executio in case of
  out-of-memory error, since we don't have malloc-like interface
  with NULL return value
 */

static bool gc_failed = false;

static noreturn void out_of_memory_failure(void);

/**
  GC internal functions, can not be called from runtime
*/

/**
  In case we would need some code to intialize gc in the future
*/
static void gc_init(void) {
  if (gc_inited)
    return;
}

/**
  Returns size of stella_object, meeasured in bytes
*/
static size_t stella_object_size(stella_object* object) {
  return WORD_SIZE *
         (STELLA_OBJECT_HEADER_FIELD_COUNT(object->object_header) + 1);
}

/**
 Increases next, fails if limit was reached
*/
static void advance_next_by(ptrdiff_t diff) {
  next = advance_void(next, diff);
  if (diff_void(next, limit) > 0) {
    out_of_memory_failure();
  }
}

/**
  Copies object from from-space to to-space without changing it's fields
  Returns number pointer to copy of object
*/
stella_object* shallow_copy(stella_object* object, size_t* memory_copied) {
  size_t object_size = stella_object_size(object);
  stella_object* forwarded_object = (stella_object*)next;
  advance_next_by(object_size);

  current_allocated_objects += 1;
  current_allocated_bytes += object_size;
  max_allocated_bytes = max_allocated_bytes > current_allocated_bytes
                            ? max_allocated_bytes
                            : current_allocated_bytes;
  max_allocated_objects = max_allocated_objects > current_allocated_objects
                              ? max_allocated_objects
                              : current_allocated_objects;

  memcpy(forwarded_object, object, object_size);
  memory_copied += object_size / WORD_SIZE;
  object->object_fields[0] = forwarded_object;

  return forwarded_object;
}

/**
  Semi-dfs implementation, copies one branch of dfs from from-space to to-space
  Returns number of copied words
*/
static size_t chase(void* raw_object) {
  stella_object* object = (stella_object*)raw_object;
  size_t memory_copied = 0;

  while (object != NULL) {
    gc_debug_log("chasing %p\n", object);
    size_t object_size = stella_object_size(object);
    size_t field_count =
        STELLA_OBJECT_HEADER_FIELD_COUNT(object->object_header);

    /* probably, violates strict aliasing rule */
    stella_object* forwarded_object = (stella_object*)next;
    advance_next_by(object_size);

    memcpy(forwarded_object, object, object_size);
    memory_copied += object_size / WORD_SIZE;

    current_allocated_objects += 1;
    current_allocated_bytes += object_size;
    max_allocated_bytes = max_allocated_bytes > current_allocated_bytes
                              ? max_allocated_bytes
                              : current_allocated_bytes;
    max_allocated_objects = max_allocated_objects > current_allocated_objects
                                ? max_allocated_objects
                                : current_allocated_objects;

    stella_object* next_object = NULL;
    for (size_t i = 0; i < field_count; i++) {
      stella_object* field =
          (stella_object*)(forwarded_object->object_fields[i]);
      if (belongs_to(field, from_space) &&
          !belongs_to(field->object_fields[0], to_space)) {
        next_object = field;
      }
    }

    object->object_fields[0] = forwarded_object;
    gc_debug_log("chased %p ---> %p\n", object, object->object_fields[0]);
    object = next_object;
  }

  return memory_copied;
}

/**
  Moves object from from-space to to-space
  Returns pointer to resulting object location
  Sets memory_copied to number of words, copied during operation
*/
static void* forward(void* raw_object, size_t* memory_copied) {
  *memory_copied = 0;

  gc_debug_log("forwarding %p\n", raw_object);

  if (belongs_to(raw_object, from_space)) {
    gc_debug_log("forwarding from from-space\n");
    stella_object* object = (stella_object*)raw_object;
    if (!belongs_to(object->object_fields[0], to_space)) {
      gc_debug_log("forwarding non-forwarded\n");
      *memory_copied = chase(object);
      gc_debug_log("forwarded %p ---> %p\n", object, object->object_fields[0]);
    } else {
      gc_debug_log("%p is already forwarded to %p\n", object,
                   object->object_fields[0]);
    }
    return object->object_fields[0];
  } else {
    return raw_object;
  }
}

/**
  Swapping to-space and from-space, forward all the roots
  Returns amount of memory, copied during operation
  Prerequirement: scan == next (all data already forwarded)
*/
static size_t flip(void) {
  gc_debug_log("flipping\n");
#ifdef STELLA_GC_DEBUG
  print_gc_roots();
#endif
  current_allocated_objects = 0;
  current_allocated_bytes = 0;


  void* t = to_space;
  to_space = from_space;
  from_space = t;

  limit = advance_void(to_space, GC_INTERNAL_SPACE_SIZE);
  next = to_space;
  scan = to_space;

  size_t total_memory_copied = 0;

  for (int i = 0; i < gc_roots_top; i++) {
    void** root = gc_roots[i];

    size_t memory_copied = 0;
    *root = forward(*root, &memory_copied);
    total_memory_copied += memory_copied;
  }

  gc_debug_log("after flip\n");
#ifdef STELLA_GC_DEBUG
  print_gc_state();
#endif

  return total_memory_copied;
}

/**
  Deeply copies shallow copied object
  Returns amount of memory, copied during operation
*/
static size_t deep_forward(stella_object* object) {
  size_t total_memory_copied = 0;
  size_t field_count = STELLA_OBJECT_HEADER_FIELD_COUNT(object->object_header);

  for (int i = 0; i < field_count; i++) {
    size_t memory_copied = 0;
    object->object_fields[i] =
        forward(object->object_fields[i], &memory_copied);
    total_memory_copied += memory_copied;
  }

  return total_memory_copied;
}

/**
  Deeply copies all shallow copied objects in to-space
  Can be utilized in non-iterative gc or in special cases in current gc.
  Returns amount of memory, copied during operation
*/
size_t gc_force_copy_all(void) {
  size_t total_memory_copied = 0;

  while (diff_void(next, scan) > 0) {
    stella_object* object = (stella_object*)scan;
    total_memory_copied += deep_forward(object);
    scan = advance_void(scan, stella_object_size(object));
  }

  return total_memory_copied;
}

static const char* stella_tag_to_string(enum TAG tag) {
  static const char* string_representation[TAG_CONS + 1] = {
      [TAG_ZERO] = "Zero",   [TAG_SUCC] = "Succ", [TAG_TRUE] = "True",
      [TAG_FN] = "Function", [TAG_REF] = "Ref",   [TAG_UNIT] = "Unit",
      [TAG_TUPLE] = "Tuple", [TAG_INL] = "Left",  [TAG_INR] = "Right",
      [TAG_EMPTY] = "Empty", [TAG_CONS] = "Cons"};

  return string_representation[tag];
}

static const char* stella_object_desc(stella_object* object) {
  if (object == &the_ZERO) {
    return "global ZERO";
  } else if (object == &the_UNIT) {
    return "global UNIT";
  } else if (object == &the_EMPTY) {
    return "global EMPTY";
  } else if (object == &the_EMPTY_TUPLE) {
    return "global EMPTY_TUPLE";
  } else if (object == &the_FALSE) {
    return "global FALSE";
  } else if (object == &the_TRUE) {
    return "global TRUE";
  }

  if (belongs_to(object, to_space)) {
    return "to-space";
  } else if (belongs_to(object, from_space)) {
    return "from-space";
  } else {
    return "invalid";
  }
}

static void print_object(stella_object* object, const char* prefix) {
  int field_count = STELLA_OBJECT_HEADER_FIELD_COUNT(object->object_header);
  enum TAG tag = STELLA_OBJECT_HEADER_FIELD_COUNT(object->object_header);

  printf("%s%p: STELLA OBJECT of %d fields WITH tag %s of value ", prefix,
         object, field_count, stella_tag_to_string(tag));
  // print_stella_object(object);
  printf("\n");

  for (int i = 0; i < field_count; i++) {
    printf("%s%p: field #%d = %p(%s)\n", prefix,
           (char*)object + (1 + i) * WORD_SIZE, i, object->object_fields[i],
           stella_object_desc(object->object_fields[i]));
  }
}

static void print_to_space_object(stella_object* object) {
  print_object(object, "");
  STELLA_OBJECT_FIELDS_FOR(object, i, field) {
    if (belongs_to(field, from_space)) {
      print_object(field, "|\t\t\t");
    }
  }
}

static void print_to_space(void) {
  printf("Objects, not handled by GC:\n");
  print_object(&the_ZERO, "");
  print_object(&the_UNIT, "");
  print_object(&the_EMPTY, "");
  print_object(&the_EMPTY_TUPLE, "");
  print_object(&the_FALSE, "");
  print_object(&the_TRUE, "");
  printf("to-space:\n");

  void* current = to_space;

  while (diff_void(next, current) > 0) {
    stella_object* object = (stella_object*)current;
    print_to_space_object(object);
    current = advance_void(current, stella_object_size(object));
  }

  while (diff_void(limit, current) > 0) {
    printf("%p: NOTHING\n", current);
    current = advance_void(current, WORD_SIZE);
  }

  while (diff_void(current, to_space) < GC_INTERNAL_SPACE_SIZE) {
    stella_object* object = (stella_object*)current;
    print_to_space_object(object);
    current = advance_void(current, stella_object_size(object));
  }
}

/**
  Garbage collector API
*/

void* gc_alloc(size_t size_in_bytes) {
  gc_debug_log("Request for %zu bytes from state:\n", size_in_bytes);
  gc_debug_log_state();

  /** Move at least one word of data from from-space to to-space on every
   * allocation */
  if (scan != next) {
    gc_debug_log("coping memory during allocation\n");
    size_t memory_copied = 0;
    while (memory_copied == 0 && diff_void(next, scan) > 0) {
      stella_object* object = (stella_object*)scan;
      gc_debug_log("processing %p, next = %p, scan = %p\n", object, next, scan);
      memory_copied = deep_forward(object);
      scan = advance_void(scan, stella_object_size(object));
    }
  }

  gc_debug_log("free space is %td\n", diff_void(limit, next));

  if (diff_void(limit, next) < (ptrdiff_t)size_in_bytes) {
    gc_debug_log("not enough memory in to-space\n");
    /**
      to-space is not enough for new object so we:
      1) Ensure all object from old from-space moved to to-space (this may
      fail with out-of-memory) 2) flip 3) Ensure that now we have enough
      memory for new object
    */

    if (scan != next) {
      gc_force_copy_all();
      /** Now all alive data is inside to-space, so we can flip */
    }

    flip();

    if (diff_void(limit, next) < size_in_bytes) {
      out_of_memory_failure();
    }
  }

  total_allocated_bytes += size_in_bytes;
  total_allocated_objects += 1;
  current_allocated_bytes += size_in_bytes;
  current_allocated_objects += 1;
  max_allocated_bytes = max_allocated_bytes > current_allocated_bytes
                            ? max_allocated_bytes
                            : current_allocated_bytes;
  max_allocated_objects = max_allocated_objects > current_allocated_objects
                              ? max_allocated_objects
                              : current_allocated_objects;

  limit = advance_void(limit, -((ptrdiff_t)size_in_bytes));
  memset(limit, 0, size_in_bytes);

  gc_debug_log("after allocation:\n");
  gc_debug_log_state();

  return limit;
}

void print_gc_roots() {
  printf("ROOTS: ");
  for (int i = 0; i < gc_roots_top; i++) {
    printf("%p ", *gc_roots[i]);
  }
  printf("\n");
}

void print_gc_alloc_stats() {
  printf("Total memory allocation: %'d bytes (%'d objects)\n",
         total_allocated_bytes, total_allocated_objects);
  printf("Maximum residency:       %'d bytes (%'d objects)\n",
         max_allocated_bytes, max_allocated_objects);
  printf("Total memory use:        %'d reads and %'d writes\n", total_reads,
         total_writes);
  printf("Max GC roots stack size: %'d roots\n", gc_roots_max_size);
}

static void print_gc_state_variables(void) {
  printf("TO-SPACE: %p\n", to_space);
  printf("FROM-SPACE: %p\n", from_space);
  printf("NEXT pointer: %p (TO-SPACE + %td)\n", next,
         diff_void(next, to_space));
  printf("SCAN pointer: %p\n", scan);
  printf("LIMIT pointer: %p (NEXT + %td)\n", limit, diff_void(limit, next));
}

void print_gc_state(void) {
  print_gc_state_variables();
  print_gc_roots();
  print_to_space();
}

static noreturn void out_of_memory_failure(void) {
  gc_failed = true;
  printf("\n======  Failure: Out of memory  =======\n\nState:\n\n");
  print_gc_state_variables();
  print_gc_roots();
  printf("========================================\n");
  print_gc_alloc_stats();
  exit(1);
}

void gc_read_barrier(void* raw_object, int field_index) {
  stella_object* object = (stella_object*)raw_object;

  if (belongs_to(object->object_fields[field_index], from_space)) {
    size_t memory_copied = 0;
    object->object_fields[field_index] =
        forward(object->object_fields[field_index], &memory_copied);
  }

  total_reads += 1;
}

void gc_write_barrier(void* object, int field_index, void* contents) {
  total_writes += 1;
}

void gc_push_root(void** ptr) {
  gc_roots[gc_roots_top++] = ptr;
  if (gc_roots_top > gc_roots_max_size) {
    gc_roots_max_size = gc_roots_top;
  }
}

void gc_pop_root(void** ptr) {
  gc_roots_top--;
}
