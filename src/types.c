/*
 * Type table and hash consing
 */

#include <string.h>
#include <assert.h>

#include "yices_limits.h"
#include "memalloc.h"
#include "refcount_strings.h"
#include "hash_functions.h"
#include "types.h"



/*
 * Finalizer for typenames in the symbol table. This function is 
 * called when record r is deleted from the symbol table.
 * All symbols must be generated by the clone function, and have
 * a reference counter (cf. refcount_strings.h).
 */
static void typename_finalizer(stbl_rec_t *r) {
  string_decref(r->string);
}


/*
 * Initialize table, with initial size = n.
 */
static void type_table_init(type_table_t *table, uint32_t n) {
  // abort if the size is too large
  if (n >= YICES_MAX_TYPES) {
    out_of_memory();
  }

  table->kind = (uint8_t *) safe_malloc(n * sizeof(uint8_t));
  table->desc = (type_desc_t *) safe_malloc(n * sizeof(type_desc_t));
  table->card = (uint32_t *) safe_malloc(n * sizeof(uint32_t));
  table->flags = (uint8_t *) safe_malloc(n * sizeof(uint8_t));
  table->name = (char **) safe_malloc(n * sizeof(char *));
  
  table->size = n;
  table->nelems = 0;
  table->free_idx = NULL_TYPE;

  init_int_htbl(&table->htbl, 0); // use default size
  init_stbl(&table->stbl, 0);     // default size too

  // install finalizer in the symbol table
  stbl_set_finalizer(&table->stbl, typename_finalizer);

  // don't allocate sup/inf table
  table->sup_tbl = NULL;
  table->inf_tbl = NULL;
}


/*
 * Extend the table: make it 50% larger
 */
static void type_table_extend(type_table_t *table) {
  uint32_t n;

  /*
   * new size = 1.5 * (old_size + 1) approximately
   * this computation can't overflow since old_size < YICES_MAX_TYPE
   * this also ensures that new size > old size (even if old_size <= 1).
   */
  n = table->size + 1;
  n += n >> 1;
  if (n >= YICES_MAX_TYPES) {
    out_of_memory(); 
  }

  table->kind = (uint8_t *) safe_realloc(table->kind, n * sizeof(uint8_t));
  table->desc = (type_desc_t *) safe_realloc(table->desc, n * sizeof(type_desc_t));
  table->card = (uint32_t *) safe_realloc(table->card, n * sizeof(uint32_t));
  table->flags = (uint8_t *) safe_realloc(table->flags, n * sizeof(uint8_t));
  table->name = (char **) safe_realloc(table->name, n * sizeof(char *));

  table->size = n;
}


/*
 * Get a free type id and initializes its name to NULL.
 * The other fields are not initialized.
 */
static type_t allocate_type_id(type_table_t *table) {
  type_t i;

  i = table->free_idx;
  if (i >= 0) {
    table->free_idx = table->desc[i].next;
  } else {
    i = table->nelems;
    table->nelems ++;
    if (i >= table->size) {
      type_table_extend(table);
    }
  }
  table->name[i] = NULL;

  return i;
}


#if 0

// NOT USED YET 
/*
 * Erase type i: free its descriptor and add i to the free list
 */
static void erase_type(type_table_t *table, type_t i) {
  switch (table->kind[i]) {
  case UNUSED_TYPE: // already deleted
  case BOOL_TYPE:
  case INT_TYPE:
  case REAL_TYPE:
    return; // never delete predefined types

  case BITVECTOR_TYPE:
  case SCALAR_TYPE:
  case UNINTERPRETED_TYPE:
    break;

  case TUPLE_TYPE:
  case FUNCTION_TYPE:
    safe_free(table->desc[i].ptr);
    break;
  }

  if (table->name[i] != NULL) {
    string_decref(table->name[i]);
    table->name[i] = NULL;
  }

  table->kind[i] = UNUSED_TYPE;
  table->desc[i].next = table->free_idx;
  table->free_idx = i;
}

#endif



/*
 * INTERNAL CACHES
 */

/*
 * Get the sup_table: create and initialize it if needed
 */
static int_hmap2_t *get_sup_table(type_table_t *table) {
  int_hmap2_t *hmap;

  hmap = table->sup_tbl;
  if (hmap == NULL) {
    hmap = (int_hmap2_t *) safe_malloc(sizeof(int_hmap2_t));
    init_int_hmap2(hmap, 0); // default size
    table->sup_tbl = hmap;
  }

  return hmap;
}


/*
 * Get the inf_table: create and initialize it if needed
 */
static int_hmap2_t *get_inf_table(type_table_t *table) {
  int_hmap2_t *hmap;

  hmap = table->inf_tbl;
  if (hmap == NULL) {
    hmap = (int_hmap2_t *) safe_malloc(sizeof(int_hmap2_t));
    init_int_hmap2(hmap, 0); // default size
    table->inf_tbl = hmap;
  }

  return hmap;
}






