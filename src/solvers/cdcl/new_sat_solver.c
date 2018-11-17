/*
 * This file is part of the Yices SMT Solver.
 * Copyright (C) 2017 SRI International.
 *
 * Yices is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Yices is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Yices.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * STAND-ALONE SAT SOLVER
 */

#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <float.h>

#include "solvers/cdcl/new_sat_solver.h"
#include "utils/cputime.h"
#include "utils/memalloc.h"
#include "utils/int_array_sort.h"
#include "utils/uint_array_sort.h"
#include "utils/uint_array_sort2.h"


/*
 * Set these flags to 1 for debugging, trace, data collection
 */
#define DEBUG 0
#define TRACE 0
#define DATA  0


#if DEBUG

/*
 * The following functions check internal consistency. They are defined
 * at the end of this file. They print an error on stderr if the checks fail.
 */
static void check_clause_pool_counters(const clause_pool_t *pool);
static void check_clause_pool_learned_index(const clause_pool_t *pool);
static void check_heap(const nvar_heap_t *heap);
static void check_candidate_clauses_to_delete(const sat_solver_t *solver, const cidx_t *a, uint32_t n);
static void check_watch_vectors(const sat_solver_t *solver);
static void check_propagation(const sat_solver_t *solver);
static void check_marks(const sat_solver_t *solver);
static void check_all_unmarked(const sat_solver_t *solver);
static void check_elim_heap(const sat_solver_t *solver);

#else

/*
 * Placeholders: do nothing
 */
static inline void check_clause_pool_counters(const clause_pool_t *pool) { }
static inline void check_clause_pool_learned_index(const clause_pool_t *pool) { }
static inline void check_heap(const nvar_heap_t *heap) { }
static inline void check_candidate_clauses_to_delete(const sat_solver_t *solver, const cidx_t *a, uint32_t n) { }
static inline void check_watch_vectors(const sat_solver_t *solver) { }
static inline void check_propagation(const sat_solver_t *solver) { }
static inline void check_marks(const sat_solver_t *solver) { }
static inline void check_all_unmarked(const sat_solver_t *solver) {}
static inline void check_elim_heap(const sat_solver_t *solver) {}

#endif



#if DATA

/*
 * DATA COLLECTION/STATISTICS
 */

/*
 * Open the internal data file
 * - if this fails, solver->data stays NULL and no data is collected
 */
void nsat_open_datafile(sat_solver_t *solver, const char *name) {
  solver->data = fopen(name, "w");
}

static void close_datafile(sat_solver_t *solver) {
  if (solver->data != NULL) {
    fclose(solver->data);
  }
}

static void reset_datafile(sat_solver_t *solver) {
  close_datafile(solver);
  solver->data = NULL;
}


/*
 * Write data after a conflict
 * - lbd = lbd of the learned clause
 *
 * When this is called:
 * - solver->conflict_tag = either CTAG_CLAUSE or CTAG_BINARY
 * - solver->conflict_index = index of the conflict clause (if CTAG_CLAUSE)
 * - solver->buffer = conflict clause (if CTAG_BINARY)
 * - solver->buffer contains the learned clause
 * - solver->decision_level = the conflict level
 * - solver->backtrack_level = where to backtrack
 * - solver->stats.conflicts = number of conflicts (including this one)
 * - solver->slow_ema, fast_ema have been updated
 *
 * Data exported:
 * - stats.conflicts
 * - stats.decisions
 * - stats.propagations
 * - slow_ema
 * - fast_ema
 * - lbd
 * - conflict level
 * - backtrack level
 * - size of the learned clause
 * - then the learned clause (as an array of literals)
 *
 * The data is stored as raw binary data (little endian for x86)
 */
typedef struct conflict_data {
  uint64_t conflicts;
  uint64_t decisions;
  uint64_t propagations;
  uint64_t slow_ema;
  uint64_t fast_ema;
  uint32_t lbd;
  uint32_t conflict_level;
  uint32_t backtrack_level;
  uint32_t learned_clause_size;
} conflict_data_t;

static void export_conflict_data(sat_solver_t *solver, uint32_t lbd) {
  conflict_data_t buffer;
  size_t w, n;

  if (solver->data != NULL) {
    buffer.conflicts = solver->stats.conflicts;
    buffer.decisions = solver->stats.decisions;
    buffer.propagations = solver->stats.propagations;
    buffer.slow_ema = solver->slow_ema;
    buffer.fast_ema = solver->fast_ema;
    buffer.lbd = lbd;
    buffer.conflict_level = solver->decision_level;
    buffer.backtrack_level = solver->backtrack_level;
    buffer.learned_clause_size = solver->buffer.size;;
    w = fwrite(&buffer, sizeof(buffer), 1, solver->data);
    if (w < 1) goto write_error;
    n = solver->buffer.size;
    w = fwrite(solver->buffer.data, sizeof(literal_t), n, solver->data);
    if (w < n) goto write_error;
  }

  return;

 write_error:
  // close and reset solver->data to zero
  perror("export_conflict_data");
  fprintf(stderr, "export_conflict_data: write failed at conflict %"PRIu64"\n", solver->stats.conflicts);
  fclose(solver->data);
  solver->data = NULL;
}

/*
 * Last conflict: at level 0, the learned clause is empty.
 */
static void export_last_conflict(sat_solver_t *solver) {
  conflict_data_t buffer;
  size_t w;

  if (solver->data != NULL) {
    buffer.conflicts = solver->stats.conflicts;
    buffer.decisions = solver->stats.decisions;
    buffer.propagations = solver->stats.propagations;
    buffer.slow_ema = solver->slow_ema;
    buffer.fast_ema = solver->fast_ema;
    buffer.lbd = 0;
    buffer.conflict_level = 0;
    buffer.backtrack_level = 0;
    buffer.learned_clause_size = 0;;
    w = fwrite(&buffer, sizeof(buffer), 1, solver->data);
    if (w < 1) goto write_error;
  }
  return;

 write_error:
  // close and reset solver->data to zero
  perror("export_last_conflict");
  fprintf(stderr, "export_last_conflict: write failed at conflict %"PRIu64"\n", solver->stats.conflicts);
  fclose(solver->data);
  solver->data = NULL;
}

#else

/*
 * Placeholders: they do nothing
 */
void nsat_open_datafile(sat_solver_t *solver, const char *name) { }

static inline void close_datafile(sat_solver_t *solver) { }
static inline void reset_datafile(sat_solver_t *solver) { }
static inline void export_conflict_data(sat_solver_t *solver, uint32_t lbd) { }
static inline void export_last_conflict(sat_solver_t *solver) { }

#endif




/************************
 *  DEFAULT PARAMETERS  *
 ***********************/

/*
 * Variable activities
 */
#define VAR_DECAY_FACTOR              0.95
#define VAR_ACTIVITY_THRESHOLD        (1e100)
#define INV_VAR_ACTIVITY_THRESHOLD    (1e-100)
#define INIT_VAR_ACTIVITY_INCREMENT   1.0

/*
 * Clause activities
 */
#define CLAUSE_DECAY_FACTOR            0.999F
#define CLAUSE_ACTIVITY_THRESHOLD      (1e20f)
#define INV_CLAUSE_ACTIVITY_THRESHOLD  (1e-20f)
#define INIT_CLAUSE_ACTIVITY_INCREMENT 1.0

/*
 * Default random_factor = 2% of decisions are random (more or less)
 * - the heuristic generates a random 24 bit integer
 * - if that number is <= random_factor * 2^24, then a random variable
 *   is chosen
 * - so we store random_factor * 2^24 = random_factor * 0x1000000 in
 *   the randomness field of a sat solver.
 */
#define VAR_RANDOM_FACTOR 0.02F

// mask to extract 24 bits out of an unsigned 32bit integer
#define VAR_RANDOM_MASK  ((uint32_t)0xFFFFFF)
#define VAR_RANDOM_SCALE (VAR_RANDOM_MASK+1)

/*
 * Clause deletion parameters
 * - we don't delete clauses of lbd <= keep_lbd
 * - we trigger the deletion when the number of learned clauses becomes
 *   larger than solver->reduce_next.
 * - the initial value of reduce_next is initially set to
 *     min(MIN_REDUCE_NEXT, number of problem clauses/4)
 * - after every reduction, the reduce_threhsold is updated to
 *    reduce_next * REDUCE_FACTOR
 * - each deletion round removes a fraction of the clauses equal
 *   to REDUCE_FRACTION/32 (approximately).
 */
#define KEEP_LBD 4
#define MIN_REDUCE_NEXT 1000
#define REDUCE_FACTOR 1.05
#define REDUCE_FRACTION 16

#define REDUCE_INTERVAL 2000
#define REDUCE_DELTA    300


/*
 * We use two modes:
 * - search_mode is the default. In this mode, we're trying to
 *   learn useful clauses (low LBD).
 * - if we don't learn small clauses for a long time, we switch
 *   to diving. In this mode, we hope the formula is satisfiable
 *   and we try to go deep into the search tree.
 * To determine when to switch to diving mode, we use a search_period
 * and a search_counter.
 * - every search_period conflicts, we check whether we're making
 *   progress. If we don't make progress for search_counter successive
 *   periods, we switch to diving.
 */
#define SEARCH_PERIOD 10000
#define SEARCH_COUNTER 20

/*
 * Minimal Number of conflicts between two restarts
 */
#define RESTART_INTERVAL 10

/*
 * Stacking of learned clauses
 * - clauses of LBD higher than this threshold are not stored in the
 *   data set but in the stack (of temporary clauses).
 */
#define STACK_THRESHOLD 4

/*
 * Diving
 * - diving budget = number of conflicts after which we stop diving
 */
#define DIVING_BUDGET 10000

/*
 * Parameters to control preprocessing
 *
 * - subsumption checks can be expensive. To reduce the cost,
 *   we don't check whether a clause C subsumes anything if that would
 *   require visiting more than subsume_skip clauses.
 *
 * - for variable elimination, we only consider variables that have
 *   few positive or few negative occurrences. If x has too many
 *   positive and negative occurrence, it's not likely that we'll be
 *   able to eliminate x anyway.
 *
 * - we also don't want to create large clauses when eliminating
 *   variables, so we don't eliminate x if that would create a
 *   clause of size > res_clause_limit
 */
#define SUBSUME_SKIP 3000
#define VAR_ELIM_SKIP 10
#define RES_CLAUSE_LIMIT 20

/*
 * Parameters to control simplify
 */
#define SIMPLIFY_INTERVAL 100
#define SIMPLIFY_BIN_DELTA 100




/**********
 *  PRNG  *
 *********/

/*
 * PARAMETERS FOR THE PSEUDO RANDOM NUMBER GENERATOR
 *
 * We  use the same linear congruence as in prng.h,
 * but we use a local implementation so that different
 * solvers can use different seeds.
 */

#define PRNG_MULTIPLIER 1664525
#define PRNG_CONSTANT   1013904223
#define PRNG_SEED       0xabcdef98


/*
 * Return a 32bit unsigned int
 */
static inline uint32_t random_uint32(sat_solver_t *s) {
  uint32_t x;

  x = s->prng;
  s->prng = x * ((uint32_t) PRNG_MULTIPLIER) + ((uint32_t) PRNG_CONSTANT);
  return x;
}


/*
 * Return a 32bit integer between 0 and n-1
 */
static inline uint32_t random_uint(sat_solver_t *s, uint32_t n) {
  return (random_uint32(s) >> 8) % n;
}


/*********************
 *  INTEGER VECTOR   *
 ********************/

/*
 * Capacity increase for vectors:
 * - about 50% increase rounded up to a multiple of four
 */
static inline uint32_t vector_cap_increase(uint32_t cap) {
  return ((cap >> 1) + 8) & ~3;
}

/*
 * Initialize
 */
static void init_vector(vector_t *v) {
  uint32_t n;

  n = DEF_VECTOR_SIZE;
  assert(n <= MAX_VECTOR_SIZE);
  v->data = (uint32_t *) safe_malloc(n * sizeof(uint32_t));
  v->capacity = n;
  v->size = 0;
}

/*
 * Make it larger.
 */
static void extend_vector(vector_t *v) {
  uint32_t n;

  n = v->capacity + vector_cap_increase(v->capacity);
  assert(n > v->capacity);
  if (n > MAX_VECTOR_SIZE) {
    out_of_memory();
  }
  v->data = (uint32_t *) safe_realloc(v->data, n * sizeof(uint32_t));
  v->capacity = n;
}

/*
 * Add integer x at the end of v
 */
static void vector_push(vector_t *v, uint32_t x) {
  uint32_t i;

  i = v->size;
  if (i == v->capacity) {
    extend_vector(v);
  }
  assert(i < v->capacity);
  v->data[i] = x;
  v->size = i+1;
}

/*
 * Remove the last element and return it
 * - v must not be empty
 */
static uint32_t vector_pop(vector_t *v) {
  assert(v->size > 0);
  v->size --;
  return v->data[v->size];
}

/*
 * Reset: empty the buffer
 */
static inline void reset_vector(vector_t *v) {
  v->size = 0;
}

/*
 * Reset and make room for one element (literal)
 */
static inline void vector_reset_and_reserve(vector_t *v) {
  assert(v->capacity >= 1);
  v->size = 1;
}

/*
 * Free memory
 */
static void delete_vector(vector_t *v) {
  safe_free(v->data);
  v->data = NULL;
}



/*******************
 *  INTEGER QUEUE  *
 ******************/

/*
 * Capacity increase: same as for vector
 */
static inline uint32_t queue_cap_increase(uint32_t cap) {
  return ((cap >> 1) + 8) & ~3;
}

/*
 * Initialize
 */
static void init_queue(queue_t *q) {
  uint32_t n;

  n = DEF_QUEUE_SIZE;
  assert(n <= MAX_QUEUE_SIZE);
  q->data = (uint32_t *) safe_malloc(n * sizeof(uint32_t));
  q->capacity = n;
  q->head = 0;
  q->tail = 0;
}

/*
 * Make the queue bigger
 */
static void extend_queue(queue_t *q) {
  uint32_t n;

  n = q->capacity + queue_cap_increase(q->capacity);
  assert(n > q->capacity);
  if (n > MAX_QUEUE_SIZE) {
    out_of_memory();
  }
  q->data = (uint32_t *) safe_realloc(q->data, n * sizeof(uint32_t));
  q->capacity = n;
}

/*
 * Add x at the end of the queue
 */
static void queue_push(queue_t *q, uint32_t x) {
  uint32_t i, n, j;

  i = q->tail;
  q->data[i] = x;
  i++;
  if (i == q->capacity) {
    i = 0;
  }
  q->tail = i;

  if (i == q->head) {
    /*
     * full queue in q->data[0 ... i-1] + q->data[head .. cap-1].
     * make the array bigger
     * if i>0, shift data[head ... cap - 1] to the end of the new array.
     */
    n = q->capacity;    // cap before increase
    extend_queue(q);
    if (i == 0) {
      q->tail = n;
    } else {
      j = q->capacity;
      do {
        n --;
        j --;
        q->data[j] = q->data[n];
      } while (n > i);
      q->head = j;
    }
  }
}


/*
 * Check emptiness
 */
static inline bool queue_is_empty(const queue_t *q) {
  return q->head == q->tail;
}


/*
 * Remove the first element and return it.
 * - the queue must not be empty
 */
static uint32_t queue_pop(queue_t *q) {
  uint32_t x;
  uint32_t i;

  assert(! queue_is_empty(q));

  i = q->head;
  x = q->data[i];
  i ++;
  q->head = (i < q->capacity) ? i : 0;

  return x;
}


/*
 * Empty the queue
 */
static inline void reset_queue(queue_t *q) {
  q->head = 0;
  q->tail = 0;
}


/*
 * Delete
 */
static void delete_queue(queue_t *q) {
  safe_free(q->data);
  q->data = NULL;
}



/*********************************
 *  STACK FOR IMPLICATION GRAPH  *
 ********************************/

/*
 * Initialize the stack. Nothing allocated yet.
 */
static void init_gstack(gstack_t *gstack) {
  gstack->data = NULL;
  gstack->top = 0;
  gstack->size = 0;
}

/*
 * Increment in size: 50% of the current size, rounded up to a multiple of 2.
 */
static inline uint32_t gstack_size_increase(uint32_t n) {
  return ((n>>1) + 3) & ~1;
}

/*
 * Make the stack larger
 */
static void extend_gstack(gstack_t *gstack) {
  uint32_t n;

  n = gstack->size;
  if (n == 0) {
    // first allocation
    n = DEF_GSTACK_SIZE;
    assert(n <= MAX_GSTACK_SIZE);
    gstack->data = (gstack_elem_t *) safe_malloc(n * sizeof(gstack_elem_t));
    gstack->size = n;
  } else {
    // increase size by 50%, rounded to a multiple of 2
    n += gstack_size_increase(n);
    if (n > MAX_GSTACK_SIZE) {
      out_of_memory();
    }
    gstack->data = (gstack_elem_t *) safe_realloc(gstack->data, n * sizeof(gstack_elem_t));
    gstack->size = n;
  }
}

/*
 * Delete the stack
 */
static void delete_gstack(gstack_t *gstack) {
  safe_free(gstack->data);
  gstack->data = NULL;
}

/*
 * Push pair (x, n) on the stack
 */
static void gstack_push_vertex(gstack_t *gstack, uint32_t x, uint32_t n) {
  uint32_t i;

  i = gstack->top;
  if (i == gstack->size) {
    extend_gstack(gstack);
  }
  assert(i < gstack->size);
  gstack->data[i].vertex = x;
  gstack->data[i].index = n;
  gstack->top = i+1;
}

/*
 * Check emptiness
 */
static inline bool gstack_is_empty(gstack_t *gstack) {
  return gstack->top == 0;
}

/*
 * Get top element
 */
static inline gstack_elem_t *gstack_top(gstack_t *gstack) {
  assert(gstack->top > 0);
  return gstack->data + (gstack->top - 1);
}

/*
 * Remove the top element
 */
static inline void gstack_pop(gstack_t *gstack) {
  assert(gstack->top > 0);
  gstack->top --;
}

/*
 * Empty the stack
 */
static inline void reset_gstack(gstack_t *gstack) {
  gstack->top = 0;
}






/******************
 *  CLAUSE POOL   *
 *****************/

/*
 * Capacity increase:
 * cap += ((cap >> 1) + (cap >> 6) + (cap >> 7) + 2048) & ~3
 *
 * Since the initial capacity is 262144, we get an increasing
 * sequence: 262144, 401408, 613568,  ..., 4265187980,
 * which gets us close to 2^32.  The next increase after that
 * causes an arithmetic overflow.
 */
static inline uint32_t pool_cap_increase(uint32_t cap) {
  return ((cap >> 1) + (cap >> 6) + (cap >> 7) + 2048) & ~3;
}

/*
 * Maximal capacity after reset.
 * On a call to reset, we try to save memory by reducing
 * the pool capacity to this. This size is what we'd get
 * after 14 rounds on pool_cal_increase (about 126 MB).
 */
#define RESET_CLAUSE_POOL_CAPACITY 33155608

static bool is_multiple_of_four(uint32_t x) {
  return (x & 3) == 0;
}


/*
 * Some consistency checks
 */
#ifndef NDEBUG
static bool clause_pool_invariant(const clause_pool_t *pool) {
  return
    pool->learned <= pool->size &&
    pool->size <= pool->capacity &&
    pool->available == pool->capacity - pool->size &&
    is_multiple_of_four(pool->learned) &&
    is_multiple_of_four(pool->size) &&
    is_multiple_of_four(pool->capacity);
}
#endif

/*
 * Global operations
 */
static void init_clause_pool(clause_pool_t *pool) {
  pool->data = (uint32_t *) safe_malloc(DEF_CLAUSE_POOL_CAPACITY * sizeof(uint32_t));
  pool->learned = 0;
  pool->size = 0;
  pool->capacity = DEF_CLAUSE_POOL_CAPACITY;
  pool->available = DEF_CLAUSE_POOL_CAPACITY;
  pool->padding = 0;

  pool->num_prob_clauses = 0;
  pool->num_prob_literals = 0;
  pool->num_learned_clauses = 0;
  pool->num_learned_literals = 0;

  assert(clause_pool_invariant(pool));
}

static void delete_clause_pool(clause_pool_t *pool) {
  assert(clause_pool_invariant(pool));
  safe_free(pool->data);
  pool->data = NULL;
}

static void reset_clause_pool(clause_pool_t *pool) {
  assert(clause_pool_invariant(pool));

  if (pool->capacity > RESET_CLAUSE_POOL_CAPACITY) {
    safe_free(pool->data);
    pool->data = (uint32_t *) safe_malloc(RESET_CLAUSE_POOL_CAPACITY * sizeof(uint32_t));
    pool->capacity = RESET_CLAUSE_POOL_CAPACITY;
  }

  pool->learned = 0;
  pool->size = 0;
  pool->available = pool->capacity;
  pool->padding = 0;

  pool->num_prob_clauses = 0;
  pool->num_prob_literals = 0;
  pool->num_learned_clauses = 0;
  pool->num_learned_literals = 0;

  assert(clause_pool_invariant(pool));
}


/*
 * Make sure there's enough room for allocating n elements
 * - this should be called only when resize is required
 */
static void resize_clause_pool(clause_pool_t *pool, uint32_t n) {
  uint32_t min_cap, cap, increase;

  assert(clause_pool_invariant(pool));

  min_cap = pool->size + n;
  if (min_cap < n || min_cap > MAX_CLAUSE_POOL_CAPACITY) {
    // can't make the pool large enough
    out_of_memory();
  }

  cap = pool->capacity;
  do {
    increase = pool_cap_increase(cap);
    cap += increase;
    if (cap < increase) { // arithmetic overflow
      cap = MAX_CLAUSE_POOL_CAPACITY;
    }
  } while (cap < min_cap);

  pool->data = (uint32_t *) safe_realloc(pool->data, cap * sizeof(uint32_t));;
  pool->capacity = cap;
  pool->available = cap - pool->size;

  assert(clause_pool_invariant(pool));
}


/*
 * Allocate an array of n integers in the pool and return its idx
 */
static cidx_t clause_pool_alloc_array(clause_pool_t *pool, uint32_t n) {
  cidx_t i;

  assert(clause_pool_invariant(pool));

  n = (n + 3) & ~3; // round up to the next multiple of 4
  if (n > pool->available) {
    resize_clause_pool(pool, n);
  }
  assert(n <= pool->available);

  i = pool->size;
  pool->size += n;
  pool->available -= n;

  assert(clause_pool_invariant(pool));

  return i;
}


/*
 * CLAUSE ADDITION
 */

/*
 * Initialize the clause that starts at index cidx:
 * - set the header: length = n, aux = 0
 * - copy the literals
 */
static void clause_pool_init_clause(clause_pool_t *pool, cidx_t cidx, uint32_t n, const literal_t *a) {
  uint32_t i;
  uint32_t *p;

  pool->data[cidx] = n;
  pool->data[cidx + 1] = 0;
  p = pool->data + cidx + 2;
  for (i=0; i<n; i++) {
    p[i] = a[i];
  }
}

/*
 * Add a problem clause
 */
static cidx_t clause_pool_add_problem_clause(clause_pool_t *pool, uint32_t n, const literal_t *a) {
  uint32_t cidx;

  assert(pool->learned == pool->size);

  cidx = clause_pool_alloc_array(pool, n+2);
  clause_pool_init_clause(pool, cidx, n, a);

  pool->num_prob_clauses ++;
  pool->num_prob_literals += n;
  pool->learned = pool->size;

  return cidx;
}

/*
 * Add a learned clause
 */
static cidx_t clause_pool_add_learned_clause(clause_pool_t *pool, uint32_t n, const literal_t *a) {
  uint32_t cidx;

  cidx = clause_pool_alloc_array(pool, n+2);
  clause_pool_init_clause(pool, cidx, n, a);

  pool->num_learned_clauses ++;
  pool->num_learned_literals += n;

  return cidx;
}


/*
 * ACCESS CLAUSES
 */
#ifndef NDEBUG
static inline bool good_clause_idx(const clause_pool_t *pool, cidx_t idx) {
  return ((idx & 3) == 0) && idx < pool->size;
}
#endif

static inline bool is_learned_clause_idx(const clause_pool_t *pool, cidx_t idx) {
  assert(good_clause_idx(pool, idx));
  return  idx >= pool->learned;
}

static inline bool is_problem_clause_idx(const clause_pool_t *pool, cidx_t idx) {
  assert(good_clause_idx(pool, idx));
  return  idx < pool->learned;
}

static inline nclause_t *clause_of_idx(const clause_pool_t *pool, cidx_t idx) {
  assert(good_clause_idx(pool, idx));
  return (nclause_t *) ((char *) (pool->data + idx));
}



/*
 * MARKS ON CLAUSES
 */

/*
 * In preprocessing and during garbage collection, we mark clauses
 * by setting the high-order bit of the clause's length.
 * This is safe since a clause can't have more than MAX_VARIABLES literals
 * and MAX_VARIABLES < 2^31.
 */
#define CLAUSE_MARK (((uint32_t) 1) << 31)

static inline void mark_clause(clause_pool_t *pool, cidx_t idx) {
  assert(good_clause_idx(pool, idx));
  pool->data[idx] |= CLAUSE_MARK;
}

static inline void unmark_clause(clause_pool_t *pool, cidx_t idx) {
  assert(good_clause_idx(pool, idx));
  pool->data[idx] &= ~CLAUSE_MARK;
}

static inline bool clause_is_unmarked(const clause_pool_t *pool, cidx_t idx) {
  assert(good_clause_idx(pool, idx));
  return (pool->data[idx] & CLAUSE_MARK) == 0;
}

#ifndef NDEBUG
static inline bool clause_is_marked(const clause_pool_t *pool, cidx_t idx) {
  return !clause_is_unmarked(pool, idx);
}
#endif


/*
 * Length of a clause
 */
static inline uint32_t clause_length(const clause_pool_t *pool, cidx_t idx) {
  assert(good_clause_idx(pool, idx));
  return pool->data[idx] & ~CLAUSE_MARK;
}

/*
 * Start of the literal array for clause idx
 */
static inline literal_t *clause_literals(const clause_pool_t *pool, cidx_t idx) {
  assert(good_clause_idx(pool, idx));
  return (literal_t *) pool->data + idx + 2;
}

/*
 * Full size of a clause of n literals:
 * - 2 + n, rounded up to the next multiple of four
 */
static inline uint32_t full_length(uint32_t n) {
  return (n + 5) & ~3;
}

static inline uint32_t clause_full_length(const clause_pool_t *pool, uint32_t idx) {
  return full_length(clause_length(pool, idx));
}


/*
 * Get watch literals of clause cidx
 * - the first literal is the implied literal if any
 */
static inline literal_t first_literal_of_clause(const clause_pool_t *pool, cidx_t cidx) {
  assert(good_clause_idx(pool, cidx));
  return pool->data[cidx + 2];
}

static inline literal_t second_literal_of_clause(const clause_pool_t *pool, cidx_t cidx) {
  assert(good_clause_idx(pool, cidx));
  return pool->data[cidx + 3];
}


/*
 * Watched literal that's not equal to l
 */
static literal_t other_watched_literal_of_clause(const clause_pool_t *pool, cidx_t cidx, literal_t l) {
  literal_t l0, l1;
  l0 = first_literal_of_clause(pool, cidx);
  l1 = second_literal_of_clause(pool, cidx);
  assert(l0 == l || l1 == l);
  return l0 ^ l1 ^ l;
}


/*
 * CLAUSE ACTIVITY
 */
static inline void set_learned_clause_activity(clause_pool_t *pool, cidx_t cidx, float act) {
  nclause_t *c;

  assert(is_learned_clause_idx(pool, cidx) && sizeof(float) == sizeof(uint32_t));

  c = clause_of_idx(pool, cidx);
  c->aux.f = act;
}

static inline float get_learned_clause_activity(const clause_pool_t *pool, cidx_t cidx) {
  nclause_t *c;

  assert(is_learned_clause_idx(pool, cidx) && sizeof(float) == sizeof(uint32_t));

  c = clause_of_idx(pool, cidx);
  return c->aux.f;
}

static inline void increase_learned_clause_activity(clause_pool_t *pool, cidx_t cidx, float incr) {
  nclause_t *c;

  assert(is_learned_clause_idx(pool, cidx) && sizeof(float) == sizeof(uint32_t));

  c = clause_of_idx(pool, cidx);
  c->aux.f += incr;
}

static inline void multiply_learned_clause_activity(clause_pool_t *pool, cidx_t cidx, float scale) {
  nclause_t *c;

  assert(is_learned_clause_idx(pool, cidx) && sizeof(float) == sizeof(uint32_t));

  c = clause_of_idx(pool, cidx);
  c->aux.f *= scale;
}


/*
 * SIGNATURE/ABSTRACTION OF A CLAUSE
 */

/*
 * To accelerate subsumption checking, we keep track of the variables occurring in clause cidx
 * as a 32-bit vector in the clause's auxiliary data.
 */
static inline uint32_t var_signature(bvar_t x) {
  return 1u << (x & 31u);
}

static void set_clause_signature(clause_pool_t *pool, cidx_t cidx) {
  nclause_t *c;
  uint32_t i, n, w;

  assert(is_problem_clause_idx(pool, cidx));

  w = 0;
  c = clause_of_idx(pool, cidx);
  n = c->len & ~CLAUSE_MARK;
  for (i=0; i<n; i++) {
    w |= var_signature(var_of(c->c[i]));
  }
  c->aux.d = w;
}

static inline uint32_t clause_signature(clause_pool_t *pool, cidx_t cidx) {
  nclause_t *c;

  assert(is_problem_clause_idx(pool, cidx));

  c = clause_of_idx(pool, cidx);
  return c->aux.d;
}




/*
 * PADDING BLOCKS
 */

/*
 * Check whether i is the start of a padding block
 */
static inline bool is_padding_start(const clause_pool_t *pool, uint32_t i) {
  assert(i < pool->size && is_multiple_of_four(i));
  return pool->data[i] == 0;
}

/*
 * Check whether i is the start of a clause
 */
static inline bool is_clause_start(const clause_pool_t *pool, uint32_t i) {
  return !is_padding_start(pool, i);
}

/*
 * Length of the padding block that starts at index i
 */
static inline uint32_t padding_length(const clause_pool_t *pool, uint32_t i) {
  assert(is_padding_start(pool, i));
  return pool->data[i+1];
}


/*
 * Store a padding block of size n at index i
 * - we want to keep i in the interval [0 ... pool->size - 1]
 */
static void clause_pool_padding(clause_pool_t *pool, uint32_t i, uint32_t n) {
  uint32_t j;

  assert(i < pool->size && is_multiple_of_four(i)
         && is_multiple_of_four(n) && n > 0);

  pool->padding += n;

  j = i+n;
  if (j < pool->size && is_padding_start(pool, j)) {
    // merge the two padding blocks
    n += padding_length(pool, j);
  }
  pool->data[i] = 0;
  pool->data[i+1] = n;

  assert(clause_pool_invariant(pool));
}


/*
 * DELETE CLAUSES
 */

/*
 * Delete the clause that start at index idx
 */
static void clause_pool_delete_clause(clause_pool_t *pool, cidx_t idx) {
  uint32_t n;

  assert(good_clause_idx(pool, idx));

  n = clause_length(pool, idx);

  // update the statistics: we must do this first because
  // padding may reduce pool->size.
  if (is_problem_clause_idx(pool, idx)) {
    assert(pool->num_prob_clauses > 0);
    assert(pool->num_prob_literals >= n);
    pool->num_prob_clauses --;
    pool->num_prob_literals -= n;
  } else {
    assert(pool->num_learned_clauses > 0);
    assert(pool->num_learned_literals >= n);
    pool->num_learned_clauses --;
    pool->num_learned_literals -= n;
  }

  clause_pool_padding(pool, idx, full_length(n));
}


/*
 * Shrink clause idx: n = new size
 */
