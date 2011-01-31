/*
 * Print an error message based on the error_report structure
 * maintained by term_api.
 */

#if defined(CYGWIN) || defined(MINGW)
#ifndef __YICES_DLLSPEC__
#define __YICES_DLLSPEC__ __declspec(dllexport)
#endif
#endif

#include <stdint.h>
#include <inttypes.h>

#include "yices.h"
#include "yices_globals.h"


void print_error(FILE *f) {
  error_report_t *error;

  error = yices_error_report();
  switch (error->code) {
  case NO_ERROR:
    fprintf(f, "no error\n");
    break;

  case INVALID_TYPE:
    fprintf(f, "invalid type: (index = %"PRId32")\n", error->type1);
    break;

  case INVALID_TERM:
    fprintf(f, "invalid term: (index = %"PRId32")\n", error->term1);
    break;

  case INVALID_CONSTANT_INDEX:
    fprintf(f, "invalid index %"PRId64" in constant creation\n", error->badval);
    break;

  case INVALID_VAR_INDEX:
    fprintf(f, "invalid index %"PRId64" in variable creation\n", error->badval);
    break;

  case INVALID_TUPLE_INDEX:
    fprintf(f, "invalid tuple index: %"PRId64"\n", error->badval);
    break;

  case INVALID_RATIONAL_FORMAT:
    fprintf(f, "invalid rational format\n");
    break;

  case INVALID_FLOAT_FORMAT:
    fprintf(f, "invalid floating-point format\n");
    break;

  case INVALID_BVBIN_FORMAT:
    fprintf(f, "invalid bitvector binary format\n");
    break;

  case INVALID_BVHEX_FORMAT:
    fprintf(f, "invalid bitvector hexadecimal format\n");
    break;

  case INVALID_BITSHIFT:
    fprintf(f, "invalid index in shift or rotate\n");
    break;

  case INVALID_BVEXTRACT:
    fprintf(f, "invalid indices in bv-extract\n");
    break;

  case TOO_MANY_ARGUMENTS:
    fprintf(f, "too many arguments (max. arity is %"PRIu32")\n", YICES_MAX_ARITY);
    break;

  case TOO_MANY_VARS:
    fprintf(f, "too many variables in quantifier (max. is %"PRIu32")\n", YICES_MAX_VARS);
    break;

  case MAX_BVSIZE_EXCEEDED:
    fprintf(f, "bitvector size is too large (max. is %"PRIu32")\n", YICES_MAX_BVSIZE);
    break;

  case DEGREE_OVERFLOW:
    fprintf(f, "overflow in polynomial: degree is too large\n");
    break;

  case DIVISION_BY_ZERO:
    fprintf(f, "division by zero\n");
    break;

  case POS_INT_REQUIRED:
    fprintf(f, "integer argument must be positive\n");
    break;

  case NONNEG_INT_REQUIRED:
    fprintf(f, "integer argument must be non-negative\n");
    break;

  case SCALAR_OR_UTYPE_REQUIRED:
    fprintf(f, "invalid type in constant creation\n");
    break;

  case FUNCTION_REQUIRED:
    fprintf(f, "argument is not a function\n");
    break;

  case TUPLE_REQUIRED:
    fprintf(f, "argument is not a tuple\n");
    break;

  case VARIABLE_REQUIRED:
    fprintf(f, "argument is not a variable\n");
    break;

  case ARITHTERM_REQUIRED:
    fprintf(f, "argument is not an arithmetic term\n");
    break;

  case BITVECTOR_REQUIRED:
    fprintf(f, "argument is not a bitvector\n");
    break;

  case WRONG_NUMBER_OF_ARGUMENTS:
    fprintf(f, "wrong number of arguments\n");
    break;

  case TYPE_MISMATCH:
    fprintf(f, "type mismatch: invalid argument\n");
    break;

  case INCOMPATIBLE_TYPES:
    fprintf(f, "incompatible types\n");
    break;

  case DUPLICATE_VARIABLE:
    fprintf(f, "duplicate variable in quantifier\n");
    break;

  case INCOMPATIBLE_BVSIZES:
    fprintf(f, "arguments have incompatible bitsizes\n");
    break;    

  case EMPTY_BITVECTOR:
    fprintf(f, "bitvector must have positive bitsize\n");
    break;

    // parser errors here
  case INVALID_TOKEN:
    fprintf(f, "invalid token\n");
    break;

  case SYNTAX_ERROR:
    fprintf(f, "syntax error\n");
    break;

  case UNDEFINED_TYPE_NAME:
    fprintf(f, "undefined type name\n");
    break;

  case UNDEFINED_TERM_NAME:
    fprintf(f, "undefined term name\n");
    break;

  case REDEFINED_TYPE_NAME:
    fprintf(f, "cannot redefine type\n");
    break;
  
  case REDEFINED_TERM_NAME:
    fprintf(f, "cannot redefine term\n");
    break;

  case DUPLICATE_NAME_IN_SCALAR:
    fprintf(f, "duplicate name in scalar type definition\n");
    break;

  case  DUPLICATE_VAR_NAME:
    fprintf(f, "duplicate variable in quantifier\n");
    break;

  case INTEGER_OVERFLOW:
    fprintf(f, "integer overflow (constant does not fit in 32bits)\n");
    break;

  case INTEGER_REQUIRED:
    fprintf(f, "integer required\n");
    break;

  case RATIONAL_REQUIRED:
    fprintf(f, "numeric constant required\n");
    break;

  case SYMBOL_REQUIRED:
    fprintf(f, "symbol required\n");
    break;

  case TYPE_REQUIRED:
    fprintf(f, "type required\n");
    break;

  case NON_CONSTANT_DIVISOR:
    fprintf(f, "invalid division (divisor is not a constant)\n");
    break;

  case NEGATIVE_BVSIZE:
    fprintf(f, "invalid bitvector size (negative number)\n");
    break;

  case INVALID_BVCONSTANT:
    fprintf(f, "invalid number in 'mk-bv'\n");
    break;

  case TYPE_MISMATCH_IN_DEF:
    fprintf(f, "type mismatch in 'define'\n");
    break;

  case ARITH_ERROR:
    fprintf(f, "error in arithemtic operation\n");
    break;

  case BVARITH_ERROR:
    fprintf(f, "error in bitvector operation\n");
    break;

  default:
    fprintf(f, "invalid error code: %"PRId32"\n", (int32_t) error->code);
    break;
  }

  fflush(f);
}