/*
 * SUPPORT FOR CARD/FLAGS COMPUTATION
 */

/*
 * Build the conjunction of flags for types a[0 ... n-1]
 *
 * In the result we have
 * - finite flag = 1 if a[0] ... a[n-1] are all finite
 * - unit   flag = 1 if a[0] ... a[n-1] are all unit types
 * - exact  flag = 1 if a[0] ... a[n-1] are all small or unit types
 * - max    flag = 1 if a[0] ... a[n-1] are all maximal types
 * - min    flag = 1 if a[0] ... a[n-1] are all minimal types
 */
static uint32_t type_flags_conjunct(type_table_t *table, uint32_t n, type_t *a) {
  uint32_t i, flg;

  flg = UNIT_TYPE_FLAGS;
  for (i=0; i<n; i++) {
    flg &= type_flags(table, a[i]);
  }

  return flg;
}


/*
 * Product of cardinalities of all types in a[0 ... n-1]
 * - return a value > UINT32_MAX if there's an overflow
 */
static uint64_t type_card_product(type_table_t *table, uint32_t n, type_t *a) {
  uint64_t prod;
  uint32_t i;

  prod = 1;
  for (i=0; i<n; i++) {
    prod *= type_card(table, a[i]);
    if (prod > UINT32_MAX) break;
  }
  return prod;
}


/*
 * Compute the cardinality of function type e[0] ... e[n-1] --> r
 * - all types e[0] ... e[n-1] must be small or unit
 * - r must be small
 * - return a value > UINT32_MAX if there's an overflow
 */
static uint64_t fun_type_card(type_table_t *table, uint32_t n, type_t *e, type_t r) {
  uint64_t power, dom;
  uint32_t range;

  dom = type_card_product(table, n, e);  // domain size
  if (dom >= 32) {
    // since the range has size 2 or more
    // power = range^dom does not fit in 32bits
    power = UINT32_MAX+1;
  } else {
    // compute power = range^dom
    // since dom is small we do this the easy way
    range = type_card(table, r);
    assert(2 <= range && dom >= 1);
    power = range;
    while (dom > 1) {
      power *= range;
      if (power > UINT32_MAX) break;
      dom --;
    }
  }

  return power;
}





/*
 * TYPE CREATION
 */

/*
 * Add the three predefined types
 */
static void add_primitive_types(type_table_t *table) {
  type_t i;

  i = allocate_type_id(table);
  assert(i == bool_id);
  table->kind[i] = BOOL_TYPE;
  table->desc[i].ptr = NULL;
  table->card[i] = 2;
  table->flags[i] = SMALL_TYPE_FLAGS;

  i = allocate_type_id(table);
  assert(i == int_id);
  table->kind[i] = INT_TYPE;
  table->desc[i].ptr = NULL;
  table->card[i] = UINT32_MAX;
  table->flags[i] = (INFINITE_TYPE_FLAGS | TYPE_IS_MINIMAL_MASK);

  i = allocate_type_id(table);
  assert(i == real_id);
  table->kind[i] = REAL_TYPE;
  table->desc[i].ptr = NULL;
  table->card[i] = UINT32_MAX;
  table->flags[i] = (INFINITE_TYPE_FLAGS | TYPE_IS_MAXIMAL_MASK);
}




/*
 * Add type (bitvector k) and return its id
 * - k must be positive and no more than YICES_MAX_BVSIZE
 */
static type_t new_bitvector_type(type_table_t *table, uint32_t k) {
  type_t i;

  assert(0 < k && k <= YICES_MAX_BVSIZE);

  i = allocate_type_id(table);
  table->kind[i] = BITVECTOR_TYPE;
  table->desc[i].integer = k;
  if (k < 32) {
    table->card[i] = ((uint32_t) 1) << (k - 1);
    table->flags[i] = SMALL_TYPE_FLAGS;
  } else {
    table->card[i] = UINT32_MAX;
    table->flags[i] = LARGE_TYPE_FLAGS;
  }

  return i;
}


/*
 * Add a scalar type and return its id
 * - k = number of elements in the type 
 * - k must be positive.
 */
type_t new_scalar_type(type_table_t *table, uint32_t k) {
  type_t i;

  assert(k > 0);

  i = allocate_type_id(table);
  table->kind[i] = SCALAR_TYPE;
  table->desc[i].integer = k;
  table->card[i] = k;
  table->flags[i] = SMALL_TYPE_FLAGS;

  return i;
}


/*
 * Add a new uninterpreted type and return its id
 * - the type is infinite and both minimal and maximal
 */
