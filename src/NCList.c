/****************************************************************************
 *                An Nested Containment List implementation                 *
 *                                                                          *
 *                           Author: Herve Pages                            *
 ****************************************************************************/
#include "IRanges.h"
#include "S4Vectors_interface.h"

#include <stdlib.h>  /* for malloc, realloc, free, qsort */
#include <math.h>    /* for log10 */


typedef struct pnclist {
	int buflength;           /* always >= 0 */
	int nelt;                /* always >= 0 and <= buflength */
	struct pnclistelt *elts;
} preNCList;

typedef struct pnclistelt {
	int i;
	struct pnclist *sublist;
} preNCListElt;


/****************************************************************************
 * preNCList_new()
 */

static void init_preNCList(preNCList *pnclist)
{
	pnclist->buflength = pnclist->nelt = 0;
	pnclist->elts = NULL;
	return;
}

/* --- .Call ENTRY POINT --- */
SEXP preNCList_new()
{
	preNCList *top_pnclist;

	top_pnclist = (preNCList *) malloc(sizeof(preNCList));
	if (top_pnclist == NULL)
		error("preNCList_new: memory allocation failed");
	init_preNCList(top_pnclist);
	return R_MakeExternalPtr(top_pnclist, R_NilValue, R_NilValue);
}


/****************************************************************************
 * preNCList_free()
 */

static void free_preNCList(preNCList *pnclist)
{
	int n;
	const preNCListElt *elt;

	if (pnclist->buflength != 0) {
		for (n = 0, elt = pnclist->elts; n < pnclist->nelt; n++, elt++)
			free_preNCList(elt->sublist);
		free(pnclist->elts);
	}
	free(pnclist);
	return;
}

/* --- .Call ENTRY POINT --- */
SEXP preNCList_free(SEXP pnclist)
{
	preNCList *top_pnclist;

	top_pnclist = (preNCList *) R_ExternalPtrAddr(pnclist);
	if (top_pnclist == NULL)
		error("preNCList_free: pointer to preNCList struct is NULL");
	free_preNCList(top_pnclist);
	R_SetExternalPtrAddr(pnclist, NULL);
	return R_NilValue;
}


/****************************************************************************
 * preNCList_build()
 */

static void init_preNCListElt(preNCListElt *elt, int i)
{
	elt->sublist = (preNCList *) malloc(sizeof(preNCList));
	if (elt->sublist == NULL)
		error("init_preNCListElt: memory allocation failed");
	elt->i = i;
	init_preNCList(elt->sublist);
	return;
}

static void extend_preNCList(preNCList *pnclist)
{
	int old_buflength, new_buflength;
	size_t elt_size;
	preNCListElt *new_elts;

	old_buflength = pnclist->buflength;
	elt_size = sizeof(preNCListElt);
	if (old_buflength == 0) {
		new_buflength = 4;
		new_elts = (preNCListElt *) malloc(new_buflength * elt_size);
	} else {
		if (old_buflength < 16384)
			new_buflength = 8 * old_buflength;
		else if (old_buflength < 4194304)
			new_buflength = 4 * old_buflength;
		else if (old_buflength < 67108864)
			new_buflength = 2 * old_buflength;
		else
			new_buflength = old_buflength + 33554432;
		new_elts = (preNCListElt *) realloc(pnclist->elts,
						    new_buflength * elt_size);
	}
	if (new_elts == NULL)
		error("extend_preNCList: memory allocation failed");
	pnclist->buflength = new_buflength;
	pnclist->elts = new_elts;
	return;
}

static preNCListElt *add_preNCList_elt(preNCList *pnclist, int i)
{
	preNCListElt *new_elt;

	if (pnclist->nelt == pnclist->buflength)
		extend_preNCList(pnclist);
	new_elt = pnclist->elts + pnclist->nelt;
	init_preNCListElt(new_elt, i);
	pnclist->nelt++;
	return new_elt;
}

static const int *aa, *bb;

static int qsort_compar(const void *p1, const void *p2)
{
	int i1, i2, ret;

	i1 = *((const int *) p1);
	i2 = *((const int *) p2);
	ret = aa[i1] - aa[i2];
	if (ret != 0)
		return ret;
	ret = bb[i2] - bb[i1];
	return ret;
}

/*
 * Setting a hard limit on the max depth of NCList objects to prevent C stack
 * overflows when running recursive code like get_query_overlaps(). A better
 * solution would be to not use recursive code at all when traversing an
 * NCList object. Then NCList objects of arbitrary depth could be supported
 * and it wouldn't be necessary to set the limit below.
 */