static void clause_pool_shrink_clause(clause_pool_t *pool, cidx_t idx, uint32_t n) {
  uint32_t old_n, old_len, new_len, mark;

  assert(good_clause_idx(pool, idx) && n >= 2 && n <= clause_length(pool, idx));

  old_n = pool->data[idx];    // length + mark
  mark = old_n & CLAUSE_MARK; // mark only
  old_n &= ~CLAUSE_MARK;      // length

  assert(old_n == clause_length(pool, idx));

  old_len = full_length(old_n);
  new_len = full_length(n);

  if (is_problem_clause_idx(pool, idx)) {
    assert(pool->num_prob_clauses > 0);
    assert(pool->num_prob_literals >= old_n);
    pool->num_prob_literals -= (old_n - n);
  } else {
    assert(pool->num_learned_clauses > 0);
    assert(pool->num_learned_literals >= old_n);
    pool->num_learned_literals -= (old_n - n);
  }

  assert(new_len <= old_len);
  if (new_len < old_len) {
    clause_pool_padding(pool, idx + new_len, old_len - new_len);
  }

  pool->data[idx] = mark | n;
}


/*
 * SCAN THE SET OF CLAUSES
 */

/*
 * Find the next clause, scanning from index i
 * - i may be the start of a clause or a padding block
 * - if there's no more clause after i then we return pool->size
 */
static cidx_t next_clause_index(const clause_pool_t *pool, cidx_t i) {
  while (i < pool->size && is_padding_start(pool, i)) {
    i += padding_length(pool, i);
  }
  return i;
}

static inline cidx_t clause_pool_first_clause(const clause_pool_t *pool) {
  return next_clause_index(pool, 0);
}

static inline cidx_t clause_pool_first_learned_clause(const clause_pool_t *pool) {
  return next_clause_index(pool, pool->learned);
}

/*
 * Clause that follows idx:
 * - idx may be either the start of a padding block, or the start of a clause,
 *   or the end mark (pool->size)
 */
static cidx_t clause_pool_next_clause(const clause_pool_t *pool, cidx_t idx) {
  uint32_t n;

  assert(idx <= pool->size);

  if (idx == pool->size) {
    return idx;
  }

  n = 0;
  if (is_clause_start(pool, idx)) {
    n = clause_full_length(pool, idx);
  }
  return next_clause_index(pool, idx + n);
}

/*
 * Check whether cidx is a valid clause
 * - cidx is an integer stored in a watch vector.
 * - it can be a placeholder for a clause that was removed from the watch vector
 *   (then cidx is not  a multiple of four).
 * - otherwise, cidx is a multiple of four, we check whether cidx
 *   is the start of a clause (it can also be the start of a padding block)
 */
static inline bool clause_is_live(const clause_pool_t *pool, cidx_t cidx) {
  return is_multiple_of_four(cidx) && is_clause_start(pool, cidx);
}



/*****************
 *  WATCH LISTS  *
 ****************/

/*
 * Initial capacity: smallish.
 *
 * We set MAX_WATCH_CAPACITY to ensure two properties:
 * 1) (MAX + watch_cap_increase(MAX)) doesn't overflow for uint32_t.
 * 2) (sizeof(watch_t) + MAX * sizeof(unit32_t)) doesn't overflow for size_t.
 *
 * For condition 1, we need MAX <= 0xAAAAAAA7 = 2863311527.
 * For condition 2, we need MAX <= (SIZE_MAX/4) - 2.
 */
#define DEF_WATCH_CAPACITY 6

#if ((SIZE_MAX/4) - 2) < 2863311527
#define MAX_WATCH_CAPACITY ((uint32_t) ((SIZE_MAX/4) - 2))
#else
#define MAX_WATCH_CAPACITY ((uint32_t) 2863311527)
#endif


/*
 * Capacity increase for watch vectors:
 * - about 50% increase, rounded up to force the increment to be a multiple of four
 */
static inline uint32_t watch_cap_increase(uint32_t cap) {
  return ((cap >> 1) + 8) & ~3;
}

/*
 * Allocate or extend vector v
 * - this makes sure there's room for k more element
 * - k should be 1 or 2
 * Returns v unchanged if v's capacity is large enough.
 * Returns the newly allocated/extended v otherwise.
 */
static watch_t *resize_watch(watch_t *v, uint32_t k) {
  uint32_t i, n;

  assert(k <= 2);

  if (v == NULL) {
    n = DEF_WATCH_CAPACITY;
    v = (watch_t *) safe_malloc(sizeof(watch_t) + n * sizeof(uint32_t));
    v->capacity = n;
    v->size = 0;
    assert(n >= k);
  } else {
    i = v->size;
    n = v->capacity;
    if (i + k > n) {
      n += watch_cap_increase(n);
      if (n > MAX_WATCH_CAPACITY) {
        out_of_memory();
      }
      v = (watch_t *) safe_realloc(v, sizeof(watch_t) + n * sizeof(uint32_t));
      v->capacity = n;
      assert(i + k <= n);
    }
  }

  return v;
}

/*
 * Make v smaller if possible.
 * - v must not be NULL
 */
static watch_t *shrink_watch(watch_t *v) {
  uint32_t n, cap;

  assert(v != NULL && v->size <= v->capacity && v->capacity <= MAX_WATCH_CAPACITY);

  n = v->size;

  // search for the minimal capacity >= v->size
  // since n <= MAX_WATCH_CAPACITY, there's no risk of numerical overflow
  cap = DEF_WATCH_CAPACITY;
  while (cap < n) {
    cap += watch_cap_increase(cap);
  }

  if (cap < v->capacity) {
    v = (watch_t *) safe_realloc(v, sizeof(watch_t) + cap * sizeof(uint32_t));
    v->capacity = cap;
    assert(v->size <= v->capacity);
  }

  return v;
}


/*
 * Reset: empty w. It must not be null
 */
static inline void reset_watch(watch_t *w) {
  w->size = 0;
}


/*
 * Add k at the end of vector *w.
 * - if *w is NULL, allocate a vector of default size
 * - if *w if full, make it 50% larger.
 */
static void add_watch(watch_t **w, uint32_t k) {
  watch_t *v;
  uint32_t i;

  v = resize_watch(*w, 1);
  *w = v;
  i = v->size;
  assert(i < v->capacity);
  v->data[i] = k;
  v->size = i+1;
}

/*
 * Add two elements k1 and k2 at the end of vector *w
 */
static void add_watch2(watch_t **w, uint32_t k1, uint32_t k2) {
  watch_t *v;
  uint32_t i;

  v = resize_watch(*w, 2);
  *w = v;
  i = v->size;
  assert(i + 1 < v->capacity);
  v->data[i] = k1;
  v->data[i+1] = k2;
  v->size = i+2;
}

/*
 * Delete all watch vectors in w[0 ... n-1]
 */
static void delete_watch_vectors(watch_t **w, uint32_t n) {
  uint32_t i;

  for (i=0; i<n; i++) {
    safe_free(w[i]);
    w[i] = NULL;
  }
}



/*************************
 *  SAVED-CLAUSE VECTOR  *
 ************************/

/*
 * Initialization: don't allocate anything yet.
 */
static void init_clause_vector(nclause_vector_t *v) {
  v->data = NULL;
  v->top = 0;
  v->capacity = 0;
}

/*
 * Free memory
 */
static void delete_clause_vector(nclause_vector_t *v) {
  safe_free(v->data);
  v->data = NULL;
}

/*
 * Empty the vector
 */
static void reset_clause_vector(nclause_vector_t *v) {
  v->top = 0;
}


/*
 * Capacity increase: add about 50%
 */
static uint32_t clause_vector_new_cap(uint32_t cap) {
  uint32_t ncap;

  if (cap == 0) {
    ncap = DEF_CLAUSE_VECTOR_CAPACITY;
  } else {
    ncap = cap + (((cap >> 1) + 8) & ~3);
    if (ncap < cap) { // arithmetic overflow
      ncap = MAX_CLAUSE_VECTOR_CAPACITY;
    }
  }
  return ncap;
}


/*
 * Make room for at least (n + 1) elements at the end of v->data.
 */
static void resize_clause_vector(nclause_vector_t *v, uint32_t n) {
  uint32_t new_top, cap;

  new_top = v->top + n + 1;
  if (new_top <= v->top || new_top > MAX_CLAUSE_VECTOR_CAPACITY) {
    // arithmetic overflow or request too large
    out_of_memory();
  }

  if (v->capacity < new_top) {
    cap = clause_vector_new_cap(v->capacity);
    while (cap < new_top) {
      cap = clause_vector_new_cap(cap);
    }
    v->data = (uint32_t *) safe_realloc(v->data, cap * sizeof(uint32_t));
    v->capacity = cap;
  }
}


/*
 * Store clause a[0 ... n-1] at the end of v
 * - l = distinguished literal in the clause (stored last).
 * - l must occur in a[0 ... n-1]
 * - the vector must have room for n literals
 */
static void clause_vector_save_clause(nclause_vector_t *v, uint32_t n, const literal_t *a, literal_t l) {
  uint32_t i, j;
  literal_t z;

  assert(v->top + n <= v->capacity);

  j = v->top;
  for (i=0; i<n; i++) {
    z = a[i];
    if (z != l) {
      v->data[j] = z;
      j ++;
    }
  }
  assert(j - v->top == n - 1);
  v->data[j] = l;
  v->top = j+1;
}


/*
 * Store s (block size) at the end of v
 */
static void clause_vector_add_block_length(nclause_vector_t *v, uint32_t s) {
  uint32_t j;

  j = v->top;
  assert(j < v->capacity);
  v->data[j] = s;
  v->top = j+1;
}

/*
 * Store block for a variable eliminated by substitution:
 * - for l := l0, we store l0, not(l), 2.
 */
static void clause_vector_save_subst_clause(nclause_vector_t *v, literal_t l0, literal_t l) {
  uint32_t j;

  resize_clause_vector(v, 2);
  assert(v->top + 3 <= v->capacity);

  j = v->top;
  v->data[j] = l0;
  v->data[j+1] = not(l);
  v->data[j+2] = 2;
  v->top = j + 3;
}



/**********************
 *  ELIMINATION HEAP  *
 *********************/

/*
 * Initialize: don't allocate anything yet
 */
static void init_elim_heap(elim_heap_t *heap) {
  heap->data = NULL;
  heap->elim_idx = NULL;
  heap->size = 0;
  heap->capacity = 0;
}

/*
 * Prepare: n = number of variables
 * - this allocates the data array and the elim_idx array
 */
static void prepare_elim_heap(elim_heap_t *heap, uint32_t n) {
  uint32_t k;

  assert(heap->data == NULL && heap->elim_idx == NULL && n > 0);

  k = DEF_ELIM_HEAP_SIZE;
  assert(0 < k && k <= MAX_ELIM_HEAP_SIZE);
  heap->data = (bvar_t *) safe_malloc(k * sizeof(bvar_t));
  heap->elim_idx = (int32_t *) safe_malloc(n * sizeof(int32_t));
  heap->size = 1;
  heap->capacity = k;

  heap->data[0] = 0;
  heap->elim_idx[0] = 0;
  for (k=1; k<n; k++) {
    heap->elim_idx[k] = -1;
  }
}

/*
 * Capacity increase for the data array
 */
static inline uint32_t elim_heap_cap_increase(uint32_t cap) {
  return ((cap >> 1) + 8) & ~3;
}

/*
 * Make the data array larger
 */
static void extend_elim_heap(elim_heap_t *heap) {
  uint32_t n;

  n = heap->capacity + elim_heap_cap_increase(heap->capacity);
  assert(n > heap->capacity);
  if (n > MAX_ELIM_HEAP_SIZE) {
    out_of_memory();
  }
  heap->data = (bvar_t *) safe_realloc(heap->data, n * sizeof(bvar_t));
  heap->capacity = n;
}

static void delete_elim_heap(elim_heap_t *heap) {
  safe_free(heap->data);
  safe_free(heap->elim_idx);
  heap->data = NULL;
  heap->elim_idx = NULL;
}

static void reset_elim_heap(elim_heap_t *heap) {
  delete_elim_heap(heap);
  heap->size = 0;
  heap->capacity = 0;
}



/**********************
 *  ASSIGNMENT STACK  *
 *********************/

/*
 * Initialize stack s for nvar
 */
static void init_stack(sol_stack_t *s, uint32_t nvar) {
  s->lit = (literal_t *) safe_malloc(nvar * sizeof(literal_t));
  s->level_index = (uint32_t *) safe_malloc(DEFAULT_NLEVELS * sizeof(uint32_t));
  s->level_index[0] = 0;
  s->top = 0;
  s->prop_ptr = 0;
  s->nlevels = DEFAULT_NLEVELS;
}

/*
 * Extend the stack: nvar = new size
 */
static void extend_stack(sol_stack_t *s, uint32_t nvar) {
  s->lit = (literal_t *) safe_realloc(s->lit, nvar * sizeof(literal_t));
}

/*
 * Extend the level_index array by 50%
 *
 * (since nlevels <= number of variables <= UINT32/4, we know
 *  that nlevels + (nlevels>>1) can't overflow).
 */
static void increase_stack_levels(sol_stack_t *s) {
  uint32_t n;

  n = s->nlevels;
  n += n>>1;
  s->level_index = (uint32_t *) safe_realloc(s->level_index, n * sizeof(uint32_t));
  s->nlevels = n;
}

/*
 * Free memory used by stack s
 */
static void delete_stack(sol_stack_t *s) {
  safe_free(s->lit);
  safe_free(s->level_index);
  s->lit = NULL;
  s->level_index = NULL;
}

/*
 * Empty the stack
 */
static void reset_stack(sol_stack_t *s) {
  s->top = 0;
  s->prop_ptr = 0;
  assert(s->level_index[0] == 0);
}

/*
 * Push literal l on top of stack s
 */
static void push_literal(sol_stack_t *s, literal_t l) {
  uint32_t i;

  i = s->top;
  s->lit[i] = l;
  s->top = i + 1;
}



/*******************
 *  CLAUSE STACK   *
 ******************/

/*
 * Initialize the stack
 */
static void init_clause_stack(clause_stack_t *s) {
  s->data = (uint32_t *) safe_malloc(DEF_CLAUSE_STACK_CAPACITY * sizeof(uint32_t));
  s->top = 0;
  s->capacity = DEF_CLAUSE_STACK_CAPACITY;
  s->level = (uint32_t *) safe_malloc(DEFAULT_NLEVELS * sizeof(uint32_t));
  s->level[0] = 0;
  s->nlevels = DEFAULT_NLEVELS;
}


/*
 * Extend the level array by 50%
 */
static void increase_clause_stack_levels(clause_stack_t *s) {
  uint32_t n;

  n = s->nlevels;
  n += n>>1;
  s->level = (uint32_t *) safe_realloc(s->level, n * sizeof(uint32_t));
  s->nlevels = n;
}

/*
 * Free memory
 */
static void delete_clause_stack(clause_stack_t *s) {
  safe_free(s->data);
  safe_free(s->level);
  s->data = NULL;
  s->level = NULL;
}

/*
 * Empty the stack
 */
static void reset_clause_stack(clause_stack_t *s) {
  s->top = 0;
  assert(s->level[0] == 0);
}

#if 0

/*
 * Capacity increase:
 * - about 50% larger than the current cap
 * - rounded up to the next multiple of four
 */
static inline uint32_t clause_stack_cap_increase(uint32_t cap) {
  return ((cap >> 1) + 8) & ~3;
}

/*
 * Increase the stack size until we have enough room for n elements
 */
static void resize_clause_stack(clause_stack_t *s, uint32_t n) {
  uint32_t min_cap, cap, increase;

  min_cap = s->top + n;
  if (min_cap < n || min_cap >= MAX_CLAUSE_STACK_CAPACITY) {
    // can't make the stack that large
    out_of_memory();
  }

  cap = s->capacity;
  do {
    increase = clause_stack_cap_increase(cap);
    cap += increase;
    if (cap < increase) {
      // arithmetic overflow
      cap = MAX_CLAUSE_STACK_CAPACITY;
    }
  } while (cap < min_cap);

  s->data = (uint32_t *) safe_realloc(s->data, cap * sizeof(uint32_t));
  s->capacity = cap;
}

/*
 * Make room to push n integers on top of the stack
 */
static cidx_t clause_stack_alloc(clause_stack_t *s, uint32_t n) {
  cidx_t i;

  i = s->top;
  n = (n + 3) & ~3; // round up to a multiple of four
  if (i + n >= s->capacity) {
    resize_clause_stack(s, n);
  }
  s->top = i+n;

  return i;
}


/*
 * Add a clause to the stack and return the clause idx.
 * - n = size of the clause
 * - a = literal array
 */
static cidx_t push_clause(clause_stack_t *s, uint32_t n, const literal_t *a) {
  uint32_t i, cidx;
  uint32_t *p;

  cidx = clause_stack_alloc(s, n+2);
  s->data[cidx] = n;
  s->data[cidx + 1] = 0;
  p = s->data + cidx + 2;
  for (i=0; i<n; i++) {
    p[i] = a[i];
  }
  return cidx;
}

#endif

/*
 * READ STACKED CLAUSES
 */
#ifndef NDEBUG
static inline bool good_stacked_clause_idx(const clause_stack_t *s, cidx_t idx) {
  return ((idx & 3) == 0) && idx < s->top;
}
#endif

static inline uint32_t stacked_clause_length(const clause_stack_t *s, cidx_t idx) {
  assert(good_stacked_clause_idx(s, idx));
  return s->data[idx];
}

static inline literal_t *stacked_clause_literals(const clause_stack_t *s, cidx_t idx) {
  assert(good_stacked_clause_idx(s, idx));
  return (literal_t *) s->data + idx + 2;
}


#if DEBUG
static inline cidx_t next_stacked_clause(const clause_stack_t *s, cidx_t idx) {
  return idx + full_length(stacked_clause_length(s, idx)); // length + 2 rounded up to a multiple of four
}
#endif

#if DEBUG || !defined(NDEBUG)
static inline literal_t first_literal_of_stacked_clause(const clause_stack_t *s, cidx_t idx) {
  assert(good_stacked_clause_idx(s, idx));
  return s->data[idx + 2];
}
#endif




/*******************
 *  ACTIVITY HEAP  *
 ******************/

/*
 * Initialize heap for size n and nv variables
 * - heap is initially empty: heap_last = 0
 * - heap[0] = 0 is a marker, with activity[0] higher
 *   than any variable activity.
 * - activity increment and threshold are set to their
 *   default initial value.
 */
static void init_heap(nvar_heap_t *heap, uint32_t n, uint32_t nv) {
  uint32_t i;

  heap->activity = (double *) safe_malloc(n * sizeof(double));
  heap->heap_index = (int32_t *) safe_malloc(n * sizeof(int32_t));
  heap->heap = (bvar_t *) safe_malloc(n * sizeof(bvar_t));

  // marker
  heap->activity[0] = DBL_MAX;
  heap->heap_index[0] = 0;
  heap->heap[0] = 0;

  for (i=1; i<nv; i++) {
    heap->heap_index[i] = -1;
    heap->activity[i] = 0.0;
  }

  heap->heap_last = 0;
  heap->size = n;
  heap->nvars = nv;
  heap->vmax = 1;

  heap->act_increment = INIT_VAR_ACTIVITY_INCREMENT;
  heap->inv_act_decay = 1/VAR_DECAY_FACTOR;

  check_heap(heap);
}

/*
 * Extend the heap: n = new size.
 * - keep nvar unchanged
 */
static void extend_heap(nvar_heap_t *heap, uint32_t n) {
  assert(heap->size < n);

  heap->activity = (double *) safe_realloc(heap->activity, n * sizeof(double));
  heap->heap_index = (int32_t *) safe_realloc(heap->heap_index, n * sizeof(int32_t));
  heap->heap = (bvar_t *) safe_realloc(heap->heap, n * sizeof(int32_t));
  heap->size = n;

  check_heap(heap);
}


/*
 * Increase the number of variables to n
 */
static void heap_add_vars(nvar_heap_t *heap, uint32_t n) {
  uint32_t old_nvars, i;

  old_nvars = heap->nvars;
  assert(n <= heap->size);
  for (i=old_nvars; i<n; i++) {
    heap->heap_index[i] = -1;
    heap->activity[i] = 0.0;
  }
  heap->nvars = n;

  check_heap(heap);
}



/*
 * Free the heap
 */
static void delete_heap(nvar_heap_t *heap) {
  safe_free(heap->activity);
  safe_free(heap->heap_index);
  safe_free(heap->heap);
  heap->activity = NULL;
  heap->heap_index = NULL;
  heap->heap = NULL;
}

/*
 * Reset: empty the heap
 */
static void reset_heap(nvar_heap_t *heap) {
  uint32_t i, n;

  heap->heap_last = 0;
  heap->vmax = 1;

  n = heap->nvars;
  for (i=1; i<n; i++) {
    heap->heap_index[i] = -1;
    heap->activity[i] = 0.0;
  }
  check_heap(heap);
}

/*
 * Move x up in the heap.
 * i = current position of x in the heap (or heap_last if x is being inserted)
 */
static void update_up(nvar_heap_t *heap, bvar_t x, uint32_t i) {
  double ax, *act;
  int32_t *index;
  bvar_t *h, y;
  uint32_t j;

  h = heap->heap;
  index = heap->heap_index;
  act = heap->activity;

  ax = act[x];

  for (;;) {
    j = i >> 1;    // parent of i
    y = h[j];      // variable at position j in the heap

    // The loop terminates since act[h[0]] = DBL_MAX
    if (act[y] >= ax) break;

    // move y down, into position i
    h[i] = y;
    index[y] = i;

    // move i up
    i = j;
  }

  // i is the new position for variable x
  h[i] = x;
  index[x] = i;

  check_heap(heap);
}

/*
 * Remove root of the heap (i.e., heap->heap[1]):
 * - move the variable currently in heap->heap[last]
 *   into a new position.
 * - decrement last.
 */
static void update_down(nvar_heap_t *heap) {
  double *act;
  int32_t *index;
  bvar_t *h;
  bvar_t x, y, z;
  double ax, ay, az;
  uint32_t i, j, last;

  last = heap->heap_last;
  heap->heap_last = last - 1;
  if (last <= 1) { // empty heap.
    assert(heap->heap_last == 0);
    return;
  }

  h = heap->heap;
  index = heap->heap_index;
  act = heap->activity;

  z = h[last];   // last element
  az = act[z];   // activity of the last element

  i = 1;      // root
  j = 2;      // left child of i
  while (j < last) {
    /*
     * find child of i with highest activity.
     */
    x = h[j];
    ax = act[x];
    if (j+1 < last) {
      y = h[j+1];
      ay = act[y];
      if (ay > ax) {
        j++;
        x = y;
        ax = ay;
      }
    }

    // x = child of node i of highest activity
    // j = position of x in the heap (j = 2i or j = 2i+1)
    if (az >= ax) break;

    // move x up, into heap[i]
    h[i] = x;
    index[x] = i;

    // go down one step.
    i = j;
    j <<= 1;
  }

  h[i] = z;
  index[z] = i;

  check_heap(heap);
}

/*
 * Insert x into the heap, using its current activity.
 * No effect if x is already in the heap.
 * - x must be between 0 and nvars - 1
 */
static void heap_insert(nvar_heap_t *heap, bvar_t x) {
  if (heap->heap_index[x] < 0) {
    // x not in the heap
    heap->heap_last ++;
    update_up(heap, x, heap->heap_last);
  }
}

/*
 * Check whether the heap is empty
 */
static inline bool heap_is_empty(nvar_heap_t *heap) {
  return heap->heap_last == 0;
}

/*
 * Get and remove the top element
 * - the heap must not be empty
 */
static bvar_t heap_get_top(nvar_heap_t *heap) {
  bvar_t top;

  assert(heap->heap_last > 0);

  // remove top element
  top = heap->heap[1];
  heap->heap_index[top] = -1;

  // repair the heap
  update_down(heap);

  return top;
}

/*
 * Rescale variable activities: divide by VAR_ACTIVITY_THRESHOLD
 */
static void rescale_var_activities(nvar_heap_t *heap) {
  uint32_t i, n;
  double *act;

  n = heap->nvars;
  act = heap->activity;
  for (i=1; i<n; i++) {
    act[i] *= INV_VAR_ACTIVITY_THRESHOLD;
  }
  heap->act_increment *= INV_VAR_ACTIVITY_THRESHOLD;
}

/*
 * Increase the activity of variable x
 */
static void increase_var_activity(nvar_heap_t *heap, bvar_t x) {
  int32_t i;

  if ((heap->activity[x] += heap->act_increment) > VAR_ACTIVITY_THRESHOLD) {
    rescale_var_activities(heap);
  }

  // move x up if it's in the heap
  i = heap->heap_index[x];
  if (i >= 0) {
    update_up(heap, x, i);
  }
}

/*
 * Decay
 */
static inline void decay_var_activities(nvar_heap_t *heap) {
  heap->act_increment *= heap->inv_act_decay;
}

/*
 * Cleanup the heap: remove variables until the top var is unassigned
 * or until the heap is empty
 */
static void cleanup_heap(sat_solver_t *solver) {
  nvar_heap_t *heap;
  bvar_t x;

  heap = &solver->heap;
  while (! heap_is_empty(heap)) {
    x = heap->heap[1];
    if (var_is_unassigned(solver, x) && solver->ante_tag[x] < ATAG_PURE) {
      break;
    }
    assert(x >= 0 && heap->heap_last > 0);
    heap->heap_index[x] = -1;
    update_down(heap);
  }
}


/*
 * Activity of variable x or a literal l
 */
static inline double var_activity(const sat_solver_t *solver, bvar_t x) {
  assert(x < solver->nvars);
  return solver->heap.activity[x];
}

static inline double lit_activity(const sat_solver_t *solver, literal_t l) {
  return var_activity(solver, var_of(l));
}


/*
 * Set activity and branching polarity for variable x
 * - polarity: true means true is preferred
 * - act = activity score
 */
void nsat_solver_activate_var(sat_solver_t *solver, bvar_t x, double act, bool polarity) {
  nvar_heap_t *heap;

  assert(0 <= x && x < solver->nvars);
  assert(0.0 <= act);

  heap = &solver->heap;
  if (heap->heap_index[x] < 0) {
    heap->activity[x] = act;
    heap_insert(heap, x);
  }
  if (polarity) {
    solver->value[pos_lit(x)] = VAL_UNDEF_TRUE;
    solver->value[neg_lit(x)] = VAL_UNDEF_FALSE;
  } else {
    solver->value[pos_lit(x)] = VAL_UNDEF_FALSE;
    solver->value[neg_lit(x)] = VAL_UNDEF_TRUE;
  }

  fprintf(stderr, "activate %"PRId32", polarity = %d\n", x, polarity);
}

/*
 * MARKS ON VARIABLES
 */

/*
 * Set/clear/test the mark on variable x
 * - we use the high order bit of the ante_tag
 * - if this bit is 1, x is marked
 */
static inline void mark_variable(sat_solver_t *solver, bvar_t x) {
  assert(x < solver->nvars);
  solver->ante_tag[x] |= (uint8_t) 0x80;
}

static inline void unmark_variable(sat_solver_t *solver, bvar_t x) {
  assert(x < solver->nvars);
  solver->ante_tag[x] &= (uint8_t) 0x7F;
}

static inline bool variable_is_marked(const sat_solver_t *solver, bvar_t x) {
  assert(x < solver->nvars);
  return (solver->ante_tag[x] & (uint8_t) 0x80) != 0;
}

static inline bool literal_is_marked(const sat_solver_t *solver, literal_t l) {
  return variable_is_marked(solver, var_of(l));
}



/********************************
 *  SAT SOLVER INITIALIZATION   *
 *******************************/

/*
 * Initialize a statistics record
 */
static void init_stats(solver_stats_t *stat) {
  stat->decisions = 0;
  stat->random_decisions = 0;
  stat->propagations = 0;
  stat->conflicts = 0;
  stat->prob_clauses_deleted = 0;
  stat->learned_clauses_deleted = 0;
  stat->subsumed_literals = 0;
  stat->starts = 0;
  stat->dives = 0;
  stat->simplify_calls = 0;
  stat->reduce_calls = 0;
  stat->subst_calls = 0;
  stat->successful_dive = 0;
  stat->scc_calls = 0;
  stat->subst_vars = 0;
  stat->pp_pure_lits = 0;
  stat->pp_unit_lits = 0;
  stat->pp_subst_vars = 0;
  stat->pp_clauses_deleted = 0;
  stat->pp_subsumptions = 0;
  stat->pp_strengthenings = 0;
  stat->pp_unit_strengthenings = 0;
  stat->pp_cheap_elims = 0;
  stat->pp_var_elims = 0;
}

/*
 * Search parameters
 */
static void init_params(solver_param_t *params) {
  params->seed = PRNG_SEED;
  params->randomness = (uint32_t) (VAR_RANDOM_FACTOR * VAR_RANDOM_SCALE);
  params->inv_cla_decay = ((float) 1)/CLAUSE_DECAY_FACTOR;
  params->stack_threshold = STACK_THRESHOLD;
  params->keep_lbd = KEEP_LBD;
  params->reduce_fraction = REDUCE_FRACTION;
  params->reduce_interval = REDUCE_INTERVAL;
  params->reduce_delta = REDUCE_DELTA;
  params->restart_interval = RESTART_INTERVAL;
  params->search_period = SEARCH_PERIOD;
  params->search_counter = SEARCH_COUNTER;
  params->diving_budget = DIVING_BUDGET;

  params->var_elim_skip = VAR_ELIM_SKIP;
  params->subsume_skip = SUBSUME_SKIP;
  params->res_clause_limit = RES_CLAUSE_LIMIT;

  params->simplify_interval = SIMPLIFY_INTERVAL;
  params->simplify_bin_delta = SIMPLIFY_BIN_DELTA;
}

/*
 * Initialization:
 * - sz = initial size of the variable-indexed arrays.
 * - pp = flag to enable preprocessing
 *
 * - if sz is zero, the default size is used.
 * - the solver is initialized with one variable (the reserved variable 0).
 */
void init_nsat_solver(sat_solver_t *solver, uint32_t sz, bool pp) {
  uint32_t n;

  if (sz > MAX_VARIABLES) {
    out_of_memory();
  }

  n = sz;
  if (sz == 0) {
    n = SAT_SOLVER_DEFAULT_VSIZE;
  }
  assert(n >= 1 && n <= MAX_VARIABLES);

  solver->status = STAT_UNKNOWN;
  solver->decision_level = 0;
  solver->backtrack_level = 0;
  solver->preprocess = pp;

  solver->verbosity = 0;
  solver->reports = 0;

  solver->nvars = 1;
  solver->nliterals = 2;
  solver->vsize = n;
  solver->lsize = 2 * n;

  solver->value = (uint8_t *) safe_malloc(n * 2 * sizeof(uint8_t));
  solver->ante_tag = (uint8_t *) safe_malloc(n * sizeof(uint8_t));
  solver->ante_data = (uint32_t *) safe_malloc(n * sizeof(uint32_t));
  solver->level = (uint32_t *) safe_malloc(n * sizeof(uint32_t));
  solver->watch = (watch_t **) safe_malloc(n * 2 * sizeof(watch_t *));

  solver->occ = NULL;
  if (solver->preprocess) {
    solver->occ = (uint32_t *) safe_malloc(n * 2 * sizeof(uint32_t)); // one counter per literal
    solver->occ[0] = 0;  // for literal 0 = true
    solver->occ[1] = 0;  // for literal 1 = false
  }

  // variable 0: true
  solver->value[0] = VAL_TRUE;
  solver->value[1] = VAL_FALSE;
  solver->ante_tag[0] = ATAG_UNIT;
  solver->ante_data[0] = 0;
  solver->level[0] = 0;
  solver->watch[0] = NULL;
  solver->watch[1] = NULL;

  init_heap(&solver->heap, n, 1);
  init_stack(&solver->stack, n);

  solver->has_empty_clause = false;
  solver->units = 0;
  solver->binaries = 0;
  init_clause_pool(&solver->pool);

  init_clause_stack(&solver->stash);

  solver->conflict_tag = CTAG_NONE;

  init_params(&solver->params);

  init_stats(&solver->stats);

  solver->cidx_array = NULL;

  init_vector(&solver->buffer);
  init_vector(&solver->aux);
  init_gstack(&solver->gstack);
  init_tag_map(&solver->map, 0); // use default size

  init_clause_vector(&solver->saved_clauses);

  init_queue(&solver->lqueue);
  init_elim_heap(&solver->elim);
  init_queue(&solver->cqueue);
  init_vector(&solver->cvector);
  solver->scan_index = 0;

  init_vector(&solver->vertex_stack);
  init_gstack(&solver->dfs_stack);
  solver->label = NULL;
  solver->visit = NULL;

  solver->data = NULL;
}


