/*
 * The Yices SMT Solver. Copyright 2014 SRI International.
 *
 * This program may only be used subject to the noncommercial end user
 * license agreement which is downloadable along with this program.
 */

/*
 * PUBLIC TYPES
 *
 * All types that are part of the API must be defined here.
 */

#ifndef __YICES_TYPES_H
#define __YICES_TYPES_H

#include <stdint.h>


/*********************
 *  TERMS AND TYPES  *
 ********************/

/*
 * Exported types
 * - term = index in a term table
 * - type = index in a type table
 */
typedef int32_t term_t;
typedef int32_t type_t;

/*
 * Error values
 */
#define NULL_TERM (-1)
#define NULL_TYPE (-1)



/************************
 *  CONTEXT AND MODELS  *
 ***********************/

/*
 * Context and models (opaque types)
 */
typedef struct context_s context_t;
typedef struct model_s model_t;



// MADE THIS PUBLIC FOR PRQA (NOT IN THE MAIN BRANCH)

/*
 * Possible branching heuristics:
 * - determine whether to assign the decision literal to true or false
 */
typedef enum {
  BRANCHING_DEFAULT,  // use internal smt_core cache
  BRANCHING_NEGATIVE, // branch l := false
  BRANCHING_POSITIVE, // branch l := true
  BRANCHING_THEORY,   // defer to the theory solver
  BRANCHING_TH_NEG,   // defer to theory solver for atoms, branch l := false otherwise
  BRANCHING_TH_POS,   // defer to theory solver for atoms, branch l := true otherwise
} branch_t;

#define NUM_BRANCHING_MODES 6

struct param_s {
  /*
   * Possible restart heuristics:
   * - as in Luby/Sinclair/Zuckerman, 1993
   * - like Picosat
   * - like Minisat
   *
   * If luby_restart is true: Luby-style
   * - c_threshold is uses as base period (10 is reasonable)
   * - the n-th restart occur after L_n * c_threshold conflicts
   *   when L_n is the n-the term in the sequence
   *    1, 1, 2, 1, 1, 2, 4, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 16, 1 ..
   *
   * If fast_restart is true and luby_restart is false: PICOSAT heuristic
   * - inner restarts based on c_threshold
   * - outer restarts based on d_threshold
   *
   * If fast_restart and luby_restart are false: MINISAT-style restarts
   * - c_threshold and c_factor are used
   * - d_threshold and d_threshold are ignored
   * - to get periodic restart set c_factor = 1.0
   */
  int32_t  luby_restart;
  int32_t  fast_restart;
  uint32_t c_threshold;     // initial value of c_threshold
  uint32_t d_threshold;     // initial value of d_threshold
  double   c_factor;        // increase factor for next c_threshold
  double   d_factor;        // increase factor for next d_threshold

  /*
   * Clause-deletion heuristic
   * - initial reduce_threshold is max(r_threshold, num_prob_clauses * r_fraction)
   * - increase by r_factor on every outer restart provided reduce was called in that loop
   */
  uint32_t r_threshold;
  double   r_fraction;
  double   r_factor;

  /*
   * SMT Core parameters:
   * - randomness and var_decay are used by the branching heuristic
   *   the default branching mode uses the cached polarity in smt_core.
   * - clause_decay influence clause deletion
   * - random seed
   *
   * SMT Core caching of theory lemmas:
   * - if cache_tclauses is true, then the core internally turns
   *   some theory lemmas into learned clauses
   * - for the core, a theory lemma is either a conflict reported by
   *   the theory solver or a theory implication
   * - a theory implication is considered for caching if it's involved
   *   in a conflict resolution
   * - parameter tclause_size controls the lemma size: only theory lemmas
   *   of size <= tclause_size are turned into learned clauses
   */
  double   var_decay;       // decay factor for variable activity
  float    randomness;      // probability of a random pick in select_unassigned_literal
  uint32_t random_seed;
  branch_t branching;       // branching heuristic
  float    clause_decay;    // decay factor for learned-clause activity