#define NCLIST_MAX_DEPTH 25000
static preNCListElt **stack = NULL;
static int stack_length = 0;

static void extend_stack()
{
	int new_length;
	preNCListElt **new_stack;

	if (stack_length == 0) {
		new_length = 1000;
		new_stack = (preNCListElt **) malloc(new_length *
						     sizeof(preNCListElt *));
	} else {
		if (stack_length == NCLIST_MAX_DEPTH)
			error("extend_stack: cannot build an NCList object "
			      "of depth >= %d", NCLIST_MAX_DEPTH);
		if (stack_length <= NCLIST_MAX_DEPTH / 2)
			new_length = 2 * stack_length;
		else
			new_length = NCLIST_MAX_DEPTH;
		new_stack = (preNCListElt **) realloc(stack,
						      new_length *
						      sizeof(preNCListElt *));
	}
	if (new_stack == NULL)
		error("extend_stack: memory allocation failed");
	stack_length = new_length;
	stack = new_stack;
	return;
}

static void build_preNCList(preNCList *top_pnclist,
			    const int *x_start, const int *x_end, int x_len)
{
	preNCListElt *new_elt;

	int *oo, k, d, i, current_end;

	// Determine order of 'x'. 'oo' will be such that 'x[oo]' is sorted
	// first by ascending start then by descending end.
	oo = (int *) R_alloc(sizeof(int), x_len);
	for (i = 0; i < x_len; i++)
		oo[i] = i;
	aa = x_start;
	bb = x_end;
	qsort(oo, x_len, sizeof(int), qsort_compar);

	init_preNCList(top_pnclist);
	for (k = 0, d = -1; k < x_len; k++) {
		i = oo[k];
		current_end = x_end[i];
		while (d >= 0 && x_end[stack[d]->i] < current_end)
			d--;  // unstack
		if (d == -1) {
			// append range i to top-level
			new_elt = add_preNCList_elt(top_pnclist, i);
		} else {
			// append range i to sublist of stack[d]
			new_elt = add_preNCList_elt(stack[d]->sublist, i);
		}
		if (++d == stack_length)
			extend_stack();
		stack[d] = new_elt;  // stack
	}
	return;
}

SEXP preNCList_build(SEXP pnclist, SEXP x_start, SEXP x_end)
{
	preNCList *top_pnclist;
	int x_len;
	const int *x_start_p, *x_end_p;

	top_pnclist = (preNCList *) R_ExternalPtrAddr(pnclist);
	if (top_pnclist == NULL)
		error("preNCList_build: pointer to preNCList struct is NULL");
	x_len = check_integer_pairs(x_start, x_end,
				    &x_start_p, &x_end_p,
				    "start(x)", "end(x)");
	build_preNCList(top_pnclist, x_start_p, x_end_p, x_len);
	return pnclist;
}


/****************************************************************************
 * new_NCList_from_preNCList()
 */

#define	NCLIST_NELT(nclist) ((nclist)[0])
#define	NCLIST_I(nclist, n) ((nclist)[((n)<<1)+1])
#define	NCSUBLIST_OFFSET(nclist, n) ((nclist)[((n)<<1)+2])

static int compute_length_of_preNCList_as_INTEGER(const preNCList *pnclist)
{
	int nelt, n;
	unsigned int ans_len, dump_len;
	const preNCListElt *elt;

	nelt = pnclist->nelt;
	if (nelt == 0)
		return 0;
	ans_len = 1U + 2U * (unsigned int) nelt;
	for (n = 0, elt = pnclist->elts; n < nelt; n++, elt++) {
		dump_len = compute_length_of_preNCList_as_INTEGER(elt->sublist);
		ans_len += dump_len;
		if (ans_len < dump_len)
			goto too_big;
	}
	if (ans_len <= INT_MAX)
		return (int) ans_len;
too_big:
	error("compute_length_of_preNCList_as_INTEGER: "
	      "preNCList object is too big to fit in an integer vector");
}

static int dump_preNCList_as_int_array(const preNCList *pnclist, int *out)
{
	int nelt, offset, dump_len, n;
	const preNCListElt *elt;

	nelt = pnclist->nelt;
	if (nelt == 0)
		return 0;
	offset = 1 + 2 * nelt;
	NCLIST_NELT(out) = nelt;
	for (n = 0, elt = pnclist->elts; n < nelt; n++, elt++) {
		NCLIST_I(out, n) = elt->i;
		dump_len = dump_preNCList_as_int_array(elt->sublist,
						       out + offset);
		NCSUBLIST_OFFSET(out, n) = dump_len != 0 ? offset : -1;
		offset += dump_len;
	}
	return offset;
}