/*
 * Set the verbosity level
 */
void nsat_set_verbosity(sat_solver_t *solver, uint32_t level) {
  solver->verbosity = level;
}

/*
 * Free memory
 */
void delete_nsat_solver(sat_solver_t *solver) {
  safe_free(solver->value);
  safe_free(solver->ante_tag);
  safe_free(solver->ante_data);
  safe_free(solver->level);
  delete_watch_vectors(solver->watch, solver->nliterals);
  safe_free(solver->watch);

  if (solver->preprocess) {
    safe_free(solver->occ);
  }

  solver->value = NULL;
  solver->ante_tag = NULL;
  solver->ante_data = NULL;
  solver->level = NULL;
  solver->watch = NULL;

  delete_heap(&solver->heap);
  delete_stack(&solver->stack);
  delete_clause_stack(&solver->stash);
  delete_clause_pool(&solver->pool);

  safe_free(solver->cidx_array);
  solver->cidx_array = NULL;

  delete_vector(&solver->buffer);
  delete_vector(&solver->aux);
  delete_gstack(&solver->gstack);
  delete_tag_map(&solver->map);

  delete_clause_vector(&solver->saved_clauses);

  delete_queue(&solver->lqueue);
  delete_elim_heap(&solver->elim);
  delete_queue(&solver->cqueue);
  delete_vector(&solver->cvector);

  delete_vector(&solver->vertex_stack);
  delete_gstack(&solver->dfs_stack);
  if (solver->label != NULL) {
    safe_free(solver->label);
    safe_free(solver->visit);
    solver->label = NULL;
    solver->visit = NULL;
  }

  close_datafile(solver);
}


/*
 * Reset: remove all variables and clauses
 * - reset heuristics parameters
 */
void reset_nsat_solver(sat_solver_t *solver) {
  solver->status = STAT_UNKNOWN;
  solver->decision_level = 0;
  solver->backtrack_level = 0;
  solver->nvars = 1;
  solver->nliterals = 2;

  reset_heap(&solver->heap);
  reset_stack(&solver->stack);

  solver->has_empty_clause = false;
  solver->units = 0;
  solver->binaries = 0;
  reset_clause_pool(&solver->pool);

  reset_clause_stack(&solver->stash);

  solver->conflict_tag = CTAG_NONE;

  init_stats(&solver->stats);

  safe_free(solver->cidx_array);
  solver->cidx_array = NULL;

  reset_vector(&solver->buffer);
  reset_vector(&solver->aux);
  reset_gstack(&solver->gstack);
  clear_tag_map(&solver->map);

  reset_clause_vector(&solver->saved_clauses);

  reset_queue(&solver->lqueue);
  reset_elim_heap(&solver->elim);
  reset_queue(&solver->cqueue);
  reset_vector(&solver->cvector);

  reset_vector(&solver->vertex_stack);
  reset_gstack(&solver->dfs_stack);
  if (solver->label != NULL) {
    safe_free(solver->label);
    safe_free(solver->visit);
    solver->label = NULL;
    solver->visit = NULL;
  }

  reset_datafile(solver);
}



/**************************
 *  HEURISTIC PARAMETERS  *
 *************************/

/*
 * Variable activity decay: must be between 0 and 1.0
 * - smaller number means faster decay
 */
void nsat_set_var_decay_factor(sat_solver_t *solver, double factor) {
  assert(0.0 < factor && factor < 1.0);
  solver->heap.inv_act_decay = 1/factor;
}

/*
 * Clause activity decay: must be between 0 and 1.0
 * - smaller means faster decay
 */
void nsat_set_clause_decay_factor(sat_solver_t *solver, float factor) {
  assert(0.0F < factor && factor < 1.0F);
  solver->params.inv_cla_decay = 1/factor;
}

/*
 * Randomness: the parameter is approximately the ratio of random
 * decisions.
 * - randomness = 0: no random decisions
 * - randomness = 1.0: all decisions are random
 */
void nsat_set_randomness(sat_solver_t *solver, float randomness) {
  assert(0.0F <= randomness && randomness <= 1.0F);
  solver->params.randomness = (uint32_t)(randomness * VAR_RANDOM_SCALE);
}

/*
 * Set the prng seed
 */
void nsat_set_random_seed(sat_solver_t *solver, uint32_t seed) {
  solver->params.seed = seed;
}

/*
 * LBD threshold for clause deletion. Clauses of lbd <= keep_lbd are not deleted.
 */
void nsat_set_keep_lbd(sat_solver_t *solver, uint32_t threshold) {
  solver->params.keep_lbd = threshold;
}

/*
 * Reduce fraction for clause deletion. f must be between 0 and 32.
 * Each call to reduce_learned_clause_set removes a fraction (f/32) of the clauses
 */
void nsat_set_reduce_fraction(sat_solver_t *solver, uint32_t f) {
  assert(f <= 32);
  solver->params.reduce_fraction = f;
}

/*
 * Interval between two calls to reduce (number of conflicts)
 */
void nsat_set_reduce_interval(sat_solver_t *solver, uint32_t n) {
  solver->params.reduce_interval = n;
}

/*
 * Adjustment to the reduce interval (check init_reduce and done_reduce).
 */
void nsat_set_reduce_delta(sat_solver_t *solver, uint32_t d) {
  solver->params.reduce_delta = d;
}

/*
 * Minimal number of conflicts between two calls to restart
 */
void nsat_set_restart_interval(sat_solver_t *solver, uint32_t n) {
  solver->params.restart_interval = n;
}

/*
 * Periodic check for switching to dive
 */
void nsat_set_search_period(sat_solver_t *solver, uint32_t n) {
  solver->params.search_period = n;
}

/*
 * Counter used in determining when to switch
 */
void nsat_set_search_counter(sat_solver_t *solver, uint32_t n) {
  solver->params.search_counter = n;
}


/*
 * Stack clause threshold: learned clauses of LBD greater than threshold are
 * treated as temporary clauses (not stored in the clause database).
 */
void nsat_set_stack_threshold(sat_solver_t *solver, uint32_t f) {
  solver->params.stack_threshold = f;
}


/*
 * Dive bugdet
 */
void nsat_set_dive_budget(sat_solver_t *solver, uint32_t n) {
  solver->params.diving_budget = n;
}



/*
 * PREPROCESSING PARAMETERS
 */

/*
 * Subsumption limit: skip subsumption checks for a clause cls if that
 * would require visiting more than subsume_skip clauses.
 */
void nsat_set_subsume_skip(sat_solver_t *solver, uint32_t limit) {
  solver->params.subsume_skip = limit;
}

/*
 * Var-elimination limit: if x has too many positive and negative occurrences,
 * we don't try to eliminate x.
 */
void nsat_set_var_elim_skip(sat_solver_t *solver, uint32_t limit) {
  solver->params.var_elim_skip = limit;
}

/*
 * Resolvent limit: if eliminating x would create a clause larger than
 * res_clause_limit, we keep x.
 */
void nsat_set_res_clause_limit(sat_solver_t *solver, uint32_t limit) {
  solver->params.res_clause_limit = limit;
}


/*
 * SIMPLIFY PARAMETERS
 */
void nsat_set_simplify_interval(sat_solver_t *solver, uint32_t n) {
  solver->params.simplify_interval = n;
}

void nsat_set_simplify_bin_delta(sat_solver_t *solver, uint32_t d) {
  solver->params.simplify_bin_delta = d;
}



/********************
 *  ADD VARIABLES   *
 *******************/

/*
 * Extend data structures:
 * - new_size = new vsize for variable indexed arrays
 */
static void sat_solver_extend(sat_solver_t *solver, uint32_t new_size) {
  if (new_size > MAX_VARIABLES) {
    out_of_memory();
  }

  solver->vsize = new_size;
  solver->lsize = 2 * new_size;

  solver->value = (uint8_t *) safe_realloc(solver->value, new_size * 2 * sizeof(uint8_t));
  solver->ante_tag = (uint8_t *) safe_realloc(solver->ante_tag, new_size * sizeof(uint8_t));
  solver->ante_data = (uint32_t *) safe_realloc(solver->ante_data, new_size * sizeof(uint32_t));
  solver->level = (uint32_t *) safe_realloc(solver->level, new_size * sizeof(uint32_t));
  solver->watch = (watch_t **) safe_realloc(solver->watch, new_size * 2 * sizeof(watch_t *));

  if (solver->preprocess) {
    solver->occ = (uint32_t *) safe_realloc(solver->occ, new_size * 2 * sizeof(uint32_t));
  }

  extend_heap(&solver->heap, new_size);
  extend_stack(&solver->stack, new_size);
}


/*
 * Add n variables
 */
void nsat_solver_add_vars(sat_solver_t *solver, uint32_t n) {
  uint32_t i, nv, new_size;

  nv = solver->nvars + n;
  if (nv  < n) {
    // arithmetic overflow: too many variables
    out_of_memory();
  }

  if (nv > solver->vsize) {
    new_size = solver->vsize + 1;
    new_size += new_size >> 1;
    if (new_size < nv) {
      new_size = nv;
    }
    sat_solver_extend(solver, new_size);
    assert(nv <= solver->vsize);
  }

  for (i=solver->nvars; i<nv; i++) {
    solver->value[pos_lit(i)] = VAL_UNDEF_FALSE;
    solver->value[neg_lit(i)] = VAL_UNDEF_TRUE;
    solver->ante_tag[i] = ATAG_NONE;
    solver->ante_data[i] = 0;
    solver->level[i] = UINT32_MAX;
    solver->watch[pos_lit(i)] = NULL;
    solver->watch[neg_lit(i)] = NULL;
  }

  if (solver->preprocess) {
    for (i=solver->nvars; i<nv; i++) {
      solver->occ[pos_lit(i)] = 0;
      solver->occ[neg_lit(i)] = 0;
    }
  }

  heap_add_vars(&solver->heap, nv);

  solver->nvars = nv;
  solver->nliterals = 2 * nv;
}


/*
 * Allocate and return a fresh Boolean variable
 */
bvar_t nsat_solver_new_var(sat_solver_t *solver) {
  bvar_t x;

  x = solver->nvars;
  nsat_solver_add_vars(solver, 1);
  assert(solver->nvars == x + 1);
  return x;
}



/*******************
 *  WATCH VECTORS  *
 ******************/

/*
 * Encode l as a watch index
 */
static inline uint32_t lit2idx(literal_t l) {
  return (l << 1) | 1;
}

/*
 * Converse: extract literal from index k
 */
static inline literal_t idx2lit(uint32_t k) {
  assert((k & 1) == 1);
  return k >> 1;
}

/*
 * Check whether k is a clause index: low-order bit is 0
 */
static inline bool idx_is_clause(uint32_t k) {
  return (k & 1) == 0;
}

/*
 * Check whether k is a literal index: low-order bit is 1
 */
static inline bool idx_is_literal(uint32_t k) {
  return (k & 1) == 1;
}

/*
 * Add a clause index to the watch vector for literal l
 * - l1 = blocker
 */
static inline void add_clause_watch(sat_solver_t *solver, literal_t l, cidx_t cidx, literal_t l1) {
  assert(l < solver->nliterals && l1 < solver->nliterals);
  add_watch2(solver->watch + l, cidx, l1);
}

/*
 * Add literal l1 to the watch vector for l
 */
static inline void add_literal_watch(sat_solver_t *solver, literal_t l, literal_t l1) {
  assert(l < solver->nliterals);
  add_watch(solver->watch + l, lit2idx(l1));
}


/*
 * All clause index cidx in the watch vectors of literals lit[0 ... n-1]
 */
static void add_clause_all_watch(sat_solver_t *solver, uint32_t n, const literal_t *lit, cidx_t cidx) {
  uint32_t i;
  literal_t l;

  for (i=0; i<n; i++) {
    l = lit[i];
    assert(l < solver->nliterals);
    add_watch(solver->watch + l, cidx);
  }
}


/*************************
 *  LITERAL ASSIGNMENT   *
 ***********************/

/*
 * Assign literal l at base level
 */
static void assign_literal(sat_solver_t *solver, literal_t l) {
  bvar_t v;

#if TRACE
  printf("---> Assigning literal %"PRIu32"\n", l);
  fflush(stdout);
#endif

  assert(l < solver->nliterals);
  assert(lit_is_unassigned(solver, l));
  assert(solver->decision_level == 0);

  push_literal(&solver->stack, l);

  solver->value[l] = VAL_TRUE;
  solver->value[not(l)] = VAL_FALSE;

  v = var_of(not(l));
  // value of v = VAL_TRUE if l = pos_lit(v) or VAL_FALSE if l = neg_lit(v)
  //  solver->value[v] = VAL_TRUE ^ sign_of_lit(l);
  solver->ante_tag[v] = ATAG_UNIT;
  solver->ante_data[v] = 0;
  solver->level[v] = 0;

  assert(lit_is_true(solver, l));
}


/* static inline int32_t l2dimacs(literal_t l) { */
/*   int x = var_of(l) + 1; */
/*   return is_pos(l) ? x : - x; */
/* } */

/*
 * Decide literal: increase decision level then
 * assign literal l to true and push it on the stack
 */
static void nsat_decide_literal(sat_solver_t *solver, literal_t l) {
  uint32_t k;
  bvar_t v;

  assert(l < solver->nliterals);
  assert(lit_is_unassigned(solver, l));

  solver->stats.decisions ++;

  // Increase decision level
  k = solver->decision_level + 1;
  solver->decision_level = k;
  if (solver->stack.nlevels <= k) {
    increase_stack_levels(&solver->stack);
  }
  solver->stack.level_index[k] = solver->stack.top;
  if (solver->stash.nlevels <= k) {
    increase_clause_stack_levels(&solver->stash);
  }
  solver->stash.level[k] = solver->stash.top;

  push_literal(&solver->stack, l);

  solver->value[l] = VAL_TRUE;
  solver->value[not(l)] = VAL_FALSE;

  v = var_of(not(l));
  solver->ante_tag[v] = ATAG_DECISION;
  solver->ante_data[v] = 0; // not used
  solver->level[v] = k;

  assert(lit_is_true(solver, l));

  //  fprintf(stderr, "decide %"PRId32"\n", l2dimacs(l));
#if TRACE
  printf("---> DPLL:   Decision: literal %"PRIu32", decision level = %"PRIu32"\n", l, k);
  fflush(stdout);
#endif
}


/*
 * Propagated literal: tag = antecedent tag, data = antecedent data
 */
static void implied_literal(sat_solver_t *solver, literal_t l, antecedent_tag_t tag, uint32_t data) {
  bvar_t v;

  assert(l < solver->nliterals);
  assert(lit_is_unassigned(solver, l));

  solver->stats.propagations ++;

  push_literal(&solver->stack, l);

  solver->value[l] = VAL_TRUE;
  solver->value[not(l)] = VAL_FALSE;

  v = var_of(not(l));
  solver->ante_tag[v] = tag;
  solver->ante_data[v] = data;
  solver->level[v] = solver->decision_level;

  assert(lit_is_true(solver, l));
}


/*
 * Literal l implied by clause cidx
 */
static void clause_propagation(sat_solver_t *solver, literal_t l, cidx_t cidx) {
  assert(good_clause_idx(&solver->pool, cidx));

  implied_literal(solver, l, ATAG_CLAUSE, cidx);

#if TRACE
  printf("\n---> DPLL:   Implied literal %"PRIu32", by clause %"PRIu32", decision level = %"PRIu32"\n", l, cidx, solver->decision_level);
  fflush(stdout);
#endif
}


/*
 * Literal l implied by a binary clause (of the form { l, l0 }})
 * - l0 = other literal in the clause
 */
static void binary_clause_propagation(sat_solver_t *solver, literal_t l, literal_t l0) {
  assert(l0 < solver->nliterals);

  implied_literal(solver, l, ATAG_BINARY, l0);

#if TRACE
  printf("\n---> DPLL:   Implied literal %"PRIu32", by literal %"PRIu32", decision level = %"PRIu32"\n", l, l0, solver->decision_level);
  fflush(stdout);
#endif
}


#if 0
/*
 * Literal l implied by stacked clause cidx
 */
static void stacked_clause_propagation(sat_solver_t *solver, literal_t l, cidx_t cidx) {
  implied_literal(solver, l, ATAG_STACKED, cidx);

#if TRACE
  printf("\n---> DPLL:   Implied literal %"PRIu32", by stacked clause %"PRIu32", decision level = %"PRIu32"\n", l, cidx, solver->decision_level);
  fflush(stdout);
#endif
}

#endif


/***********************
 *  OCCURRENCE COUNTS  *
 **********************/

/*
 * Scan clause stored in lit[0 ... n-1] and increase occurrence counts
 * for these literals.
 */
static void increase_occurrence_counts(sat_solver_t *solver, uint32_t n, const literal_t *lit) {
  uint32_t i;

  for (i=0; i<n; i++) {
    solver->occ[lit[i]] ++;
  }
}



/**********************
 *  CLAUSE ADDITION   *
 *********************/

/*
 * Add the empty clause
 */
static void add_empty_clause(sat_solver_t *solver) {
  solver->has_empty_clause = true;
  solver->status = STAT_UNSAT;
}


/*
 * Add unit clause { l }: push l on the assignment stack
 */
static void add_unit_clause(sat_solver_t *solver, literal_t l) {
  assert(lit_is_unassigned(solver, l));
  assign_literal(solver, l);
  solver->units ++;
}


/*
 * Add clause { l0, l1 }
 */
static void add_binary_clause(sat_solver_t *solver, literal_t l0, literal_t l1) {
  solver->binaries ++;
  add_literal_watch(solver, l0, l1);
  add_literal_watch(solver, l1, l0);
}


/*
 * Add an n-literal clause
 * - n must be at least 2
 * - if solver->preprocess is true, add the new clause to all occurrence lists
 * - otherwise, pick lit[0] and lit[1] as watch literals
 */
static void add_large_clause(sat_solver_t *solver, uint32_t n, const literal_t *lit) {
  cidx_t cidx;

  assert(n >= 2);

#ifndef NDEBUG
  // check that all literals are valid
  for (uint32_t i=0; i<n; i++) {
    assert(lit[i] < solver->nliterals);
  }
#endif

  cidx = clause_pool_add_problem_clause(&solver->pool, n, lit);
  if (solver->preprocess) {
    add_clause_all_watch(solver, n, lit, cidx);
    set_clause_signature(&solver->pool, cidx);
  } else {
    add_clause_watch(solver, lit[0], cidx, lit[1]);
    add_clause_watch(solver, lit[1], cidx, lit[0]);
  }
}


/*
 * Simplify the clause then add it
 * - n = number of literals
 * - l = array of n literals
 * - the array is modified
 */
void nsat_solver_simplify_and_add_clause(sat_solver_t *solver, uint32_t n, literal_t *lit) {
  uint32_t i, j;
  literal_t l, l_aux;

  if (n == 0) {
    add_empty_clause(solver);
    return;
  }

  /*
   * Remove duplicates and check for opposite literals l, not(l)
   * (sorting ensure that not(l) is just after l)
   */
  int_array_sort(lit, n);
  l = lit[0];
  j = 1;
  for (i=1; i<n; i++) {
    l_aux = lit[i];
    if (l_aux != l) {
      if (l_aux == not(l)) return; // true clause
      lit[j] = l_aux;
      l = l_aux;
      j ++;
    }
  }
  n = j; // new clause size

  /*
   * Remove false literals/check for a true literal
   */
  j = 0;
  for (i=0; i<n; i++) {
    l = lit[i];
    switch (lit_value(solver, l)) {
    case VAL_FALSE:
      break;
    case VAL_UNDEF_FALSE :
    case VAL_UNDEF_TRUE :
      lit[j] = l;
      j++;
      break;
    default: // true literal, so the clause is true
      return;
    }
  }
  n = j; // new clause size


  /*
   * Add the clause lit[0 ... n-1]
   */
  if (n == 0) {
    add_empty_clause(solver);
  } else if (n == 1) {
    add_unit_clause(solver, lit[0]);
  } else if (n == 2 && !solver->preprocess) {
    add_binary_clause(solver, lit[0], lit[1]);
  } else {
    add_large_clause(solver, n, lit);
  }

  if (solver->preprocess) {
    increase_occurrence_counts(solver, n, lit);
  }
}



/****************************
 *  VARIABLE SUBSTITUTION   *
 ***************************/

/*
 * Check whether variable x is eliminated (i.e., tag = PURE or ELIM or SUBST)
 */
static inline bool var_is_eliminated(const sat_solver_t *solver, bvar_t x) {
  assert(x < solver->nvars);
  return solver->ante_tag[x] >= ATAG_PURE;
}


/*
 * Check whether variable x is active (i.e., not assigned at level 0) and not eliminated
 */
static bool var_is_active(const sat_solver_t *solver, bvar_t x) {
  return var_is_unassigned(solver, x) & ! var_is_eliminated(solver, x);
}

/*
 * Same thing for literal l
 */
static inline bool lit_is_eliminated(const sat_solver_t *solver, literal_t l) {
  return var_is_eliminated(solver, var_of(l));
}

static inline bool lit_is_active(const sat_solver_t *solver, literal_t l) {
  return var_is_active(solver, var_of(l));
}


/*
 * Literal that replaces l.
 * - var_of(l) must be marked as substituted variable.
 * - if l is pos_lit(x) then subst(l) is ante_data[x]
 * - if l is neg_lit(x) then subst(l) is not(ante_data[x])
 * In both cases, subst(l) is ante_data[x] ^ sign_of_lit(l)
 */
#ifndef NDEBUG
static inline literal_t base_subst(const sat_solver_t *solver, literal_t l) {
  assert(l < solver->nliterals && solver->ante_tag[var_of(l)] == ATAG_SUBST);
  return solver->ante_data[var_of(l)] ^ sign_of_lit(l);
}
#endif

/*
 * Substitution for l:
 * - if l is not replaced by anything, return l
 * - otherwise return subst[l]
 */
static literal_t lit_subst(const sat_solver_t *solver, literal_t l) {
  assert(l < solver->nliterals);

  if (solver->ante_tag[var_of(l)] == ATAG_SUBST) {
    l = solver->ante_data[var_of(l)] ^ sign_of_lit(l);
    assert(solver->ante_tag[var_of(l)] != ATAG_SUBST);
  }
  return l;
}

/*
 * Full substitution: follow the substitution chain
 * - if l is not replaced by anything, return l
 * - otherwise, replace l by subst(l) and iterate
 */
static literal_t full_lit_subst(const sat_solver_t *solver, literal_t l) {
  assert(l < solver->nliterals);

  while (solver->ante_tag[var_of(l)] == ATAG_SUBST) {
    l = solver->ante_data[var_of(l)] ^ sign_of_lit(l);
  }
  return l;
}

static literal_t full_var_subst(const sat_solver_t *solver, bvar_t x) {
  assert(x < solver->nvars);
  return full_lit_subst(solver, pos_lit(x));
}

/*
 * Store subst[l1] := l2
 */
static void set_lit_subst(sat_solver_t *solver, literal_t l1, literal_t l2) {
  bvar_t x;

  x = var_of(l1);
  assert(! var_is_eliminated(solver, x));

  solver->stats.subst_vars ++;
  solver->ante_tag[x] = ATAG_SUBST;
  solver->ante_data[x] = l2 ^ sign_of_lit(l1);
}


/**********************************
 *  ADDITION OF LEARNED CLAUSES   *
 *********************************/

/*
 * Rescale the activity of all the learned clauses.
 * (divide all the activities by CLAUSE_ACTIVITY_THRESHOLD).
 */
static void rescale_clause_activities(sat_solver_t *solver) {
  cidx_t cidx, end;

  end = solver->pool.size;
  cidx = clause_pool_first_learned_clause(&solver->pool);
  while (cidx < end) {
    multiply_learned_clause_activity(&solver->pool, cidx, INV_CLAUSE_ACTIVITY_THRESHOLD);
    cidx = clause_pool_next_clause(&solver->pool, cidx);
  }
  solver->cla_inc *= INV_CLAUSE_ACTIVITY_THRESHOLD;
}


/*
 * Increase the activity of a learned clause.
 * - cidx = its index
 */
static void increase_clause_activity(sat_solver_t *solver, cidx_t cidx) {
  increase_learned_clause_activity(&solver->pool, cidx, solver->cla_inc);
  if (get_learned_clause_activity(&solver->pool, cidx) > CLAUSE_ACTIVITY_THRESHOLD) {
    rescale_clause_activities(solver);
  }
}

/*
 * Decay
 */
static inline void decay_clause_activities(sat_solver_t *solver) {
  solver->cla_inc *= solver->params.inv_cla_decay;
}

/*
 * Add an array of literals as a new learned clause
 *
 * Preconditions:
 * - n must be at least 2.
 * - lit[0] must be the literal of highest decision level in the clause.
 * - lit[1] must be a literal with second highest decision level
 */
static cidx_t add_learned_clause(sat_solver_t *solver, uint32_t n, const literal_t *lit) {
  cidx_t cidx;

  assert(n > 2);

  cidx = clause_pool_add_learned_clause(&solver->pool, n, lit);
  set_learned_clause_activity(&solver->pool, cidx, solver->cla_inc);
  add_clause_watch(solver, lit[0], cidx, lit[1]);
  add_clause_watch(solver, lit[1], cidx, lit[0]);

  return cidx;
}



/****************
 *  CLAUSE LBD  *
 ***************/

/*
 * The Literal-Block Distance is a heuristic estimate of the usefulness
 * of a learned clause. Clauses with low LBD are better.
 * The LBD is the number of distinct decision levels among the literals
 * in a clause.
 *
 * Since backtracking does not clear solver->level[x], we compute the
 * LBD of a learned clause even if some of its literals are not
 * currently assigned.  If a literal l in the clause is not currently
 * assigned, then solver->level[var_of(l)] is the decision level of l,
 * at the last time l was assigned.
 */

/*
 * Decision level of literal l
 */
static inline uint32_t d_level(const sat_solver_t *solver, literal_t l) {
  return solver->level[var_of(l)];
}

/*
 * The following function computes the LBD of a clause:
 * - n = number of literals
 * - lit = array of n literals
 */
static uint32_t clause_lbd(sat_solver_t *solver, uint32_t n, const literal_t *lit) {
  tag_map_t *map;
  uint32_t i, r;

  map = &solver->map;
  for (i=0; i<n; i++) {
    tag_map_write(map, d_level(solver, lit[i]), 1);
  }
  r = tag_map_size(map);
  clear_tag_map(map);

  return r;
}


/*
 * Check whether the LBD of a clause is no more than k
 */
static bool clause_lbd_le(sat_solver_t *solver, uint32_t n, const literal_t *lit, uint32_t k) {
  tag_map_t *map;
  uint32_t i;
  bool result;

  result = true;
  map = &solver->map;
  for (i=0; i<n; i++) {
    tag_map_write(map, d_level(solver, lit[i]), 1);
    if (tag_map_size(map) > k) {
      result = false;
      break;
    }
  }
  clear_tag_map(map);

  return result;
}


/************************
 *  GARBAGE COLLECTION  *
 ***********************/

/*
 * Garbage collection compacts the clause pool by removing padding
 * blocks. There are two variants: either compact the whole pool or
 * just the learned clauses. We use a base_idx as starting point for
 * deletion. The base_idx is either 0 (all the clauses) or
 * pool->learned (only the learned clauses).
 */

/*
 * Remove all clause indices >= base_idx from w
 */
static void watch_vector_remove_clauses(watch_t *w, cidx_t base_idx) {
  uint32_t i, j, k, n;

  assert(w != NULL);
  n = w->size;
  j = 0;
  i = 0;
  while (i<n) {
    k = w->data[i];
    if (idx_is_literal(k)) {
      w->data[j] = k;
      j ++;
      i ++;
    } else {
      if (k < base_idx) {
        w->data[j] = k;
        w->data[j+1] = w->data[i+1];
        j += 2;
      }
      i += 2;
    }
  }
  w->size = j;
}

/*
 * Prepare for clause deletion and compaction:
 * - go through all the watch vectors are remove all clause indices >= base_idx
 */
static void prepare_watch_vectors(sat_solver_t *solver, cidx_t base_idx) {
  uint32_t i, n;
  watch_t *w;

  n = solver->nliterals;
  for (i=0; i<n; i++) {
    w = solver->watch[i];
    if (w != NULL) {
      watch_vector_remove_clauses(w, base_idx);
    }
  }
}

/*
 * Mark all the antecedent clauses of idx >= base_idx
 */
static void mark_antecedent_clauses(sat_solver_t *solver, cidx_t base_idx) {
  uint32_t i, n;
  bvar_t x;
  cidx_t cidx;

  n = solver->stack.top;
  for (i=0; i<n; i++) {
    x = var_of(solver->stack.lit[i]);
    assert(var_is_assigned(solver, x));
    if (solver->ante_tag[x] == ATAG_CLAUSE) {
      cidx = solver->ante_data[x];
      if (cidx >= base_idx) {
        mark_clause(&solver->pool, cidx);
      }
    }
  }
}

/*
 * Restore antecedent when clause cidx is moved to new_idx
 * - this is called before the move.
 */
static void restore_clause_antecedent(sat_solver_t *solver, cidx_t cidx, cidx_t new_idx) {
  bvar_t x;

  x = var_of(first_literal_of_clause(&solver->pool, cidx));
  assert(var_is_assigned(solver, x) && solver->ante_tag[x] == ATAG_CLAUSE &&
         solver->ante_data[x] == cidx);
  solver->ante_data[x] = new_idx;
}

/*
 * Move clause from src_idx to dst_idx
 * - requires dst_idx < src_idx
 * - this copies header + literals
 * - n = length of the source clause
 */
static void clause_pool_move_clause(clause_pool_t *pool, cidx_t dst_idx, cidx_t src_idx, uint32_t n) {
  uint32_t i;

  assert(dst_idx < src_idx);
  for (i=0; i<n+2; i++) {
    pool->data[dst_idx + i] = pool->data[src_idx + i];
  }
}

/*
 * Compact the pool:
 * - remove all padding blocks
 * - cidx = where to start = base_idx
 *
 * For every clause that's marked and moved, restore the antecedent data.
 */
static void compact_clause_pool(sat_solver_t *solver, cidx_t cidx) {
  clause_pool_t *pool;
  uint32_t k, n, end;
  cidx_t i;

  pool = &solver->pool;

  assert(clause_pool_invariant(pool));

  i = cidx;
  end = pool->learned;
  for (k=0; k<2; k++) {
    /*
     * First iteration: deal with problem clauses (or do nothing)
     * Second iteration: deal with learned clauses.
     */
    while (cidx < end) {
      n = pool->data[cidx];
      if (n == 0) {
        // padding block: skip it
        n = padding_length(pool, cidx);
        cidx += n;
        assert(pool->padding >= n);
        pool->padding -= n;
      } else {
        // keep the clause: store it at index i
        assert(i <= cidx);
        if ((n & CLAUSE_MARK) != 0) {
          // marked clause: restore the antecedent data
          // and remove the mark
          n &= ~CLAUSE_MARK;
          pool->data[cidx] = n;
          restore_clause_antecedent(solver, cidx, i);
        }
        if (i < cidx) {
          clause_pool_move_clause(pool, i, cidx, n);
        }
        i += full_length(n);
        cidx += full_length(n);;
      }
    }
    if (k == 0) {
      assert(end == pool->learned);
      if (i < pool->learned) {
        pool->learned = i;
      }
      end = pool->size; // prepare for next iteration
    }
  }

  assert(end == pool->size);
  pool->size = i;
  pool->available = pool->capacity - i;

  assert(clause_pool_invariant(pool));
}