  /*
   * Budget: bound on the total number of conflicts
   * - if this bound is reached the search stops (result = SMT_INTERRUPTED)
   */
  uint64_t conflict_budget;
};


/*
 * Search parameters
 */
typedef struct param_s param_t;


/*
 * Context status code
 */
typedef enum smt_status {
  STATUS_IDLE,
  STATUS_SEARCHING,
  STATUS_UNKNOWN,
  STATUS_SAT,
  STATUS_UNSAT,
  STATUS_INTERRUPTED,
  STATUS_ERROR
} smt_status_t;





/********************************
 *  VECTORS OF TERMS OR TYPES   *
 *******************************/

/*
 * Some functions return a collection of terms or types
 * via a vector. The vector is an array that gets resized
 * by the library as needed.
 *
 * For each vector type, the API provide three functions:
 * - yices_init_xxx_vector(xxx_vector_t *v)
 * - yices_reset_xxx_vector(xxx_vector_t *v)
 * - yices_delete_xxx_vector(xxx_vector_t *v)
 *
 * The first function must be called first to initialize a vector.
 * The reset function can be used to empty vector v. It just resets
 * v->size to zero.
 * The delete function must be called to delete a vector that is no
 * longer needed. This is required to avoid memory leaks.
 */
typedef struct term_vector_s {
  uint32_t capacity;
  uint32_t size;
  term_t *data;
} term_vector_t;



/***********************
 *  TERM CONSTRUCTORS  *
 **********************/

/*
 * These codes are part of the term exploration API.
 */
typedef enum term_constructor {
  YICES_CONSTRUCTOR_ERROR = -1, // to report an error

  // atomic terms
  YICES_BOOL_CONSTANT,       // boolean constant
  YICES_BV_CONSTANT,         // bitvector constant
  YICES_UNINTERPRETED_TERM,  // (i.e., global variables, can't be bound)

  // composite terms
  YICES_ITE_TERM,            // if-then-else
  YICES_EQ_TERM,             // equality
  YICES_DISTINCT_TERM,       // distinct t_1 ... t_n
  YICES_NOT_TERM,            // (not t)
  YICES_OR_TERM,             // n-ary OR
  YICES_XOR_TERM,            // n-ary XOR

  YICES_BV_ARRAY,            // array of boolean terms
  YICES_BV_DIV,              // unsigned division
  YICES_BV_REM,              // unsigned remainder
  YICES_BV_SDIV,             // signed division
  YICES_BV_SREM,             // remainder in signed division (rounding to 0)
  YICES_BV_SMOD,             // remainder in signed division (rounding to -infinity)
  YICES_BV_SHL,              // shift left (padding with 0)
  YICES_BV_LSHR,             // logical shift right (padding with 0)
  YICES_BV_ASHR,             // arithmetic shift right (padding with sign bit)
  YICES_BV_GE_ATOM,          // unsigned comparison: (t1 >= t2)
  YICES_BV_SGE_ATOM,         // signed comparison (t1 >= t2)
  
  // projections
  YICES_BIT_TERM,            // bit-select: extract the i-th bit of a bitvector

  // sums
  YICES_BV_SUM,              // sum of pairs a * t where a is a bitvector constant (and t is a bitvector term)

  // products
  YICES_POWER_PRODUCT        // power products: (t1^d1 * ... * t_n^d_n)
} term_constructor_t;



/*****************
 *  ERROR CODES  *
 ****************/

/*
 * Error reports
 * - the API function return a default value if there's an error
 *   (e.g., term constructors return NULL_TERM, type constructors return NULL_TYPE).
 * - details about the cause of the error are stored in an error_report structure
 *   defined below.
 * - the error report contains an error code and extra information
 *   that depends on the error code.
 */