/* --- .Call ENTRY POINT --- */
SEXP new_NCList_from_preNCList(SEXP pnclist)
{
	SEXP ans;
	const preNCList *top_pnclist;
	int ans_len;

	top_pnclist = (preNCList *) R_ExternalPtrAddr(pnclist);
	if (top_pnclist == NULL)
		error("new_NCList_from_preNCList: "
		      "pointer to preNCList struct is NULL");
	ans_len = compute_length_of_preNCList_as_INTEGER(top_pnclist);
	PROTECT(ans = NEW_INTEGER(ans_len));
	dump_preNCList_as_int_array(top_pnclist, INTEGER(ans));
	UNPROTECT(1);
	return ans;
}


/****************************************************************************
 * NCList_print()
 */

/* Print 1 line per range in 'nclist'. Return max depth. */
static int print_NCList(const int *nclist,
			const int *x_start, const int *x_end,
			int depth, const char *format)
{
	int max_depth, nelt, n, d, i, offset, tmp;

	max_depth = depth;
	nelt = NCLIST_NELT(nclist);
	for (n = 0; n < nelt; n++) {
		for (d = 1; d < depth; d++)
			Rprintf("|");
		i = NCLIST_I(nclist, n);
		Rprintf(format, i + 1);
		Rprintf(": [%d, %d]\n", x_start[i], x_end[i]);
		offset = NCSUBLIST_OFFSET(nclist, n);
		if (offset != -1) {
			tmp = print_NCList(nclist + offset,
					   x_start, x_end, depth + 1,
					   format);
			if (tmp > max_depth)
				max_depth = tmp;
		}
	}
	return max_depth;
}

/* --- .Call ENTRY POINT --- */
SEXP NCList_print(SEXP x_nclist, SEXP x_start, SEXP x_end)
{
	const int *top_nclist;
	int x_len, max_digits, max_depth;
	const int *x_start_p, *x_end_p;
	char format[10];

	top_nclist = INTEGER(x_nclist);
	x_len = check_integer_pairs(x_start, x_end,
				    &x_start_p, &x_end_p,
				    "start(x)", "end(x)");
	if (x_len == 0) {
		max_depth = 0;
	} else {
		max_digits = (int) log10((double) x_len) + 1;
		sprintf(format, "%c%d%c", '%', max_digits, 'd');
		max_depth = print_NCList(top_nclist, x_start_p, x_end_p,
					 1, format);
	}
	Rprintf("max depth = %d\n", max_depth);
	return R_NilValue;
}


/****************************************************************************
 * NCList_find_overlaps()
 */

/* The 6 supported types of overlap. */
#define TYPE_ANY		1
#define TYPE_START		2
#define TYPE_END		3
#define TYPE_WITHIN		4
#define TYPE_EXTEND		5
#define TYPE_EQUAL		6

/* The 4 supported select modes. */
#define SELECT_ALL		1
#define SELECT_FIRST		2
#define SELECT_LAST		3
#define SELECT_ARBITRARY	4

static int get_min_overlap_score(SEXP min_score)
{
	int min_overlap_score;

	if (!IS_INTEGER(min_score) || LENGTH(min_score) != 1)
		error("'min_score' must be a single integer");
	min_overlap_score = INTEGER(min_score)[0];
	if (min_overlap_score == NA_INTEGER)
		error("'min_score' cannot be NA");
	return min_overlap_score;
}

static int get_overlap_type(SEXP type)
{
	const char *type0;

	if (!IS_CHARACTER(type) || LENGTH(type) != 1)
		error("'type' must be a single string");
	type = STRING_ELT(type, 0);
	if (type == NA_STRING)
		error("'type' cannot be NA");
	type0 = CHAR(type);
	if (strcmp(type0, "any") == 0)
		return TYPE_ANY;
	if (strcmp(type0, "start") == 0)
		return TYPE_START;
	if (strcmp(type0, "end") == 0)
		return TYPE_END;
	if (strcmp(type0, "within") == 0)
		return TYPE_WITHIN;
	if (strcmp(type0, "extend") == 0)
		return TYPE_EXTEND;
	if (strcmp(type0, "equal") == 0)
		return TYPE_EQUAL;
	error("'type' must be \"any\", \"start\", \"end\", "
	      "\"within\", \"extend\", or \"equal\"");
	return 0;
}