/*
 * Restore the watch vectors:
 * - scan the clauses starting from index cidx
 *   and add them to the watch vectors
 */
static void restore_watch_vectors(sat_solver_t *solver, cidx_t cidx) {
  literal_t l0, l1;
  cidx_t end;

  end = solver->pool.size;
  while (cidx < end) {
    l0 = first_literal_of_clause(&solver->pool, cidx);
    l1 = second_literal_of_clause(&solver->pool, cidx);
    add_clause_watch(solver, l0, cidx, l1);
    add_clause_watch(solver, l1, cidx, l0);
    cidx = clause_pool_next_clause(&solver->pool, cidx);
  }
}

/*
 * Garbage collection:
 * - this removes dead clauses from the pool and from the watch vectors
 * - base_index = either 0 to go through all clauses
 *   or solver->pool.learned to cleanup only the learned clauses.
 *
 * Flag 'watches_ready' means that the watch vectors don't contain
 * any clause idx. So we can skip the prepare_watch_vectors step.
 */
static void collect_garbage(sat_solver_t *solver, cidx_t base_index, bool watches_ready) {
  check_clause_pool_counters(&solver->pool);      // DEBUG
  mark_antecedent_clauses(solver, base_index);
  if (! watches_ready) {
    prepare_watch_vectors(solver, base_index);
  }
  compact_clause_pool(solver, base_index);
  check_clause_pool_learned_index(&solver->pool); // DEBUG
  check_clause_pool_counters(&solver->pool);      // DEBUG
  restore_watch_vectors(solver, base_index);
}


/*************
 *  REPORTS  *
 ************/

/*
 * Number of active variables (i.e., not assigned and not removed by
 * substitution).
 */
static uint32_t num_active_vars(const sat_solver_t *solver) {
  uint32_t c, i, n;

  c = 0;
  n = solver->nvars;
  for (i=0; i<n; i++) {
    c += var_is_active(solver, i);
  }
  return c;
}


/*
 * Statistics produced:
 * - a four-character string identify the operation
 * - number of conflicts
 * - number of restarts
 * - average level after conflict resolution (level_ema)
 * - number of active variables
 * - binary and problem clauses
 * - average glue score for learned clauses (slow_ema)
 * - average size of learned clauses
 * - number of learned clauses
 */
static void report(sat_solver_t *solver, const char *code) {
  double lits_per_clause, slow, lev;
  uint32_t vars;

  if (solver->verbosity >= 2) {
    if (solver->reports == 0) {
      fprintf(stderr, "c\n");
      fprintf(stderr, "c                        level   max  |                    prob.  |   learned  lbd\n");
      fprintf(stderr, "c        confl.  starts   ema   depth |    vars     bins  clauses |   clauses  ema   lits/cls\n");
      fprintf(stderr, "c\n");
    }
    solver->reports ++;
    solver->reports &= 31;

    lits_per_clause = 0.0;
    if (solver->pool.num_learned_clauses > 0) {
      lits_per_clause = ((double) solver->pool.num_learned_literals) / solver->pool.num_learned_clauses;
    }
    slow = ((double) solver->slow_ema)/4.3e9;
    lev = ((double) solver->level_ema)/4.3e9;

    if (solver->decision_level == 0) {
      vars = num_active_vars(solver);
      fprintf(stderr, "c %4s %8"PRIu64" %7"PRIu32" %6.2f %6"PRIu32" | %7"PRIu32" %8"PRIu32" %8"PRIu32" | %8"PRIu32" %6.2f %6.2f\n",
	      code, solver->stats.conflicts, solver->stats.starts, lev, solver->max_depth,
	      vars, solver->binaries, solver->pool.num_prob_clauses,
	      solver->pool.num_learned_clauses, slow, lits_per_clause);
    } else {
      fprintf(stderr, "c %4s %8"PRIu64" %7"PRIu32" %6.2f %6"PRIu32" |         %8"PRIu32" %8"PRIu32" | %8"PRIu32" %6.2f %6.2f\n",
	      code, solver->stats.conflicts, solver->stats.starts, lev, solver->max_depth,
	      solver->binaries, solver->pool.num_prob_clauses,
	      solver->pool.num_learned_clauses, slow, lits_per_clause);
    }
    solver->max_depth = 0;
  }
}


/*********************************
 *  DELETION OF LEARNED CLAUSES  *
 ********************************/

/*
 * Allocate the internal cidx_array for n clauses
 * - n must be positive
 */
static void alloc_cidx_array(sat_solver_t *solver, uint32_t n) {
  assert(solver->cidx_array == NULL && n > 0);
  solver->cidx_array = (cidx_t *) safe_malloc(n * sizeof(cidx_t));
}

/*
 * Delete the array
 */
static void free_cidx_array(sat_solver_t *solver) {
  assert(solver->cidx_array != NULL);
  safe_free(solver->cidx_array);
  solver->cidx_array = NULL;
}

/*
 * Check whether clause cidx is used as an antecedent.
 * (This means that it can't be deleted).
 */
static bool clause_is_locked(const sat_solver_t *solver, cidx_t cidx) {
  bvar_t x0;

  x0 = var_of(first_literal_of_clause(&solver->pool, cidx));
  return solver->ante_tag[x0] == ATAG_CLAUSE &&
    solver->ante_data[x0] == cidx && var_is_assigned(solver, x0);
}


/*
 * Check whether clause cidx should be kept
 * - heuristic: the clause is considered precious if its LDB is 4 or less
 * - this can be changed by setting keep_lbd to something other than 4.
 */
static bool clause_is_precious(sat_solver_t *solver, cidx_t cidx) {
  uint32_t n, k;

  k = solver->params.keep_lbd;
  n = clause_length(&solver->pool, cidx);
  return n <= k || clause_lbd_le(solver, n, clause_literals(&solver->pool, cidx), k);
}

/*
 * Collect learned clauses indices into solver->cidx_array
 * - initialize the array with size = number of learned clauses
 * - store all clauses that are not locked and not precious into the array
 * - return the number of clauses collected
 */
static uint32_t collect_learned_clauses(sat_solver_t *solver) {
  cidx_t *a;
  cidx_t cidx, end;
  uint32_t i;

  alloc_cidx_array(solver, solver->pool.num_learned_clauses);

  a = solver->cidx_array;
  i = 0;

  end = solver->pool.size;
  cidx = clause_pool_first_learned_clause(&solver->pool);
  while (cidx < end) {
    if (! clause_is_locked(solver, cidx) &&
        ! clause_is_precious(solver, cidx)) {
      assert(i < solver->pool.num_learned_clauses);
      a[i] = cidx;
      i ++;
    }
    cidx = clause_pool_next_clause(&solver->pool, cidx);
  }

  return i;
}

/*
 * Sort cidx_array in increasing activity order
 * - use stable sort
 * - n = number of clauses stored in the cidx_array
 */
// ordering: aux = solver, c1 and c2 are the indices of two learned clauses
static bool less_active(void *aux, cidx_t c1, cidx_t c2) {
  sat_solver_t *solver;
  float act1, act2;

  solver = aux;
  act1 = get_learned_clause_activity(&solver->pool, c1);
  act2 = get_learned_clause_activity(&solver->pool, c2);
  return act1 < act2 || (act1 == act2 && c1 < c2);
}

static void sort_learned_clauses(sat_solver_t *solver, uint32_t n) {
  uint_array_sort2(solver->cidx_array, n, solver, less_active);
}


/*
 * Delete a fraction of the learned clauses (Minisat-style)
 */
static void nsat_reduce_learned_clause_set(sat_solver_t *solver) {
  uint32_t i, n, n0;
  cidx_t *a;

  if (solver->verbosity >= 4) {
    fprintf(stderr, "\nc Reduce learned clause set\n");
    fprintf(stderr, "c  on entry: %"PRIu32" clauses, %"PRIu32" literals\n",
            solver->pool.num_learned_clauses, solver->pool.num_learned_literals);
  }
  n = collect_learned_clauses(solver);
  sort_learned_clauses(solver, n);
  a = solver->cidx_array;

  check_candidate_clauses_to_delete(solver, a, n); // DEBUG

  if (solver->verbosity >= 4) {
    fprintf(stderr, "c  possible deletion: %"PRIu32" clauses\n", n);
  }

  // a contains the clauses that can be deleted
  // less useful clauses (i.e., low-activity clauses) occur first
  n0 = solver->params.reduce_fraction * (n/32);
  for (i=0; i<n0; i++) {
    clause_pool_delete_clause(&solver->pool, a[i]);
    solver->stats.learned_clauses_deleted ++;
  }

  free_cidx_array(solver);

  collect_garbage(solver, solver->pool.learned, false);
  solver->stats.reduce_calls ++;

  check_watch_vectors(solver);

  if (solver->verbosity >= 4) {
    fprintf(stderr, "c  on exit: %"PRIu32" clauses, %"PRIu32" literals\n",
            solver->pool.num_learned_clauses, solver->pool.num_learned_literals);
  }

  report(solver, "red");
}



/********************************************
 *  SIMPLIFICATION OF THE CLAUSE DATABASE   *
 *******************************************/

/*
 * Cleanup watch vector w:
 * - remove all the assigned (true) literals from w
 * - also remove all the clause indices
 * - after clauses are deleted from the pool, we call 'collect_garbage'
 *   to do a full cleanup and restore the watch vectors.
 */
static void cleanup_watch_vector(sat_solver_t *solver, watch_t *w) {
  uint32_t i, j, k, n;

  assert(solver->decision_level == 0 &&
         solver->stack.top == solver->stack.prop_ptr &&
         w != NULL);

  n = w->size;
  j = 0;
  i = 0;
  while (i < n) {
    k = w->data[i];
    if (idx_is_clause(k)) {
      i += 2;
    } else {
      if (lit_is_unassigned(solver, idx2lit(k))) {
        w->data[j] = k;
        j ++;
      }
      i ++;
    }
  }
  w->size = j;
}


/*
 * Simplify the binary clauses:
 * - if l is assigned at level 0, delete its watched vector
 *   (this assumes that all Boolean propagations have been done).
 * - otherwise, remove the assigned literals from watch[l].
 */
static void simplify_binary_clauses(sat_solver_t *solver) {
  uint32_t i, n;
  watch_t *w;

  assert(solver->decision_level == 0 &&
         solver->stack.top == solver->stack.prop_ptr);

  n = solver->nliterals;
  for (i=2; i<n; i++) {
    w = solver->watch[i];
    if (w != NULL) {
      switch (lit_value(solver, i)) {
      case VAL_UNDEF_TRUE:
      case VAL_UNDEF_FALSE:
        cleanup_watch_vector(solver, w);
        break;

      case VAL_TRUE:
      case VAL_FALSE:
        safe_free(w);
        solver->watch[i] = NULL;
        break;
      }
    }
  }
}


/*
 * After deletion: count the number of binary clauses left
 */
static uint32_t num_literals_in_watch_vector(watch_t *w) {
  uint32_t i, n, count;

  assert(w != NULL);
  count = 0;
  n = w->size;
  i = 0;
  while (i < n) {
    if (idx_is_literal(w->data[i])) {
      count ++;
      i ++;
    } else {
      i += 2;
    }
  }
  return count;
}


static uint32_t count_binary_clauses(sat_solver_t *solver) {
  uint32_t i, n, sum;
  watch_t *w;

  sum = 0;
  n = solver->nliterals;
  for (i=2; i<n; i++) {
    w = solver->watch[i];
    if (w != NULL) {
      sum += num_literals_in_watch_vector(w);
    }
  }
  assert((sum & 1) == 0 && sum/2 <= solver->binaries);

  return sum >> 1;
}


/*
 * Simplify the clause that starts at cidx:
 * - remove all literals that are false at the base level
 * - delete the clause if it is true
 * - if the clause cidx is reduced to a binary clause { l0, l1 }
 *   then delete cidx and add { l0, l1 } as a binary clause
 *
 * - return true if the clause is deleted
 * - return false otherwise
 */
static bool simplify_clause(sat_solver_t *solver, cidx_t cidx) {
  uint32_t i, j, n;
  literal_t *a;
  literal_t l;

  assert(solver->decision_level == 0 && good_clause_idx(&solver->pool, cidx));

  n = clause_length(&solver->pool, cidx);
  a = clause_literals(&solver->pool, cidx);

  j = 0;
  for (i=0; i<n; i++) {
    l = a[i];
    switch (lit_value(solver, l)) {
    case VAL_FALSE:
      break;

    case VAL_UNDEF_FALSE:
    case VAL_UNDEF_TRUE:
      a[j] = l;
      j ++;
      break;

    case VAL_TRUE:
      // the clause is true
      clause_pool_delete_clause(&solver->pool, cidx);
      return true;
    }
  }

  assert(j >= 2);

  if (j == 2) {
    // convert to a binary clause
    add_binary_clause(solver, a[0], a[1]); // must be done first
    clause_pool_delete_clause(&solver->pool, cidx);
    solver->simplify_new_bins ++;
    return true;
  }

  if (j < n) {
    clause_pool_shrink_clause(&solver->pool, cidx, j);
  }
  return false;
}


/*
 * Remove dead antecedents (of literals assigned at level 0)
 * - if l is implied at level 0 by a clause cidx,
 *   then cidx will be deleted by simplify_clause_database.
 *   so l ends up with a dead antecedent.
 * - to fix this, we force the ante_tag of all variables
 *   assigned at level 0 to ATAG_UNIT.
 */
static void remove_dead_antecedents(sat_solver_t *solver) {
  uint32_t i, n;
  literal_t l;

  assert(solver->decision_level == 0);

  n = solver->stack.top;
  for (i=0; i<n; i++) {
    l = solver->stack.lit[i];
    assert(solver->level[var_of(l)] == 0);
    solver->ante_tag[var_of(l)] = ATAG_UNIT;
  }
}


/*
 * Simplify all the clauses
 * - this does basic simplifications: remove all false literals
 *   and remove all true clauses.
 */
static void simplify_clause_database(sat_solver_t *solver) {
  cidx_t cidx;
  uint32_t d;

  assert(solver->decision_level == 0 && solver->stack.top == solver->stack.prop_ptr);

  if (solver->verbosity >= 4) {
    fprintf(stderr, "\nc Simplify clause database\n");
    fprintf(stderr, "c  on entry: prob: %"PRIu32" cls/%"PRIu32" lits, learned: %"PRIu32" cls/%"PRIu32" lits\n",
            solver->pool.num_prob_clauses, solver->pool.num_prob_literals,
            solver->pool.num_learned_clauses, solver->pool.num_learned_literals);
  }

  simplify_binary_clauses(solver);

  d = 0; // count deleted clauses
  cidx = clause_pool_first_clause(&solver->pool);
  // Note: pool.size may change within the loop if clauses are deleted
  while (cidx < solver->pool.size) {
    d += simplify_clause(solver, cidx);
    cidx = clause_pool_next_clause(&solver->pool, cidx);
  }

  solver->stats.prob_clauses_deleted += d;
  remove_dead_antecedents(solver);
  collect_garbage(solver, 0, true);

  solver->binaries = count_binary_clauses(solver);
  solver->stats.simplify_calls ++;

  check_watch_vectors(solver);

  if (solver->verbosity >= 4) {
    fprintf(stderr, "c  on exit: prob: %"PRIu32" cls/%"PRIu32" lits, learned: %"PRIu32" cls/%"PRIu32" lits\n\n",
            solver->pool.num_prob_clauses, solver->pool.num_prob_literals,
            solver->pool.num_learned_clauses, solver->pool.num_learned_literals);
  }

  report(solver, "simp");
}


/*******************************
 *  BINARY IMPLICATION GRAPH   *
 ******************************/

/*
 * The binary implication graph is defined by the binary clauses.
 * Its vertices are literals. A binary clause {l0, l1} defines two
 * edges in the graph: ~l0 --> l1 and ~l1 --> l0.
 *
 * If there's a circuit in this graph: l0 --> l1 --> .... --> l_n --> l0
 * then all the literals on the circuit are equivalent. We can reduce the
 * problem by replacing l1, ..., l_n by l0.
 */

#if 1
/*
 * Convert l to the original dimacs index:
 * - dimacs(pos_lit(x)) = x
 * - dimacs(neg_lit(x)) = -x
 */
static int32_t dimacs(uint32_t l) {
  int32_t x;
  x = var_of(l);
  return is_pos(l) ? x : - x;
}

/*
 * Display a strongly-connected component C
 * - l = root of the component C
 * - the elements of C are stored in solver->vertex_stack, above l
 */
static void show_scc(FILE *f, const sat_solver_t *solver, literal_t l) {
  literal_t l0;
  uint32_t i;
  const vector_t *v;

  v = &solver->vertex_stack;
  assert(v->size > 0);
  i = v->size - 1;
  l0 = v->data[i];
  if (l0 != l) {
    // interesting SCC: not reduced to { l }
    fprintf(f, "c ");
    if (solver->label[not(l)] == UINT32_MAX) {
      fprintf(f, "dual ");
    }
    fprintf(f, "SCC: { %"PRId32" ", dimacs(l0));
    do {
      assert(i > 0);
      i --;
      l0 = v->data[i];
      fprintf(f, "%"PRId32" ", dimacs(l0));
    } while (l0 != l);
    fprintf(f, "}\n");
  }
}

#endif

/*
 * Find a representative literal in a strongly-connected component
 * - l = root of the component C
 * - the elements of C are stored in solver->vertex_stack, above l
 *
 * In preprocessing mode, the representative is the smallest literal in C.
 * In search mode, the representative is the most active literal in C.
 */
static literal_t scc_representative(sat_solver_t *solver, literal_t l) {
  uint32_t i;
  literal_t rep, l0;
  double max_act, act;

  i = solver->vertex_stack.size;
  rep = l;
  if (solver->preprocess) {
    do {
      assert(i > 0);
      i --;
      l0 = solver->vertex_stack.data[i];
      if (l0 < rep) rep = l0;
    } while (l0 != l);

  } else {
    max_act = lit_activity(solver, rep);
    do {
      assert(i > 0);
      i --;
      l0 = solver->vertex_stack.data[i];
      act = lit_activity(solver, l0);
      if (act > max_act || (act == max_act && l0 < rep)) {
	max_act = act;
	rep = l0;
      }
    } while (l0 != l);
  }

  return rep;
}

/*
 * Process a strongly-connected component
 * - l = root of the component C
 * - the elements of C are stored in solver->vertex_stack, above l
 *
 * If the complementary component has been processed before, we just
 * mark that literals of C have been fully explored.
 *
 * Otherwise, we select a representative 'rep' in C. For every other
 * literal l0 in C, we record subst[l0] := rep.
 * - the antecedent tag for var_of(l0) is set to ATAG_SUBST
 * - the antecedent data for var_of(l0) is set to rep or not(rep)
 *   depending on l0's polarity.
 *
 * If we detect that C contains complementary literals l0 and not(l0),
 * we add the empty clause and exit.
 */
static void process_scc(sat_solver_t *solver, literal_t l) {
  literal_t l0, rep;
  bool unsat;

  assert(solver->label[l] < UINT32_MAX);

  if (solver->verbosity >= 400) {
    show_scc(stderr, solver, l);
  }

  if (solver->label[not(l)] == UINT32_MAX) {
    /*
     * This SCC is of the form { l_0 ..., l }. The complementary SCC {
     * not(l_0) ... not(l) } has been processed before.  We mark l0,
     * ..., l as fully explored and remove C from the
     * vertex_stack.
     */
    do {
      l0 = vector_pop(&solver->vertex_stack);
      solver->label[l0] = UINT32_MAX; // fully explored mark
    } while (l0 != l);

  } else {
    /*
     * We check for inconsistency and store the substitution
     */
    unsat = false;
    rep = scc_representative(solver, l);

    do {
      l0 = vector_pop(&solver->vertex_stack);
      solver->label[l0] = UINT32_MAX; // mark l0 as fully explored/SCC known
      if (lit_is_eliminated(solver, l0)) {
	// both l0 and not(l0) are in the SCC
	assert(base_subst(solver, l0) == not(rep));
	unsat = true;
	add_empty_clause(solver);
	break;
      }
      // record substitution: subst[l0] := rep
      if (l0 != rep) {
	set_lit_subst(solver, l0, rep);
      }
    } while (l0 != l);

    if (unsat) {
      fprintf(stderr, "c found inconsistent SCC\n");
      show_scc(stderr, solver, l);
    }
  }
}

/*
 * Get the next successor of l0 in the implication graph:
 * - i = index in the watch vector of ~l0 to scan from
 * - if there's a binary clause {~l0, l1} at some index k >= i, then
 *   we return true, store l1 in *successor,  and store k+1 in *i.
 * - otherwise, the function returns false.
 */
static bool next_successor(const sat_solver_t *solver, literal_t l0, uint32_t *i, literal_t *successor) {
  uint32_t k, idx, n;
  watch_t *w;

  w = solver->watch[not(l0)];
  if (w != NULL) {
    n = w->size;
    k = *i;
    assert(k <= n);

    if (solver->preprocess) {
      /*
       * in preprocessing mode:
       * all elements in w->data are clause indices
       */
      while (k < n) {
	idx = w->data[k];
	if (clause_is_live(&solver->pool, idx) && clause_length(&solver->pool, idx) == 2) {
	  *i = k+1;
	  *successor = other_watched_literal_of_clause(&solver->pool, idx, not(l0));
	  return true;
	}
	k ++;
      }

    } else {
      /*
       * in search mode:
       * elements in w->data encode either a single literal
       * or a pair clause index + blocker
       */
      while (k < n) {
	idx = w->data[k];
	if (idx_is_literal(idx)) {
	  *i = k+1;
	  *successor = idx2lit(idx);
	  return true;
	} else if (clause_length(&solver->pool, idx) == 2) {
	  *i = k+2;
	  *successor = other_watched_literal_of_clause(&solver->pool, idx, not(l0));
	  return true;
	}
	k += 2;
      }
    }
  }

  return false;
}

/*
 * Compute strongly-connected components. Explore the graph starting from literal l.
 * - visit stores the visit index of a literal: visit[l1] = k means that l1 is reachable
 *   from l and is the k-th vertex visited (where k>=1).
 * - label stores the smallest index of a reachable literal: label[l1] = index of a
 *   vertex l2 reachable from l1 and with visit[l2] <= visit[l1]
 * - for vertices that have been fully explored, we set label[l] = UINT32_MAX (cf.
 *   process_scc).
 */
static void dfs_explore(sat_solver_t *solver, literal_t l) {
  gstack_elem_t *e;
  uint32_t k;
  literal_t x, y;

  //  fprintf(stderr, "dfs: root = %"PRId32"\n", dimacs(l));

  assert(solver->visit[l] == 0 &&
         gstack_is_empty(&solver->dfs_stack) &&
         solver->vertex_stack.size == 0);

  k = 1;
  solver->visit[l] = k;
  solver->label[l] = k;
  gstack_push_vertex(&solver->dfs_stack, l, 0);
  vector_push(&solver->vertex_stack, l);

  for (;;) {
    e = gstack_top(&solver->dfs_stack);
    x = e->vertex;
    if (next_successor(solver, x, &e->index, &y)) {
      // x --> y in the implication graph
      if (solver->visit[y] == 0) {
        // y not visited yet
        k ++;
        solver->visit[y] = k;
        solver->label[y] = k;
        gstack_push_vertex(&solver->dfs_stack, y, 0);
        vector_push(&solver->vertex_stack, y);
      } else if (solver->label[y] < solver->label[x]) {
        // y has a successor visited before x on the dfs stack
        solver->label[x] = solver->label[y];
      }

    } else {
      // all successors of x have been explored
      assert(solver->label[x] <= solver->visit[x]);
      if (solver->label[x] == solver->visit[x]) {
        // x is the root of its SCC
        process_scc(solver, x);
	if (solver->has_empty_clause) {
	  // unsat detected
	  reset_gstack(&solver->dfs_stack);
	  break;
	}
      }
      // pop x
      gstack_pop(&solver->dfs_stack);
      if (gstack_is_empty(&solver->dfs_stack)) {
        break; // all done
      }
      // update the label of x's predecessor
      y = gstack_top(&solver->dfs_stack)->vertex;
      if (solver->label[x] < solver->label[y]) {
        solver->label[y] = solver->label[x];
      }
    }
  }
}


/*
 * Compute all SCCs and build/extend the variable substitution.
 * - sets solver->has_empty_clause to true if an SCC
 *   contains complementary literals.
 */
static void compute_sccs(sat_solver_t *solver) {
  uint32_t i, n;

  assert(solver->label == NULL && solver->visit == NULL);

  n = solver->nliterals;
  solver->label = (uint32_t *) safe_malloc(n * sizeof(uint32_t));
  solver->visit = (uint32_t *) safe_malloc(n * sizeof(uint32_t));
  for (i=0; i<n; i++) {
    solver->visit[i] = 0;
    solver->label[i] = 0;
  }
  for (i=2; i<n; i++) {
    if (lit_is_active(solver, i) && solver->label[i] == 0) {
      dfs_explore(solver, i);
      if (solver->has_empty_clause) break; // UNSAT detected
    }
  }

  safe_free(solver->label);
  safe_free(solver->visit);
  solver->label = NULL;
  solver->visit = NULL;
}



/*************************************
 *  APPLY THE VARIABLE SUBSTITUTION  *
 ************************************/

/*
 * Trick to detect duplicates and complementary literals in a clause:
 * - when literal l is added to a clause, we temporarily set its
 *   truth value to false.
 * - so the next occurrence of l is ignored and an occurrence of not(l)
 *   makes the clause true.
 */
// make l false and not l true (temporarily)
// preserve the preferred polarity in bits 3-2 of solver->value[l]
static void mark_false_lit(sat_solver_t *solver, literal_t l) {
  uint8_t v;

  assert(l < solver->nliterals);
  assert(lit_is_unassigned(solver, l));

  v = solver->value[l];
  solver->value[l] = (v<<2) | VAL_FALSE;
  v = solver->value[not(l)];
  solver->value[not(l)] = (v<<2) | VAL_TRUE;
}

// remove the mark on l and restore the preferred polarity
static void clear_false_lit(sat_solver_t *solver, literal_t l) {
  bvar_t x;
  uint8_t v;

  assert(l < solver->nliterals);
  assert((solver->value[l] & 3) == VAL_FALSE);

  x = var_of(l);
  v = solver->value[pos_lit(x)];
  solver->value[pos_lit(x)] = v >> 2;
  v  = solver->value[neg_lit(x)];
  solver->value[neg_lit(x)] = v >> 2;

  assert(solver->value[pos_lit(x)] < 2 &&
	 solver->value[neg_lit(x)] < 2 &&
	 (solver->value[pos_lit(x)] ^ solver->value[neg_lit(x)]) == 1);
}

// remove the marks on a[0 ... n-1]
static void clear_false_lits(sat_solver_t *solver, uint32_t n, const literal_t *a) {
  uint32_t i;

  for (i=0; i<n; i++) {
    clear_false_lit(solver, a[i]);
  }
}

/*
 * Simplify the clause that starts at cidx and apply the substitution.
 * - if the clause becomes empty: set solver->has_empty_clause to true
 * - if the clause simplifies to a unit clause: add a unit literal
 *
 * - delete the clause if it is true or if it reduces to a clause
 *   of length <= 2.
 */
static bool subst_and_simplify_clause(sat_solver_t *solver, cidx_t cidx) {
  uint32_t i, j, n;
  literal_t *a;
  literal_t l;

  assert(solver->decision_level == 0 && good_clause_idx(&solver->pool, cidx));

  n = clause_length(&solver->pool, cidx);
  a = clause_literals(&solver->pool, cidx);

  j = 0;
  for (i=0; i<n; i++) {
    l = lit_subst(solver, a[i]);
    switch (solver->value[l] & 3) {
    case VAL_FALSE:
      break;

    case VAL_UNDEF_FALSE:
    case VAL_UNDEF_TRUE:
      a[j] = l;
      j ++;
      mark_false_lit(solver, l);
      break;

    case VAL_TRUE:
      // the clause is true
      goto done;
    }
  }

 done:
  /*
   * a[0 ... j-1]: literals after substitution.
   * If i < n, the clause is true and must be deleted.
   * Otherwise, we keep the clause a[0 ... j-1].
   * We change representation if j <= 2.
   */
  clear_false_lits(solver, j, a);

  if (i < n) { // true clause
    clause_pool_delete_clause(&solver->pool, cidx);
    return true;
  }

  if (j <= 2) {
    // reduced to a small clause
    if (j == 0) {
      add_empty_clause(solver);
    } else if (j == 1) {
      add_unit_clause(solver, a[0]);
      solver->simplify_new_units ++;
    } else {
      add_binary_clause(solver, a[0], a[1]);
      solver->simplify_new_bins ++;
    }
    clause_pool_delete_clause(&solver->pool, cidx);
    return true;
  }

  if (j < n) {
    clause_pool_shrink_clause(&solver->pool, cidx, j);
  }
  return false;
}


/*
 * Apply the substitution to binary clause { l0, l1 }
 */
static void subst_and_simplify_binary_clause(sat_solver_t *solver, literal_t l0, literal_t l1) {
  literal_t a[2];
  literal_t l;
  uint32_t i, j;

  a[0] = l0;
  a[1] = l1;

  j = 0;
  for (i=0; i<2; i++) {
    l = lit_subst(solver, a[i]);
    switch(lit_value(solver, l)) {
    case VAL_FALSE:
      break;

    case VAL_UNDEF_TRUE:
    case VAL_UNDEF_FALSE:
      a[j] = l;
      j ++;
      break;

    case VAL_TRUE:
      return;
    }
  }

  if (j == 0) {
    add_empty_clause(solver);

  } else if (j == 1) {
    assert(lit_is_unassigned(solver, a[0]));
    add_unit_clause(solver, a[0]);

  } else {
    assert(lit_is_unassigned(solver, a[0]));
    assert(lit_is_unassigned(solver, a[1]));

    if (a[0] == a[1]) {
      add_unit_clause(solver, a[0]);
    } else if (a[0] != not(a[1])) {
      add_binary_clause(solver, a[0], a[1]);
    }
  }
}

/*
 * Scan vector w = watch[l0] where l0 is unassigned
 * - collect the binary clauses implicitly stored in w and add them to v
 *   then reset w
 * - to avoid duplicate clauses in v, we collect only the clauses of the
 *   form {l0 , l} with l > l0. We also ignore { l0, l} if l is true.
 */
static void collect_binary_clauses_of_watch(sat_solver_t *solver, watch_t *w, literal_t l0, vector_t *v) {
  uint32_t i, k, n;
  literal_t l;

  assert(lit_is_unassigned(solver, l0) && solver->watch[l0] == w);

  n = w->size;
  i = 0;
  while (i<n) {
    k = w->data[i];
    if (idx_is_literal(k)) {
      i ++;
      l = idx2lit(k);
      assert(! lit_is_false(solver, l));
      if (l > l0 && lit_is_unassigned(solver, l)) {
	vector_push(v, l0);
	vector_push(v, l);
      }
    } else {
      i += 2;
    }
  }

  w->size = 0;
}

/*
 * Scan all the watch vectors:
 * - add all the binary clauses to vector v
 * - reset all watch vectors
 */
static void collect_binary_clauses_and_reset_watches(sat_solver_t *solver, vector_t *v) {
  uint32_t i, n;
  watch_t *w;

  assert(solver->decision_level == 0 && solver->stack.top == solver->stack.prop_ptr);

  n = solver->nliterals;
  for (i=2; i<n; i++) {
    w = solver->watch[i];
    if (w != NULL) {
      if (lit_is_assigned(solver, i)) {
	/*
	 * Since boolean propagation is done, all binary
	 * clauses of w are true at level 0.
	 */
	safe_free(w);
	solver->watch[i] = NULL;
      } else {
	collect_binary_clauses_of_watch(solver, w, i, v);
      }
    }
  }
}

/*
 * Apply the substitution to all the binary clauses
 * - first, collect all binary clauses in an vector v and
 *   empty the watch vectors.
 * - then process all the clauses of v
 */