type_t new_uninterpreted_type(type_table_t *table) {
  type_t i;

  i = allocate_type_id(table);
  table->kind[i] = UNINTERPRETED_TYPE;
  table->desc[i].ptr = NULL;
  table->card[i] = UINT32_MAX;
  table->flags[i] = (INFINITE_TYPE_FLAGS | TYPE_IS_MAXIMAL_MASK | TYPE_IS_MINIMAL_MASK);

  return i;
}


/*
 * Add tuple type: e[0], ..., e[n-1]
 */
static type_t new_tuple_type(type_table_t *table, uint32_t n, type_t *e) {
  tuple_type_t *d;
  uint64_t card;
  type_t i;
  uint32_t j, flag;

  assert(0 < n && n <= YICES_MAX_ARITY);

  d = (tuple_type_t *) safe_malloc(sizeof(tuple_type_t) + n * sizeof(type_t));
  d->nelem = n;
  for (j=0; j<n; j++) d->elem[j] = e[j];

  i = allocate_type_id(table);
  table->kind[i] = TUPLE_TYPE;
  table->desc[i].ptr = d;

  /*
   * set flags and card
   * - type_flags_conjunct sets all the bits correctky
   *   except possibly the exact card bit
   */
  flag = type_flags_conjunct(table, n, e);
  switch (flag) {
  case UNIT_TYPE_FLAGS: 
    // all components are unit types
    card = 1;
    break;

  case SMALL_TYPE_FLAGS:
    // all components are unit or small types
    card = type_card_product(table, n, e);
    if (card > UINT32_MAX) { 
      // the product does not fit in 32bits
      // change exact card to inexact card
      card = UINT32_MAX;
      flag = LARGE_TYPE_FLAGS;
    }
    break;

  default:
    assert(flag == LARGE_TYPE_FLAGS || 
	   (flag & CARD_FLAGS_MASK) == INFINITE_TYPE_FLAGS);
    card = UINT32_MAX;
    break;
  }

  assert(0 < card && card <= UINT32_MAX);
  table->card[i] = card;
  table->flags[i] = flag;

  return i;
}


/*
 * Add function type: (e[0], ..., e[n-1] --> r)
 */
static type_t new_function_type(type_table_t *table, uint32_t n, type_t *e, type_t r) {
  function_type_t *d;
  uint64_t card;
  type_t i;
  uint32_t j, flag, minmax;
  
  assert(0 < n && n <= YICES_MAX_ARITY);

  d = (function_type_t *) safe_malloc(sizeof(function_type_t) + n * sizeof(type_t));
  d->range = r;
  d->ndom = n;
  for (j=0; j<n; j++) d->domain[j] = e[j];

  i = allocate_type_id(table);
  table->kind[i] = FUNCTION_TYPE;
  table->desc[i].ptr = d;

  /*
   * Three of the function type's flags are inherited from the range:
   * - fun type is unit iff range is unit
   * - fun type is maximal iff range is maximal
   * - fun type is minimal iff range is minimal
   */
  flag = type_flags(table, r);
  minmax = flag & MINMAX_FLAGS_MASK; // save min and max bits

  /*
   * If the range is finite but not unit, then we check
   * whether all domains are finite.
   */
  if ((flag & (TYPE_IS_FINITE_MASK|TYPE_IS_UNIT_MASK)) == TYPE_IS_FINITE_MASK) {
    assert(flag == SMALL_TYPE_FLAGS || flag == LARGE_TYPE_FLAGS);
    flag &= type_flags_conjunct(table, n, e);
  }

  switch (flag) {
  case UNIT_TYPE_FLAGS:
    // singleton range so the function type is also a singleton
    card = 1;
    break;

  case SMALL_TYPE_FLAGS:
    // the range is small finite
    // all domains are small finite or unit
    card = fun_type_card(table, n, e, r);
    if (card > UINT32_MAX) {
      card = UINT32_MAX;
      flag = LARGE_TYPE_FLAGS;
    }
    break;

  default:
    // the range or at least one domain is infinite
    // or the range and all domains are finite but at least one 
    // of them is large.
    assert(flag == LARGE_TYPE_FLAGS || 
	   (flag | CARD_FLAGS_MASK) == INFINITE_TYPE_FLAGS);
    card = UINT32_MAX;
    break;
  }

  assert(0 < card && card <= UINT32_MAX);
  table->card[i] = card;
  table->flags[i] = minmax | (flag & CARD_FLAGS_MASK);

  return i;
}



/*
 * HASH CONSING
 */

/*
 * Objects for hash-consing
 */
typedef struct bv_type_hobj_s {
  int_hobj_t m;      // methods
  type_table_t *tbl;
  uint32_t size;
} bv_type_hobj_t;

typedef struct tuple_type_hobj_s {
  int_hobj_t m;
  type_table_t *tbl;
  uint32_t n;
  type_t *elem;
} tuple_type_hobj_t;