static int get_select_mode(SEXP select)
{
	const char *select0;

	if (!IS_CHARACTER(select) || LENGTH(select) != 1)
		error("'select' must be a single string");
	select = STRING_ELT(select, 0);
	if (select == NA_STRING)
		error("'select' cannot be NA");
	select0 = CHAR(select);
	if (strcmp(select0, "all") == 0)
		return SELECT_ALL;
	if (strcmp(select0, "first") == 0)
		return SELECT_FIRST;
	if (strcmp(select0, "last") == 0)
		return SELECT_LAST;
	if (strcmp(select0, "arbitrary") == 0)
		return SELECT_ARBITRARY;
	error("'select' must be \"all\", \"first\", "
	      "\"last\", or \"arbitrary\"");
	return 0;
}

typedef struct backpack {
	int q_start;
	int q_end;
	int ext_q_start;
	int ext_q_end;
	const int *s_start_p;
	const int *s_end_p;
	int min_overlap_score;
	int query_extension;
	int overlap_type;
	int select_mode;
	IntAE *sh_buf;
} Backpack;

static Backpack prepare_backpack(const int *s_start_p, const int *s_end_p,
				 SEXP min_score, SEXP type, SEXP select,
				 IntAE *sh_buf)
{
	Backpack backpack;
	int min_overlap_score, query_extension;

	min_overlap_score = get_min_overlap_score(min_score);
	if (min_overlap_score >= 1) {
		query_extension = 0;
	} else {
		query_extension = 1 - min_overlap_score;
		min_overlap_score = 1;
	}
	backpack.s_start_p = s_start_p;
	backpack.s_end_p = s_end_p;
	backpack.min_overlap_score = min_overlap_score;
	backpack.query_extension = query_extension;
	backpack.overlap_type = get_overlap_type(type);
	backpack.select_mode = get_select_mode(select);
	backpack.sh_buf = sh_buf;
	return backpack;
}

static int bsearch_n1(int q_start, const int *nclist, const int *s_end_p)
{
	int n1, n2, nelt, n, end;

	/* Check first element. */
	n1 = 0;
	end = s_end_p[NCLIST_I(nclist, n1)];
	if (end >= q_start)
		return n1;

	/* Check last element. */
	nelt = NCLIST_NELT(nclist);
	n2 = nelt - 1;
	end = s_end_p[NCLIST_I(nclist, n2)];
	if (end < q_start)
		return nelt;
	if (end == q_start)
		return n2;

	/* Binary search. */
	while ((n = (n1 + n2) / 2) != n1) {
		end = s_end_p[NCLIST_I(nclist, n)];
		if (end == q_start)
			return n;
		if (end < q_start)
			n1 = n;
		else
			n2 = n;
	}
	return n2;
}

/* Recursive! */
static void get_query_overlaps(const int *nclist, Backpack *backpack)
{
	int nelt, n, i, s_start, s_end,
	    ov_start, ov_end, score, score_is_ok, type_is_ok, i1, tmp, offset;

	nelt = NCLIST_NELT(nclist);
	n = bsearch_n1(backpack->ext_q_start, nclist, backpack->s_end_p);
	for ( ; n < nelt; n++) {
		i = NCLIST_I(nclist, n);
		s_start = backpack->s_start_p[i];
		if (backpack->ext_q_end < s_start)
			break;
		/* Do we have a hit? */
		s_end = backpack->s_end_p[i];
		if (backpack->min_overlap_score == 1) {
			score_is_ok = 1;
		} else {
			ov_start = backpack->q_start > s_start ?
				   backpack->q_start : s_start;
			ov_end   = backpack->q_end < s_end ?
				   backpack->q_end : s_end;
			score = ov_end - ov_start + 1;
			score_is_ok = score >= backpack->min_overlap_score;
		}
		switch (backpack->overlap_type) {
		    case TYPE_START:
			type_is_ok = backpack->q_start == s_start;
			break;
		    case TYPE_END:
			type_is_ok = backpack->q_end == s_end;
			break;
		    case TYPE_WITHIN:
			type_is_ok = backpack->q_start >= s_start &&
				     backpack->q_end <= s_end;
			break;
		    case TYPE_EXTEND:
			type_is_ok = backpack->q_start <= s_start &&
				     backpack->q_end >= s_end;
			break;
		    case TYPE_EQUAL:
			type_is_ok = backpack->q_start == s_start &&
				     backpack->q_end == s_end;
			break;
		    default:
			type_is_ok = 1;
		}
		if (score_is_ok && type_is_ok) {
			/* Report the hit. */
			i1 = i + 1;
			if (backpack->select_mode != SELECT_ALL) {
				tmp = backpack->sh_buf->elts[0];
				if (tmp == NA_INTEGER
				 || (backpack->select_mode == SELECT_FIRST)
				     == (i1 < tmp))
					backpack->sh_buf->elts[0] = i1;
				if (backpack->select_mode == SELECT_ARBITRARY)
					break;
			} else {
				IntAE_insert_at(backpack->sh_buf,
					IntAE_get_nelt(backpack->sh_buf), i1);
			}
		}
		offset = NCSUBLIST_OFFSET(nclist, n);
		if (offset != -1)
			get_query_overlaps(nclist + offset, backpack);
	}
	return;
}