static void apply_subst_to_binary_clauses(sat_solver_t *solver) {
  vector_t aux;
  uint32_t i, n;

  init_vector(&aux);
  collect_binary_clauses_and_reset_watches(solver, &aux);
  n = aux.size;
  for (i=0; i<n; i += 2) {
    subst_and_simplify_binary_clause(solver, aux.data[i], aux.data[i+1]);
    if (solver->has_empty_clause) break;
  }
  delete_vector(&aux);
}


/*
 * Apply the substitution to all clauses
 */
static void apply_substitution(sat_solver_t *solver) {
  cidx_t cidx;
  uint32_t d;

  assert(solver->decision_level == 0 && solver->stack.top == solver->stack.prop_ptr);

  apply_subst_to_binary_clauses(solver);
  if (solver->has_empty_clause) return;

  d = 0; // count deleted clauses
  cidx = clause_pool_first_clause(&solver->pool);
  while (cidx < solver->pool.size) {
    d += subst_and_simplify_clause(solver, cidx);
    if (solver->has_empty_clause) return;
    cidx = clause_pool_next_clause(&solver->pool, cidx);
  }

  solver->stats.prob_clauses_deleted += d;
  remove_dead_antecedents(solver);
  collect_garbage(solver, 0, true);

  solver->binaries = count_binary_clauses(solver);
  solver->stats.subst_calls ++;

  check_watch_vectors(solver);
}




/*******************
 *  PREPROCESSING  *
 ******************/

/*
 * Statistics after preprocessing
 */
static void show_preprocessing_stats(sat_solver_t *solver, double time) {
  fprintf(stderr, "c\n"
	          "c After preprocessing\n");
  fprintf(stderr, "c  unit literals        : %"PRIu32"\n", solver->stats.pp_unit_lits);
  fprintf(stderr, "c  pure literals        : %"PRIu32"\n", solver->stats.pp_pure_lits);
  fprintf(stderr, "c  substitutions        : %"PRIu32"\n", solver->stats.pp_subst_vars);
  fprintf(stderr, "c  cheap var elims      : %"PRIu32"\n", solver->stats.pp_cheap_elims);
  fprintf(stderr, "c  less cheap var elims : %"PRIu32"\n", solver->stats.pp_var_elims);
  fprintf(stderr, "c  active vars          : %"PRIu32"\n", num_active_vars(solver));
  fprintf(stderr, "c  deleted clauses      : %"PRIu32"\n", solver->stats.pp_clauses_deleted);
  fprintf(stderr, "c  subsumed clauses     : %"PRIu32"\n", solver->stats.pp_subsumptions);
  fprintf(stderr, "c  strengthenings       : %"PRIu32"\n", solver->stats.pp_strengthenings);
  fprintf(stderr, "c  unit strengthenings  : %"PRIu32"\n", solver->stats.pp_unit_strengthenings);
  fprintf(stderr, "c  unit clauses         : %"PRIu32"\n", solver->units);           // should be zero
  fprintf(stderr, "c  bin clauses          : %"PRIu32"\n", solver->binaries);
  fprintf(stderr, "c  big clauses          : %"PRIu32"\n", solver->pool.num_prob_clauses);
  fprintf(stderr, "c\n"
	          "c Preprocessing time    : %.4f\nc\n", time);
  if (solver->has_empty_clause) {
    fprintf(stderr, "c\nc found unsat by preprocessing\nc\n");
  }
}


/*
 * QUEUE OF CLAUSES/SCAN INDEX
 */

/*
 * The queue cqueue + the scan index define a set of clauses to visit:
 * - cqueue contains clause idx that are smaller (strictly) than scan index.
 * - every clause in cqueue is marked.
 * - the set of clauses to visit is the union of the clauses in cqueue and
 *   the clauses of index >= scan_index.
 */

/*
 * Reset: empty the queue and remove marks
 */
static void reset_clause_queue(sat_solver_t *solver) {
  cidx_t cidx;

  solver->scan_index = 0;
  while (! queue_is_empty(&solver->cqueue)) {
    cidx = queue_pop(&solver->cqueue);
    if (clause_is_live(&solver->pool, cidx)) {
      unmark_clause(&solver->pool, cidx);
    }
  }
}


/*
 * Add cidx to the queue:
 * - cidx is the index of a clause that shrunk (so it may subsume more clauses)
 * - do nothing if cidx is marked (i.e., already in cqueue) or if cidx >= scan_index
 */
static void clause_queue_push(sat_solver_t *solver, cidx_t cidx) {
  if (cidx < solver->scan_index && clause_is_unmarked(&solver->pool, cidx)) {
    mark_clause(&solver->pool, cidx);
    queue_push(&solver->cqueue, cidx);
  }
}


/*
 * Next clause from scan index: return solver->pool.size if
 * all clauses have been scanned
 */
static cidx_t clause_scan_next(sat_solver_t *solver) {
  cidx_t i;

  i = solver->scan_index;
  if (i < solver->pool.size) {
    solver->scan_index = clause_pool_next_clause(&solver->pool, i);
  }
  return i;
}

/*
 * Get the next element in the queue
 * - return solver->pool.size if the queue is empty
 */
static cidx_t clause_queue_pop(sat_solver_t *solver) {
  cidx_t i;

  while(! queue_is_empty(&solver->cqueue)) {
    i = queue_pop(&solver->cqueue);
    if (clause_is_live(&solver->pool, i)) {
      unmark_clause(&solver->pool, i);
      goto done;
    }
  }
  i = solver->pool.size; // all done
 done:
  return i;
}



/*
 * HEURISTIC/HEAP FOR VARIABLE ELIMINATION
 */

/*
 * Variables that have too many positive and negative occurrences are not eliminated.
 * - the cutoff is solver->val_elim_skip (10 by default)
 */
static bool pp_elim_candidate(const sat_solver_t *solver, bvar_t x) {
  assert(x < solver->nvars);
  return solver->occ[pos_lit(x)] < solver->params.var_elim_skip
    || solver->occ[neg_lit(x)] < solver->params.var_elim_skip;
}

/*
 * Cost of eliminating x (heuristic estimate)
 */
static uint64_t pp_elim_cost(const sat_solver_t *solver, bvar_t x) {
  assert(pp_elim_candidate(solver, x));
  return ((uint64_t) solver->occ[pos_lit(x)]) * solver->occ[neg_lit(x)];
}


/*
 * Number of occurrences of x
 */
static inline uint32_t var_occs(const sat_solver_t *solver, bvar_t x) {
  assert(x < solver->nvars);
  return solver->occ[pos_lit(x)] + solver->occ[neg_lit(x)];
}

/*
 * Ordering for elimination:
 * - elim_lt(solver, x, y) returns true if x < y for our heuristic ordering.
 * - we want to do cheap eliminations first (i.e., variables with one positive or
 *   one negative occurrences).
 * - for other variables, we use occ[pos_lit(x)] * occ[neg_lit(x)] as an estimate of the cost
 *   of eliminating x
 */
static bool elim_lt(const sat_solver_t *solver, bvar_t x, bvar_t y) {
  uint32_t cx, cy, ox, oy;

  cx = pp_elim_cost(solver, x);
  ox = var_occs(solver, x);
  cy = pp_elim_cost(solver, y);
  oy = var_occs(solver, y);

  if (cx < ox && cy >= oy) return true;     // x cheap, y not cheap
  if (cy < oy && cx >= ox) return false;    // y cheap, x not cheap
  return cx < cy;
}


/*
 * Simpler heuristic: not used
 */
/*
 * static bool elim_lt(const sat_solver_t *solver, bvar_t x, bvar_t y) {
 *   return pp_elim_cost(solver, x) < pp_elim_cost(solver, y);
 * }
 */


/*
 * Move the variable at position i up the tree
 */
static void elim_heap_move_up(sat_solver_t *solver, uint32_t i) {
  elim_heap_t *heap;
  bvar_t x, y;
  uint32_t j;

  heap = &solver->elim;

  assert(0 < i && i < heap->size);

  x = heap->data[i];
  for (;;) {
    j = i >> 1;        // parent of i
    if (j == 0) break; // top of the heap

    y = heap->data[j];
    if (!elim_lt(solver, x, y)) break; // x >= y: stop here

    // move y down into i
    heap->data[i] = y;
    heap->elim_idx[y] = i;
    i = j;
  }

  heap->data[i] = x;
  heap->elim_idx[x] = i;
}


/*
 * Move the variable at position i down the tree
 */
static void elim_heap_move_down(sat_solver_t *solver, uint32_t i) {
  elim_heap_t *heap;
  uint32_t j;
  bvar_t x, y, z;

  heap = &solver->elim;

  assert(0 < i && i < heap->size);

  x = heap->data[i];

  j = i<<1; // j = left child of i. (this can't overflow since heap->size < 2^32/4)

  while (j < heap->size) {
    // y = smallest of the two children of i
    y = heap->data[j];
    if (j + 1 < heap->size) {
      z = heap->data[j+1];
      if (elim_lt(solver, z, y)) {
        y = z;
        j ++;
      }
    }

    // if x < y then x goes into i
    if (elim_lt(solver, x, y)) break;

    // move y up into i
    heap->data[i] = y;
    heap->elim_idx[y] = i;
    i = j;
    j <<= 1;
  }

  heap->data[i] = x;
  heap->elim_idx[x] = i;
}


/*
 * Move variable at position i either up or down
 */
static void elim_heap_update(sat_solver_t *solver, uint32_t i) {
  elim_heap_move_up(solver, i);
  elim_heap_move_down(solver, i);
  check_elim_heap(solver);
}


/*
 * Check whether the heap is empty
 */
static inline bool elim_heap_is_empty(const sat_solver_t *solver) {
  return solver->elim.size == 1;
}


/*
 * Check whether x is in the heap
 */
#ifndef NDEBUG
static inline bool var_is_in_elim_heap(const sat_solver_t *solver, bvar_t x) {
  assert(x < solver->nvars);
  return solver->elim.elim_idx[x] >= 0;
}
#endif


/*
 * Remove the top variable from the heap
 */
static bvar_t elim_heap_get_top(sat_solver_t *solver) {
  elim_heap_t *heap;
  bvar_t x, y;

  heap = &solver->elim;

  assert(heap->size > 1);

  x = heap->data[1];
  heap->elim_idx[x] = -1;
  heap->size --;

  if (heap->size > 1) {
    y = heap->data[heap->size];
    heap->data[1] = y;
    heap->elim_idx[y] = 1;
    elim_heap_move_down(solver, 1);
  }

  check_elim_heap(solver);

  return x;
}


/*
 * Add variable x to the heap:
 * - x must not be present in the heap
 */
static void elim_heap_insert_var(sat_solver_t *solver, bvar_t x) {
  elim_heap_t *heap;
  uint32_t i;

  assert(pp_elim_candidate(solver, x));

  heap = &solver->elim;

  assert(heap->elim_idx[x] < 0); // x must not be in the heap

  i = heap->size;
  if (i == heap->capacity) {
    extend_elim_heap(heap);
  }
  assert(i < heap->capacity);
  heap->size ++;
  heap->data[i] = x;
  heap->elim_idx[x] = i;
  elim_heap_move_up(solver, i);

  check_elim_heap(solver);
}


/*
 * Remove x from the heap if it's there
 */
static void elim_heap_remove_var(sat_solver_t *solver, bvar_t x) {
  elim_heap_t *heap;
  int32_t i;
  bvar_t y;

  assert(x < solver->nvars);

  heap = &solver->elim;
  i = heap->elim_idx[x];
  if (i >= 0) {
    heap->elim_idx[x] = -1;
    heap->size --;
    if (heap->size > i) {
      y = heap->data[heap->size];
      heap->data[i] = y;
      heap->elim_idx[y] = i;
      elim_heap_update(solver, i);
    }
    check_elim_heap(solver);
  }
}


/*
 * Update: move/add x when its occurrence counts have changed
 */
static void elim_heap_update_var(sat_solver_t *solver, bvar_t x) {
  int32_t i;

  assert(x < solver->nvars);

  if (var_is_unassigned(solver, x) && pp_elim_candidate(solver, x)) {
    i = solver->elim.elim_idx[x];
    if (i < 0) {
      elim_heap_insert_var(solver, x);
    } else {
      elim_heap_update(solver, i);
    }
  } else {
    elim_heap_remove_var(solver, x);
  }
}


/*
 * GARBAGE COLLECTION DURING PREPROCESSING
 */

/*
 * Go through the pool and remove all the padding blocks
 * - if a clause is marked, add it to the clause queue (after the move)
 * - also restore the scan index
 */
static void pp_compact_clause_pool(sat_solver_t *solver) {
  clause_pool_t *pool;
  uint32_t k, n, len, end;
  cidx_t i, j;

  pool = &solver->pool;

  assert(clause_pool_invariant(pool) && pool->learned == pool->size);

  i = 0;
  j = 0;
  end = solver->scan_index;
  for (k=0; k<2; k++) {
    /*
     * First iteration, move the clauses that are before the scan index
     * Second iteration, clauses after the scan index.
     */
    while (i < end) {
      assert(good_clause_idx(pool, i));
      n = pool->data[i];
      if (n == 0) {
        // padding block, skip it
        i += padding_length(pool, i);
      } else {
        assert(j <= i);
        len = n;
        if ((n & CLAUSE_MARK) != 0) {
          // marked clause: store it in the clause queue
          queue_push(&solver->cqueue, j);
          len &= ~CLAUSE_MARK;
        }
        if (j < i) {
          clause_pool_move_clause(pool, j, i, len);
        }
        i += full_length(len);
        j += full_length(len);
      }
    }
    if (k == 0) {
      solver->scan_index = j;
      end = pool->size;
    }
  }

  assert(end == pool->size);
  pool->size = j;
  pool->learned = j;
  pool->available = pool->capacity - j;
  pool->padding = 0;

  assert(clause_pool_invariant(pool));
}


/*
 * Reconstruct the watch vectors after compaction
 */
static void pp_restore_watch_vectors(sat_solver_t *solver) {
  uint32_t i, n;
  cidx_t cidx;
  watch_t *w;

  n = solver->nliterals;
  for (i=0; i<n; i++) {
    w = solver->watch[i];
    if (w != NULL) {
      reset_watch(w);
    }
  }

  cidx = clause_pool_first_clause(&solver->pool);
  while (cidx < solver->pool.size) {
    assert(clause_is_live(&solver->pool, cidx));
    n = clause_length(&solver->pool, cidx);
    add_clause_all_watch(solver, n, clause_literals(&solver->pool, cidx), cidx);
    cidx += full_length(n);
  }
}


/*
 * Garbage collection
 */
static void pp_collect_garbage(sat_solver_t *solver) {
#if TRACE
  fprintf(stderr, "gc: pool size = %"PRIu32", literals = %"PRIu32", padding = %"PRIu32"\n",
          solver->pool.size, solver->pool.num_prob_literals, solver->pool.padding);
#endif
  check_clause_pool_counters(&solver->pool);
  reset_queue(&solver->cqueue);
  pp_compact_clause_pool(solver);
  pp_restore_watch_vectors(solver);
  check_clause_pool_counters(&solver->pool);
#if TRACE
  fprintf(stderr, "done: pool size = %"PRIu32", literals = %"PRIu32", padding = %"PRIu32"\n",
          solver->pool.size, solver->pool.num_prob_literals, solver->pool.padding);
#endif
}


/*
 * Heuristic for garbage collection:
 * - at least 10000 cells wasted in the clause database
 * - at least 12.5% of wasted cells
 */
static void pp_try_gc(sat_solver_t *solver) {
  if (solver->pool.padding > 10000 && solver->pool.padding > solver->pool.size >> 3) {
    pp_collect_garbage(solver);
  }
}


/*
 * REMOVE PURE AND UNIT LITERALS
 */

/*
 * Push pure or unit literal l into the queue
 * - l must not be assigned
 * - the function assigns l to true
 * - tag = either ATAG_UNIT or ATAG_PURE
 */
static void pp_push_literal(sat_solver_t *solver, literal_t l, antecedent_tag_t tag) {
  bvar_t v;

  assert(l < solver->nliterals);
  assert(lit_is_unassigned(solver, l));
  assert(solver->decision_level == 0);
  assert(tag == ATAG_UNIT || tag == ATAG_PURE);

  queue_push(&solver->lqueue, l);

  solver->value[l] = VAL_TRUE;
  solver->value[not(l)] = VAL_FALSE;

  v = var_of(not(l));
  solver->ante_tag[v] = tag;
  solver->ante_data[v] = 0;
  solver->level[v] = 0;

  if (solver->elim.data != NULL) {
    elim_heap_remove_var(solver, v);
  }
}

static inline void pp_push_pure_literal(sat_solver_t *solver, literal_t l) {
  pp_push_literal(solver, l, ATAG_PURE);
  solver->stats.pp_pure_lits ++;
}

static inline void pp_push_unit_literal(sat_solver_t *solver, literal_t l) {
  pp_push_literal(solver, l, ATAG_UNIT);
  solver->stats.pp_unit_lits ++;
}


/*
 * Decrement the occurrence counter of l.
 * - if occ[l] goes to zero, add not(l) to the queue as a pure literal (unless
 *   l is already assigned or eliminated).
 */
static void pp_decrement_occ(sat_solver_t *solver, literal_t l) {
  assert(solver->occ[l] > 0);
  solver->occ[l] --;
  if (solver->occ[l] == 0 && solver->occ[not(l)] > 0 && !lit_is_assigned(solver, l)) {
    pp_push_pure_literal(solver, not(l));
  }
}

/*
 * Decrement occ counts for all literals in a[0 ... n-1]
 */
static void pp_decrement_occ_counts(sat_solver_t *solver, literal_t *a, uint32_t n) {
  uint32_t i;

  if (solver->elim.data == NULL) {
    // no elimination heap: update only the occurrence counters
    for (i=0; i<n; i++) {
      pp_decrement_occ(solver, a[i]);
    }
  } else {
    // update the occurrence counters and the elimination heap
    for (i=0; i<n; i++) {
      pp_decrement_occ(solver, a[i]);
      elim_heap_update_var(solver, var_of(a[i]));
    }
  }
}

/*
 * Increment occ counts for all literals in a[0 ... n-1]
 */
static void pp_increment_occ_counts(sat_solver_t *solver, literal_t *a, uint32_t n) {
  uint32_t i;

  if (solver->elim.data == NULL) {
    // no elimination heap: update only the occurrence counters
    for (i=0; i<n; i++) {
      solver->occ[a[i]] ++;
    }
  } else {
    // update the occurrence counters and the elimination heap
    for (i=0; i<n; i++) {
      solver->occ[a[i]] ++;
      elim_heap_update_var(solver, var_of(a[i]));
    }
  }
}

/*
 * Delete clause cidx and update occ counts
 */
static void pp_remove_clause(sat_solver_t *solver, cidx_t cidx) {
  literal_t *a;
  uint32_t n;

  assert(clause_is_live(&solver->pool, cidx));

  n = clause_length(&solver->pool, cidx);
  a = clause_literals(&solver->pool, cidx);
  pp_decrement_occ_counts(solver, a, n);
  clause_pool_delete_clause(&solver->pool, cidx);
  solver->stats.pp_clauses_deleted ++;
}

/*
 * Visit clause at cidx and remove all assigned literals
 * - if the clause is true remove it
 * - otherwise remove all false literals from the clause
 * - if the result is empty, record this (solver->has_empty_clause := true)
 * - if the result is a unit clause, push the corresponding literal into the queue
 */
static void pp_visit_clause(sat_solver_t *solver, cidx_t cidx) {
  uint32_t i, j, n;
  literal_t *a;
  literal_t l;
  bool true_clause;

  assert(clause_is_live(&solver->pool, cidx));

  n = clause_length(&solver->pool, cidx);
  a = clause_literals(&solver->pool, cidx);
  true_clause = false;

  j = 0;
  for (i=0; i<n; i++) {
    l = a[i];
    switch (lit_value(solver, l)) {
    case VAL_TRUE:
      true_clause = true; // fall-through intended to keep the occ counts accurate
    case VAL_FALSE:
      assert(solver->occ[l] > 0);
      solver->occ[l] --;
      break;

    default:
      a[j] = l;
      j ++;
      break;
    }
  }

  if (true_clause) {
    pp_decrement_occ_counts(solver, a, j);
    clause_pool_delete_clause(&solver->pool, cidx);
    solver->stats.pp_clauses_deleted ++;
  } else if (j == 0) {
    add_empty_clause(solver);
    clause_pool_delete_clause(&solver->pool, cidx);
  } else if (j == 1) {
    pp_push_unit_literal(solver, a[0]);
    clause_pool_delete_clause(&solver->pool, cidx);
  } else {
    clause_pool_shrink_clause(&solver->pool, cidx, j);
    set_clause_signature(&solver->pool, cidx);
    clause_queue_push(solver, cidx);
  }
}



/*
 * Delete all the clauses that contain l (because l is true)
 */
static void pp_remove_true_clauses(sat_solver_t *solver, literal_t l) {
  watch_t *w;
  uint32_t i, n, k;

  assert(lit_is_true(solver, l));

  w = solver->watch[l];
  if (w != NULL) {
    n = w->size;
    for (i=0; i<n; i++) {
      k = w->data[i];
      if (clause_is_live(&solver->pool, k)) {
        pp_remove_clause(solver, k);
      }
    }
    // delete w
    safe_free(w);
    solver->watch[l] = NULL;
  }
}


/*
 * Visit all the clauses that contain l (because l is false)
 */
static void pp_visit_clauses_of_lit(sat_solver_t *solver, literal_t l) {
  watch_t *w;
  uint32_t i, n, k;

  assert(lit_is_false(solver, l));

  w = solver->watch[l];
  if (w != NULL) {
    n = w->size;
    for (i=0; i<n; i++) {
      k = w->data[i];
      if (clause_is_live(&solver->pool, k)) {
        pp_visit_clause(solver, k);
        if (solver->has_empty_clause) break;
      }
    }
    // delete w
    safe_free(w);
    solver->watch[l] = NULL;
  }
}



/*
 * Initialize the queue: store all unit and pure literals.
 */
static void collect_unit_and_pure_literals(sat_solver_t *solver) {
  uint32_t i, n;
  uint32_t pos_occ, neg_occ;

  assert(queue_is_empty(&solver->lqueue));

  n = solver->nvars;
  for (i=1; i<n; i++) {
    switch (var_value(solver, i)) {
    case VAL_TRUE:
      assert(solver->ante_tag[i] == ATAG_UNIT);
      queue_push(&solver->lqueue, pos_lit(i));
      solver->stats.pp_unit_lits ++;
      break;

    case VAL_FALSE:
      assert(solver->ante_tag[i] == ATAG_UNIT);
      queue_push(&solver->lqueue, neg_lit(i));
      solver->stats.pp_unit_lits ++;
      break;

    default:
      pos_occ = solver->occ[pos_lit(i)];
      neg_occ = solver->occ[neg_lit(i)];
      /*
       * if i doesn't occur at all then both pos_occ/neg_occ are zero.
       * we still record neg_lit(i) as a pure literal in this case to force
       * i to be assigned.
       */
      if (pos_occ == 0) {
        pp_push_pure_literal(solver, neg_lit(i));
      } else if (neg_occ == 0) {
        pp_push_pure_literal(solver, pos_lit(i));
      }
      break;
    }
  }
}


/*
 * Process the queue:
 * - return false if a conflict is detected
 * - return true otherwise
 */
static bool pp_empty_queue(sat_solver_t *solver) {
  literal_t l;

  while (! queue_is_empty(&solver->lqueue)) {
    l = queue_pop(&solver->lqueue);
    assert(lit_is_true(solver, l));
    assert(solver->ante_tag[var_of(l)] == ATAG_UNIT ||
           solver->ante_tag[var_of(l)] == ATAG_PURE);
    pp_remove_true_clauses(solver, l);
    if (solver->ante_tag[var_of(l)] == ATAG_UNIT) {
      pp_visit_clauses_of_lit(solver, not(l));
      if (solver->has_empty_clause) {
        reset_queue(&solver->lqueue);
        return false;
      }
    }
  }

  return true;
}


/*
 * VARIABLE SUBSTITUTION
 */

/*
 * Decrement occ counts of a[0 ... n-1]
 * This is like pp_decrement_occ_counts but doesn't try to detect
 * pure literals.
 */
static void pp_simple_decrement_occ_counts(sat_solver_t *solver, literal_t *a, uint32_t n) {
  uint32_t i;

  for (i=0; i<n; i++) {
    assert(solver->occ[a[i]] > 0);
    solver->occ[a[i]] --;
  }
}

/*
 * Apply the scc-based substitution to the clause that starts at cidx
 * - if the clause simplifies to a unit clause, add the literal to
 *   the unit-literal queue
 */
static void pp_apply_subst_to_clause(sat_solver_t *solver, cidx_t cidx) {
  literal_t *a;
  vector_t *b;
  uint32_t i, n;
  literal_t l;

  assert(clause_is_live(&solver->pool, cidx));

  n = clause_length(&solver->pool, cidx);
  a = clause_literals(&solver->pool, cidx);

  // apply substitution to all literals in a[0 ... n-1]
  // store the result in vector b
  b = &solver->buffer;
  reset_vector(b);

  for (i=0; i<n; i++) {
    l = lit_subst(solver, a[i]);
    assert(! lit_is_eliminated(solver, l));
    switch (solver->value[l] & 3) {
    case VAL_FALSE:
      break;

    case VAL_UNDEF_TRUE:
    case VAL_UNDEF_FALSE:
      vector_push(b, l);
      mark_false_lit(solver, l);
      break;

    case VAL_TRUE:
      goto done; // the clause is true
    }
  }

 done:
  clear_false_lits(solver, b->size, (literal_t *) b->data);

  /*
   * Decrement occ counts and delete the clause.
   * We don't want to use pp_remove_clauses because it has side effects
   * that are not correct here (i.e., finding pure literals).
   */
  pp_simple_decrement_occ_counts(solver, a, n);
  clause_pool_delete_clause(&solver->pool, cidx);

  /*
   * b = new clause after substitution.
   * if i < n, the clause is true.
   */
  if (i < n) {
    solver->stats.pp_clauses_deleted ++;
    return; // clause b is true. Nothing more to do
  }

  /*
   * Store b as a new problem clause
   */
  n = b->size;
  if (n == 1) {
    // unit clause
    pp_push_unit_literal(solver, b->data[0]);
  } else {
    // regular clause
    assert(n >= 2);

    uint_array_sort(b->data, n); // keep the clause sorted
    cidx = clause_pool_add_problem_clause(&solver->pool, n, (literal_t *) b->data);
    add_clause_all_watch(solver, n, (literal_t *) b->data, cidx);
    set_clause_signature(&solver->pool, cidx);
  }
  pp_increment_occ_counts(solver, (literal_t *) b->data, n);
}


/*
 * Apply the substitution to all the clauses in vector w
 */
static void pp_apply_subst_to_watch_vector(sat_solver_t *solver, watch_t *w) {
  uint32_t i, n, k;

  n = w->size;
  for (i=0; i<n; i++) {
    k = w->data[i];
    if (clause_is_live(&solver->pool, k)) {
      pp_apply_subst_to_clause(solver, k);
    }
  }
}

/*
 * Apply the substitution to all clauses that contain variable x
 * - then delete the watch vectors for x
 */
static void pp_apply_subst_to_variable(sat_solver_t *solver, bvar_t x) {
  watch_t *w;

  assert(solver->ante_tag[x] == ATAG_SUBST);

  w = solver->watch[pos_lit(x)];
  if (w != NULL) {
    pp_apply_subst_to_watch_vector(solver, w);
    safe_free(w);
    solver->watch[pos_lit(x)] = NULL;
  }

  w = solver->watch[neg_lit(x)];
  if (w != NULL) {
    pp_apply_subst_to_watch_vector(solver, w);
    safe_free(w);
    solver->watch[neg_lit(x)] = NULL;
  }

  //  pp_try_gc(solver);
}


/*
 * Compute the SCCs from the binary clauses
 * - return false if a conflict is detected
 * - return true otherwise
 */
static bool pp_scc_simplification(sat_solver_t *solver) {
  uint32_t subst_count;
  uint32_t i, n;

  subst_count = solver->stats.subst_vars;

  compute_sccs(solver);
  if (solver->has_empty_clause) {
    return false;
  }

  if (solver->stats.subst_vars > subst_count && solver->verbosity >= 3) {
    fprintf(stderr, "c scc found %"PRIu32" variable substitutions\n", solver->stats.subst_vars - subst_count);
  }

  n = solver->nvars;
  for (i=1; i<n; i++) {
    if (solver->ante_tag[i] == ATAG_SUBST) {
      // force a value for pos_lit(i) and neg_lit(i) so that they don't
      // get considered in other simplification procedures
      solver->value[pos_lit(i)] = VAL_TRUE;
      solver->value[neg_lit(i)] = VAL_FALSE;

      // save clause l := l0 to reconstruct the model: l0 = ante_data[i], l = pos_lit(i)
      clause_vector_save_subst_clause(&solver->saved_clauses, solver->ante_data[i], pos_lit(i));

      pp_apply_subst_to_variable(solver, i);
    }
  }

  return true;
}


/*
 * SUBSUMPTION/STRENGTHENING
 */

#ifndef NDEBUG
/*
 * In preprocessing, all clauses and watch vectors should be sorted
 */
static bool clause_is_sorted(const sat_solver_t *solver, cidx_t cidx) {
  uint32_t i, n;
  literal_t *a;

  n = clause_length(&solver->pool, cidx);
  a = clause_literals(&solver->pool, cidx);
  for (i=1; i<n; i++) {
    if (a[i-1] >= a[i]) {
      return false;
    }
  }
  return true;
}

static bool watch_vector_is_sorted(const watch_t *w) {
  uint32_t i, n;

  if (w != NULL) {
    n = w->size;
    for (i=1; i<n; i++) {
      if (w->data[i-1] >= w->data[i]) {
        return false;
      }
    }
  }

  return true;
}

#endif



/*
 * Search for variable x in array a[l, ..., m-1]
 * - a must be sorted in increasing order
 * - must have l <= m (also m <= MAX_CLAUSE_SIZE)
 * - returns m is there's no literal in a with variable x
 * - returns an index i such that a[i] is pos_lit(x) or neg_lit(x) otherwise
 */
static uint32_t pp_search_for_var(bvar_t x, uint32_t l, uint32_t m, const literal_t *a) {
  uint32_t i, h;
  bvar_t y;

  assert(l <= m);

  h = m;
  while (l < h) {
    i = (l + h) >> 1; // can't overflow since h <= MAX_CLAUSE_SIZE
    assert(l <= i && i < h);
    y = var_of(a[i]);
    if (x == y) return i;
    if (x < y) {
      h = i;
    } else {
      l = i+1;
    }
  }

  // not found
  return m;
}

/*
 * Remove the k-th literal from a[0... n-1]
 */
static void pp_remove_literal(uint32_t n, uint32_t k, literal_t *a) {
  assert(k < n);
  n --;
  while (k < n) {
    a[k] = a[k+1];
    k ++;
  }
}

/*
 * Remove clause cidx from watch[l]
 * - cidx must occur in the watch vector
 * - we mark cidx as a dead clause by replacing it with cidx + 2
 */
static void pp_remove_clause_from_watch(sat_solver_t *solver, literal_t l, cidx_t cidx) {
  watch_t *w;
  uint32_t i, j, n;

  w = solver->watch[l];
  assert(w != NULL && watch_vector_is_sorted(w));

  n = w->size;
  i = 0;
  assert(i < n);
  for (;;) {
    j = (i + n) >> 1;
    assert(i <= j && j < n);
    if (w->data[j] == cidx) break;
    if (w->data[j] < cidx) {
      i = j;
    } else {
      n = j;
    }
  }
  // replace cidx by cidx + 2 to keep the watch vector sorted and
  // make sure all elements are multiple of 2
  w->data[j] = cidx + 2;
}