typedef struct function_type_hobj_s {
  int_hobj_t m;
  type_table_t *tbl;
  type_t range;
  uint32_t n;
  type_t *dom;
} function_type_hobj_t;


/*
 * Hash functions
 */
static uint32_t hash_bv_type(bv_type_hobj_t *p) {
  return jenkins_hash_pair(p->size, 0, 0x7838abe2);
}

static uint32_t hash_tuple_type(tuple_type_hobj_t *p) {
  return jenkins_hash_intarray_var(p->n, p->elem, 0x8193ea92);
}

static uint32_t hash_function_type(function_type_hobj_t *p) {
  uint32_t h;

  h = jenkins_hash_intarray_var(p->n, p->dom, 0x5ad7b72f);
  return jenkins_hash_pair(p->range, 0, h);
}


/*
 * Comparison functions
 */
static bool eq_bv_type(bv_type_hobj_t *p, type_t i) {
  type_table_t *table;

  table = p->tbl;
  return table->kind[i] == BITVECTOR_TYPE && table->desc[i].integer == p->size;
}

static bool eq_tuple_type(tuple_type_hobj_t *p, type_t i) {
  type_table_t *table;
  tuple_type_t *d;
  int32_t j;

  table = p->tbl;
  if (table->kind[i] != TUPLE_TYPE) return false;

  d = (tuple_type_t *) table->desc[i].ptr;
  if (d->nelem != p->n) return false;

  for (j=0; j<p->n; j++) {
    if (d->elem[j] != p->elem[j]) return false;
  }

  return true;
}

static bool eq_function_type(function_type_hobj_t *p, type_t i) {
  type_table_t *table;
  function_type_t *d;
  int32_t j;

  table = p->tbl;
  if (table->kind[i] != FUNCTION_TYPE) return false;

  d = (function_type_t *) table->desc[i].ptr;
  if (d->range != p->range || d->ndom != p->n) return false;

  for (j=0; j<p->n; j++) {
    if (d->domain[j] != p->dom[j]) return false;
  }

  return true;
}


/*
 * Builder functions
 */
static type_t build_bv_type(bv_type_hobj_t *p) {
  return new_bitvector_type(p->tbl, p->size);
}

static type_t build_tuple_type(tuple_type_hobj_t *p) {
  return new_tuple_type(p->tbl, p->n, p->elem);
}

static type_t build_function_type(function_type_hobj_t *p) {
  return new_function_type(p->tbl, p->n, p->dom, p->range);
}


/*
 * Global Hash Objects
 */
static bv_type_hobj_t bv_hobj = {
  { (hobj_hash_t) hash_bv_type, (hobj_eq_t) eq_bv_type, 
    (hobj_build_t) build_bv_type },
  NULL,
  0,
};

static tuple_type_hobj_t tuple_hobj = {
  { (hobj_hash_t) hash_tuple_type, (hobj_eq_t) eq_tuple_type, 
    (hobj_build_t) build_tuple_type },
  NULL,
  0,
  NULL,
};

static function_type_hobj_t function_hobj = {
  { (hobj_hash_t) hash_function_type, (hobj_eq_t) eq_function_type,
    (hobj_build_t) build_function_type },
  NULL,
  0,
  0,
  NULL,
};





/*
 * TABLE MANAGEMENT + EXPORTED TYPE CONSTRUCTORS
 *
 * NOTE: The constructors for uninterpreted and scalar types
 * are defined above. Thay don't use hash consing.
 */


/*
 * Initialize table: add the predefined types
 */
void init_type_table(type_table_t *table, uint32_t n) {
  type_table_init(table, n);
  add_primitive_types(table);
}

/*
 * Delete table: free all allocated memory
 */
void delete_type_table(type_table_t *table) {
  uint32_t i;

  // decrement refcount for all names
  for (i=0; i<table->nelems; i++) {
    if (table->name[i] != NULL) {
      string_decref(table->name[i]);
    }
  }

  // delete all allocated descriptors
  for (i=0; i<table->nelems; i++) {
    if (table->kind[i] == TUPLE_TYPE || table->kind[i] == FUNCTION_TYPE) {
      safe_free(table->desc[i].ptr);
    }
  }

  safe_free(table->kind);
  safe_free(table->desc);
  safe_free(table->card);
  safe_free(table->flags);
  safe_free(table->name);

  table->kind = NULL;
  table->desc = NULL;
  table->card = NULL;
  table->flags = NULL;
  table->name = NULL;

  delete_int_htbl(&table->htbl);
  delete_stbl(&table->stbl);

  if (table->sup_tbl != NULL) {
    delete_int_hmap2(table->sup_tbl);
    safe_free(table->sup_tbl);
    table->sup_tbl = NULL;
  }

  if (table->inf_tbl != NULL) {
    delete_int_hmap2(table->inf_tbl);
    safe_free(table->inf_tbl);
    table->inf_tbl = NULL;
  }
}