typedef enum error_code {
  NO_ERROR = 0,

  /*
   * Errors in type or term construction
   */
  INVALID_TYPE,
  INVALID_TERM,
  INVALID_CONSTANT_INDEX,
  INVALID_VAR_INDEX,       // Not used anymore
  INVALID_TUPLE_INDEX,
  INVALID_RATIONAL_FORMAT,
  INVALID_FLOAT_FORMAT,
  INVALID_BVBIN_FORMAT,
  INVALID_BVHEX_FORMAT,
  INVALID_BITSHIFT,
  INVALID_BVEXTRACT,
  INVALID_BITEXTRACT,      // added 2014/02/17
  TOO_MANY_ARGUMENTS,
  TOO_MANY_VARS,
  MAX_BVSIZE_EXCEEDED,
  DEGREE_OVERFLOW,
  DIVISION_BY_ZERO,
  POS_INT_REQUIRED,
  NONNEG_INT_REQUIRED,
  SCALAR_OR_UTYPE_REQUIRED,
  FUNCTION_REQUIRED,
  TUPLE_REQUIRED,
  VARIABLE_REQUIRED,
  ARITHTERM_REQUIRED,
  BITVECTOR_REQUIRED,
  SCALAR_TERM_REQUIRED,
  WRONG_NUMBER_OF_ARGUMENTS,
  TYPE_MISMATCH,
  INCOMPATIBLE_TYPES,
  DUPLICATE_VARIABLE,
  INCOMPATIBLE_BVSIZES,
  EMPTY_BITVECTOR,
  ARITHCONSTANT_REQUIRED,  // added 2013/01/23
  INVALID_MACRO,           // added 2013/03/31
  TOO_MANY_MACRO_PARAMS,   // added 2013/03/31
  TYPE_VAR_REQUIRED,       // added 2013/03/31
  DUPLICATE_TYPE_VAR,      // added 2013/03/31
  BVTYPE_REQUIRED,         // added 2013/05/27
  BAD_TERM_DECREF,         // added 2013/10/03
  BAD_TYPE_DECREF,         // added 2013/10/03
  INVALID_TYPE_OP,         // added 2014/12/03
  INVALID_TERM_OP,         // added 2014/12/04

  /*
   * Parser errors
   */
  INVALID_TOKEN = 100,
  SYNTAX_ERROR,
  UNDEFINED_TYPE_NAME,
  UNDEFINED_TERM_NAME,
  REDEFINED_TYPE_NAME,
  REDEFINED_TERM_NAME,
  DUPLICATE_NAME_IN_SCALAR,
  DUPLICATE_VAR_NAME,
  INTEGER_OVERFLOW,
  INTEGER_REQUIRED,
  RATIONAL_REQUIRED,
  SYMBOL_REQUIRED,
  TYPE_REQUIRED,
  NON_CONSTANT_DIVISOR,
  NEGATIVE_BVSIZE,
  INVALID_BVCONSTANT,
  TYPE_MISMATCH_IN_DEF,
  ARITH_ERROR,
  BVARITH_ERROR,


  /*
   * Errors in assertion processing.
   * These codes mean that the context, as configured,
   * cannot process the assertions.
   */
  CTX_FREE_VAR_IN_FORMULA = 300,
  CTX_LOGIC_NOT_SUPPORTED,
  CTX_UF_NOT_SUPPORTED,
  CTX_ARITH_NOT_SUPPORTED,
  CTX_BV_NOT_SUPPORTED,
  CTX_ARRAYS_NOT_SUPPORTED,
  CTX_QUANTIFIERS_NOT_SUPPORTED,
  CTX_LAMBDAS_NOT_SUPPORTED,
  CTX_NONLINEAR_ARITH_NOT_SUPPORTED,
  CTX_FORMULA_NOT_IDL,
  CTX_FORMULA_NOT_RDL,
  CTX_TOO_MANY_ARITH_VARS,
  CTX_TOO_MANY_ARITH_ATOMS,
  CTX_TOO_MANY_BV_VARS,
  CTX_TOO_MANY_BV_ATOMS,
  CTX_ARITH_SOLVER_EXCEPTION,
  CTX_BV_SOLVER_EXCEPTION,
  CTX_ARRAY_SOLVER_EXCEPTION,
  CTX_SCALAR_NOT_SUPPORTED,   // added 2015/03/26
  CTX_TUPLE_NOT_SUPPORTED,    // added 2015/03/26 
  CTX_UTYPE_NOT_SUPPORTED,    // added 2015/03/26


  /*
   * Error codes for other operations
   */
  CTX_INVALID_OPERATION = 400,
  CTX_OPERATION_NOT_SUPPORTED,


  /*
   * Errors in context configurations and search parameter settings
   */
  CTX_INVALID_CONFIG = 500,
  CTX_UNKNOWN_PARAMETER,
  CTX_INVALID_PARAMETER_VALUE,
  CTX_UNKNOWN_LOGIC,

  /*
   * Error codes for model queries
   */
  EVAL_UNKNOWN_TERM = 600,
  EVAL_FREEVAR_IN_TERM,
  EVAL_QUANTIFIER,
  EVAL_LAMBDA,
  EVAL_OVERFLOW,
  EVAL_FAILED,
  EVAL_CONVERSION_FAILED,
  EVAL_NO_IMPLICANT,

  /*
   * Error codes for model construction
   */
  MDL_UNINT_REQUIRED = 700,
  MDL_CONSTANT_REQUIRED,
  MDL_DUPLICATE_VAR,
  MDL_FTYPE_NOT_ALLOWED,
  MDL_CONSTRUCTION_FAILED,  

  /*
   * Error codes in DAG/node queries
   */
  YVAL_INVALID_OP = 800,
  YVAL_OVERFLOW,

  /*
   * Error codes for model generalization
   */
  MDL_GEN_TYPE_NOT_SUPPORTED = 900,
  MDL_GEN_NONLINEAR,
  MDL_GEN_FAILED,

  /*
   * Input/output and system errors
   */
  OUTPUT_ERROR = 9000,

  /*
   * Catch-all code for anything else.
   * This is a symptom that a bug has been found.
   */
  INTERNAL_EXCEPTION = 9999
} error_code_t;