/*
 * Check whether clause a[0 ... n-1] subsumes or strengthens clause cidx:
 * - subsumes means that all literals a[0] ... a[n-1] occur in clause cidx
 * - strengthens means that all literals a[0] .. a[n-1] but one occur
 *   in cidx and that (not a[i]) occurs in cidx.
 *
 * In the first case, we can remove clause cidx.
 *
 * In the second case, we can remove (not a[i]) from clause cidx. This is
 * subsumption/resolution:
 * - clause cidx is of the from (A, not a[i], B)
 * - clause a[0 ... n-1] is of the from (A, a[i])
 * - resolving these two clauses produces (A, B) which subsumes cidx
 *
 * - s is the signature of a[0 ... n-1]
 * - clause cidx may be marked.
 *
 * Return true if there's no conflict, false otherwise.
 */
static bool try_subsumption(sat_solver_t *solver, uint32_t n, const literal_t *a, uint32_t s, cidx_t cidx) {
  uint32_t i, j, k, m, q;
  literal_t *b;
  literal_t l;

  assert(clause_is_live(&solver->pool, cidx));
  assert(clause_is_sorted(solver, cidx));

  m = clause_length(&solver->pool, cidx);
  q = clause_signature(&solver->pool, cidx);
  b = clause_literals(&solver->pool, cidx);

  assert(m >= 2);

  if (m < n || ((~q & s) != 0)) return true;

  k = m;
  j = 0;

  /*
   * in this loop:
   * - k < m => b[k] = not(a[i0]) for some 0 <= i0 < i
   * - all literals in of a[0 ... i-1] occur in b,
   *   except possibly a[i0] which occurs negated.
   * - all elements of b[0 .. j-1] are < a[i]
   */
  for (i=0; i<n; i++) {
    // search for a[i] or not(a[i]) in array b[j ... m-1]
    j = pp_search_for_var(var_of(a[i]), j, m, b);
    if (j == m) return true;  // a[i] not in cidx
    assert(b[j] == a[i] || b[j] == not(a[i]));
    if (a[i] != b[j]) {
      if (k < m) return true;
      k = j;
    }
    j ++;
  }

  if (k < m) {
    // strengthening: remove literal b[k] form clause cidx
    l = b[k];
    pp_decrement_occ(solver, l);
    pp_remove_literal(m, k, b);
    pp_remove_clause_from_watch(solver, l, cidx);
    elim_heap_update_var(solver, var_of(l));
    m --;
    if (m == 1) {
      pp_push_unit_literal(solver, b[0]);
      clause_pool_delete_clause(&solver->pool, cidx);
      solver->stats.pp_unit_strengthenings ++;
    } else {
      clause_pool_shrink_clause(&solver->pool, cidx, m);
      set_clause_signature(&solver->pool, cidx);
      clause_queue_push(solver, cidx);
      solver->stats.pp_strengthenings ++;
    }
  } else {
    // subsumption: remove clause cidx
    pp_decrement_occ_counts(solver, b, m);
    clause_pool_delete_clause(&solver->pool, cidx);
    solver->stats.pp_subsumptions ++;
  }

  // deal with unit or pure literals
  return pp_empty_queue(solver);
}


/*
 * Variable in a[0 ... n-1] with smallest number of total occurrences
 */
static literal_t pp_key_literal(sat_solver_t *solver, const literal_t *a, uint32_t n) {
  literal_t k, l;
  uint32_t i, c;

  assert(n >= 2);

  k = a[0];
  c = solver->occ[k] + solver->occ[not(k)];

  for (i=1; i<n; i++) {
    l = a[i];
    if (solver->occ[l] + solver->occ[not(l)] < c) {
      c = solver->occ[l] + solver->occ[not(l)];
      k = l;
    }
  }

  return k;
}


#if TRACE
static uint32_t w_len(sat_solver_t *solver, literal_t l) {
  watch_t *w;
  uint32_t len;

  len = 0;
  w = solver->watch[l];
  if (w != NULL) len += w->size;
  w = solver->watch[not(l)];
  if (w != NULL) len += w->size;

  return len;
}
#endif

/*
 * Check backward subsumption from clause cidx:
 * - checks whether cidx subsumes or strengthen any clause of index >= start
 * - remove all such clauses subsumed by cidx
 * - add strengthened clauses to the clause queue.
 *
 * Return false if there's a conflict, true otherwise.
 */
static bool pp_clause_subsumption(sat_solver_t *solver, uint32_t cidx, uint32_t start) {
  literal_t *a;
  uint32_t i, n, m, k, s;
  literal_t key;
  watch_t *w;

  assert(clause_is_live(&solver->pool, cidx));
  assert(clause_is_sorted(solver, cidx));

  n = clause_length(&solver->pool, cidx);
  s = clause_signature(&solver->pool, cidx);
  a = clause_literals(&solver->pool, cidx);
  key = pp_key_literal(solver, a, n);

#if TRACE
  fprintf(stderr, "subsumption check: cidx = %"PRIu32", len = %"PRIu32", key = %"PRIu32", occs = %"PRIu32", watch size = %"PRIu32"\n",
          cidx, n, key, solver->occ[key] + solver->occ[not(key)], w_len(solver, key));
#endif

  w = solver->watch[key];
  if (w != NULL) {
    m = w->size;
    if (m < solver->params.subsume_skip) {
      for (i=0; i<m; i++) {
        k = w->data[i];
        if (k >= start && k != cidx && clause_is_live(&solver->pool, k)) {
          if (!try_subsumption(solver, n, a, s, k)) {
            return false;
          }
          if (!clause_is_live(&solver->pool, cidx)) {
            goto done;
          }
        }
      }
    }
  }

  w = solver->watch[not(key)];
  if (w != NULL) {
    m = w->size;
    if (m < solver->params.subsume_skip) {
      for (i=0; i<m; i++) {
        k = w->data[i];
        if (k >= start && clause_is_live(&solver->pool, k)) {
          assert(k != cidx);
          if (!try_subsumption(solver, n, a, s, k)) {
            return false;
          }
          if (!clause_is_live(&solver->pool, cidx)) {
            goto done;
          }
        }
      }
    }
  }

 done:
  return true;
}


/*
 * Collect and mark all variables in clause cidx
 * - the variables are added to solver->aux
 */
static void pp_collect_vars_of_clause(sat_solver_t *solver, cidx_t cidx) {
  literal_t *a;
  uint32_t i, n;
  bvar_t x;

  assert(clause_is_live(&solver->pool, cidx));

  n = clause_length(&solver->pool, cidx);
  a = clause_literals(&solver->pool, cidx);
  for (i=0; i<n; i++) {
    x = var_of(a[i]);
    if (! variable_is_marked(solver, x)) {
      mark_variable(solver, x);
      vector_push(&solver->aux, x);
    }
  }
}


/*
 * Collect clauses of index < s from w
 * - if a clause is marked we skip it
 * - otherwise we mark it and add it to cvector
 */
static void pp_collect_subsume_candidates_in_watch(sat_solver_t *solver, watch_t *w, uint32_t s) {
  uint32_t i, n, cidx;

  if (w != NULL) {
    n = w->size;
    for (i=0; i<n; i++) {
      cidx = w->data[i];
      if (cidx < s && clause_is_live(&solver->pool, cidx) && clause_is_unmarked(&solver->pool, cidx)) {
        mark_clause(&solver->pool, cidx);
        vector_push(&solver->cvector, cidx);
      }
    }
  }
}

/*
 * Collect clauses that may subsume a clause of index >= s
 * - solver->aux contains variables of clauses >= s
 * - all variables in solver->aux are marked.
 * - the relevant clauses are stored in solver->cvector
 * - all variable marks are cleared
 *
 * To avoid duplication, we mark clauses as we add them to cvector.
 * If a clause is already marked, it's in the clause queue so don't
 * need to add it to cvector.
 */
static void pp_collect_subsume_candidates(sat_solver_t *solver, uint32_t s) {
  vector_t *v;
  uint32_t i, n;
  bvar_t x;

  reset_vector(&solver->cvector);

  v = &solver->aux;
  n = v->size;
  for (i=0; i<n; i++) {
    x = v->data[i];
    assert(variable_is_marked(solver, x));
    unmark_variable(solver, x);
    pp_collect_subsume_candidates_in_watch(solver, solver->watch[pos_lit(x)], s);
    pp_collect_subsume_candidates_in_watch(solver, solver->watch[neg_lit(x)], s);
  }
  reset_vector(v); // cleanup

  // cleanup: remove the marks of all clauses in cvector
  v = &solver->cvector;
  n = v->size;
  for (i=0; i<n; i++) {
    assert(clause_is_marked(&solver->pool, v->data[i]));
    unmark_clause(&solver->pool, v->data[i]);
  }
}



/*
 * One round of subsumption starting from solver->scan_index
 *
 * The set of clauses is split in two:
 * - S1: clauses of index < scan_index
 * - S2: clauses of index >= scan_index
 * We know that the clauses in S1 don't subsume each other.
 *
 * We first scan clauses of S2 and we check whether they subsume or
 * strengthen anything. Then we compute the set of variables that
 * occur in clauses of S2 and we construct the set of clauses from S1
 * that contain any such variable. We check for subsumption from theses
 * clauses. Finally, we process the queue of clauses.
 */
static bool pp_subsumption(sat_solver_t *solver) {
  uint32_t i, n, s;
  cidx_t cidx;

  // save the scan index in s
  s = solver->scan_index;

  // First pass: scan clauses of S2
  for (;;) {
    cidx = clause_scan_next(solver);
    if (cidx >= solver->pool.size) break;
    if (clause_is_live(&solver->pool, cidx) &&
        !pp_clause_subsumption(solver, cidx, 0)) {
      return false;
    }
  }

  if (s > 0) {
    // collect variables of S2 into solver->aux
    reset_vector(&solver->aux);
    cidx = next_clause_index(&solver->pool, s);
    while (cidx < solver->pool.size) {
      if (clause_is_live(&solver->pool, cidx)) {
        pp_collect_vars_of_clause(solver, cidx);
      }
      cidx = clause_pool_next_clause(&solver->pool, cidx);
    }

    // clauses of S1 that may subsume/strengthen a clause of S2
    pp_collect_subsume_candidates(solver, s);
    n = solver->cvector.size;
    for (i=0; i<n; i++) {
      cidx = solver->cvector.data[i];
      // cidx was live when it was added but it can
      // be deleted within this loop in pp_empty_queue
      if (clause_is_live(&solver->pool, cidx) &&
          !pp_clause_subsumption(solver, cidx, s)) {
        return false;
      }
    }
  }


  // Final step: empty the queue
  for (;;) {
    cidx = clause_queue_pop(solver);
    if (cidx >= solver->pool.size) break;
    assert(clause_is_live(&solver->pool, cidx));
    if (!pp_clause_subsumption(solver, cidx, 0)) {
      return false;
    }
  }

  return true;
}

/*
 * RESOLUTION/VARIABLE ELIMINATION
 */

/*
 * Total size of all live clauses in vector w
 */
static uint32_t live_clauses_size(const clause_pool_t *pool, const watch_t *w) {
  uint32_t s, i, n, cidx;

  assert(w != NULL);

  s = 0;
  n = w->size;
  for (i=0; i<n; i++) {
    cidx = w->data[i];
    if (clause_is_live(pool, cidx)) {
      s += clause_length(pool, cidx);
    }
  }

  return s;
}

/*
 * Save clause of given idx
 */
static void pp_save_clause(sat_solver_t *solver, uint32_t cidx, literal_t l) {
  assert(clause_is_live(&solver->pool, cidx));
  clause_vector_save_clause(&solver->saved_clauses, clause_length(&solver->pool, cidx),
                            clause_literals(&solver->pool, cidx), l);

}


/*
 * Save half the clauses that contain x so that we can later extend the truth-assignment to x.
 */
static void pp_save_elim_clauses_for_var(sat_solver_t *solver, bvar_t x) {
  watch_t *w;
  literal_t l;
  uint32_t s, n, i, cidx;

  l = pos_lit(x);
  w = solver->watch[pos_lit(x)];
  s = live_clauses_size(&solver->pool, solver->watch[pos_lit(x)]);

  n = live_clauses_size(&solver->pool, solver->watch[neg_lit(x)]);
  if (n < s) {
    l = neg_lit(x);
    w = solver->watch[neg_lit(x)];
    s = n;
  }

  resize_clause_vector(&solver->saved_clauses, s);
  n = w->size;
  for (i=0; i<n; i++) {
    cidx = w->data[i];
    if (clause_is_live(&solver->pool, cidx)) {
      pp_save_clause(solver, cidx, l);
    }
  }
  clause_vector_add_block_length(&solver->saved_clauses, s);
}


/*
 * Check whether the resolvent of clauses c1 and c2 is not trivial
 * - l = pivot literal
 * - both clauses must be sorted
 * - c1 must contain l and c2 must contain (not l)
 * - return true if the resolvent is not trivial, and store its length in *length
 */
static bool non_trivial_resolvent(const sat_solver_t *solver, uint32_t c1, uint32_t c2, literal_t l, uint32_t *length) {
  literal_t *a1, *a2;
  uint32_t i1, i2, n1, n2, len;

  assert(clause_is_live(&solver->pool, c1) && clause_is_sorted(solver, c1));
  assert(clause_is_live(&solver->pool, c2) && clause_is_sorted(solver, c2));

  n1 = clause_length(&solver->pool, c1);
  a1 = clause_literals(&solver->pool, c1);
  n2 = clause_length(&solver->pool, c2);
  a2 = clause_literals(&solver->pool, c2);

  len = n1 + n2;
  i1 = 0;
  i2 = 0;
  do {
    if (var_of(a1[i1]) < var_of(a2[i2])) {
      i1 ++;
    } else if (var_of(a1[i1]) > var_of(a2[i2])) {
      i2 ++;
    } else if (a1[i1] != a2[i2] && a1[i1] != l) {
      assert(a1[i1] == not(a2[i2])); // trivial resolvent
      return false;
    } else {
      i1 ++;
      i2 ++;
      len --;
    }
  } while (i1 < n1 && i2 < n2);

  *length = len;

  return true;
}

/*
 * Construct the resolvent of clauses c1 and c2
 * - l = literal
 * - both clauses must be sorted
 * - c1 must contain l and c2 must contain (not l)
 * - store it in solver->buffer
 * - return true if the resolvent is not trivial/false if it is
 */
static bool pp_build_resolvent(sat_solver_t *solver, uint32_t c1, uint32_t c2, literal_t l) {
  literal_t *a1, *a2;
  uint32_t i1, i2, n1, n2;

  assert(clause_is_live(&solver->pool, c1) && clause_is_sorted(solver, c1));
  assert(clause_is_live(&solver->pool, c2) && clause_is_sorted(solver, c2));

  reset_vector(&solver->buffer);
  n1 = clause_length(&solver->pool, c1);
  a1 = clause_literals(&solver->pool, c1);
  n2 = clause_length(&solver->pool, c2);
  a2 = clause_literals(&solver->pool, c2);

  i1 = 0;
  i2 = 0;
  do {
    if (var_of(a1[i1]) < var_of(a2[i2])) {
      vector_push(&solver->buffer, a1[i1]);
      i1 ++;
    } else if (var_of(a1[i1]) > var_of(a2[i2])) {
      vector_push(&solver->buffer, a2[i2]);
      i2 ++;
    } else if (a1[i1] == a2[i2]) {
      vector_push(&solver->buffer, a1[i1]);
      i1 ++;
      i2 ++;
    } else {
      assert(a1[i1] == not(a2[i2]));
      if (a1[i1] != l) return false;
      i1 ++;
      i2 ++;
    }
  } while (i1 < n1 && i2 < n2);

  while (i1 < n1) {
    vector_push(&solver->buffer, a1[i1]);
    i1 ++;
  }
  while (i2 < n2) {
    vector_push(&solver->buffer, a2[i2]);
    i2 ++;
  }
  return true;
}


/*
 * Add l as a new clause (unit resolvent)
 * - do nothing if l is already true
 * - add the empty clause if l is already false
 */
static void pp_add_unit_resolvent(sat_solver_t *solver, literal_t l) {
  switch (lit_value(solver, l)) {
  case VAL_TRUE:
    break;

  case VAL_FALSE:
    add_empty_clause(solver);
    break;

  default:
    pp_push_unit_literal(solver, l);
    break;
  }
}

/*
 * Construct the resolvent of c1 and c2 and add it if it's not trivial.
 * - if the resolvent is a unit clause, add its literal to the unit queue
 * - return false if there's a conflict, true otherwise.
 */
static void pp_add_resolvent(sat_solver_t *solver, uint32_t c1, uint32_t c2, literal_t l) {
  vector_t *b;
  uint32_t n, cidx;

  if (pp_build_resolvent(solver, c1, c2, l)) {
    b = &solver->buffer;
    n = b->size;
    assert(n > 0);
    if (n == 1) {
      pp_add_unit_resolvent(solver, b->data[0]);
    } else {
      cidx = clause_pool_add_problem_clause(&solver->pool, n, (literal_t *) b->data);
      add_clause_all_watch(solver, n, (literal_t *) b->data, cidx);
      set_clause_signature(&solver->pool, cidx);
    }
    pp_increment_occ_counts(solver, (literal_t *) b->data, n);
  }
}


/*
 * Mark x as an eliminated variable:
 * - we also give it a value to make sure pos_lit(x) and neg_lit(x) don't get
 *   added to the queue of pure_literals.
 */
static void pp_mark_eliminated_variable(sat_solver_t *solver, bvar_t x) {
  assert(var_is_unassigned(solver, x));
  assert(solver->decision_level == 0);

  solver->value[pos_lit(x)] = VAL_TRUE;
  solver->value[neg_lit(x)] = VAL_FALSE;
  solver->ante_tag[x] = ATAG_ELIM;
  solver->ante_data[x] = 0;
  solver->level[x] = 0;
}

/*
 * Eliminate variable x:
 * - get all the clauses that contain pos_lit(x) and neg_lit(x) and construct
 *   their resolvents
 * - any pure or unit literals created as a result are added to solver->lqueue
 * - may also set solver->has_empty_clause to true
 */
static void pp_eliminate_variable(sat_solver_t *solver, bvar_t x) {
  watch_t *w1, *w2;
  uint32_t i1, i2, n1, n2;
  cidx_t c1, c2;

  assert(x < solver->nvars);

  w1 = solver->watch[pos_lit(x)];
  w2 = solver->watch[neg_lit(x)];

  if (w1 == NULL || w2 == NULL) return;

  n1 = w1->size;
  n2 = w2->size;
  for (i1=0; i1<n1; i1++) {
    c1 = w1->data[i1];
    assert(idx_is_clause(c1));
    if (clause_is_live(&solver->pool, c1)) {
      for (i2=0; i2<n2; i2++) {
        c2 = w2->data[i2];
        assert(idx_is_clause(c2));
        if (clause_is_live(&solver->pool, c2)) {
          pp_add_resolvent(solver, c1, c2, pos_lit(x));
          if (solver->has_empty_clause) return;
        }
      }
    }
  }
  // save enough clauses to extend the model to x
  pp_save_elim_clauses_for_var(solver, x);

  /*
   * We must mark x as an eliminated variable before deleting the clauses
   * that contain x.
   */
  pp_mark_eliminated_variable(solver, x);

  // Delete the clauses that contain x
  for (i1=0; i1<n1; i1++) {
    c1 = w1->data[i1];
    assert(idx_is_clause(c1));
    if (clause_is_live(&solver->pool, c1)) {
      pp_remove_clause(solver, c1);
    }
  }
  for (i2=0; i2<n2; i2++) {
    c2 = w2->data[i2];
    assert(idx_is_clause(c2));
    if (clause_is_live(&solver->pool, c2)) {
      pp_remove_clause(solver, c2);
    }
  }
  safe_free(w1);
  safe_free(w2);
  solver->watch[pos_lit(x)] = NULL;
  solver->watch[neg_lit(x)] = NULL;

  pp_try_gc(solver);
}




/*
 * Check whether eliminating variable x creates too many clauses.
 * - return true if the number of non-trivial resolvent is more than
 *   the number of clauses that contain x
 */
static bool pp_variable_worth_eliminating(const sat_solver_t *solver, bvar_t x) {
  watch_t *w1, *w2;
  uint32_t i1, i2, n1, n2;
  cidx_t c1, c2;
  uint32_t n, new_n, len;

  assert(x < solver->nvars);

  w1 = solver->watch[pos_lit(x)];
  w2 = solver->watch[neg_lit(x)];

  if (w1 == NULL || w2 == NULL) return true;

  n1 = w1->size;
  n2 = w2->size;
  if (n1 >= 10 && n2 >= 10) return false;

  // number of clauses that contain x
  n = solver->occ[pos_lit(x)] + solver->occ[neg_lit(x)];
  new_n = 0;
  len = 0; // Prevents a GCC warning

  for (i1=0; i1<n1; i1++) {
    c1 = w1->data[i1];
    assert(idx_is_clause(c1));
    if (clause_is_live(&solver->pool, c1)) {
      for (i2=0; i2<n2; i2++) {
        c2 = w2->data[i2];
        assert(idx_is_clause(c2));
        if (clause_is_live(&solver->pool, c2)) {
          new_n += non_trivial_resolvent(solver, c1, c2, pos_lit(x), &len);
          if (new_n > n || len > solver->params.res_clause_limit) return false;
        }
      }
    }
  }
  assert(new_n <= n);

  return true;
}


/*
 * Add variables to the elimination heap.
 */
static void collect_elimination_candidates(sat_solver_t *solver) {
  uint32_t i, n;

  n = solver->nvars;
  for (i=1; i<n; i++) {
    if (var_is_active(solver, i) && pp_elim_candidate(solver, i)) {
      assert(!var_is_in_elim_heap(solver, i));
      elim_heap_insert_var(solver, i);
    }
  }
}


/*
 * Eliminate variables: iterate over all variables in the elimination
 * heap.
 */
static void process_elimination_candidates(sat_solver_t *solver) {
  uint32_t pp, nn;
  bvar_t x;
  bool cheap;

  while (! elim_heap_is_empty(solver)) {
    x = elim_heap_get_top(solver);

    if (var_is_assigned(solver, x)) {
      assert(solver->ante_tag[x] == ATAG_PURE ||
             solver->ante_tag[x] == ATAG_UNIT ||
             solver->ante_tag[x] == ATAG_ELIM ||
	     solver->ante_tag[x] == ATAG_SUBST);
      continue;
    }
    assert(!var_is_eliminated(solver, x));

    pp = solver->occ[pos_lit(x)];
    nn = solver->occ[neg_lit(x)];
    if (pp == 0 || nn == 0) {
      continue;
    }
    if (pp_variable_worth_eliminating(solver, x)) {
      pp_eliminate_variable(solver, x);
      cheap = (pp == 1 || nn == 1 || (pp == 2 && nn == 2));
      solver->stats.pp_cheap_elims += cheap;
      solver->stats.pp_var_elims += (1 - cheap);
      // check for conflicts + process unit/pure literals
      if (solver->has_empty_clause || !pp_empty_queue(solver)) return;
    }
  }
}


/*
 * END OF PREPROCESSING
 */

/*
 * Cleanup all the watch vectors
 */
static void pp_reset_watch_vectors(sat_solver_t *solver) {
  uint32_t i, n;
  watch_t *w;

  n = solver->nliterals;
  for (i=2; i<n; i++) {
    w = solver->watch[i];
    if (w != NULL) {
      w->size = 0;
    }
  }
}

#ifndef NDEBUG
/*
 * Check that clause at index cidx has no assigned literals.
 */
static bool clause_is_clean(const sat_solver_t *solver, cidx_t cidx) {
  uint32_t i, n;
  literal_t *a;

  n = clause_length(&solver->pool, cidx);
  a = clause_literals(&solver->pool, cidx);
  for (i=0; i<n; i++) {
    if (lit_is_assigned(solver, a[i])) {
      return false;
    }
  }
  return true;
}
#endif

/*
 * Scan all live clauses in the pool
 * - remove binary clauses from the pool and move them to the watch vectors
 * - also compact the pool
 */
static void pp_rebuild_watch_vectors(sat_solver_t *solver) {
  clause_pool_t *pool;
  uint32_t n;
  cidx_t i, j;
  literal_t l1, l2;

  pool = &solver->pool;

  assert(clause_pool_invariant(pool));
  assert(pool->learned == pool->size &&
         pool->num_learned_clauses == 0 &&
         pool->num_learned_literals == 0);

  pool->num_prob_clauses = 0;
  pool->num_prob_literals = 0;

  i = 0;
  j = 0;
  while (i < pool->size) {
    n = pool->data[i];
    if (n == 0) {
      // padding block: skip it
      i += padding_length(pool, i);
    } else {
      assert(n >= 2 && (n & CLAUSE_MARK) == 0);
      assert(clause_is_clean(solver, i));
      l1 = first_literal_of_clause(pool, i);
      l2 = second_literal_of_clause(pool, i);
      if (n == 2) {
        // binary clause
        add_binary_clause(solver, l1, l2);
        i += full_length(2);
      } else {
        // regular clause at index j
        if (j < i) {
          clause_pool_move_clause(pool, j, i, n);
        }
        pool->num_prob_clauses ++;
        pool->num_prob_literals += n;
        add_clause_watch(solver, l1, j, l2);
        add_clause_watch(solver, l2, j, l1);
        i += full_length(n);
        j += full_length(n);
      }
    }
  }
  pool->learned = j;
  pool->size = j;
  pool->available = pool->capacity - j;
  pool->padding = 0;

  assert(clause_pool_invariant(pool));
}

/*
 * Shrink watch vectors that are less than 25% full
 */
static void shrink_watch_vectors(sat_solver_t *solver) {
  uint32_t i, n;
  watch_t *w;

  n = solver->nliterals;
  for (i=2; i<n; i++) {
    w = solver->watch[i];
    if (false && w != NULL && w->capacity >= 100 && w->size < (w->capacity >> 2)) {
      solver->watch[i] = shrink_watch(w);
    }
  }
}


static void prepare_for_search(sat_solver_t *solver) {
  check_clause_pool_counters(&solver->pool);      // DEBUG
  solver->units = 0;
  solver->binaries = 0;
  reset_stack(&solver->stack);
  pp_reset_watch_vectors(solver);
  pp_rebuild_watch_vectors(solver);
  shrink_watch_vectors(solver);
  check_clause_pool_counters(&solver->pool);      // DEBUG
  check_watch_vectors(solver);                    // DEBUG
}



/*
 * PREPROCESSING
 */

/*
 * On entry to preprocess:
 * - watch[l] contains all the clauses in which l occurs
 * - occ[l] = number of occurrences of l
 * Unit clauses are stored implicitly in the propagation queue.
 * Binary clauses are stored in the pool.
 *
 * On exit:
 * - either solver->has_empty_clause is true or the clauses and watch
 *   vectors are ready for search: binary clauses are stored directly
 *   in the watch vectors; other clauses have two watch literals.
 */
static void nsat_preprocess(sat_solver_t *solver) {
  if (solver->verbosity >= 4) fprintf(stderr, "c Eliminate pure and unit literals\n");

  collect_unit_and_pure_literals(solver);
  do {
    if (! pp_empty_queue(solver)) goto done;
    pp_try_gc(solver);
    if (! pp_scc_simplification(solver)) goto done;
  } while (! queue_is_empty(&solver->lqueue));

  prepare_elim_heap(&solver->elim, solver->nvars);
  collect_elimination_candidates(solver);
  assert(solver->scan_index == 0);
  do {
    if (solver->verbosity >= 4) fprintf(stderr, "c Elimination\n");
    process_elimination_candidates(solver);
    if (solver->verbosity >= 4) fprintf(stderr, "c Subsumption\n");
    if (solver->has_empty_clause || !pp_subsumption(solver)) break;
  } while (!elim_heap_is_empty(solver));

  do {
    if (! pp_empty_queue(solver)) goto done;
    pp_try_gc(solver);
    if (! pp_scc_simplification(solver)) goto done;
  } while (! queue_is_empty(&solver->lqueue));

 done:
  solver->stats.pp_subst_vars = solver->stats.subst_vars;

  if (solver->verbosity >= 4) fprintf(stderr, "c Done\nc\n");

  reset_clause_queue(solver);
  reset_elim_heap(&solver->elim);
  if (!solver->has_empty_clause) {
    prepare_for_search(solver);
  }
}



/**************************
 *  BOOLEAN PROPAGATION   *
 *************************/

/*
 * Conflict: binary clause {l0, l1} is false
 */
static void record_binary_conflict(sat_solver_t *solver, literal_t l0, literal_t l1) {
  assert(lit_is_false(solver, l0) && lit_is_false(solver, l1));

#if TRACE
  printf("\n---> DPLL:   Binary conflict: %"PRIu32" %"PRIu32"\n", l0, l1);
  fflush(stdout);
#endif

  solver->conflict_tag = CTAG_BINARY;
  solver->conflict_buffer[0] = l0;
  solver->conflict_buffer[1] = l1;
  solver->stats.conflicts ++;
}

/*
 * For debugging: check that clause cidx is false
 */
#ifndef NDEBUG
static bool clause_is_false(const sat_solver_t *solver, cidx_t cidx) {
  literal_t *l;
  uint32_t i, n;

  assert(good_clause_idx(&solver->pool, cidx));
  n = clause_length(&solver->pool, cidx);
  l = clause_literals(&solver->pool, cidx);
  for (i=0; i<n; i++) {
    if (!lit_is_false(solver, l[i])) {
      return false;
    }
  }
  return true;
}
#endif

/*
 * Conflict: clause cidx is false
 */
static void record_clause_conflict(sat_solver_t *solver, cidx_t cidx) {
  assert(clause_is_false(solver, cidx));

#if TRACE
  printf("\n---> DPLL:   Clause conflict: cidx = %"PRIu32"\n");
  fflush(stdout);
#endif

  solver->conflict_tag = CTAG_CLAUSE;
  solver->conflict_index = cidx;
  solver->stats.conflicts ++;
}


/*
 * Propagation from literal l0
 * - l0 must be false in the current assignment
 * - sets solver->conflict_tag if there's a conflict
 */
static void propagate_from_literal(sat_solver_t *solver, literal_t l0) {
  watch_t *w;
  literal_t *lit;
  uint32_t i, j, n, k, len, t;
  literal_t l, l1;
  bval_t vl;

  assert(lit_is_false(solver, l0));

  w = solver->watch[l0];
  if (w == NULL || w->size == 0) return; // nothing to do

  n = w->size;
  j = 0;
  i = 0;
  while (i < n) {
    k = w->data[i];
    w->data[j] = k; // Keep k in w. We'll undo this later if needed.
    i ++;
    j ++;
    if (idx_is_literal(k)) {
      /*
       * Binary clause
       */
      l = idx2lit(k);
      vl = lit_value(solver, l);
      if (vl == VAL_TRUE) continue;
      if (vl == VAL_FALSE) {
        record_binary_conflict(solver, l0, l);
        goto conflict;
      }
      assert(bval_is_undef(vl));
      binary_clause_propagation(solver, l, l0);
      continue;

    } else {
      /*
       * Clause in the pool
       */
      // get the blocker
      l = w->data[i];
      w->data[j] = l;
      i ++;
      j ++;
      if (lit_is_true(solver, l)) {
        continue;
      }

      // read len directly (the clause should not be marked)
      len = solver->pool.data[k];
      assert(len == clause_length(&solver->pool, k));

      lit = clause_literals(&solver->pool, k);
      assert(lit[0] == l0 || lit[1] == l0);
      // Get the other watched literal in clause k
      l = lit[0] ^ lit[1] ^ l0;
      // If l is true, nothing to do
      vl = lit_value(solver, l);
      if (vl == VAL_TRUE) {
        w->data[j-1] = l; // change blocker
        continue;
      }

      // Force l to go into lit[0] and l0 into lit[1]
      lit[0] = l;
      lit[1]  = l0;

      // Search for an unassigned or true literal in lit[2 ... len-1]
      for (t=2; t<len; t++) {
        if (! lit_is_false(solver, lit[t])) {
          // lit[t] is either true or not assigned.
          // It can replace l0 as watched literal
          l1 = lit[t];
          lit[1] = l1;
          lit[t] = l0;
          add_clause_watch(solver, l1, k, l);
          j -= 2; // remove [k, blocker] from l0's watch vector
          goto done;
        }
      }

      // All literals in lit[1 ... len-1] are false
      assert(t == len);
      if (vl == VAL_FALSE) {
        record_clause_conflict(solver, k);
        goto conflict;
      }
      assert(bval_is_undef(vl));
      clause_propagation(solver, l, k);
    done:
      continue;
    }
  }
  w->size = j;
  return;

 conflict:
  while (i<n) {
    w->data[j] = w->data[i];
    j ++;
    i ++;
  }
  w->size = j;
}