/*
 * Bitvector type
 */
type_t bv_type(type_table_t *table, uint32_t size) {
  assert(size > 0);
  bv_hobj.tbl = table;
  bv_hobj.size = size;
  return int_htbl_get_obj(&table->htbl, (int_hobj_t *) &bv_hobj);
}

/*
 * Tuple type
 */
type_t tuple_type(type_table_t *table, uint32_t n, type_t elem[]) {
  assert(0 < n && n <= YICES_MAX_ARITY);
  tuple_hobj.tbl = table;
  tuple_hobj.n = n;
  tuple_hobj.elem = elem;
  return int_htbl_get_obj(&table->htbl, (int_hobj_t *) &tuple_hobj);
}

/*
 * Function type
 */
type_t function_type(type_table_t *table, type_t range, uint32_t n, type_t dom[]) {
  assert(0 < n && n <= YICES_MAX_ARITY);
  function_hobj.tbl = table;
  function_hobj.range = range;
  function_hobj.n = n;
  function_hobj.dom = dom;
  return int_htbl_get_obj(&table->htbl, (int_hobj_t *) &function_hobj);  
}




/*
 * Assign name to type i.
 * - previous mapping of name to other types (if any) are hidden.
 * - name must have a reference counter attached to it (cf. clone_string
 *   in memalloc.h).
 */
void set_type_name(type_table_t *table, type_t i, char *name) {
  if (table->name[i] == NULL) {
    table->name[i] = name;
    string_incref(name);
  }
  stbl_add(&table->stbl, name, i);
  string_incref(name);
}

/*
 * Get type mapped to the name (or NULL_TYPE)
 */
type_t get_type_by_name(type_table_t *table, char *name) {
  // NULL_TYPE = -1 and stbl_find returns -1 if name is absent
  return stbl_find(&table->stbl, name);
}

/*
 * Remove a type name.
 */
void remove_type_name(type_table_t *table, char *name) {
  stbl_remove(&table->stbl, name);
}



/*
 * CARDINALITY
 */

/*
 * Approximate cardinality of tau[0] x ... x tau[n-1]
 * - returns the same value as card_of(tuple_type(tau[0] ... tau[n-1])) but does not
 *   construct the tuple type.
 */
uint32_t card_of_type_product(type_table_t *table, uint32_t n, type_t *tau) {
  uint64_t card;

  card = type_card_product(table, n, tau);
  if (card > UINT32_MAX) {
    card = UINT32_MAX;
  }
  assert(1 <= card && card <= UINT32_MAX);

  return (uint32_t) card;
}



/*
 * Approximate cardinality of the domain and range of a function type tau
 */
uint32_t card_of_domain_type(type_table_t *table, type_t tau) {
  function_type_t *d;

  d = function_type_desc(table, tau);
  return card_of_type_product(table, d->ndom, d->domain);
}

uint32_t card_of_range_type(type_table_t *table, type_t tau) {
  return type_card(table, function_type_range(table, tau));
}



/*
 * Check whether a function type has a finite domain or range
 * - tau must be a function type.
 */
bool type_has_finite_domain(type_table_t *table, type_t tau) {
  function_type_t *fun;
  uint32_t flag;

  fun = function_type_desc(table, tau);
  flag = type_flags_conjunct(table, fun->ndom, fun->domain);
  return flag & TYPE_IS_FINITE_MASK;
}

bool type_has_finite_range(type_table_t *table, type_t tau) {
  return is_finite_type(table, function_type_range(table, tau));
}





#if 0
/*
 * SUBTYPE RELATION
 */

/*
 * Sup of tau1 and tau2:
 * - returns the smallest type tau such that tau1 <= tau and tau2 <= tau
 * - returns NULL_TYPE if tau1 and tau2 are incompatible
 */
// construct (tuple-type (sup a[0] b[0]) ... (sup a[n-1] b[n-1]))
static type_t sup_tuple_types(type_table_t *table, uint32_t n, type_t *a, type_t *b) {
  type_t tmp[n]; // Warning: GCC/C99 extension 
  type_t aux;
  uint32_t i;

  for (i=0; i<n; i++) {
    aux = super_type(table, a[i], b[i]);
    if (aux == NULL_TYPE) return aux;
    tmp[i] = aux;
  }

  return tuple_type(table, n, tmp);
}

// construct (fun-type a[0] ... a[n-1] --> (sup tau1 tau2))
static type_t sup_fun_types(type_table_t *table, uint32_t n, type_t *a, type_t tau1, type_t tau2) {
  type_t range;

  range = super_type(table, tau1, tau2);
  if (range == NULL_TYPE) return range;

  return function_type(table, range, n, a);

}