/*
 * Error report = a code + line and column + 1 or 2 terms + 1 or 2 types
 * + an (erroneous) integer value.
 *
 * The yices API returns a negative number and set an error code on
 * error. The fields other than the error code depend on the code.  In
 * addition, the parsing functions (yices_parse_type and
 * yices_parse_term) set the line/column fields on error.
 *
 *  error code                 meaningful fields
 *
 *  NO_ERROR                   none
 *
 *  INVALID_TYPE               type1
 *  INVALID_TERM               term1
 *  INVALID_CONSTANT_INDEX     type1, badval
 *  INVALID_VAR_INDEX          badval
 *  INVALID_TUPLE_INDEX        type1, badval
 *  INVALID_RATIONAL_FORMAT    none
 *  INVALID_FLOAT_FORMAT       none
 *  INVALID_BVBIN_FORMAT       none
 *  INVALID_BVHEX_FORMAT       none
 *  INVALID_BITSHIFT           badval
 *  INVALID_BVEXTRACT          none
 *  INVALID_BITEXTRACT         none
 *  TOO_MANY_ARGUMENTS         badval
 *  TOO_MANY_VARS              badval
 *  MAX_BVSIZE_EXCEEDED        badval
 *  DEGREE_OVERFLOW            badval
 *  DIVISION_BY_ZERO           none
 *  POS_INT_REQUIRED           badval
 *  NONNEG_INT_REQUIRED        none
 *  SCALAR_OR_UTYPE_REQUIRED   type1
 *  FUNCTION_REQUIRED          term1
 *  TUPLE_REQUIRED             term1
 *  VARIABLE_REQUIRED          term1
 *  ARITHTERM_REQUIRED         term1
 *  BITVECTOR_REQUIRED         term1
 *  SCALAR_TERM_REQUIRED       term1
 *  WRONG_NUMBER_OF_ARGUMENTS  type1, badval
 *  TYPE_MISMATCH              term1, type1
 *  INCOMPATIBLE_TYPES         term1, type1, term2, type2
 *  DUPLICATE_VARIABLE         term1
 *  INCOMPATIBLE_BVSIZES       term1, type1, term2, type2
 *  EMPTY_BITVECTOR            none
 *  ARITHCONSTANT_REQUIRED    term1
 *  INVALID_MACRO              badval
 *  TOO_MANY_MACRO_PARAMS      badval
 *  TYPE_VAR_REQUIRED          type1
 *  DUPLICATE_TYPE_VAR         type1
 *  BVTYPE_REQUIRED            type1
 *  BAD_TERM_DECREF            term1
 *  BAD_TYPE_DECREF            type1
 *
 * The following error codes are used only by the parsing functions.
 * No field other than line/column is set.
 *
 *  INVALID_TOKEN
 *  SYNTAX_ERROR
 *  UNDEFINED_TERM_NAME
 *  UNDEFINED_TYPE_NAME
 *  REDEFINED_TERM_NAME
 *  REDEFINED_TYPE_NAME
 *  DUPLICATE_NAME_IN_SCALAR
 *  DUPLICATE_VAR_NAME
 *  INTEGER_OVERFLOW
 *  INTEGER_REQUIRED
 *  RATIONAL_REQUIRED
 *  SYMBOL_REQUIRED
 *  TYPE_REQUIRED
 *  NON_CONSTANT_DIVISOR
 *  NEGATIVE_BVSIZE
 *  INVALID_BVCONSTANT
 *  TYPE_MISMATCH_IN_DEF
 *  ARITH_ERROR
 *  BVARITH_ERROR
 *
 * The following error codes are triggered by invalid operations
 * on a context. For these errors, no fields of error_report (other
 * than the code) is meaningful.
 *
 *  CTX_FREE_VAR_IN_FORMULA
 *  CTX_LOGIC_NOT_SUPPORTED
 *  CTX_UF_NOT_SUPPORTED
 *  CTX_ARITH_NOT_SUPPORTED
 *  CTX_BV_NOT_SUPPORTED
 *  CTX_ARRAYS_NOT_SUPPORTED
 *  CTX_QUANTIFIERS_NOT_SUPPORTED
 *  CTX_LAMBDAS_NOT_SUPPORTED
 *  CTX_NONLINEAR_ARITH_NOT_SUPPORTED
 *  CTX_FORMULA_NOT_IDL
 *  CTX_FORMULA_NOT_RDL
 *  CTX_TOO_MANY_ARITH_VARS
 *  CTX_TOO_MANY_ARITH_ATOMS
 *  CTX_TOO_MANY_BV_VARS
 *  CTX_TOO_MANY_BV_ATOMS
 *  CTX_ARITH_SOLVER_EXCEPTION
 *  CTX_BV_SOLVER_EXCEPTION
 *  CTX_ARRAY_SOLVER_EXCEPTION
 *  CTX_SCALAR_NOT_SUPPORTED,
 *  CTX_TUPLE_NOT_SUPPORTED,
 *  CTX_UTYPE_NOT_SUPPORTED,
 *
 *  CTX_INVALID_OPERATION
 *  CTX_OPERATION_NOT_SUPPORTED
 *
 *  CTX_INVALID_CONFIG
 *  CTX_UNKNOWN_PARAMETER
 *  CTX_INVALID_PARAMETER_VALUE
 *  CTX_UNKNOWN_LOGIC
 *
 *
 * Errors for functions that operate on a model (i.e., evaluate
 * terms in a model).
 *  EVAL_UNKNOWN_TERM
 *  EVAL_FREEVAR_IN_TERM
 *  EVAL_QUANTIFIER
 *  EVAL_LAMBDA
 *  EVAL_OVERFLOW
 *  EVAL_FAILED
 *  EVAL_CONVERSION_FAILED
 *  EVAL_NO_IMPLICANT
 *
 *
 * Other error codes. No field is meaningful in the error_report,
 * except the error code:
 *
 *  OUTPUT_ERROR
 *  INTERNAL_EXCEPTION
 */
typedef struct error_report_s {
  error_code_t code;
  uint32_t line;
  uint32_t column;
  term_t term1;
  type_t type1;
  term_t term2;
  type_t type2;
  int64_t badval;
} error_report_t;

#endif  /* YICES_TYPES_H */