/*
 * Boolean propagation
 * - on entry, solver->conflict_tag must be CTAG_NONE
 * - on exit, it's set to CTAG_BINARY or CTAG_CLAUSE if there's a conflict
 */
static void nsat_boolean_propagation(sat_solver_t *solver) {
  literal_t l;
  uint32_t i;

  assert(solver->conflict_tag == CTAG_NONE);

  for (i = solver->stack.prop_ptr; i< solver->stack.top; i++) {
    l = not(solver->stack.lit[i]);
    propagate_from_literal(solver, l);
    if (solver->conflict_tag != CTAG_NONE) {
      return;
    }
  }
  solver->stack.prop_ptr = i;

  check_propagation(solver);
}


/*
 * Level-0 propagation: boolean propagation + set status to UNSAT
 * and add the empty clause if a conflict is detected.
 */
static void level0_propagation(sat_solver_t *solver) {
  assert(solver->decision_level == 0);
  nsat_boolean_propagation(solver);
  if (solver->conflict_tag != CTAG_NONE) {
    add_empty_clause(solver);
  }
}


/******************
 *  BACKTRACKING  *
 *****************/

/*
 * Backtrack to back_level
 * - undo all assignments at levels >= back_level + 1
 * - solver->decision_level must be larger than back_level
 *   (otherwise level_index[back_level + 1] may not be set properly).
 */
static void backtrack(sat_solver_t *solver, uint32_t back_level) {
  uint32_t i, d;
  literal_t l;
  bvar_t x;

  assert(back_level < solver->decision_level);

  d = solver->stack.level_index[back_level + 1];
  i = solver->stack.top;
  while (i > d) {
    i --;
    l = solver->stack.lit[i];
    x = var_of(l);
    assert(lit_is_true(solver, l) && solver->level[x] > back_level);
    solver->value[pos_lit(x)] ^= (uint8_t) 0x2;   // clear assign bit
    solver->value[neg_lit(x)] ^= (uint8_t) 0x2;   // clear assign bit
    assert(var_is_unassigned(solver, x));
    heap_insert(&solver->heap, x);
  }
  solver->stack.top = i;
  solver->stack.prop_ptr = i;

  // same thing for the clause stack
  solver->stash.top = solver->stash.level[back_level + 1];

  solver->decision_level = back_level;
}



/*
 * Check whether all variables assigned at level k have activity less than ax
 */
static bool level_has_lower_activity(sat_solver_t *solver, double ax, uint32_t k) {
  sol_stack_t *stack;
  uint32_t i, n;
  bvar_t x;

  assert(k <= solver->decision_level);
  stack = &solver->stack;

  // i := start of level k
  // n := end of level k
  i = stack->level_index[k];
  n = stack->top;
  if (k < solver->decision_level) {
    n = stack->level_index[k+1];
  }

  while (i < n) {
    x = var_of(stack->lit[i]);
    assert(var_is_assigned(solver, x) && solver->level[x] == k);
    if (solver->heap.activity[x] >= ax) {
      return false;
    }
    i ++;
  }

  return true;
}

/*
 * Partial restart:
 * - find the unassigned variable of highest activity
 * - keep all the decision levels that have at least one variable
 *   with activity higher than that.
 * - do nothing if the decision_level is 0
 */
static void partial_restart(sat_solver_t *solver) {
  double ax;
  bvar_t x;
  uint32_t i, n;

  solver->stats.starts ++;
  if (solver->decision_level > 0) {
    cleanup_heap(solver);

    if (heap_is_empty(&solver->heap)) {
      // full restart
      backtrack(solver, 0);
    } else {
      x = solver->heap.heap[1];
      assert(var_is_unassigned(solver, x));
      ax = solver->heap.activity[x];

      n = solver->decision_level;
      for (i=1; i<=n; i++) {
	if (level_has_lower_activity(solver, ax, i)) {
	  backtrack(solver, i-1);
	  break;
	}
      }
    }
  }
}

/*
 * Full restart: backtrack to level 0
 */
static void full_restart(sat_solver_t *solver) {
  solver->stats.starts ++;
  if (solver->decision_level > 0) {
    backtrack(solver, 0);
  }
}


/*******************************************************
 *  CONFLICT ANALYSIS AND CREATION OF LEARNED CLAUSES  *
 ******************************************************/

/*
 * During conflict resolution, we build a clause in solver->buffer.
 * Except at the very end, all literals in this buffer have decision
 * level < conflict level. To prevent duplicates, we mark all of them.
 *
 * In addition, we also mark the literals that must be resolved.
 * These literals have decision level equal to the conflict level.
 */

/*
 * Process literal l during conflict resolution.
 * - l is either a part of the learned clause or a literal to resolve
 * - if l is marked do nothing (already seen)
 * - if l has decision level 0, ignore it
 * - otherwise:
 *     mark l
 *     increase variable activity
 *     if l's decision_level < conflict level then add l to the buffer
 *
 * - return 1 if l is to be resolved
 * - return 0 otherwise
 */
static uint32_t process_literal(sat_solver_t *solver, literal_t l) {
  bvar_t x;

  x = var_of(l);

  assert(solver->level[x] <= solver->decision_level);
  assert(lit_is_false(solver, l));

  if (! variable_is_marked(solver, x) && solver->level[x] > 0) {
    mark_variable(solver, x);
    increase_var_activity(&solver->heap, x);
#if 0
    if (!solver->diving) {
      // in diving mode, we don't touch activities.
      increase_var_activity(&solver->heap, x);
    }
#endif
    increase_var_activity(&solver->heap, x);
    if (solver->level[x] == solver->decision_level) {
      return 1;
    }
    vector_push(&solver->buffer, l);
  }

  return 0;
}

/*
 * Process clause cidx:
 * - process literals starting from i0
 * - i0 is either 0 or 1
 * - increase the clause activity if it's a learned clause
 * - return the number of literals to resolved
 */
static uint32_t process_clause(sat_solver_t *solver, cidx_t cidx, uint32_t i0) {
  literal_t *lit;
  uint32_t i, n, toresolve;

  assert(i0 <= 1);

  if (is_learned_clause_idx(&solver->pool, cidx)) {
    increase_clause_activity(solver, cidx);
  }

  toresolve = 0;
  n = clause_length(&solver->pool, cidx);
  lit = clause_literals(&solver->pool, cidx);
  for (i=i0; i<n; i++) {
    toresolve += process_literal(solver, lit[i]);
  }
  return toresolve;
}

/*
 * Stacked clause cidx
 * - process literals at indexes 1 to n
 * - the first literal is the implied literal
 * - return the number of literals to resolve.
 */
static uint32_t process_stacked_clause(sat_solver_t *solver, cidx_t cidx) {
  literal_t *lit;
  uint32_t i, n, toresolve;

  toresolve = 0;
  n = stacked_clause_length(&solver->stash, cidx);
  lit = stacked_clause_literals(&solver->stash, cidx);
  assert(n >= 2);
  for (i=1; i<n; i++) {
    toresolve += process_literal(solver, lit[i]);
  }
  return toresolve;
}


/*
 * Build learned clause and find UIP
 *
 * Result:
 * - the learned clause is stored in solver->buffer
 * - the implied literal is in solver->buffer.data[0]
 * - all literals in the learned clause are marked
 */
static void analyze_conflict(sat_solver_t *solver) {
  literal_t *stack;
  literal_t b;
  bvar_t x;
  uint32_t j, unresolved;

  assert(solver->decision_level > 0);

  unresolved = 0;
  vector_reset_and_reserve(&solver->buffer); // make room for one literal

  /*
   * Scan the conflict clause
   */
  if (solver->conflict_tag == CTAG_BINARY) {
    unresolved += process_literal(solver, solver->conflict_buffer[0]);
    unresolved += process_literal(solver, solver->conflict_buffer[1]);
  } else {
    assert(solver->conflict_tag == CTAG_CLAUSE);
    unresolved += process_clause(solver, solver->conflict_index, 0);
  }

  /*
   * Scan the assignment stack from top to bottom and process
   * the antecedent of all literals to resolve.
   */
  stack = solver->stack.lit;
  j = solver->stack.top;
  for (;;) {
    j --;
    b = stack[j];
    assert(d_level(solver, b) == solver->decision_level);
    if (literal_is_marked(solver, b)) {
      if (unresolved == 1) {
        // found UIP
        solver->buffer.data[0] = not(b);
        break;
      } else {
        unresolved --;
        x = var_of(b);
        unmark_variable(solver, x);
        switch (solver->ante_tag[x]) {
        case ATAG_BINARY:
          // solver->ante_data[x] = antecedent literal
          unresolved += process_literal(solver, solver->ante_data[x]);
          break;

        case ATAG_CLAUSE:
          assert(first_literal_of_clause(&solver->pool, solver->ante_data[x]) == b);
          // solver->ante_data[x] = antecedent clause
          unresolved += process_clause(solver, solver->ante_data[x], 1);
          break;

        default:
          assert(solver->ante_tag[x] == ATAG_STACKED);
          assert(first_literal_of_stacked_clause(&solver->stash, solver->ante_data[x]) == b);
          // solver->ante_data[x] = antecedent stacked clause
          unresolved += process_stacked_clause(solver, solver->ante_data[x]);
          break;
        }
      }
    }
  }

  check_marks(solver);
}


/*
 * CLAUSE SIMPLIFICATION
 */

/*
 * Check whether literal l is redundant (can be removed from the learned clause)
 * - l must be a literal in the learned clause
 * - it's redundant if it's implied by other literals in the learned clause
 * - we assume that all these literals are marked.
 *
 * To check this, we explore the implication graph recursively from l.
 * Variables already visited are marked in solver->map:
 * - solver->map[x] == NOT_SEEN means x has not been seen yet
 * - solver->map[x] == IMPLIED means x is 'implied by marked literals'
 * - solver->map[x] == NOT_IMPLIED means x is 'not implied by marked literals'
 *
 * We use the following rules:
 * - a decision literal is not removable
 * - if l all immediate predecessors of l are marked or are are removable
 *   then l is removable.
 * - if one of l's predecessor is not marked and not removable then l
 *   is not removable.
 */
enum {
  NOT_SEEN = 0,
  IMPLIED = 1,
  NOT_IMPLIED = 2
};

// number of predecessors of x in the implication graph
static uint32_t num_predecessors(sat_solver_t *solver, bvar_t x) {
  uint32_t n;

  switch (solver->ante_tag[x]) {
  case ATAG_BINARY:
    n = 1;
    break;

  case ATAG_CLAUSE:
    n = clause_length(&solver->pool, solver->ante_data[x]) - 1;
    break;

  default:
    assert(solver->ante_tag[x] == ATAG_STACKED);
    n = stacked_clause_length(&solver->stash, solver->ante_data[x]) - 1;
    break;
  }
  return n;
}

// get the i-th predecessor of x
static bvar_t predecessor(sat_solver_t *solver, bvar_t x, uint32_t i) {
  literal_t *lit;
  literal_t l;

  switch (solver->ante_tag[x]) {
  case ATAG_BINARY:
    assert(i == 0);
    l = solver->ante_data[x];
    break;

  case ATAG_CLAUSE:
    assert(i < clause_length(&solver->pool, solver->ante_data[x]) - 1);
    lit = clause_literals(&solver->pool, solver->ante_data[x]);
    l = lit[i + 1];
    break;

  default:
    assert(solver->ante_tag[x] == ATAG_STACKED);
    assert(i < stacked_clause_length(&solver->stash, solver->ante_data[x]) - 1);
    lit = stacked_clause_literals(&solver->stash, solver->ante_data[x]);
    l = lit[i + 1];
    break;
  }

  return var_of(l);
}

// auxiliary function: check whether x is already explored and IMPLIED or
// trivially implied by marked literals
static inline bool var_is_implied(const sat_solver_t *solver, bvar_t x) {
  return variable_is_marked(solver, x) ||
    solver->ante_tag[x] == ATAG_UNIT ||
    tag_map_read(&solver->map, x) == IMPLIED;
}

// check whether x is already explored and NOT_IMPLIED
// or whether x is a decision variable (can't be implied by marked literals)
static inline bool var_is_not_implied(const sat_solver_t *solver, bvar_t x) {
  assert(!variable_is_marked(solver, x));
  return solver->ante_tag[x] == ATAG_DECISION || tag_map_read(&solver->map, x) == NOT_IMPLIED;
}


static bool implied_by_marked_literals(sat_solver_t *solver, literal_t l) {
  gstack_t *gstack;
  tag_map_t *map;
  gstack_elem_t *top;
  bvar_t x, y;
  uint32_t i;

  x = var_of(l);
  map = &solver->map;

  if (var_is_implied(solver, x)) {
    return true;
  }
  if (var_is_not_implied(solver, x)) {
    return false;
  }

  gstack = &solver->gstack;
  assert(gstack_is_empty(gstack));
  gstack_push_vertex(gstack, x, 0);

  do {
    top = gstack_top(gstack);
    x = top->vertex;
    if (top->index == num_predecessors(solver, x)) {
      tag_map_write(map, x, IMPLIED);
      gstack_pop(gstack);
    } else {
      y = predecessor(solver, x, top->index);
      top->index ++;
      if (var_is_implied(solver, y)) {
        continue;
      }
      if (var_is_not_implied(solver, y)) {
        goto not_implied;
      }
      gstack_push_vertex(gstack, y, 0);
    }
  } while (! gstack_is_empty(gstack));

  return true;

 not_implied:
  for (i=0; i<gstack->top; i++) {
    tag_map_write(map, gstack->data[i].vertex, NOT_IMPLIED);
  }
  reset_gstack(gstack);
  return false;
}

// check whether literals a[1 ... n-1] are all implied by marked literals
static bool array_implied_by_marked_literals(sat_solver_t *solver, literal_t *a, uint32_t n) {
  uint32_t i;

  for (i=1; i<n; i++) {
    if (! implied_by_marked_literals(solver, a[i])) {
      return false;
    }
  }
  return true;
}

// check whether l is implied by other literals in the learned clause
// (l is in the learned clause, so it is marked).
static bool literal_is_redundant(sat_solver_t *solver, literal_t l) {
  literal_t *lit;
  bvar_t x;
  antecedent_tag_t atag;
  cidx_t cidx;
  uint32_t n;

  x = var_of(l);
  assert(var_is_assigned(solver, x) && variable_is_marked(solver, x));

  atag = solver->ante_tag[x] & 0x7F; // remove mark bit
  switch (atag) {
  case ATAG_BINARY:
    // ante_data[x] = literal that implies not(l)
    return implied_by_marked_literals(solver, solver->ante_data[x]);

  case ATAG_CLAUSE:
    // ante_data[x] = clause that implies not(l)
    cidx = solver->ante_data[x];
    n = clause_length(&solver->pool, cidx);
    lit = clause_literals(&solver->pool, cidx);
    assert(lit[0] == not(l));
    return array_implied_by_marked_literals(solver, lit, n);

  case ATAG_STACKED:
    // ante_data[x] = stacked clause that implies not(l)
    cidx = solver->ante_data[x];
    n = stacked_clause_length(&solver->stash, cidx);
    lit = stacked_clause_literals(&solver->stash, cidx);
    assert(lit[0] == not(l));
    return array_implied_by_marked_literals(solver, lit, n);

  default:
    assert(atag == ATAG_DECISION);
    return false;
  }
}


/*
 * Simplify the learned clause:
 * - it's in solver->buffer
 * - all literals in solver->buffer are marked
 * - solver->buffer.data[0] is the implied literal
 * - all other literals have a decision level < solver->decision_level
 *
 * On exit:
 * - the simplified learned clause is in solver->buffer.
 * - all marks are removed.
 */
static void simplify_learned_clause(sat_solver_t *solver) {
  vector_t *buffer;
  uint32_t i, j, n;
  literal_t l;

  assert(solver->aux.size == 0);

  buffer = &solver->buffer;
  n = buffer->size;
  j = 1;
  for (i=1; i<n; i++) { // The first literal is not redundant
    l = buffer->data[i];
    if (literal_is_redundant(solver, l)) {
      // move l to the aux buffer to clean the marks later
      vector_push(&solver->aux, l);
      solver->stats.subsumed_literals ++;
    } else {
      // keep l into buffer
      buffer->data[j] = l;
      j ++;
    }
  }
  buffer->size = j;

  // cleanup: remove marks and reset the map
  clear_tag_map(&solver->map);
  for (i=0; i<j; i++) {
    unmark_variable(solver, var_of(buffer->data[i]));
  }
  n = solver->aux.size;
  for (i=0; i<n; i++) {
    unmark_variable(solver, var_of(solver->aux.data[i]));
  }
  reset_vector(&solver->aux);

  check_all_unmarked(solver);
}


/*
 * Prepare for backtracking:
 * - search for a literal of second highest decision level in
 *   the learned clause.
 * - solver->buffer contains the learned clause.
 * - the implied literal is in solver->buffer.data[0]
 */
static void prepare_to_backtrack(sat_solver_t *solver) {
  uint32_t i, j, d, x, n;
  literal_t l, *b;

  b = (literal_t *) solver->buffer.data;
  n = solver->buffer.size;

  if (n == 1) {
    solver->backtrack_level = 0;
    return;
  }

  j = 1;
  l = b[1];
  d = d_level(solver, l);
  for (i=2; i<n; i++) {
    x = d_level(solver, b[i]);
    if (x > d) {
      d = x;
      j = i;
    }
  }

  // swap b[1] and b[j]
  b[1] = b[j];
  b[j] = l;

  // record backtrack level
  solver->backtrack_level = d;
}


/*
 * Update the exponential moving averages used by the restart heuristics
 *
 * We have
 *     ema_0 = 0
 *     ema_t+1 = 2^(32 - k) x + (1 - 2^k) ema_t
 * where k is less than 32 and x is the lbd of the learned clause
 * - as in the paper by Biere & Froehlich, we use
 *    k = 5  for the 'fast' ema
 *    k = 14 for the 'slow' ema
 *
 * Update: experimental change (07/28/2017): use k=16 for the slow ema
 * (same as cadical).
 *
 * NOTE: these updates can't overflow: the LDB is bounded by U < 2^30
 * then we have ema <= 2^32*U. Same thing for the number of assigned
 * variables.
 */
static void update_emas(sat_solver_t *solver, uint32_t x) {
#if 0
  if (! solver->diving) {
    solver->slow_ema -= solver->slow_ema >> 16;
    solver->slow_ema += ((uint64_t) x) << 16;
    solver->fast_ema -= solver->fast_ema >> 5;
    solver->fast_ema += ((uint64_t) x) << 27;
    solver->fast_count ++;
  }
#endif
  solver->slow_ema -= solver->slow_ema >> 16;
  solver->slow_ema += ((uint64_t) x) << 16;
  solver->fast_ema -= solver->fast_ema >> 5;
  solver->fast_ema += ((uint64_t) x) << 27;
  solver->fast_count ++;
}

// update the search depth = number of assigned literals at the time
// of a conflict
static void update_max_depth(sat_solver_t *solver) {
  if (solver->stack.top > solver->max_depth) {
    solver->max_depth = solver->stack.top;
    solver->max_depth_conflicts = solver->stats.conflicts;
  }
}

// update the conflict level EMA
static void update_level(sat_solver_t *solver) {
  solver->level_ema -= solver->level_ema >> 16;solver->level_ema -= solver->level_ema >> 16;
  solver->level_ema += ((uint64_t) solver->decision_level) << 16;
}


/*
 * Resolve a conflict and add a learned clause
 * - solver->decision_level must be positive
 */
static void resolve_conflict(sat_solver_t *solver) {
  uint32_t n, d;
  literal_t l;
  cidx_t cidx;

  //  update_max_depth(solver);

  analyze_conflict(solver);
  simplify_learned_clause(solver);
  prepare_to_backtrack(solver);

  // EMA statistics
  n = solver->buffer.size;
  d = clause_lbd(solver, n, (literal_t *) solver->buffer.data);
  update_emas(solver, d);

  // Collect data if compiled with DATA=1
  export_conflict_data(solver, d);

  backtrack(solver, solver->backtrack_level);
  solver->conflict_tag = CTAG_NONE;

  // statistics
  update_level(solver);

  // add the learned clause
  l = solver->buffer.data[0];
  if (n >= 3) {
#if 0
    if (solver->diving && n >= solver->params.stack_threshold) {
      cidx = push_clause(&solver->stash, n, (literal_t *) solver->buffer.data);
      stacked_clause_propagation(solver, l, cidx);
    } else {
      cidx = add_learned_clause(solver, n, (literal_t *) solver->buffer.data);
      clause_propagation(solver, l, cidx);
    }
#endif
    cidx = add_learned_clause(solver, n, (literal_t *) solver->buffer.data);
    clause_propagation(solver, l, cidx);
  } else if (n == 2) {
    add_binary_clause(solver, l, solver->buffer.data[1]);
    binary_clause_propagation(solver, l, solver->buffer.data[1]);
  } else {
    assert(n > 0);
    add_unit_clause(solver, l);
  }
}




/*****************************************************
 *  VARIABLE SUBSTITUTION + DATABASE SIMPLIFICATION  *
 ****************************************************/

/*
 * Compute SCCs and apply the substitution if any + perform one
 * round of propagation.
 *
 * - sets solver->has_empty_clause to true if a conflict is detected.
 */
static void try_scc_simplification(sat_solver_t *solver) {
  uint32_t subst_count, units;

  assert(solver->decision_level == 0);

  solver->stats.scc_calls ++;
  subst_count = solver->stats.subst_vars;
  units = solver->units;

  compute_sccs(solver);
  if (solver->has_empty_clause) return;

  report(solver, "scc");

  if (solver->stats.subst_vars > subst_count) {
    apply_substitution(solver);
    if (solver->has_empty_clause) {
      fprintf(stderr, "c empty clause after substitution\n");
      return;
    }
    if (solver->units > units) {
      level0_propagation(solver);
      if (solver->has_empty_clause) {
	fprintf(stderr, "c empty clause after substitution and propagation\n");
	return;
      }
    }
  }
}




/*************************************************
 *  RECOVER TRUTH VALUE OF ELIMINATED VARIABLES  *
 ************************************************/

/*
 * Check whether all literals in a[0 ... n] are false
 */
static bool saved_clause_is_false(sat_solver_t *solver, uint32_t *a, uint32_t n) {
  uint32_t i;

  for (i=0; i<n; i++) {
    if (lit_value(solver, a[i]) == VAL_TRUE) {
      return false;
    }
    assert(lit_value(solver, a[i]) == VAL_FALSE);
  }

  return true;
}

/*
 * Process a block of saved clauses
 * - a = start of the block
 * - n = block length
 * - a[n-1] = literal to flip if needed
 */
static void extend_assignment_for_block(sat_solver_t *solver, uint32_t *a, uint32_t n) {
  literal_t l;
  uint32_t i, j;
  bval_t val;

  l = a[n-1];
  assert(solver->ante_tag[var_of(l)] == ATAG_ELIM || solver->ante_tag[var_of(l)] == ATAG_SUBST);

  val = VAL_FALSE; // default value for l
  i = 0;
  while (i < n) {
    j = i;
    while (a[j] != l) j++;
    // a[i ... j] = saved clause with a[j] == l
    if (saved_clause_is_false(solver, a+i, j-i)) {
      // all literals in a[i ... j-1] are false so l is forced to true
      val = VAL_TRUE;
      break;
    }
    i = j+1;
  }

  solver->value[l] = val;
  solver->value[not(l)] = opposite_val(val);
}

/*
 * Extend the current assignment to variables eliminated by substitution
 */
static void extend_assignment_by_substitution(sat_solver_t *solver) {
  uint32_t i, n;
  literal_t l;
  bval_t val;

  n = solver->nvars;
  for (i=1; i<n; i++) {
    if (solver->ante_tag[i] == ATAG_SUBST) {
      l = full_var_subst(solver, i);
      assert(lit_is_assigned(solver, l));
      val = lit_value(solver, l);

      solver->value[pos_lit(i)] = val;
      solver->value[neg_lit(i)] = opposite_val(val);
    }
  }
}


/*
 * Extend the current assignment to all eliminated variables
 */
static void extend_assignment(sat_solver_t *solver) {
  nclause_vector_t *v;
  uint32_t n, block_size;;

  /*
   * NOTE: this works because we do not alternate between elimination
   * by substitution and other techniques.  (i.e., we eliminate
   * variables by resolution only as a pre-processing step).
   */
  extend_assignment_by_substitution(solver);

  v = &solver->saved_clauses;
  n = v->top;
  while (n > 0) {
    n --;
    block_size = v->data[n];
    assert(block_size >= 1 && block_size <= n);
    n -= block_size;
    extend_assignment_for_block(solver, v->data + n, block_size);
  }
}



/*****************
 *  HEURISTICS   *
 ****************/

/*
 * Number of literals assigned at level 0
 * - this is used to decide whether to call simplify_clause_database
 */
static uint32_t level0_literals(const sat_solver_t *solver) {
  uint32_t n;

  n = solver->stack.top;
  if (solver->decision_level > 0) {
    n = solver->stack.level_index[1];
  }
  return n;
}


/*
 * MODE
 */

/*
 * Initial mode
 */
static void init_mode(sat_solver_t *solver) {
  solver->progress_units = 0;
  solver->progress_binaries = 0;
  solver->progress = solver->params.search_counter;
  solver->check_next = solver->params.search_period;
  solver->diving = false;
  solver->dive_budget = solver->params.diving_budget;
  solver->max_depth = 0;
  solver->max_depth_conflicts = 0;
  solver->dive_start = 0;
}

#if 0
/*
 * Check whether we're making progress (in search mode).
 * - we declare progress when we've learned new unit or binary clauses
 */
static bool made_progress(sat_solver_t *solver) {
  uint32_t units, binaries;
  bool progress;

  units = level0_literals(solver);
  binaries = solver->binaries;
  progress = units > solver->progress_units || binaries > solver->progress_binaries;
  solver->progress_units = units;
  solver->progress_binaries = binaries;

  return progress;
}

static inline bool need_check(const sat_solver_t *solver) {
  return solver->stats.conflicts >= solver->check_next;
}

static bool switch_to_diving(sat_solver_t *solver) {
  assert(! solver->diving);

  solver->check_next += solver->params.search_period;

  if (made_progress(solver)) {
    solver->progress = solver->params.search_counter;
  } else {
    assert(solver->progress > 0);
    solver->progress --;
    if (solver->progress == 0) {
      solver->diving = true;
      solver->max_depth_conflicts = solver->stats.conflicts;
      solver->max_depth = 0;
      solver->dive_start = solver->stats.conflicts;
      solver->stats.dives ++;
      return true;
    }
  }

  return false;
}

static void done_diving(sat_solver_t *solver) {
  uint64_t delta;

  solver->diving = false;
  if (solver->dive_budget <= 200000) {
    solver->dive_budget += solver->dive_budget >> 2;
  }
  solver->progress = solver->params.search_counter;
  solver->progress_units = level0_literals(solver);
  solver->progress_binaries = solver->binaries;

  // adjust reduce_next, restart_next, simplify_next, etc.
  // delta = number of conflicts in the dive
  delta = solver->stats.conflicts - solver->dive_start;
  solver->reduce_next += delta;
  solver->restart_next += delta;
  solver->simplify_next += delta;
  solver->check_next += delta;
}

#endif

/*
 * WHEN TO RESTART
 */

/*
 * Glucose-style restart condition:
 * 1) solver->fast_ema is an estimate of the quality of the recently
 *    learned clauses.
 * 2) solver->slow_ema is an estimate of the average quality of all
 *    learned clauses.
 *
 * Intuition:
 * - if solver->fast_ema is larger than solver->slow_ema then recent
 *   learned clauses don't seem too good. We want to restart.
 *
 * To make this more precise: we use a magic constant K = 0.9 (approximately)
 * Worse than average learned clauses is 'fast_ema * K > slow_ema'
 * For our fixed point implementation, we use
 *    K = (1 - 1/2^4 - 1/2^5) = 0.90625
 *
 * To avoid restarting every time, we keep track of the number of
 * samples from which fast_ema is computed (in solver->fast_count).
 * We wait until fast_count >= 50 before restarting.
 */


/*
 * Initialize the restart counters
 */
static void init_restart(sat_solver_t *solver) {
  solver->slow_ema = 0;
  solver->fast_ema = 0;
  solver->level_ema = 0;
  solver->restart_next = solver->params.restart_interval;
  solver->fast_count = 0;
}

/*
 * Check for restart
 */
#if 0
static bool need_restart(sat_solver_t *solver) {
  uint64_t aux;

  if (solver->diving) {
    return solver->stats.conflicts > solver->max_depth_conflicts + solver->dive_budget;
  }

  if (solver->stats.conflicts >= solver->restart_next &&
      solver->decision_level >= (uint32_t) (solver->fast_ema >> 32)) {
    aux = solver->fast_ema;
    //    aux -= (aux >> 3) + (aux >> 4) + (aux >> 6); // K * fast_ema
    aux -= (aux >> 4) + (aux >> 5);    // approximately 0.9 * fast_ema
    if (aux >= solver->slow_ema) {
      return true;
    }
  }

  return solver->stats.conflicts >= solver->check_next;
}

#endif

static bool need_restart(sat_solver_t *solver) {
  uint64_t aux;

  if (solver->stats.conflicts >= solver->restart_next &&
      solver->decision_level >= (uint32_t) (solver->fast_ema >> 32)) {
    aux = solver->fast_ema;
    //    aux -= (aux >> 3) + (aux >> 4) + (aux >> 6); // K * fast_ema
    aux -= (aux >> 4) + (aux >> 5);    // approximately 0.9 * fast_ema
    if (aux >= solver->slow_ema) {
      return true;
    }
  }

  return false;
}


static void done_restart(sat_solver_t *solver) {
  solver->restart_next = solver->stats.conflicts + solver->params.restart_interval;
}



/*
 * WHEN TO REDUCE
 */

/*
 * Heuristic similar to Cadical:
 * - we keep three counters:
 *    reduce_next
 *    reduce_inc
 *    reduce_inc2
 * - when the number of conflicts is bigger than reduce_next
 *   we call reduce
 * - after reduce, we update the counters:
 *    reduce_inc = reduce_inc + reduce_inc2
 *    reduce_next = reduce_next + reduce_inc
 *    reduce_inc2 = max(0, reduce_inc2 - 1)
 */

/*
 * Initialize the reduce counters
 */
static void init_reduce(sat_solver_t *solver) {
  solver->reduce_next = solver->params.reduce_interval;
  solver->reduce_inc = solver->params.reduce_interval;
  solver->reduce_inc2 = solver->params.reduce_delta;
}

/*
 * Check to trigger call to reduce_learned_clause_set
 */
static inline bool need_reduce(const sat_solver_t *solver) {
  //  return !solver->diving && solver->stats.conflicts >= solver->reduce_next;
  return solver->stats.conflicts >= solver->reduce_next;
}

/*
 * Update counters after a call to reduce
 */
static void done_reduce(sat_solver_t *solver) {
  solver->reduce_inc += solver->reduce_inc2;
  solver->reduce_next = solver->stats.conflicts + solver->reduce_inc;
  if (solver->reduce_inc2 > 0) {
    solver->reduce_inc2 --;
  }
}


/*
 * WHEN TO SIMPLIFY
 */

/*
 * Initialize counters
 */
static void init_simplify(sat_solver_t *solver) {
  solver->simplify_assigned = 0;
  solver->simplify_binaries = 0;
  solver->simplify_next = 0;
}