/*
 * Compute the smallest supertype of tau1 and tau2 and stroe 
 * the result in the sup_tbl cache.
 */
static type_t super_type_recur(type_table_t *table, int_hmap2_t *sup_tbl, type_t tau1, type_t tau2) {  
  int_hmap2_rec_t *r;
  bool new_rec;

  assert(table->sup_tbl == sup_tbl && sup_tbl != NULL && 
	 good_type(table, tau1) && good_type(table, tau2));

  r = int_hmap2_get(sup_tbl, tau1, tau2, &new);
  if (new) {
    // the super type is not in the cache
    
  }

  return r->val;
}


/*
 * Compute the smallest supertype of tau1 and tau2
 * - return NULL_TYPE if tau1 and tau2 are not compatible.
 */
type_t super_type(type_table_t *table, type_t tau1, type_t tau2) {
  assert(good_type(table, tau1) && good_type(table, tau2));

  if (tau1 == tau2) {
    return tau1;
  }

  if ((tau1 == int_id && tau2 == real_id) || 
      (tau1 == real_id && tau2 == int_id)) {
    return real_id;
  }

  if (table->kind[tau1] != table->kind[tau2]) {
    return NULL_TYPE;
  }

  if (table->kind[tau1] <= UNINTERPRETED_TYPE) {
    return NULL_TYPE;
  }
  
  // for two tuple types or two function types
  // use sup_tbl as a cache.
  return super_type_recur(table, get_sup_table(table), tau1, tau2);
}

#endif



#if 0

/*
 * TYPE CHECKING
 */

/*
 * Check whether tau1 is a subtype of tau2, using the rules
 * 1) int <= real
 * 2) tau <= tau
 * 3) if tau_1 <= sigma_1 ... tau_n <= sigma_n then
 *   (tuple-type tau_1 ... tau_n) <= (tuple-typle sigma_1 ... sigma_n)
 * 4) if sigma_1 <= sigma_2 then 
 *   (tau_1 ... tau_n --> sigma_1) <= (tau_1 ... tau_n -> sigma_2)
 */
bool is_subtype(type_table_t *table, type_t tau1, type_t tau2) {
  tuple_type_t *tup1, *tup2;
  function_type_t *fun1, *fun2;
  int32_t i, n;

  assert(good_type(table, tau1) && good_type(table, tau2));

  if (tau1 == tau2 || (tau1 == int_id && tau2 == real_id)) {
    return true;
  }

  switch (table->kind[tau1]) {
  case TUPLE_TYPE: 
    if (table->kind[tau2] == TUPLE_TYPE) {
      tup1 = tuple_type_desc(table, tau1);
      tup2 = tuple_type_desc(table, tau2);
      n = tup1->nelem;
      if (n != tup2->nelem) {
	return false;
      }
      for (i=0; i<n; i++) {
	if (! is_subtype(table, tup1->elem[i], tup2->elem[i])) {
	  return false;
	}
      }
      return true;
    }
    break;

  case FUNCTION_TYPE:
    if (table->kind[tau2] == FUNCTION_TYPE) {
      fun1 = function_type_desc(table, tau1);
      fun2 = function_type_desc(table, tau2);
      n = fun1->ndom;
      if (n != fun2->ndom) {
	return false;
      }
      for (i=0; i<n; i++) {
	if (fun1->domain[i] != fun2->domain[i]) {
	  return false;
	}
      }

      return is_subtype(table, fun1->range, fun2->range);
    }
    break;

  default:
    break;
  }

  return false;
}




type_t super_type(type_table_t *table, type_t tau1, type_t tau2) {
  tuple_type_t *tup1, *tup2;
  function_type_t *fun1, *fun2;
  int32_t i, n;

  if (tau1 == tau2) {
    return tau1;
  }
  if ((tau1 == int_id && tau2 == real_id) || 
      (tau1 == real_id && tau2 == int_id)) {
    return real_id;
  }

  switch (table->kind[tau1]) {
  case TUPLE_TYPE:
    if (table->kind[tau2] == TUPLE_TYPE) {
      tup1 = tuple_type_desc(table, tau1);
      tup2 = tuple_type_desc(table, tau2);
      n = tup1->nelem;
      if (n != tup2->nelem) {
	return NULL_TYPE;
      }
      return sup_tuple_types(table, n, tup1->elem, tup2->elem);
    }
    break;

  case FUNCTION_TYPE:
    if (table->kind[tau2] == FUNCTION_TYPE) {
      fun1 = function_type_desc(table, tau1);
      fun2 = function_type_desc(table, tau2);
      n = fun1->ndom;
      if (n != fun2->ndom) {
	return NULL_TYPE;
      }

      for (i=0; i<n; i++) {
	if (fun1->domain[i] != fun2->domain[i]) {
	  return NULL_TYPE;
	}
      }
      return sup_fun_types(table, n, fun1->domain, fun1->range, fun2->range);
    }
    break;

  default:
    break;
  }

  return NULL_TYPE;
}