static SEXP new_Hits_from_IntPairAE(const IntPairAE *x, int q_len, int s_len)
{
	SEXP classdef, ans,
	     ans_queryHits, ans_subjectHits,
	     ans_queryLength, ans_subjectLength;

	PROTECT(classdef = MAKE_CLASS("Hits"));
	PROTECT(ans = NEW_OBJECT(classdef));

	PROTECT(ans_queryHits = new_INTEGER_from_IntAE(&(x->a)));
	SET_SLOT(ans, install("queryHits"), ans_queryHits);
	UNPROTECT(1);

	PROTECT(ans_subjectHits = new_INTEGER_from_IntAE(&(x->b)));
	SET_SLOT(ans, install("subjectHits"), ans_subjectHits);
	UNPROTECT(1);

	PROTECT(ans_queryLength = ScalarInteger(q_len));
	SET_SLOT(ans, install("queryLength"), ans_queryLength);
	UNPROTECT(1);

	PROTECT(ans_subjectLength = ScalarInteger(s_len));
	SET_SLOT(ans, install("subjectLength"), ans_subjectLength);
	UNPROTECT(1);

	UNPROTECT(2);
	return ans;
}

/* --- .Call ENTRY POINT --- */
SEXP NCList_find_overlaps(SEXP q_start, SEXP q_end,
			  SEXP s_nclist, SEXP s_start, SEXP s_end,
			  SEXP min_score, SEXP type, SEXP select)
{
	const int *top_nclist;
	int q_len, s_len, i, old_nhit, new_nhit, k;
	const int *q_start_p, *q_end_p, *s_start_p, *s_end_p;
	IntPairAE hits_buf;
	IntAE *qh_buf, *sh_buf;
	Backpack backpack;
	SEXP ans;
	int *ans_elt_p;

	top_nclist = INTEGER(s_nclist);
	q_len = check_integer_pairs(q_start, q_end,
				    &q_start_p, &q_end_p,
				    "start(query)", "end(query)");
	s_len = check_integer_pairs(s_start, s_end,
				    &s_start_p, &s_end_p,
				    "start(subject)", "end(subject)");
	hits_buf = new_IntPairAE(0, 0);
	qh_buf = &(hits_buf.a);
	sh_buf = &(hits_buf.b);
	backpack = prepare_backpack(s_start_p, s_end_p,
				    min_score, type, select,
				    sh_buf);
	if (backpack.select_mode != SELECT_ALL) {
		IntAE_insert_at(sh_buf, 0, NA_INTEGER);
		PROTECT(ans = NEW_INTEGER(q_len));
		ans_elt_p = INTEGER(ans);
	}
	for (i = 1; i <= q_len; i++, q_start_p++, q_end_p++) {
		if (s_len != 0) {
			/* Update backpack. */
			backpack.q_start = *q_start_p;
			backpack.q_end = *q_end_p;
			backpack.ext_q_start = *q_start_p -
						backpack.query_extension;
			backpack.ext_q_end = *q_end_p +
						backpack.query_extension;
			get_query_overlaps(top_nclist, &backpack);
		}
		if (backpack.select_mode != SELECT_ALL) {
			*(ans_elt_p++) = sh_buf->elts[0];
			sh_buf->elts[0] = NA_INTEGER;
		} else {
			old_nhit = IntAE_get_nelt(qh_buf);
			new_nhit = IntAE_get_nelt(sh_buf);
			for (k = old_nhit; k < new_nhit; k++)
				IntAE_insert_at(qh_buf, k, i);
		}
	}
	if (backpack.select_mode != SELECT_ALL) {
		UNPROTECT(1);
		return ans;
	}
	return new_Hits_from_IntPairAE(&hits_buf, q_len, s_len);
}