/*
 * Heuristic to trigger a call to simplify_clause_database:
 * - we call simplify when there's more literals assigned at level 0
 *   (or more binary clauses)
 */
static bool need_simplify(const sat_solver_t *solver) {
  return (level0_literals(solver) > solver->simplify_assigned ||
	  solver->binaries > solver->simplify_binaries + solver->params.simplify_bin_delta ||
	  (solver->binaries > solver->simplify_binaries && solver->stats.conflicts >= solver->simplify_next + 100000))
    && solver->stats.conflicts >= solver->simplify_next;
}


/*
 * Update counters after simplify
 */
static void done_simplify(sat_solver_t *solver) {
  /*
   * new_bins = number of binary clauses produced in this
   *            simplification round
   * these clauses have not been seen by the SCC construction.
   * Some of the new clauses may have been deleted.
   */
  if (solver->simplify_new_bins > solver->binaries) {
    solver->simplify_binaries = solver->binaries;
  } else {
    solver->simplify_binaries = solver->binaries - solver->simplify_new_bins;
  }
  solver->simplify_assigned = solver->stack.top;
  solver->simplify_next = solver->stats.conflicts + solver->params.simplify_interval;

  solver->check_next = solver->stats.conflicts + solver->params.search_period;
  solver->progress = solver->params.search_counter;
  solver->progress_units = level0_literals(solver);
  solver->progress_binaries = solver->binaries;
}




/*****************************
 *  MAIN SOLVING PROCEDURES  *
 ****************************/

/*
 * Select an unassigned decision variable
 * - return 0 if all variables are assigned
 */
static bvar_t nsat_select_decision_variable(sat_solver_t *solver) {
  uint32_t rnd;
  bvar_t x;

  if (solver->params.randomness > 0) {
    rnd = random_uint32(solver) & VAR_RANDOM_MASK;
    if (rnd < solver->params.randomness) {
      x = random_uint(solver, solver->nvars);
      if (var_is_active(solver, x)) {
        assert(x > 0);
        solver->stats.random_decisions ++;
        return x;
      }
    }
  }

  /*
   * Unassigned variable of highest activity
   */
  while (! heap_is_empty(&solver->heap)) {
    x = heap_get_top(&solver->heap);
    if (var_is_active(solver, x)) {
      assert(x > 0);
      return x;
    }
  }

  /*
   * Check the variables in [heap->vmax ... heap->nvars - 1]
   */
  x = solver->heap.vmax;
  while (x < solver->heap.nvars) {
    if (var_is_active(solver, x)) {
      solver->heap.vmax = x+1;
      return x;
    }
    x ++;
  }

  assert(x == solver->heap.nvars);
  solver->heap.vmax = x;

  return 0;
}

/*
 * Preferred literal when x is selected as decision variable.
 * - we pick l := pos_lit(x) then check whether value[l] is 0b00 or 0b01
 * - in the first case, the preferred value for l is false so we return not(l)
 */
static inline literal_t preferred_literal(const sat_solver_t *solver, bvar_t x) {
  literal_t l;

  assert(var_is_unassigned(solver, x));

  l = pos_lit(x);
  /*
   * Since l is not assigned, value[l] is either VAL_UNDEF_FALSE (i.e., 0)
   * or VAL_UNDEF_TRUE (i.e., 1).
   *
   * We return l if value[l] = VAL_UNDEF_TRUE = 1.
   * We return not(l) if value[l] = VAL_UNDEF_FALSE = 0.
   * Since not(l) is l^1, the returned value is (l ^ 1 ^ value[l]).
   */
  l ^= 1 ^ solver->value[l];
  assert((var_prefers_true(solver, x) && l == pos_lit(x)) ||
         (!var_prefers_true(solver, x) && l == neg_lit(x)));

  return l;
}


/*
 * Search until we get sat/unsat or we restart
 * - restart is based on the LBD/Glucose heuristics as modified by
 *   Biere & Froehlich.
 */
static void sat_search(sat_solver_t *solver) {
  bvar_t x;

  assert(solver->stack.prop_ptr == solver->stack.top);

  check_propagation(solver);
  check_watch_vectors(solver);

  for (;;) {
    nsat_boolean_propagation(solver);
    if (solver->conflict_tag == CTAG_NONE) {
      // No conflict
#if 0
      if (need_restart(solver) || need_simplify(solver)) {
        break;
      }
#endif
      if (need_restart(solver)) {
	break;
      }
      if (need_reduce(solver)) {
        nsat_reduce_learned_clause_set(solver);
        check_watch_vectors(solver);
	done_reduce(solver);
      }

      update_max_depth(solver);

      x = nsat_select_decision_variable(solver);
      if (x == 0) {
        solver->status = STAT_SAT;
        break;
      }
      nsat_decide_literal(solver, preferred_literal(solver, x));
    } else {
      // Conflict
      if (solver->decision_level == 0) {
        export_last_conflict(solver);
        solver->status = STAT_UNSAT;
        break;
      }
      resolve_conflict(solver);
      check_watch_vectors(solver);

#if 0
      if (! solver->diving) {
	decay_clause_activities(solver);
	decay_var_activities(&solver->heap);
      }
#endif
      decay_clause_activities(solver);
      decay_var_activities(&solver->heap);
    }
  }
}



/*
 * Simplify: call try_scc_simplification, then simplify clause database
 * - add empty clause and set status to UNSAT if there's a conflict.
 */
static void nsat_simplify(sat_solver_t *solver) {
  solver->simplify_new_units = 0;
  solver->simplify_new_bins = 0;
  if (solver->binaries > solver->simplify_binaries) {
    try_scc_simplification(solver);
    if (solver->has_empty_clause) return;
  }
  if (level0_literals(solver) > solver->simplify_assigned) {
    simplify_clause_database(solver);
  }
}


/*
 * Preprocessing: call nsat_preprocess and print statistics
 */
static void nsat_do_preprocess(sat_solver_t *solver) {
  double start, end;

  if (solver->verbosity >= 1) {
    start = get_cpu_time();
    nsat_preprocess(solver);
    end = get_cpu_time();
    show_preprocessing_stats(solver, time_diff(end, start));
  } else {
    nsat_preprocess(solver);
  }

  solver->preprocess = false;
}


/*
 * Solving procedure
 */
solver_status_t nsat_solve(sat_solver_t *solver) {

  //  open_stat_file();

  if (solver->has_empty_clause) goto done;

  solver->prng = solver->params.seed;
  solver->cla_inc = INIT_CLAUSE_ACTIVITY_INCREMENT;

  init_mode(solver);
  init_restart(solver);
  init_reduce(solver);
  init_simplify(solver);

  if (solver->preprocess) {
    // preprocess + one round of simplification
    nsat_do_preprocess(solver);
    if (solver->has_empty_clause) goto done;
    nsat_simplify(solver);
    done_simplify(solver);
  } else {
    // one round of propagation + one round of simplification
    level0_propagation(solver);
    if (solver->has_empty_clause) goto done;
    nsat_simplify(solver);
    done_simplify(solver);
  }

  // main loop: simplification may detect unsat
  // and set has_empty_clause to true
  while (! solver->has_empty_clause) {
    sat_search(solver);
    if (solver->status != STAT_UNKNOWN) break;

#if 0
    if (need_simplify(solver)) {
      if (solver->diving) {
	done_diving(solver);
      }
      full_restart(solver);
      done_restart(solver);
      nsat_simplify(solver);
      done_simplify(solver);
    } else if (solver->diving) {
      done_diving(solver);
      full_restart(solver);
      report(solver, "");
    } else if (need_check(solver)) {
      //      report(solver, "chk");
      if (switch_to_diving(solver)) {
	full_restart(solver);
	report(solver, "dive");
      }
    } else {
      partial_restart(solver);
      done_restart(solver);
    }
#endif

    if (need_simplify(solver)) {
      full_restart(solver);
      done_restart(solver);
      nsat_simplify(solver);
      done_simplify(solver);
    } else {
      partial_restart(solver);
      done_restart(solver);
    }
  }

 done:
  assert(solver->status == STAT_UNSAT || solver->status == STAT_SAT);

  report(solver, "end");

  if (solver->status == STAT_SAT) {
    solver->stats.successful_dive = solver->diving;
    extend_assignment(solver);
  }

  //  close_stat_file();

  return solver->status;
}


/************
 *  MODELS  *
 ***********/

/*
 * Return the model: copy all variable value into val
 * - val's size must be at least solver->nvars
 * - val[0] is always true
 */
void nsat_get_allvars_assignment(const sat_solver_t *solver, bval_t *val) {
  uint32_t i, n;

  n = solver->nvars;
  for (i=0; i<n; i++) {
    val[i] = var_value(solver, i);
  }
}


/*
 * Copy all true literals in array a:
 * - a must have size >= solver->nvars.
 * return the number of literals added to a.
 */
uint32_t nsat_get_true_literals(const sat_solver_t *solver, literal_t *a) {
  uint32_t n;
  literal_t l;

  n = 0;
  for (l = 0; l< solver->nliterals; l++) {
    if (lit_value(solver, l) == VAL_TRUE) {
      a[n] = l;
      n ++;
    }
  }

  return n;
}



/***********************
 *  EXPORT/DUMP STATE  *
 **********************/

static void show_clause(FILE *f, const clause_pool_t *pool, cidx_t idx) {
  uint32_t n, i;
  literal_t *lit;

  assert(good_clause_idx(pool, idx));

  n = clause_length(pool, idx);
  lit = clause_literals(pool, idx);

  fprintf(f, "%"PRIu32":", idx);
  for (i=0; i<n; i++) {
    fprintf(f, " %"PRIu32, lit[i]);
  }
  fprintf(f, "\n");
}

static void show_all_clauses(FILE *f, const clause_pool_t *pool) {
  uint32_t cidx;

  cidx = clause_pool_first_clause(pool);
  while (cidx < pool->size) {
    show_clause(f, pool, cidx);
    cidx = clause_pool_next_clause(pool, cidx);
  }
}

static void show_watch_vector(FILE *f, const sat_solver_t *solver, literal_t l) {
  watch_t *w;
  uint32_t i, n, k;

  assert(l < solver->nliterals);
  w = solver->watch[l];
  fprintf(f, "watch[%"PRIu32"]:", l);
  if (w == NULL) {
    fprintf(f, " null\n");
  } else {
    n = w->size;
    i = 0;
    if (n == 0) {
      fprintf(f, " empty\n");
    } else {
      while (i<n) {
        k = w->data[i];
        if (idx_is_literal(k)) {
          fprintf(f, " lit(%"PRIu32")", idx2lit(k));
          i ++;
        } else {
          fprintf(f, " cl(%"PRIu32")", k);
          i += 2;
        }
      }
      fprintf(f, "\n");
    }
  }
}

static void show_all_watch_vectors(FILE *f, const sat_solver_t *solver) {
  uint32_t i;

  for (i=0; i<solver->nliterals; i++) {
    show_watch_vector(f, solver, i);
  }
}

void show_state(FILE *f, const sat_solver_t *solver) {
  fprintf(f, "nvars: %"PRIu32"\n", solver->nvars);
  fprintf(f, "nliterals: %"PRIu32"\n", solver->nliterals);
  fprintf(f, "num prob. clauses: %"PRIu32"\n", solver->pool.num_prob_clauses);
  fprintf(f, "num learned clauses: %"PRIu32"\n", solver->pool.num_learned_clauses);
  fprintf(f, "clauses\n");
  show_all_clauses(f, &solver->pool);
  fprintf(f, "watch vectors\n");
  show_all_watch_vectors(f, solver);
}




/****************************************
 *   CONSISTENCY CHECKS FOR DEBUGGING   *
 ***************************************/

#if DEBUG

/*
 * Check whether the clause pool counters are correct.
 */
static bool good_counters(const clause_pool_t *pool) {
  uint32_t prob_clauses, prob_lits, learned_clauses, learned_lits, i;

  prob_clauses = 0;
  prob_lits = 0;
  learned_clauses = 0;
  learned_lits = 0;

  i = clause_pool_first_clause(pool);
  while (i < pool->learned) {
    prob_clauses ++;
    prob_lits += clause_length(pool, i);
    i = clause_pool_next_clause(pool, i);
  }
  while (i < pool->size) {
    learned_clauses ++;
    learned_lits += clause_length(pool, i);
    i = clause_pool_next_clause(pool, i);
  }

  return
    prob_clauses == pool->num_prob_clauses &&
    prob_lits == pool->num_prob_literals &&
    learned_clauses == pool->num_learned_clauses &&
    learned_lits == pool->num_learned_literals;
}

/*
 * Check that the padding counter is correct
 */
static bool good_padding_counter(const clause_pool_t *pool) {
  cidx_t cidx;
  uint32_t n, len;

  n = 0;
  cidx = 0;
  while (cidx < pool->size) {
    if (is_clause_start(pool, cidx)) {
      cidx += clause_full_length(pool, cidx);
    } else {
      len = padding_length(pool, cidx);
      cidx += len;
      n += len;
    }
  }

  return n == pool->padding;
}


/*
 * Check the counters, assuming pool->learned and pool->size are correct.
 */
static void check_clause_pool_counters(const clause_pool_t *pool) {
  if (!good_counters(pool)) {
    fprintf(stderr, "**** BUG: inconsistent pool counters ****\n");
    fflush(stderr);
  }
  if (!good_padding_counter(pool)) {
    fprintf(stderr, "**** BUG: inconsistent padding pool counter ****\n");
    fflush(stderr);
  }
}


/*
 * Check that all problem clauses have index < pool->learned
 * and that all learned clause have index >= pool->learned;
 * This assumes that pool->num_prob_clauses is correct.
 */
static void check_clause_pool_learned_index(const clause_pool_t *pool) {
  cidx_t cidx, end, next;
  uint32_t n, i;

  /*
   * Find the index of the last problem clause:
   *   cidx = 0 if there are no problem clauses
   *   cidx = pool->size if there are less problem clauses than expected
   */
  n = pool->num_prob_clauses;
  cidx = 0;
  end = 0;
  for (i=0; i<n; i++) {
    cidx = next_clause_index(pool, end);
    if (cidx >= pool->size) break;
    end = cidx + clause_full_length(pool, cidx);
  }

  if (cidx == pool->size) {
    fprintf(stderr, "**** BUG: expected %"PRIu32" problem clauses. Found %"PRIu32". ****\n",
            pool->num_prob_clauses, i + 1);
    fflush(stderr);
  } else {
    next = next_clause_index(pool, end);        // next clause after that (i.e., first learned clause or nothing)
    if (cidx >= pool->learned) {
      fprintf(stderr, "**** BUG: last problem clause starts at %"PRIu32". Learned index is %"PRIu32" ****\n",
              cidx, pool->learned);
      fflush(stderr);
    } else if (end > pool->learned) {
      fprintf(stderr, "**** BUG: last problem clause ends at %"PRIu32". Learned index is %"PRIu32" ****\n",
              end, pool->learned);
      fflush(stderr);
    } else if (next < pool->size && next < pool->learned) {
      fprintf(stderr, "**** BUG: first learned clause starts at %"PRIu32". Learned index is %"PRIu32" ****\n",
              next, pool->learned);
      fflush(stderr);
    }
  }
}



/*
 * HEAP INVARIANTS
 */
static void check_heap(const nvar_heap_t *heap) {
  uint32_t i, j, n;
  int32_t k;
  bvar_t x, y;

  n = heap->heap_last;
  for (i=0; i<=n; i++) {
    x = heap->heap[i];
    if (heap->heap_index[x] != (int32_t) i) {
      fprintf(stderr, "*** BUG: heap[%"PRIu32"] = %"PRIu32" but heap_index[%"PRIu32"] = %"PRId32" ****\n",
              i, x, x, heap->heap_index[x]);
    }
    j = i>>1; // parent of i (or j=i=0 for the special marker)
    y = heap->heap[j];
    if (heap->activity[y] < heap->activity[x]) {
      fprintf(stderr, "*** BUG: bad heap ordering: activity[%"PRIu32"] < activity[%"PRIu32"] ****\n", j, i);
    }
  }

  n = heap->nvars;
  for (i=0; i<n; i++) {
    k= heap->heap_index[i];
    if (k >= 0 && heap->heap[k] != i) {
      fprintf(stderr, "*** BUG: heap_index[%"PRIu32"] = %"PRId32" but heap[%"PRId32"] = %"PRIu32" ****\n",
              i, k, k, heap->heap[k]);
    }
  }
}


/*
 * SORTING FOR CLAUSE DELETION
 * - a = array of clause idx
 * - n = number of elements in a
 * We check that all elements in a can be deleted and that a is sorted in increasing order.
 */
static void check_candidate_clauses_to_delete(const sat_solver_t *solver, const cidx_t *a, uint32_t n) {
  uint32_t i;
  cidx_t c1, c2;
  float a1, a2;

  for (i=0; i<n; i++) {
    c1 = a[i];
    if (clause_is_locked(solver, c1)) {
      fprintf(stderr, "**** BUG: locked clause (cidx = %"PRIu32") is candidate for deletion ****\n", c1);
      fflush(stderr);
    }
  }

  if (n <= 1) return;

  c1 = a[0];
  a1 = get_learned_clause_activity(&solver->pool, c1);
  for (i=1; i<n; i++) {
    c2 = a[i];
    a2 = get_learned_clause_activity(&solver->pool, c2);
    if (a1 > a2 || (a1 == a2 && c1 > c2)) {
      fprintf(stderr, "**** BUG: candidates for deletion not sorted (at position i = %"PRIu32")\n", i);
      fflush(stderr);
    }
    a1 = a2;
    c1 = c2;
  }
}


/*
 * WATCH VECTORS
 */

/*
 * Check that cidx occurs in vector watch[l]
 */
static bool clause_is_in_watch_vector(const sat_solver_t *solver, literal_t l, cidx_t cidx) {
  const watch_t *w;
  uint32_t i, n;

  w = solver->watch[l];
  if (w != NULL) {
    n = w->size;
    i = 0;
    while (i < n) {
      if (idx_is_literal(w->data[i])) {
        i ++;
      } else {
        if (w->data[i] == cidx) {
          return true;
        }
        i += 2;
      }
    }
  }

  return false;
}

static void check_all_clauses_are_in_watch_vectors(const sat_solver_t *solver) {
  cidx_t cidx, end;
  literal_t l0, l1;

  cidx = clause_pool_first_clause(&solver->pool);
  end = solver->pool.size;

  while (cidx < end) {
    l0 = first_literal_of_clause(&solver->pool, cidx);
    l1 = second_literal_of_clause(&solver->pool, cidx);
    assert(l0 < solver->nliterals && l1 < solver->nliterals);
    if (!clause_is_in_watch_vector(solver, l0, cidx)) {
      fprintf(stderr, "*** BUG: missing clause index (%"PRIu32") in watch vector for literal %"PRIu32" ***\n",
              cidx, l0);
      fflush(stderr);
    }
    if (!clause_is_in_watch_vector(solver, l1, cidx)) {
      fprintf(stderr, "*** BUG: missing clause index (%"PRIu32") in watch vector for literal %"PRIu32" ***\n",
              cidx, l1);
      fflush(stderr);
    }
    cidx = clause_pool_next_clause(&solver->pool, cidx);
  }
}

static void check_watch_vector_is_good(const sat_solver_t *solver, const watch_t *w, literal_t l) {
  uint32_t i, n, k;

  assert(w != NULL && w == solver->watch[l]);

  n = w->size;
  i = 0;
  while (i < n) {
    k = w->data[i];
    if (idx_is_clause(k)) {
      if (first_literal_of_clause(&solver->pool, k) != l &&
          second_literal_of_clause(&solver->pool, k) != l) {
        fprintf(stderr, "*** BUG: clause %"PRIu32" is in watch vector for literal %"PRIu32"\n, but the literal is not first or second ***\n", k, l);
        fflush(stderr);
      }
      i += 2;
    } else {
      i ++;
    }
  }
}

static void check_all_watch_vectors_are_good(const sat_solver_t *solver) {
  uint32_t i, n;
  watch_t *w;

  n = solver->nliterals;
  for (i=0; i<n; i++) {
    w = solver->watch[i];
    if (w != NULL) {
      check_watch_vector_is_good(solver, w, i);
    }
  }
}

static void check_watch_vectors(const sat_solver_t *solver) {
  check_all_clauses_are_in_watch_vectors(solver);
  check_all_watch_vectors_are_good(solver);
}


/*
 * PROPAGATION
 */

/*
 * Check whether clause cidx is true
 */
static bool clause_is_true(const sat_solver_t *solver, cidx_t cidx) {
  uint32_t i, n;
  literal_t *lit;

  assert(good_clause_idx(&solver->pool, cidx));

  n = clause_length(&solver->pool, cidx);
  lit = clause_literals(&solver->pool, cidx);
  for (i=0; i<n; i++) {
    if (lit_is_true(solver, lit[i])) {
      return true;
    }
  }

  return false;
}


/*
 * Get the number of false literals in clause cidx
 */
static uint32_t num_false_literals_in_clause(const sat_solver_t *solver, cidx_t cidx) {
  uint32_t i, n, cnt;
  literal_t *lit;

  assert(good_clause_idx(&solver->pool, cidx));

  n = clause_length(&solver->pool, cidx);
  lit = clause_literals(&solver->pool, cidx);
  cnt = 0;
  for (i=0; i<n; i++) {
    if (lit_is_false(solver, lit[i])) {
      cnt ++;
    }
  }

  return cnt;
}

/*
 * Same thing for a stacked clause cidx
 */
static uint32_t num_false_literals_in_stacked_clause(const sat_solver_t *solver, cidx_t cidx) {
  uint32_t i, n, cnt;
  literal_t *lit;

  assert(good_stacked_clause_idx(&solver->stash, cidx));

  n = stacked_clause_length(&solver->stash, cidx);
  lit = stacked_clause_literals(&solver->stash, cidx);
  cnt = 0;
  for (i=0; i<n; i++) {
    if (lit_is_false(solver, lit[i])) {
      cnt ++;
    }
  }

  return cnt;
}


/*
 * Check that no propagation was missed (for the clause pool)
 * - this is called when there's no conflict reported
 */
static void check_pool_propagation(const sat_solver_t *solver) {
  cidx_t cidx;
  uint32_t f, n;

  for (cidx = clause_pool_first_clause(&solver->pool);
       cidx < solver->pool.size;
       cidx = clause_pool_next_clause(&solver->pool, cidx)) {
    if (! clause_is_true(solver, cidx)) {
      f = num_false_literals_in_clause(solver, cidx);
      n = clause_length(&solver->pool, cidx);
      if (f == n) {
        fprintf(stderr, "*** BUG: missed conflict. Clause %"PRIu32" is false ***\n", cidx);
        fflush(stderr);
      } else if (f == n -1) {
        fprintf(stderr, "*** BUG: missed propagation for clause %"PRIu32" ***\n", cidx);
        fflush(stderr);
      }
    }
  }
}


/*
 * Report missed conflicts and propagation for vector w
 * - l = literal corresponding to w (i.e., solver->watch[l] is w)
 * - l is false in the solver.
 */
static void check_missed_watch_prop(const sat_solver_t *solver, const watch_t *w, literal_t l) {
  uint32_t i, k, n;
  literal_t l1;

  assert(lit_is_false(solver, l) && solver->watch[l] == w);

  n = w->size;
  i = 0;
  while (i < n) {
    k = w->data[i];
    if (idx_is_literal(k)) {
      l1 = idx2lit(k);
      if (lit_is_false(solver, l1)) {
        fprintf(stderr, "*** BUG: missed binary conflict for clause %"PRIu32" %"PRIu32" ***\n", l, l1);
        fflush(stderr);
      } else if (lit_is_unassigned(solver, l1)) {
        fprintf(stderr, "*** BUG: missed binary propagation for clause %"PRIu32" %"PRIu32" ***\n", l, l1);
        fflush(stderr);
      }
      i ++;
    } else {
      i += 2;
    }
  }
}


/*
 * Check that no propagation was missed (for the binary clauses)
 * - this is called when no conflict was reported
 */
static void check_binary_propagation(const sat_solver_t *solver) {
  uint32_t i, n;
  const watch_t *w;

  n = solver->nliterals;
  for (i=0; i<n; i++) {
    if (lit_is_false(solver, i)) {
      w = solver->watch[i];
      if (w != NULL) {
        check_missed_watch_prop(solver, w, i);
      }
    }
  }
}


/*
 * Check that all literals implied by a clause cidx are in first
 * position in that clause.
 */
static void check_clause_antecedents(const sat_solver_t *solver) {
  uint32_t i;
  literal_t l;
  cidx_t cidx;

  for (i=0; i<solver->stack.top; i++) {
    l = solver->stack.lit[i];
    if (solver->ante_tag[var_of(l)] == ATAG_CLAUSE) {
      cidx = solver->ante_data[var_of(l)];
      if (first_literal_of_clause(&solver->pool, cidx) != l) {
        fprintf(stderr, "*** BUG: implied literal %"PRIu32" is not first in clause %"PRIu32" ****\n", l, cidx);
        fflush(stderr);
      }
    }
  }
}


/*
 * Check that all propagations are sound:
 * - in a binary propagation {l, l1} then l1 must be false
 * - in a clause propagation {l, l1 .... l_k} then l1 ... l_k must all be false
 */
static void check_sound_propagation(const sat_solver_t *solver) {
  uint32_t i, n, f;
  cidx_t cidx;
  literal_t l, l1;

  for (i=0; i<solver->stack.top; i++) {
    l = solver->stack.lit[i];
    assert(lit_is_true(solver, l));
    switch (solver->ante_tag[var_of(l)]) {
    case ATAG_BINARY:
      l1 = solver->ante_data[var_of(l)];
      if (! lit_is_false(solver, l1)) {
        fprintf(stderr, "*** BUG: unsound propagation for binary clause %"PRIu32" %"PRIu32" ***\n", l, l1);
        fflush(stderr);
      }
      break;

    case ATAG_CLAUSE:
      cidx = solver->ante_data[var_of(l)];
      f = num_false_literals_in_clause(solver, cidx);
      n = clause_length(&solver->pool, cidx);
      if (f != n - 1) {
        fprintf(stderr, "*** BUG: unsound propagation. Clause %"PRIu32" antecedent of literal %"PRIu32" ***\n",
                cidx, l);
        fflush(stderr);
      }
      break;

    default:
      break;
    }
  }
}


/*
 * Check the stacked clauses:
 * - if an assigned literal l has stack clause cidx as antecedent then
 *   l must be first in the clause
 */
static void check_stacked_clause_antecedents(const sat_solver_t *solver) {
  uint32_t i;
  literal_t l;
  cidx_t cidx;

  for (i=0; i<solver->stack.top; i++) {
    l = solver->stack.lit[i];
    if (solver->ante_tag[var_of(l)] == ATAG_STACKED) {
      cidx = solver->ante_data[var_of(l)];
      if (first_literal_of_stacked_clause(&solver->stash, cidx) != l) {
        fprintf(stderr, "*** BUG: implied literal %"PRIu32" is not first in stacked clause %"PRIu32" ****\n", l, cidx);
        fflush(stderr);
      }
    }
  }
}

/*
 * Check the stacked clauses (continued)
 * - for every stacked clause cidx:
 *   its first literal must be assigned and true
 * - all the other literals must be false
 */
static void check_stacked_clauses(const sat_solver_t *solver) {
  cidx_t cidx;
  uint32_t f, n;
  literal_t l;

  for (cidx = 0;
       cidx < solver->stash.top;
       cidx = next_stacked_clause(&solver->stash, cidx)) {
    l = first_literal_of_stacked_clause(&solver->stash, cidx);
    if (solver->ante_tag[var_of(l)] != ATAG_STACKED ||
        solver->ante_data[var_of(l)] != cidx) {
      fprintf(stderr, "*** BUG: bad antecedent for literal %"PRIu32" (first in stacked clause %"PRIu32") ****\n", l, cidx);
      fflush(stderr);
    }
    if (!lit_is_true(solver, l)) {
      fprintf(stderr, "*** BUG: literal %"PRIu32" (first in stacked clause %"PRIu32") is not true ****\n", l, cidx);
      fflush(stderr);
    }
    n = stacked_clause_length(&solver->stash, cidx);
    f = num_false_literals_in_stacked_clause(solver, cidx);
    if (f != n-1) {
      fprintf(stderr, "*** BUG: stacked clause %"PRIu32" has %"PRIu32" false literals (out of %"PRIu32") ***\n", cidx, f, n);
      fflush(stderr);
    }
  }
}

/*
 * Full check
 */
static void check_propagation(const sat_solver_t *solver) {
  check_binary_propagation(solver);
  check_pool_propagation(solver);
  check_clause_antecedents(solver);
  check_sound_propagation(solver);
  check_stacked_clause_antecedents(solver);
  check_stacked_clauses(solver);
}


/*******************************
 *  MARKS AND LEARNED CLAUSES  *
 ******************************/

/*
 * Check that all literals in solver->buffer are marked
 */
static void check_buffer_marks(const sat_solver_t *solver) {
  uint32_t n, i;
  literal_t l;

  n = solver->buffer.size;
  for (i=0; i<n; i++) {
    l = solver->buffer.data[i];
    if (! variable_is_marked(solver, var_of(l))) {
      fprintf(stderr, "*** BUG: literal %"PRIu32" in the learned clause is not marked ***\n", l);
      fflush(stderr);
    }
  }
}

/*
 * Count the number of marked variables
 */
static uint32_t num_marked_variables(const sat_solver_t *solver) {
  uint32_t n, i, c;

  c = 0;
  n = solver->nvars;
  for (i=0; i<n; i++) {
    if (variable_is_marked(solver, i)) {
      c ++;
    }
  }

  return c;
}


/*
 * After construction of the learned clause (before it's simplified):
 * - all literals in the clause must be marked.
 * - no other literals should be marked.
 */
static void check_marks(const sat_solver_t *solver) {
  uint32_t n;

  n = num_marked_variables(solver);
  if (n != solver->buffer.size) {
    fprintf(stderr, "*** BUG: expected %"PRIu32" marked variables; found %"PRIu32" ***\n",
            solver->buffer.size, n);
  } else {
    check_buffer_marks(solver);
  }
}


/*
 * When we've simplified the learned clause: no variable should be marked
 */
static void check_all_unmarked(const sat_solver_t *solver) {
  uint32_t n;

  n = num_marked_variables(solver);
  if (n > 0) {
    fprintf(stderr, "*** BUG: found %"PRIu32" marked variables: should be 0 ***\n", n);
    fflush(stderr);
  }
}


/**********************
 *  ELIMINATION HEAP  *
 *********************/

static void check_elim_heap(const sat_solver_t *solver) {
  const elim_heap_t *heap;
  uint32_t i, n;
  bvar_t x;

  heap = &solver->elim;
  n = heap->size;
  for (i=2; i<n; i++) {
    if (elim_lt(solver, heap->data[i], heap->data[i>>1])) {
      fprintf(stderr, "*** BUG: invalid elimination heap: at index %"PRIu32" ***\n", i);
      fflush(stderr);
    }
  }

  for (i=0; i<n; i++) {
    x = heap->data[i];
    if (heap->elim_idx[x] != i) {
      fprintf(stderr, "*** BUG: invalid heap index: data[%"PRIu32"] = %"PRIu32", but elim_idx[%"PRIu32"] /= %"PRIu32" ***\n", i, x, x, i);
      fflush(stderr);
    }
  }

  for (x=0; x<solver->nvars; x++) {
    if (heap->elim_idx[x] >= 0) {
      i = heap->elim_idx[x];
      if (i >= heap->size) {
        fprintf(stderr, "*** BUG: bad elim_idx for variable %"PRIu32": index = %"PRIu32", heap size = %"PRIu32"\n", x, i, heap->size);
        fflush(stderr);
      }
      if (heap->data[i] != x) {
        fprintf(stderr, "*** BUG: invalid data: elim_idx[%"PRIu32"] = %"PRIu32", but data[%"PRIu32"] /= %"PRIu32" ***\n", x, i, i, x);
        fflush(stderr);
      }
    }
  }
}

#endif