/*
 * Inf of tau1 and tau2:
 * - returns the largest type tau such that tau <= tau1 and tau <= tau2
 * - returns NULL_TYPE if tau1 and tau2 are incompatible
 */
// construct (tuple-type (inf a[0] b[0]) ... (inf a[n-1] b[n-1]))
static type_t inf_tuple_types(type_table_t *table, uint32_t n, type_t *a, type_t *b) {
  type_t tmp[n]; // Warning: GCC/C99 extension 
  type_t aux;
  uint32_t i;

  for (i=0; i<n; i++) {
    aux = inf_type(table, a[i], b[i]);
    if (aux == NULL_TYPE) return aux;
    tmp[i] = aux;
  }

  return tuple_type(table, n, tmp);
}

// construct (fun-type a[0] ... a[n-1] --> (inf tau1 tau2))
static type_t inf_fun_types(type_table_t *table, uint32_t n, type_t *a, type_t tau1, type_t tau2) {
  type_t range;

  range = inf_type(table, tau1, tau2);
  if (range == NULL_TYPE) return range;

  return function_type(table, range, n, a);

}

type_t inf_type(type_table_t *table, type_t tau1, type_t tau2) {
  tuple_type_t *tup1, *tup2;
  function_type_t *fun1, *fun2;
  int32_t i, n;

  if (tau1 == tau2) {
    return tau1;
  }
  if ((tau1 == int_id && tau2 == real_id) || 
      (tau1 == real_id && tau2 == int_id)) {
    return int_id;
  }


  switch (table->kind[tau1]) {
  case TUPLE_TYPE:
    if (table->kind[tau2] == TUPLE_TYPE) {
      tup1 = tuple_type_desc(table, tau1);
      tup2 = tuple_type_desc(table, tau2);
      n = tup1->nelem;
      if (n != tup2->nelem) {
	return NULL_TYPE;
      }

      return inf_tuple_types(table, n, tup1->elem, tup2->elem);
    }
    break;

  case FUNCTION_TYPE:
    if (table->kind[tau2] == FUNCTION_TYPE) {
      fun1 = function_type_desc(table, tau1);
      fun2 = function_type_desc(table, tau2);
      n = fun1->ndom;
      if (n != fun2->ndom) {
	return NULL_TYPE;
      }

      for (i=0; i<n; i++) {
	if (fun1->domain[i] != fun2->domain[i]) {
	  return NULL_TYPE;
	}
      }

      return inf_fun_types(table, n, fun1->domain, fun1->range, fun2->range);
    }
    break;

  default:
    break;
  }

  return NULL_TYPE;
}


/*
 * Check whether tau1 and tau2 are compatible
 * (i.e., have a common supertype)
 */
bool compatible_types(type_table_t *table, type_t tau1, type_t tau2) {
  tuple_type_t *tup1, *tup2;
  function_type_t *fun1, *fun2;
  int32_t i, n;

  if (tau1 == tau2 ||
      (tau1 == int_id && tau2 == real_id) ||
      (tau1 == real_id && tau2 == int_id)) {
    return true;
  }

  switch (table->kind[tau1]) {
  case TUPLE_TYPE:
    if (table->kind[tau2] == TUPLE_TYPE) {
      // Two tuple types
      tup1 = tuple_type_desc(table, tau1);
      tup2 = tuple_type_desc(table, tau2);
      n = tup1->nelem;
      if (n != tup2->nelem) {
	return false;
      }
      for (i=0; i<n; i++) {
	if (! compatible_types(table, tup1->elem[i], tup2->elem[i])) {
	  return false;
	}
      }
      return true;
    }
    break;

  case FUNCTION_TYPE:
    if (table->kind[tau2] == FUNCTION_TYPE) {
      // Two function types
      fun1 = function_type_desc(table, tau1);
      fun2 = function_type_desc(table, tau2);
      n = fun1->ndom;
      if (n != fun2->ndom) {
	return false;
      }

      for (i=0; i<n; i++) {
	if (fun1->domain[i] != fun2->domain[i]) {
	  return false;
	}
      }
      return compatible_types(table, fun1->range, fun2->range);
    }
    break;

  default:
    break;
  }

  return false;
}









/*
 * GARBAGE COLLECTION
 */
void set_root_type_flag(type_table_t *table, type_t i) {
  set_bit(table->root, i);
}

void clr_root_type_flag(type_table_t *table, type_t i) {
  clr_bit(table->root, i);
}


/*
 * Remove i from the hash-cons table
 */
static uint32_t hash_bvtype(int32_t size) {
  return jenkins_hash_pair(size, 0, 0x7838abe2);  
}

static uint32_t hash_tupletype(tuple_type_t *p) {
  return jenkins_hash_intarray_var(p->nelem, p->elem, 0x8193ea92);
}

static uint32_t hash_funtype(function_type_t *p) {
  uint32_t h;
  h = jenkins_hash_intarray_var(p->ndom, p->domain, 0x5ad7b72f);
  return jenkins_hash_pair(p->range, 0, h);
}

static void erase_hcons_type(type_table_t *table, type_t i) {
  uint32_t k;

  switch (table->kind[i]) {
  case BITVECTOR_TYPE:
    k = hash_bvtype(table->desc[i].integer);
    break;

  case TUPLE_TYPE:
    k = hash_tupletype(table->desc[i].ptr);
    break;

  case FUNCTION_TYPE:
    k = hash_funtype(table->desc[i].ptr);
    break;

  default: 
    return;
  }

  int_htbl_erase_record(&table->htbl, k, i);
}

/*
 * Push element t into if it's not already marked
 */
static void gc_mark_push_type(int_queue_t *q, byte_t *mark, type_t t) {
  if (! tst_bit(mark, t)) {
    set_bit(mark, t);
    int_queue_push(q, t);
  }
}

/*
 * Push all unmarked elements of array a into q, and mark them
 */
static void gc_mark_push_array(int_queue_t *q, byte_t *mark, type_t *a, int32_t n) {
  int32_t i;
  for (i=0; i<n; i++) {
    gc_mark_push_type(q, mark, a[i]);
  }
}

/*
 * Mark type t to prevent its deletion. Must be called only from
 * within a gc_notifier. All types reachable from t's definition
 * are marked too.
 */
void gc_mark_type(type_table_t *table, type_t t) {
  int_queue_t *q;
  byte_t *mark;
  tuple_type_t *td;
  function_type_t *fd;
  
  assert(table->gc_mark != NULL);
  assert(table->gc_mark_queue != NULL);
  assert(int_queue_is_empty(table->gc_mark_queue));

  mark = table->gc_mark;
  if (! tst_bit(mark, t)) {
    set_bit(mark, t);
    q = table->gc_mark_queue;
    for (;;) {
      switch (table->kind[t]) {
      case TUPLE_TYPE:
	td = (tuple_type_t *) table->desc[t].ptr;
	gc_mark_push_array(q, mark, td->elem, td->nelem);
	break;

      case FUNCTION_TYPE:
	fd = (function_type_t *) table->desc[t].ptr;
	gc_mark_push_type(q, mark, fd->range);
	gc_mark_push_array(q, mark, fd->domain, fd->ndom);
	break;
      }

      if (int_queue_is_empty(q)) break;
      t = int_queue_pop(q);
    }
  }
  
}

/*
 * Mark all types with root_flag set and all types accessible from the symbol table.
 */
static void mark_live_types(type_table_t *table) {
  stbl_t *sym_table;
  stbl_bank_t *b;
  stbl_rec_t *r;
  uint32_t k;
  type_t i;

  // scan symbol table
  sym_table = &table->stbl;
  k = sym_table->free_idx;
  for (b = sym_table->bnk; b != NULL; b = b->next) {
    for (r = b->block + k; r < b->block + STBL_BANK_SIZE; r ++) {
      if (r->string != NULL) {
	gc_mark_type(table, r->value);
      }
    }
    k = 0;
  }

  // mark every type with root_flag == 1
  for (i=0; i<table->nelems; i++) {
    if (table->kind[i] != UNUSED_TYPE && tst_bit(table->root, i)) {
      gc_mark_type(table, i);
    }
  }
}


/*
 * Trigger garbage collection
 */
void type_table_garbage_collection(type_table_t *table) {
  int32_t n;
  int_queue_t queue;
  byte_t *mark;
  type_t i;

  // allocate/initialize gc_mark and mark_queue
  init_int_queue(&queue, 0);
  n = table->nelems;
  mark = allocate_bitvector(n);
  clear_bitvector(mark, n);
  table->gc_mark = mark;
  table->gc_mark_queue = &queue;

  // mark types: primitive types + internal + any types marked by the notifier
  set_bit(mark, bool_id);
  set_bit(mark, int_id);
  set_bit(mark, real_id);
  mark_live_types(table);

  table->gc_notifier(table);

  // delete all unmarked types
  for (i=0; i<n; i++) {
    if (! tst_bit(mark, i)) {
      erase_hcons_type(table, i);
      erase_type(table, i);
    }
  }

  // cleanup
  table->gc_mark_queue = NULL;
  table->gc_mark = NULL;
  delete_bitvector(mark);
  delete_int_queue(&queue);
}

#endif
