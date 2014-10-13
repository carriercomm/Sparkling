/*
 * rtlb.c
 * Sparkling, a lightweight C-style scripting language
 *
 * Created by Árpád Goretity on 02/05/2013
 * Licensed under the 2-clause BSD License
 *
 * Run-time support library
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <limits.h>
#include <errno.h>

#include "rtlb.h"
#include "str.h"
#include "array.h"
#include "hashmap.h"
#include "ctx.h"
#include "private.h"

#ifndef LINE_MAX
#define LINE_MAX 0x1000
#endif

/* definitions for maths library and others */

#ifndef M_E
#define M_E        2.71828182845904523536028747135266250
#endif

#ifndef M_LN2
#define M_LN2      0.693147180559945309417232121458176568
#endif

#ifndef M_PI
#define M_PI       3.14159265358979323846264338327950288
#endif

#ifndef M_SQRT2
#define M_SQRT2    1.41421356237309504880168872420969808
#endif

#ifndef M_PHI
#define M_PHI      1.61803398874989484820458683436563811
#endif

/* Returns the class descriptor for a type tag
 * 'typetag' is one of the SPN_TTAG_* enum members.
 */
static SpnHashMap *get_class_for_typetag(SpnVMachine *vm, int typetag)
{
	SpnHashMap *classes = spn_vm_getclasses(vm);

	SpnValue indexval = makeint(typetag);
	SpnValue classval;
	spn_hashmap_get(classes, &indexval, &classval);

	assert(ishashmap(&classval));
	return hashmapvalue(&classval);
}

/* Adds methods to the class of a type. */
static void load_methods(SpnVMachine *vm, int typetag, const SpnExtFunc fns[], size_t n)
{
	SpnHashMap *classdesc = get_class_for_typetag(vm, typetag);

	size_t i;
	for (i = 0; i < n; i++) {
		SpnValue method = makenativefunc(fns[i].name, fns[i].fn);
		spn_hashmap_set_strkey(classdesc, fns[i].name, &method);
		spn_value_release(&method);
	}
}

/***************
 * I/O library *
 ***************/

static void rtlb_aux_getline(SpnValue *ret, FILE *f)
{
	int no_lf = 1;
	size_t n = 0, alloc_size = 0x10;
	char *buf = spn_malloc(alloc_size);

	while (1) {
		int ch = fgetc(f);

		if (ch == EOF) {
			break;
		}

		if (ch == '\n') {
			no_lf = 0;
			break;
		}

		/* >=: make room for terminating NUL too */
		if (++n >= alloc_size) {
			alloc_size *= 2;
			buf = spn_realloc(buf, alloc_size);
		}

		buf[n - 1] = ch;
	}

	/* handle empty file */
	if (n == 0 && no_lf) {
		free(buf);
		*ret = spn_nilval;
	} else {
		buf[n] = 0;
		*ret = makestring_nocopy_len(buf, n, 1);
	}
}

static int rtlb_getline(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	rtlb_aux_getline(ret, stdin);
	return 0;
}

static int rtlb_print(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	int i;
	for (i = 0; i < argc; i++) {
		spn_value_print(&argv[i]);
	}

	printf("\n");

	return 0;
}

static int rtlb_dbgprint(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	int i;
	for (i = 0; i < argc; i++) {
		spn_debug_print(&argv[i]);
	}

	printf("\n");

	return 0;
}

static int rtlb_printf(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	SpnString *fmt;
	SpnString *res;
	char *errmsg;

	if (argc < 1) {
		spn_ctx_runtime_error(ctx, "at least one argument is required", NULL);
		return -1;
	}

	if (!isstring(&argv[0])) {
		spn_ctx_runtime_error(ctx, "first argument must be a format string", NULL);
		return -2;
	}

	fmt = stringvalue(&argv[0]);
	res = spn_string_format_obj(fmt, argc - 1, &argv[1], &errmsg);

	if (res != NULL) {
		fputs(res->cstr, stdout);
		*ret = makeint(res->len);
		spn_object_release(res);
	} else {
		const void *args[1];
		args[0] = errmsg;
		spn_ctx_runtime_error(ctx, "error in format string: %s", args);
		free(errmsg);
		return -3;
	}

	return 0;
}

static int rtlb_fopen(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	FILE *fp;
	SpnString *fname, *mode;

	if (argc != 2) {
		spn_ctx_runtime_error(ctx, "exactly two arguments are required", NULL);
		return -1;
	}

	if (!isstring(&argv[0]) || !isstring(&argv[1])) {
		spn_ctx_runtime_error(ctx, "filename and mode must be strings", NULL);
		return -2;
	}

	fname = stringvalue(&argv[0]);
	mode = stringvalue(&argv[1]);
	fp = fopen(fname->cstr, mode->cstr);

	if (fp != NULL) {
		*ret = makeweakuserinfo(fp);
	}
	/* else implicitly return nil */

	return 0;
}

static int rtlb_fclose(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	FILE *fp;

	if (argc != 1) {
		spn_ctx_runtime_error(ctx, "exactly one argument is required", NULL);
		return -1;
	}

	if (!isweakuserinfo(&argv[0])) {
		spn_ctx_runtime_error(ctx, "argument must be a file handle", NULL);
		return -2;
	}

	fp = ptrvalue(&argv[0]);
	fclose(fp);
	return 0;
}

static int rtlb_fprintf(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	SpnString *fmt;
	SpnString *res;
	FILE *stream;
	char *errmsg;

	if (argc < 2) {
		spn_ctx_runtime_error(ctx, "at least two arguments are required", NULL);
		return -1;
	}

	if (!isweakuserinfo(&argv[0])) {
		spn_ctx_runtime_error(ctx, "first argument must be a file handle", NULL);
		return -2;
	}

	if (!isstring(&argv[1])) {
		spn_ctx_runtime_error(ctx, "second argument must be a format string", NULL);
		return -2;
	}

	stream = ptrvalue(&argv[0]);
	fmt = stringvalue(&argv[1]);
	res = spn_string_format_obj(fmt, argc - 2, &argv[2], &errmsg);

	if (res != NULL) {
		fputs(res->cstr, stream);
		*ret = makeint(res->len);
		spn_object_release(res);
	} else {
		const void *args[1];
		args[0] = errmsg;
		spn_ctx_runtime_error(ctx, "error in format string: %s", args);
		free(errmsg);
		return -3;
	}

	return 0;
}

/* XXX: should this remove newlines as well, like getline()? */
static int rtlb_fgetline(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	FILE *fp;

	if (argc != 1) {
		spn_ctx_runtime_error(ctx, "exactly one argument is required", NULL);
		return -1;
	}

	if (!isweakuserinfo(&argv[0])) {
		spn_ctx_runtime_error(ctx, "argument must be a file handle", NULL);
		return -2;
	}

	fp = ptrvalue(&argv[0]);
	rtlb_aux_getline(ret, fp);
	return 0;
}

static int rtlb_fread(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	long n;
	char *buf;
	FILE *fp;

	if (argc != 2) {
		spn_ctx_runtime_error(ctx, "exactly two arguments are required", NULL);
		return -1;
	}

	if (!isweakuserinfo(&argv[0])) {
		spn_ctx_runtime_error(ctx, "first argument must be a file handle", NULL);
		return -2;
	}

	if (!isint(&argv[1])) {
		spn_ctx_runtime_error(ctx, "second argument must be an integer", NULL);
		return -2;
	}

	fp = ptrvalue(&argv[0]);
	n = intvalue(&argv[1]);

	buf = spn_malloc(n + 1);
	buf[n] = 0;

	if (fread(buf, n, 1, fp) != 1) {
		free(buf);
		/* implicitly return nil */
	} else {
		*ret = makestring_nocopy_len(buf, n, 1);
	}

	return 0;
}

static int rtlb_fwrite(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	int success;
	FILE *fp;
	SpnString *str;

	if (argc != 2) {
		spn_ctx_runtime_error(ctx, "exactly two arguments are required", NULL);
		return -1;
	}

	if (!isweakuserinfo(&argv[0])) {
		spn_ctx_runtime_error(ctx, "first argument must be a file handle", NULL);
		return -2;
	}

	if (!isstring(&argv[1])) {
		spn_ctx_runtime_error(ctx, "second argument must be a string", NULL);
		return -2;
	}

	fp = ptrvalue(&argv[0]);
	str = stringvalue(&argv[1]);

	success = fwrite(str->cstr, str->len, 1, fp) == 1;
	*ret = makebool(success);

	return 0;
}

/* if passed `nil`, flushes all streams by calling `fflush(NULL)` */
static int rtlb_fflush(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	FILE *fp;

	if (argc != 1) {
		spn_ctx_runtime_error(ctx, "expecting one argument", NULL);
		return -1;
	}

	if (!isweakuserinfo(&argv[0])) {
		spn_ctx_runtime_error(ctx, "argument must be an output file handle", NULL);
		return -2;
	}

	fp = ptrvalue(&argv[0]);

	*ret = makebool(!fflush(fp));
	return 0;
}

static int rtlb_ftell(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	FILE *fp;

	if (argc != 1) {
		spn_ctx_runtime_error(ctx, "exactly one argument is required", NULL);
		return -1;
	}

	if (!isweakuserinfo(&argv[0])) {
		spn_ctx_runtime_error(ctx, "argument must be a file handle", NULL);
		return -2;
	}

	fp = ptrvalue(&argv[0]);

	*ret = makeint(ftell(fp));

	return 0;
}

static int rtlb_fseek(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	long off;
	int flag;
	FILE *fp;
	SpnString *whence;

	if (argc != 3) {
		spn_ctx_runtime_error(ctx, "exactly three arguments are required", NULL);
		return -1;
	}

	if (!isweakuserinfo(&argv[0])) {
		spn_ctx_runtime_error(ctx, "first argument must be a file handle", NULL);
		return -2;
	}

	if (!isint(&argv[1])) {
		spn_ctx_runtime_error(ctx, "second argument must be an integer", NULL);
		return -2;
	}

	if (!isstring(&argv[2])) {
		spn_ctx_runtime_error(ctx, "third argument must be a mode string", NULL);
		return -2;
	}

	fp = ptrvalue(&argv[0]);
	off = intvalue(&argv[1]);
	whence = stringvalue(&argv[2]);

	if (strcmp(whence->cstr, "set") == 0) {
		flag = SEEK_SET;
	} else if (strcmp(whence->cstr, "cur") == 0) {
		flag = SEEK_CUR;
	} else if (strcmp(whence->cstr, "end") == 0) {
		flag = SEEK_END;
	} else {
		spn_ctx_runtime_error(ctx, "third argument must be one of \"set\", \"cur\" or \"end\"", NULL);
		return -3;
	}

	*ret = makebool(!fseek(fp, off, flag));

	return 0;
}

static int rtlb_feof(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	FILE *fp;

	if (argc != 1) {
		spn_ctx_runtime_error(ctx, "exactly one argument is required", NULL);
		return -1;
	}

	if (!isweakuserinfo(&argv[0])) {
		spn_ctx_runtime_error(ctx, "argument must be a file handle", NULL);
		return -2;
	}

	fp = ptrvalue(&argv[0]);

	*ret = makebool(feof(fp) != 0);

	return 0;
}

static int rtlb_remove(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	SpnString *fname;

	if (argc != 1) {
		spn_ctx_runtime_error(ctx, "exactly one argument is required", NULL);
		return -1;
	}

	if (!isstring(&argv[0])) {
		spn_ctx_runtime_error(ctx, "argument must be a file path", NULL);
		return -2;
	}

	fname = stringvalue(&argv[0]);

	*ret = makebool(remove(fname->cstr) == 0);

	return 0;
}

static int rtlb_rename(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	SpnString *oldname, *newname;

	if (argc != 2) {
		spn_ctx_runtime_error(ctx, "exactly two arguments are required", NULL);
		return -1;
	}

	if (!isstring(&argv[0]) || !isstring(&argv[1])) {
		spn_ctx_runtime_error(ctx, "arguments must be file paths", NULL);
		return -2;
	}

	oldname = stringvalue(&argv[0]);
	newname = stringvalue(&argv[1]);

	*ret = makebool(rename(oldname->cstr, newname->cstr) == 0);

	return 0;
}

static int rtlb_tmpfile(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	FILE *fp = tmpfile();

	if (fp != NULL) {
		*ret = makeweakuserinfo(fp);
	}
	/* else implicitly return nil */

	return 0;
}

static int rtlb_readfile(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	const char *fname;
	char *buf;
	size_t size;
	int status = 0;
	FILE *f;

	if (argc != 1) {
		spn_ctx_runtime_error(ctx, "exactly one argument is required", NULL);
		return -1;
	}

	if (!isstring(&argv[0])) {
		spn_ctx_runtime_error(ctx, "argument must be a string (filename)", NULL);
		return -2;
	}

	fname = stringvalue(&argv[0])->cstr;
	f = fopen(fname, "rb");

	if (f == NULL) {
		const void *args[2];
		args[0] = fname;
		args[1] = strerror(errno);
		spn_ctx_runtime_error(ctx, "can't open file `%s': %s", args);
		return -3;
	}

	fseek(f, 0, SEEK_END);
	size = ftell(f);

	fseek(f, 0, SEEK_SET);
	buf = spn_malloc(size + 1);

	if (fread(buf, size, 1, f) != 1) {
		const void *args[2];
		args[0] = fname;
		args[1] = strerror(errno);
		spn_ctx_runtime_error(ctx, "can't read file `%s': %s", args);

		free(buf);
		status = -4;
	} else {
		buf[size] = 0;
		*ret = makestring_nocopy_len(buf, size, 1);
	}

	fclose(f);
	return status;
}

static void loadlib_io(SpnVMachine *vm)
{
	/* Free functions */
	static const SpnExtFunc F[] = {
		{ "getline",  rtlb_getline  },
		{ "print",    rtlb_print    },
		{ "dbgprint", rtlb_dbgprint },
		{ "printf",   rtlb_printf   },
		{ "fopen",    rtlb_fopen    },
		{ "fclose",   rtlb_fclose   },
		{ "fprintf",  rtlb_fprintf  },
		{ "fgetline", rtlb_fgetline },
		{ "fread",    rtlb_fread    },
		{ "fwrite",   rtlb_fwrite   },
		{ "fflush",   rtlb_fflush   },
		{ "ftell",    rtlb_ftell    },
		{ "fseek",    rtlb_fseek    },
		{ "feof",     rtlb_feof     },
		{ "remove",   rtlb_remove   },
		{ "rename",   rtlb_rename   },
		{ "tmpfile",  rtlb_tmpfile  },
		{ "readfile", rtlb_readfile }
	};

	/* Constants */
	SpnExtValue C[3];

	C[0].name = "stdin";
	C[0].value = makeweakuserinfo(stdin);

	C[1].name = "stdout";
	C[1].value = makeweakuserinfo(stdout);

	C[2].name = "stderr";
	C[2].value = makeweakuserinfo(stderr);

	spn_vm_addlib_cfuncs(vm, NULL, F, COUNT(F));
	spn_vm_addlib_values(vm, NULL, C, COUNT(C));
}

/******************
 * String library *
 ******************/

static int rtlb_str_find(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	SpnString *haystack, *needle;
	const char *pos;
	long off = 0;
	long len; /* length of haystack, because we need a signed type */

	if (argc != 2 && argc != 3) {
		spn_ctx_runtime_error(ctx, "two or three arguments are required", NULL);
		return -1;
	}

	if (!isstring(&argv[0]) || !isstring(&argv[1])) {
		spn_ctx_runtime_error(ctx, "first two arguments must be strings", NULL);
		return -2;
	}

	/* if an offset is specified, respect it */
	if (argc == 3) {
		if (!isint(&argv[2])) {
			/* not an integer */
			spn_ctx_runtime_error(ctx, "third argument must be an integer", NULL);
			return -3;
		}

		off = intvalue(&argv[2]);
	}

	haystack = stringvalue(&argv[0]);
	needle   = stringvalue(&argv[1]);
	len = haystack->len;

	/* if the offset is negative, count from the end of the string */
	if (off < 0) {
		off = len + off;
	}

	/* if still not good (absolute value of offset too big), then throw */
	if (off < 0 || off > len) {
		spn_ctx_runtime_error(ctx, "normalized index out of bounds", NULL);
		return -4;
	}

	pos = strstr(haystack->cstr + off, needle->cstr);

	*ret = makeint(pos != NULL ? pos - haystack->cstr : -1);

	return 0;
}

/* main substring function, used by substr(), substrto() and substrfrom() */
static int rtlb_aux_substr(SpnValue *ret, SpnString *str, long begin, long length, SpnContext *ctx)
{
	char *buf;
	long slen = str->len;

	if (begin < 0 || begin > slen) {
		spn_ctx_runtime_error(ctx, "starting index is negative or too high", NULL);
		return -1;
	}

	if (length < 0 || length > slen) {
		spn_ctx_runtime_error(ctx, "length is negative or too big", NULL);
		return -2;
	}

	if (begin + length > slen) {
		spn_ctx_runtime_error(ctx, "end of substring is out of bounds", NULL);
		return -3;
	}

	buf = spn_malloc(length + 1);
	memcpy(buf, str->cstr + begin, length);
	buf[length] = 0;

	*ret = makestring_nocopy_len(buf, length, 1);

	return 0;
}

static int rtlb_substr(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	SpnString *str;
	long begin, length;

	if (argc != 3) {
		spn_ctx_runtime_error(ctx, "exactly three arguments are required", NULL);
		return -1;
	}

	if (!isstring(&argv[0])) {
		spn_ctx_runtime_error(ctx, "first argument must be a string", NULL);
		return -2;
	}

	if (!isint(&argv[1]) || !isint(&argv[2])) {
		spn_ctx_runtime_error(ctx, "second and third argument must be integers", NULL);
		return -2;
	}

	str = stringvalue(&argv[0]);
	begin = intvalue(&argv[1]);
	length = intvalue(&argv[2]);

	return rtlb_aux_substr(ret, str, begin, length, ctx);
}

static int rtlb_substrto(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	SpnString *str;
	long length;

	if (argc != 2) {
		spn_ctx_runtime_error(ctx, "exactly two arguments are required", NULL);
		return -1;
	}

	if (!isstring(&argv[0])) {
		spn_ctx_runtime_error(ctx, "first argument must be a string", NULL);
		return -2;
	}

	if (!isint(&argv[1])) {
		spn_ctx_runtime_error(ctx, "second argument must be an integer", NULL);
		return -2;
	}

	str = stringvalue(&argv[0]);
	length = intvalue(&argv[1]);

	return rtlb_aux_substr(ret, str, 0, length, ctx);
}

static int rtlb_substrfrom(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	SpnString *str;
	long begin, length, slen;

	if (argc != 2) {
		spn_ctx_runtime_error(ctx, "exactly two arguments are required", NULL);
		return -1;
	}

	if (!isstring(&argv[0])) {
		spn_ctx_runtime_error(ctx, "first argument must be a string", NULL);
		return -2;
	}

	if (!isint(&argv[1])) {
		spn_ctx_runtime_error(ctx, "second argument must be an integer", NULL);
		return -2;
	}

	str = stringvalue(&argv[0]);
	slen = str->len;
	begin = intvalue(&argv[1]);
	length = slen - begin;

	return rtlb_aux_substr(ret, str, begin, length, ctx);
}

static int rtlb_split(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	const char *s, *t;

	SpnString *haystack, *needle;
	SpnArray *arr;

	if (argc != 2) {
		spn_ctx_runtime_error(ctx, "exactly two arguments are required", NULL);
		return -1;
	}

	if (!isstring(&argv[0]) || !isstring(&argv[1])) {
		spn_ctx_runtime_error(ctx, "arguments must be strings", NULL);
		return -2;
	}

	haystack = stringvalue(&argv[0]);
	needle = stringvalue(&argv[1]);

	if (needle->len == 0) {
		spn_ctx_runtime_error(ctx, "cannot split on empty string", NULL);
		return -3;
	}

	arr = spn_array_new();

	ret->type = SPN_TYPE_ARRAY;
	ret->v.o = arr;

	s = haystack->cstr;
	t = strstr(s, needle->cstr);

	while (1) {
		SpnValue val;
		const char *p = t ? t : haystack->cstr + haystack->len;
		size_t len = p - s;
		char *buf = spn_malloc(len + 1);

		memcpy(buf, s, len);
		buf[len] = 0;

		val = makestring_nocopy_len(buf, len, 1);
		spn_array_push(arr, &val);
		spn_value_release(&val);

		if (t == NULL) {
			break;
		}

		s = t + needle->len;
		t = strstr(s, needle->cstr);
	}

	return 0;
}

static int rtlb_repeat(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	char *buf;
	size_t i, len, n;
	SpnString *str;

	if (argc != 2) {
		spn_ctx_runtime_error(ctx, "exactly two arguments are required", NULL);
		return -1;
	}

	if (!isstring(&argv[0])) {
		spn_ctx_runtime_error(ctx, "first argument must be a string", NULL);
		return -2;
	}

	if (!isint(&argv[1])) {
		spn_ctx_runtime_error(ctx, "second argument must be an integer", NULL);
		return -2;
	}

	if (intvalue(&argv[1]) < 0) {
		spn_ctx_runtime_error(ctx, "second argument must not be negative", NULL);
		return -3;
	}

	str = stringvalue(&argv[0]);
	n = intvalue(&argv[1]);
	len = str->len * n;

	buf = spn_malloc(len + 1);

	for (i = 0; i < n; i++) {
		memcpy(buf + i * str->len, str->cstr, str->len);
	}

	buf[len] = 0;

	*ret = makestring_nocopy_len(buf, len, 1);

	return 0;
}

static int rtlb_aux_trcase(SpnValue *ret, int argc, SpnValue *argv, int upc, SpnContext *ctx)
{
	const char *p, *end;
	char *buf, *s;
	SpnString *str;

	if (argc != 1) {
		spn_ctx_runtime_error(ctx, "exactly one argument is required", NULL);
		return -1;
	}

	if (!isstring(&argv[0])) {
		spn_ctx_runtime_error(ctx, "argument must be a string", NULL);
		return -2;
	}

	str = stringvalue(&argv[0]);
	p = str->cstr;
	end = p + str->len;

	buf = spn_malloc(str->len + 1);
	s = buf;

	while (p < end) {
		*s++ = upc ? toupper(*p++) : tolower(*p++);
	}

	*s = 0;

	*ret = makestring_nocopy_len(buf, str->len, 1);

	return 0;
}

static int rtlb_tolower(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	return rtlb_aux_trcase(ret, argc, argv, 0, ctx);
}

static int rtlb_toupper(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	return rtlb_aux_trcase(ret, argc, argv, 1, ctx);
}

static int rtlb_format(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	SpnString *fmt;
	SpnString *res;
	char *errmsg;

	if (argc < 1) {
		spn_ctx_runtime_error(ctx, "at least one argument is required", NULL);
		return -1;
	}

	if (!isstring(&argv[0])) {
		spn_ctx_runtime_error(ctx, "first argument must be a format string", NULL);
		return -2;
	}

	fmt = stringvalue(&argv[0]);
	res = spn_string_format_obj(fmt, argc - 1, &argv[1], &errmsg);

	if (res != NULL) {
		ret->type = SPN_TYPE_STRING;
		ret->v.o = res;
	} else {
		const void *args[1];
		args[0] = errmsg;
		spn_ctx_runtime_error(ctx, "error in format string: %s", args);
		free(errmsg);
		return -3;
	}

	return 0;
}

static void loadlib_string(SpnVMachine *vm)
{
	/* Methods */
	static const SpnExtFunc M[] = {
		{ "find",       rtlb_str_find   },
		{ "substr",     rtlb_substr     },
		{ "substrto",   rtlb_substrto   },
		{ "substrfrom", rtlb_substrfrom },
		{ "split",      rtlb_split      },
		{ "repeat",     rtlb_repeat     },
		{ "tolower",    rtlb_tolower    },
		{ "toupper",    rtlb_toupper    },
		{ "format",     rtlb_format     }
	};

	load_methods(vm, SPN_TTAG_STRING, M, COUNT(M));
}

/*****************
 * Array library *
 *****************/

/* The following functions realize an optimized in-place quicksort.
 * Most of this has been transliterated from Wikipedia's
 * pseudo-code at https://en.wikipedia.org/wiki/Quicksort
 */
static void rtlb_aux_swap(SpnArray *a, int i, int j)
{
	SpnValue x, y;
	spn_array_get(a, i, &x);
	spn_array_get(a, j, &y);

	spn_value_retain(&x);
	spn_array_set(a, i, &y);
	spn_array_set(a, j, &x);
	spn_value_release(&x);
}

static int rtlb_aux_partition(SpnArray *a, int left, int right,
	SpnFunction *comp, SpnContext *ctx, int *error)
{
	int store_idx = left;
	int pivot_idx = left + (right - left) / 2;
	int i;

	SpnValue pivot;
	spn_array_get(a, pivot_idx, &pivot);

	rtlb_aux_swap(a, pivot_idx, right);

	for (i = left; i < right; i++) {
		int lessthan;

		SpnValue ith_elem;
		spn_array_get(a, i, &ith_elem);

		/* compare pivot to i-th element */
		if (comp != NULL) {
			SpnValue ret;
			SpnValue argv[2];
			argv[0] = ith_elem;
			argv[1] = pivot;

			if (spn_ctx_callfunc(ctx, comp, &ret, 2, argv) != 0) {
				*error = 1;
				return 0;
			}

			if (!isbool(&ret)) {
				spn_ctx_runtime_error(ctx, "comparator function must return a Boolean", NULL);
				spn_value_release(&ret);
				*error = 1;
				return 0;
			}

			lessthan = boolvalue(&ret);
		} else {
			if (!spn_values_comparable(&ith_elem, &pivot)) {
				const void *args[2];
				args[0] = spn_type_name(ith_elem.type);
				args[1] = spn_type_name(pivot.type);

				spn_ctx_runtime_error(
					ctx,
					"attempt to sort uncomparable values"
					" of type %s and %s",
					args
				);

				*error = 1;
				return 0;
			}

			lessthan = spn_value_compare(&ith_elem, &pivot) < 0;
		}

		if (lessthan) {
			rtlb_aux_swap(a, i, store_idx++);
		}
	}

	rtlb_aux_swap(a, store_idx, right);
	return store_idx;
}

static int rtlb_aux_qsort(SpnArray *a, int left, int right, SpnFunction *comp, SpnContext *ctx)
{
	int pivot_index;
	int error = 0;

	if (left >= right) {
		return 0;
	}

	pivot_index = rtlb_aux_partition(a, left, right, comp, ctx, &error);
	if (error) {
		return -1;
	}

	if (rtlb_aux_qsort(a, left, pivot_index - 1, comp, ctx) != 0) {
		return -1;
	}

	if (rtlb_aux_qsort(a, pivot_index + 1, right, comp, ctx) != 0) {
		return -1;
	}

	return 0;
}

static int rtlb_sort(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	SpnArray *array;
	SpnFunction *comparator = NULL;

	if (argc < 1 || argc > 2) {
		spn_ctx_runtime_error(ctx, "one or two arguments are required", NULL);
		return -1;
	}

	if (!isarray(&argv[0])) {
		spn_ctx_runtime_error(ctx, "first argument must be an array", NULL);
		return -2;
	}

	array = arrayvalue(&argv[0]);

	if (argc == 2) {
		if (!isfunc(&argv[1])) {
			spn_ctx_runtime_error(ctx, "second argument must be a comparator function", NULL);
			return -3;
		}

		comparator = funcvalue(&argv[1]);
	}

	return rtlb_aux_qsort(array, 0, spn_array_count(array) - 1, comparator, ctx);
}

static int rtlb_join(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	size_t n, i, len = 0;
	char *buf = NULL;
	SpnArray *arr;
	SpnString *delim;

	if (argc != 2) {
		spn_ctx_runtime_error(ctx, "exactly two arguments are required", NULL);
		return -1;
	}

	if (!isarray(&argv[0])) {
		spn_ctx_runtime_error(ctx, "first argument must be an array", NULL);
		return -2;
	}

	if (!isstring(&argv[1])) {
		spn_ctx_runtime_error(ctx, "second argument must be a string", NULL);
		return -2;
	}

	arr = arrayvalue(&argv[0]);
	n = spn_array_count(arr);

	delim = stringvalue(&argv[1]);

	for (i = 0; i < n; i++) {
		size_t addlen;
		SpnString *str;

		SpnValue val;

		spn_array_get(arr, i, &val);
		if (!isstring(&val)) {
			free(buf);
			spn_ctx_runtime_error(ctx, "array must contain strings only", NULL);
			return -3;
		}

		/* XXX: this should really be solved using
		 * exponential buffer expansion. Maybe in alpha 2...
		 */

		str = stringvalue(&val);
		addlen = i > 0 ? delim->len + str->len : str->len;

		buf = spn_realloc(buf, len + addlen + 1);

		if (i > 0) {
			memcpy(buf + len, delim->cstr, delim->len);
			memcpy(buf + len + delim->len, str->cstr, str->len);
		} else {
			memcpy(buf + len, str->cstr, str->len);
		}

		len += addlen;
	}

	if (i > 0) {
		/* this may catch one error or two */
		assert(buf != NULL);

		/* add NUL terminator */
		buf[len] = 0;
		*ret = makestring_nocopy_len(buf, len, 1);
	} else {
		assert(buf == NULL);

		/* if there were no items to concatenate, return empty string
		 * (this is necessary because `buf` is now NULL,
		 * and we definitely don't want this to segfault)
		 */
		*ret = makestring_nocopy_len("", 0, 0);
	}

	return 0;
}

/* argv[0] is the array to enumerate
 * argv[1] is the callback function
 * args[0] is the value passed to the callback
 * args[1] is the index passed to the callback
 * cbret is the return value of the callback function
 */
static int rtlb_array_foreach(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	size_t i, n;
	SpnArray *arr;
	SpnFunction *predicate;

	if (argc != 2) {
		spn_ctx_runtime_error(ctx, "two arguments are required", NULL);
		return -1;
	}

	if (!isarray(&argv[0])) {
		spn_ctx_runtime_error(ctx, "first argument must be an array", NULL);
		return -2;
	}

	if (!isfunc(&argv[1])) {
		spn_ctx_runtime_error(ctx, "second argument must be a function", NULL);
		return -3;
	}

	arr = arrayvalue(&argv[0]);
	predicate = funcvalue(&argv[1]);
	n = spn_array_count(arr);

	for (i = 0; i < n; i++) {
		int err;
		SpnValue cbret;
		SpnValue args[2]; /* value and index */

		spn_array_get(arr, i, &args[0]);
		args[1] = makeint(i);

		err = spn_ctx_callfunc(ctx, predicate, &cbret, COUNT(args), args);

		if (err != 0) {
			return -4;
		}

		/* the callback must return a Boolean or nothing */
		if (isbool(&cbret)) {
			if (boolvalue(&cbret) == 0) {
				break;
			}
		} else if (notnil(&cbret)) {
			spn_value_release(&cbret);
			spn_ctx_runtime_error(ctx, "callback function must return boolean or nil", NULL);
			return -5;
		}
	}

	return 0;
}

static int rtlb_reduce(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	size_t i, n;
	SpnArray *arr;
	SpnValue args[2], tmp;
	SpnValue *arrval, *first;
	SpnFunction *func;

	if (argc != 3) {
		spn_ctx_runtime_error(ctx, "expecting three arguments", NULL);
		return -1;
	}

	arrval = &argv[0];
	first  = &argv[1];

	if (!isarray(arrval)) {
		spn_ctx_runtime_error(ctx, "first argument must be an array", NULL);
		return -2;
	}

	if (!isfunc(&argv[2])) {
		spn_ctx_runtime_error(ctx, "third argument must be a function", NULL);
		return -3;
	}

	func = funcvalue(&argv[2]);
	arr = arrayvalue(arrval);
	n = spn_array_count(arr);

	spn_value_retain(first);
	tmp = *first;

	for (i = 0; i < n; i++) {
		int err;
		args[0] = tmp;

		/* args[1] needn't be released: values returned by array getter
		 * functions are non-owning
		 */
		spn_array_get(arr, i, &args[1]);

		err = spn_ctx_callfunc(ctx, func, &tmp, COUNT(args), args);
		spn_value_release(&args[0]);

		if (err != 0) {
			return -4;
		}
	}

	*ret = tmp;
	return 0;
}

/* args[0] is the value passed to the predicate, args[1] is the index */
static int rtlb_array_filter(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	size_t i, n;
	SpnArray *orig, *filt;
	SpnFunction *predicate;

	if (argc != 2) {
		spn_ctx_runtime_error(ctx, "expecting two arguments", NULL);
		return -1;
	}

	if (!isarray(&argv[0])) {
		spn_ctx_runtime_error(ctx, "first argument must be an array", NULL);
		return -2;
	}

	if (!isfunc(&argv[1])) {
		spn_ctx_runtime_error(ctx, "second argument must be a function", NULL);
		return -3;
	}

	orig = arrayvalue(&argv[0]);
	predicate = funcvalue(&argv[1]);
	n = spn_array_count(orig);
	filt = spn_array_new();

	for (i = 0; i < n; i++) {
		SpnValue args[2];
		SpnValue cond;

		spn_array_get(orig, i, &args[0]);
		args[1] = makeint(i);

		if (spn_ctx_callfunc(ctx, predicate, &cond, COUNT(args), args) != 0) {
			spn_object_release(filt);
			return -4;
		}

		if (isbool(&cond)) {
			if (boolvalue(&cond)) {
				spn_array_push(filt, &args[0]);
			}
		} else {
			spn_value_release(&cond);
			spn_object_release(filt);
			spn_ctx_runtime_error(ctx, "predicate must return a boolean", NULL);
			return -5;
		}
	}

	ret->type = SPN_TYPE_ARRAY;
	ret->v.o = filt;
	return 0;
}

static int rtlb_array_map(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	size_t i, n;
	SpnArray *orig, *mapped;
	SpnFunction *predicate;

	if (argc != 2) {
		spn_ctx_runtime_error(ctx, "expecting two arguments", NULL);
		return -1;
	}

	if (!isarray(&argv[0])) {
		spn_ctx_runtime_error(ctx, "first argument must be an array", NULL);
		return -2;
	}

	if (!isfunc(&argv[1])) {
		spn_ctx_runtime_error(ctx, "second argument must be a function", NULL);
		return -3;
	}

	orig = arrayvalue(&argv[0]);
	predicate = funcvalue(&argv[1]);
	n = spn_array_count(orig);
	mapped = spn_array_new();

	for (i = 0; i < n; i++) {
		SpnValue result;
		SpnValue args[2];

		spn_array_get(orig, i, &args[0]);
		args[1] = makeint(i);

		if (spn_ctx_callfunc(ctx, predicate, &result, COUNT(args), args) != 0) {
			spn_object_release(mapped);
			return -4;
		}

		spn_array_push(mapped, &result);
		spn_value_release(&result);
	}

	ret->type = SPN_TYPE_ARRAY;
	ret->v.o = mapped;
	return 0;
}

static int rtlb_push(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	SpnArray *arr;

	if (argc != 2) {
		spn_ctx_runtime_error(ctx, "expecting 2 arguments", NULL);
		return -1;
	}

	if (!isarray(&argv[0])) {
		spn_ctx_runtime_error(ctx, "first argument must be an array", NULL);
		return -2;
	}

	arr = arrayvalue(&argv[0]);
	spn_array_push(arr, &argv[1]);

	return 0;
}

static int rtlb_pop(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	size_t n;
	SpnArray *arr;

	if (argc != 1) {
		spn_ctx_runtime_error(ctx, "expecting one argument", NULL);
		return -1;
	}

	if (!isarray(&argv[0])) {
		spn_ctx_runtime_error(ctx, "argument must be an array", NULL);
		return -2;
	}

	arr = arrayvalue(&argv[0]);
	n = spn_array_count(arr);

	if (n == 0) {
		spn_ctx_runtime_error(ctx, "cannot pop() empty array", NULL);
		return -3;
	}

	/* return last element */
	spn_array_get(arr, n - 1, ret);
	spn_value_retain(ret);

	/* remove it from array */
	spn_array_pop(arr);

	return 0;
}

static int rtlb_last(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	size_t n;
	SpnArray *arr;

	if (argc != 1) {
		spn_ctx_runtime_error(ctx, "expecting one argument", NULL);
		return -1;
	}

	if (!isarray(&argv[0])) {
		spn_ctx_runtime_error(ctx, "argument must be an array", NULL);
		return -2;
	}

	arr = arrayvalue(&argv[0]);
	n = spn_array_count(arr);

	if (n == 0) {
		spn_ctx_runtime_error(ctx, "cannot get last element of empty array", NULL);
		return -3;
	}

	spn_array_get(arr, n - 1, ret);
	spn_value_retain(ret);

	return 0;
}

static int rtlb_swap(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	SpnArray *arr;
	SpnValue tmp_a, tmp_b;
	long idx_a, idx_b;
	size_t n;

	if (argc != 3) {
		spn_ctx_runtime_error(ctx, "expecting 3 arguments", NULL);
		return -1;
	}

	if (!isarray(&argv[0])) {
		spn_ctx_runtime_error(ctx, "first argument must be an array", NULL);
		return -2;
	}

	if (!isint(&argv[1]) || !isint(&argv[2])) {
		spn_ctx_runtime_error(ctx, "second and third arguments must be integers", NULL);
		return -2;
	}

	arr = arrayvalue(&argv[0]);
	idx_a = intvalue(&argv[1]);
	idx_b = intvalue(&argv[2]);

	n = spn_array_count(arr);

	if (idx_a < 0 || idx_a >= n
	 || idx_b < 0 || idx_b >= n) {
		long sn = n;
		const void *args[2];
		args[0] = idx_a < 0 || idx_a >= n ? &idx_a : &idx_b;
		args[1] = &sn;
		spn_ctx_runtime_error(ctx, "index %d is out of bounds for array of size %d", args);
		return -3;
	}

	spn_array_get(arr, idx_a, &tmp_a);
	spn_array_get(arr, idx_b, &tmp_b);

	/* retain the original value of A, because it may
	 * be released, causing premature deallocation:
	 * when "arr[idx_a] = tmp_b" is executed, tmp_a may be released (if tmp_b is nil).
	 * - However, tmp_b is safe without an additional retain,
	 *   since by the time it may be released, it has already
	 *   been retained by the first setter.
	 */
	spn_value_retain(&tmp_a);

	spn_array_set(arr, idx_a, &tmp_b);
	spn_array_set(arr, idx_b, &tmp_a);

	spn_value_release(&tmp_a);

	return 0;
}

static int rtlb_reverse(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	size_t i, n;
	SpnArray *arr, *result;

	if (argc != 1) {
		spn_ctx_runtime_error(ctx, "expecting one argument", NULL);
		return -1;
	}

	if (!isarray(&argv[0])) {
		spn_ctx_runtime_error(ctx, "argument must be an array", NULL);
		return -2;
	}

	arr = arrayvalue(&argv[0]);
	n = spn_array_count(arr);

	*ret = makearray();
	result = arrayvalue(ret);

	/* copy elements of 'arr' into 'result' in reverse order */
	for (i = n; i > 0; i--) {
		SpnValue tmp;
		spn_array_get(arr, i - 1, &tmp);
		spn_array_push(result, &tmp);
	}

	return 0;
}

/* if "any" is nonzero, this function will return true if the
 * predicate returns true for any of the elements in the array.
 * if, however, "any" is zero, then it will only return true
 * if the predicate is true for all of the elements.
 */
static int rtlb_aux_anyall(SpnValue *ret, int argc, SpnValue *argv, void *ctx, int any)
{
	size_t i, n;
	SpnArray *arr;
	SpnFunction *predicate;

	if (argc != 2) {
		spn_ctx_runtime_error(ctx, "expecting two arguments", NULL);
		return -1;
	}

	if (!isarray(&argv[0])) {
		spn_ctx_runtime_error(ctx, "first argument must be an array", NULL);
		return -2;
	}

	if (!isfunc(&argv[1])) {
		spn_ctx_runtime_error(ctx, "second argument must be a function", NULL);
		return -3;
	}

	arr = arrayvalue(&argv[0]);
	predicate = funcvalue(&argv[1]);

	*ret = any ? spn_falseval : spn_trueval;

	n = spn_array_count(arr);

	for (i = 0; i < n; i++) {
		SpnValue keyval[2]; /* item 0: value, item 1: index */
		SpnValue result;

		spn_array_get(arr, i, &keyval[0]);
		keyval[1] = makeint(i);

		if (spn_ctx_callfunc(ctx, predicate, &result, COUNT(keyval), keyval) != 0) {
			return -4;
		}

		if (!isbool(&result)) {
			spn_value_release(&result);
			spn_ctx_runtime_error(ctx, "predicate must return a Boolean", NULL);
			return -5;
		}

		if (any && boolvalue(&result)) {
			*ret = spn_trueval;
			break;
		} else if (!any && !boolvalue(&result)) {
			*ret = spn_falseval;
			break;
		}
	}

	return 0;
}

static int rtlb_any(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	return rtlb_aux_anyall(ret, argc, argv, ctx, 1);
}

static int rtlb_all(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	return rtlb_aux_anyall(ret, argc, argv, ctx, 0);
}

static int rtlb_arr_find(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	size_t i, n;
	SpnArray *arr;

	if (argc != 2) {
		spn_ctx_runtime_error(ctx, "expecting two arguments", NULL);
		return -1;
	}

	if (!isarray(&argv[0])) {
		spn_ctx_runtime_error(ctx, "first argument must be an array", NULL);
		return -2;
	}

	arr = arrayvalue(&argv[0]);
	n = spn_array_count(arr);

	for (i = 0; i < n; i++) {
		SpnValue tmp;
		spn_array_get(arr, i, &tmp);
		if (spn_value_equal(&tmp, &argv[1])) {
			*ret = makeint(i);
			return 0;
		}
	}

	*ret = makeint(-1);
	return 0;
}

static int rtlb_pfind(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	size_t i, n;
	SpnArray *arr;
	SpnFunction *predicate;

	if (argc != 2) {
		spn_ctx_runtime_error(ctx, "expecting two arguments", NULL);
		return -1;
	}

	if (!isarray(&argv[0])) {
		spn_ctx_runtime_error(ctx, "first argument must be an array", NULL);
		return -2;
	}

	if (!isfunc(&argv[1])) {
		spn_ctx_runtime_error(ctx, "second argument must be a function", NULL);
		return -3;
	}

	arr = arrayvalue(&argv[0]);
	predicate = funcvalue(&argv[1]);
	n = spn_array_count(arr);

	for (i = 0; i < n; i++) {
		SpnValue tmp, pret;
		spn_array_get(arr, i, &tmp);

		if (spn_ctx_callfunc(ctx, predicate, &pret, 1, &tmp) != 0) {
			return -4;
		}

		if (!isbool(&pret)) {
			spn_value_release(&pret);
			spn_ctx_runtime_error(ctx, "predicate must return a Boolean", NULL);
			return -5;
		}

		if (boolvalue(&pret)) {
			*ret = makeint(i);
			return 0;
		}
	}

	*ret = makeint(-1);
	return 0;
}

/* return value:
 * > 0 if a < b
 * = 0 if a >= b
 * < 0 if an error occurred
 */
static int rtlb_aux_bsearch_compare(SpnValue vals[2], SpnFunction *predicate, SpnContext *ctx)
{
	const void *args[2];

	if (predicate) {
		SpnValue pret;
		if (spn_ctx_callfunc(ctx, predicate, &pret, 2, vals) != 0) {
			return -1;
		}

		if (!isbool(&pret)) {
			spn_value_release(&pret);
			spn_ctx_runtime_error(ctx, "predicate must return a Boolean", NULL);
			return -2;
		}

		return boolvalue(&pret);
	}

	/* if no predicate was supplied, use the "<" operator */
	if (spn_values_comparable(&vals[0], &vals[1])) {
		/* "spn_value_compare(a, b) < 0" <=> "a < b" */
		return spn_value_compare(&vals[0], &vals[1]) < 0;
	}

	/* if the values are not orderable, we're in trouble */
	args[0] = spn_type_name(vals[0].type);
	args[1] = spn_type_name(vals[1].type);
	spn_ctx_runtime_error(ctx, "cannot compare values of type %s and %s", args);
	return -3;
}

static int rtlb_aux_bsearch(SpnValue *ret, SpnArray *arr, SpnValue *elem,
	size_t lower, size_t upper, SpnFunction *predicate, SpnContext *ctx) {
	int is_smaller, is_greater;
	size_t middle;
	SpnValue vals[2];

	assert(lower <= upper);

	if (lower == upper) {
		*ret = makeint(-1);
		return 0;
	}

	middle = lower + (upper - lower) / 2;

	/* first, check if elem < middle */
	vals[0] = *elem;
	spn_array_get(arr, middle, &vals[1]);
	is_smaller = rtlb_aux_bsearch_compare(vals, predicate, ctx);

	/* check for errors */
	if (is_smaller < 0) {
		return -1;
	}

	/* key is smaller than middle element,
	 * so we search the lower half of the array
	 */
	if (is_smaller > 0) {
		return rtlb_aux_bsearch(ret, arr, elem, lower, middle, predicate, ctx);
	}

	/* key was not smaller than middle element,
	 * so check if "elem > middle"
	 */
	vals[1] = *elem;
	spn_array_get(arr, middle, &vals[0]);
	is_greater = rtlb_aux_bsearch_compare(vals, predicate, ctx);

	/* check for errors again */
	if (is_greater < 0) {
		return -1;
	}

	/* key is greater than middle element, continue
	 * search with upper half of the array
	 */
	if (is_greater > 0) {
		return rtlb_aux_bsearch(ret, arr, elem, middle + 1, upper, predicate, ctx);
	}

	/* otherwise, the key is neither smaller nor greater than
	 * the middle element, so they are equal to each other
	 */
	*ret = makeint(middle);
	return 0;
}

static int rtlb_bsearch(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	SpnArray *arr;
	SpnFunction *predicate;
	size_t n;

	if (argc < 2 || argc > 3) {
		spn_ctx_runtime_error(ctx, "expecting 2 or 3 arguments", NULL);
		return -1;
	}

	if (!isarray(&argv[0])) {
		spn_ctx_runtime_error(ctx, "first argument must be an array", NULL);
		return -2;
	}

	if (argc >= 3 && !isfunc(&argv[2])) {
		spn_ctx_runtime_error(ctx, "third argument must be a function", NULL);
		return -3;
	}

	arr = arrayvalue(&argv[0]);
	predicate = argc >= 3 ? funcvalue(&argv[2]) : NULL;
	n = spn_array_count(arr);

	return rtlb_aux_bsearch(ret, arr, &argv[1], 0, n, predicate, ctx);
}

static int rtlb_slice(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	SpnArray *arr, *result;
	long i, idx, len, n;

	if (argc != 3) {
		spn_ctx_runtime_error(ctx, "expecting 3 arguments", NULL);
		return -1;
	}

	if (!isarray(&argv[0])) {
		spn_ctx_runtime_error(ctx, "first argument must be an array", NULL);
		return -2;
	}

	if (!isint(&argv[1])) {
		spn_ctx_runtime_error(ctx, "second argument must be an integer index", NULL);
		return -3;
	}

	if (!isint(&argv[2])) {
		spn_ctx_runtime_error(ctx, "third argument must be an integer length", NULL);
		return -4;
	}

	idx = intvalue(&argv[1]);
	len = intvalue(&argv[2]);
	arr = arrayvalue(&argv[0]);
	n = spn_array_count(arr);

	if (idx < 0 || idx > n) {
		const void *args[2];
		args[0] = &idx;
		args[1] = &n;
		spn_ctx_runtime_error(ctx, "starting index %d is out of bounds for array of length %d", args);
		return -5;
	}

	if (len < 0) {
		const void *args[1];
		args[0] = &len;
		spn_ctx_runtime_error(ctx, "length was negative (%d)", args);
		return -6;
	}

	if (idx + len > n) {
		const void *args[3];
		long end = idx + len;
		args[0] = &idx;
		args[1] = &end;
		args[2] = &n;
		spn_ctx_runtime_error(ctx, "range [%d, %d) out of bounds for array of size %d", args);
		return -7;
	}

	*ret = makearray();
	result = arrayvalue(ret);

	for (i = 0; i < len; i++) {
		SpnValue tmp;
		spn_array_get(arr, idx + i, &tmp);
		spn_array_push(result, &tmp);
	}

	return 0;
}

static int rtlb_insert(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	SpnArray *arr;
	long index, size;

	if (argc != 3) {
		spn_ctx_runtime_error(ctx, "expecting 3 arguments", NULL);
		return -1;
	}

	if (!isarray(&argv[0])) {
		spn_ctx_runtime_error(ctx, "first argument must be an array", NULL);
		return -2;
	}

	if (!isint(&argv[2])) {
		spn_ctx_runtime_error(ctx, "third argument must be an integer", NULL);
		return -3;
	}

	arr = arrayvalue(&argv[0]);
	size = spn_array_count(arr);
	index = intvalue(&argv[2]);

	if (index < 0 || index > size) {
		const void *args[2];
		args[0] = &index;
		args[1] = &size;
		spn_ctx_runtime_error(ctx, "index %d out of bounds for array of size %d", args);
		return -4;
	}

	spn_array_insert(arr, index, &argv[1]);

	return 0;
}

static int rtlb_inject(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	SpnArray *haystack, *needle;
	long index, hsize;

	if (argc < 2 || argc > 3) {
		spn_ctx_runtime_error(ctx, "expecting 2 or 3 arguments", NULL);
		return -1;
	}

	if (!isarray(&argv[0]) || !isarray(&argv[1])) {
		spn_ctx_runtime_error(ctx, "haystack and needle must be arrays", NULL);
		return -2;
	}

	if (argc >= 3 && !isint(&argv[2])) {
		spn_ctx_runtime_error(ctx, "index must be an integer", NULL);
		return -3;
	}

	haystack = arrayvalue(&argv[0]);
	needle = arrayvalue(&argv[1]);
	hsize = spn_array_count(haystack);
	index = argc >= 3 ? intvalue(&argv[2]) : hsize;

	if (index < 0 || index > hsize) {
		const void *args[2];
		args[0] = &index;
		args[1] = &hsize;
		spn_ctx_runtime_error(ctx, "index %d out of bounds for array of size %d", args);
		return -4;
	}

	spn_array_inject(haystack, index, needle);

	return 0;
}

static int rtlb_erase(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	SpnArray *arr;
	long index, n;

	if (argc != 2) {
		spn_ctx_runtime_error(ctx, "expecting 2 arguments", NULL);
		return -1;
	}

	if (!isarray(&argv[0])) {
		spn_ctx_runtime_error(ctx, "first argument must be an array", NULL);
		return -2;
	}

	if (!isint(&argv[1])) {
		spn_ctx_runtime_error(ctx, "second argument must be an integer index", NULL);
		return -3;
	}

	arr = arrayvalue(&argv[0]);
	n = spn_array_count(arr);
	index = intvalue(&argv[1]);

	if (index < 0 || index >= n) {
		const void *args[2];
		args[0] = &index;
		args[1] = &n;
		spn_ctx_runtime_error(ctx, "index %d out of bounds for array of size %d", args);
		return -4;
	}

	spn_array_remove(arr, index);

	return 0;
}

static int rtlb_concat(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	SpnArray *result;
	int i;

	*ret = makearray();
	result = arrayvalue(ret);

	for (i = 0; i < argc; i++) {
		SpnArray *arr;
		size_t j, n;

		if (!isarray(&argv[i])) {
			const void *args[2];
			int argidx = i + 1;
			args[0] = &argidx;
			args[1] = spn_type_name(argv[i].type);
			spn_ctx_runtime_error(ctx, "arguments must be arrays (arg %i was %s)", args);
			spn_value_release(ret);
			return -1;
		}

		arr = arrayvalue(&argv[i]);
		n = spn_array_count(arr);

		for (j = 0; j < n; j++) {
			SpnValue tmp;
			spn_array_get(arr, j, &tmp);
			spn_array_push(result, &tmp);
		}
	}

	return 0;
}

static void loadlib_array(SpnVMachine *vm)
{
	/* Methods */
	static const SpnExtFunc M[] = {
		{ "sort",       rtlb_sort          },
		{ "find",       rtlb_arr_find      },
		{ "pfind",      rtlb_pfind         },
		{ "bsearch",    rtlb_bsearch       },
		{ "any",        rtlb_any           },
		{ "all",        rtlb_all           },
		{ "slice",      rtlb_slice         },
		{ "join",       rtlb_join          },
		{ "foreach",    rtlb_array_foreach },
		{ "reduce",     rtlb_reduce        },
		{ "filter",     rtlb_array_filter  },
		{ "map",        rtlb_array_map     },
		{ "insert",     rtlb_insert        },
		{ "inject",     rtlb_inject        },
		{ "erase",      rtlb_erase         },
		{ "concat",     rtlb_concat        },
		{ "push",       rtlb_push          },
		{ "pop",        rtlb_pop           },
		{ "last",       rtlb_last          },
		{ "swap",       rtlb_swap          },
		{ "reverse",    rtlb_reverse       }
	};

	load_methods(vm, SPN_TTAG_ARRAY, M, COUNT(M));
}

/*******************
 * Hashmap library *
 *******************/

static int rtlb_hashmap_foreach(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	size_t cursor = 0;
	SpnHashMap *hm;
	SpnFunction *callback;
	SpnValue valkey[2];

	if (argc != 2) {
		spn_ctx_runtime_error(ctx, "expecting two arguments", NULL);
		return -1;
	}

	if (!ishashmap(&argv[0])) {
		spn_ctx_runtime_error(ctx, "first argument must be a hashmap", NULL);
		return -2;
	}

	if (!isfunc(&argv[1])) {
		spn_ctx_runtime_error(ctx, "second argument must be a function", NULL);
		return -3;
	}

	hm = hashmapvalue(&argv[0]);
	callback = funcvalue(&argv[1]);

	while ((cursor = spn_hashmap_next(hm, cursor, &valkey[1], &valkey[0])) != 0) {
		if (spn_ctx_callfunc(ctx, callback, NULL, COUNT(valkey), valkey) != 0) {
			return -4;
		}
	}

	return 0;
}

static int rtlb_hashmap_map(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	size_t cursor = 0;
	SpnHashMap *hm;
	SpnHashMap *result;
	SpnFunction *transform;
	SpnValue valkey[2];

	if (argc != 2) {
		spn_ctx_runtime_error(ctx, "expecting two arguments", NULL);
		return -1;
	}

	if (!ishashmap(&argv[0])) {
		spn_ctx_runtime_error(ctx, "first argument must be a hashmap", NULL);
		return -2;
	}

	if (!isfunc(&argv[1])) {
		spn_ctx_runtime_error(ctx, "second argument must be a function", NULL);
		return -3;
	}

	result = spn_hashmap_new();
	hm = hashmapvalue(&argv[0]);
	transform = funcvalue(&argv[1]);

	while ((cursor = spn_hashmap_next(hm, cursor, &valkey[1], &valkey[0])) != 0) {
		SpnValue tmp;
		if (spn_ctx_callfunc(ctx, transform, &tmp, COUNT(valkey), valkey) != 0) {
			spn_object_release(result);
			return -4;
		}

		spn_hashmap_set(result, &valkey[1], &tmp);
		spn_value_release(&tmp);
	}

	ret->type = SPN_TYPE_HASHMAP;
	ret->v.o = result;

	return 0;
}

static int rtlb_hashmap_filter(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	size_t cursor = 0;
	SpnHashMap *hm;
	SpnHashMap *result;
	SpnFunction *predicate;
	SpnValue valkey[2];

	if (argc != 2) {
		spn_ctx_runtime_error(ctx, "expecting two arguments", NULL);
		return -1;
	}

	if (!ishashmap(&argv[0])) {
		spn_ctx_runtime_error(ctx, "first argument must be a hashmap", NULL);
		return -2;
	}

	if (!isfunc(&argv[1])) {
		spn_ctx_runtime_error(ctx, "second argument must be a function", NULL);
		return -3;
	}

	result = spn_hashmap_new();
	hm = hashmapvalue(&argv[0]);
	predicate = funcvalue(&argv[1]);

	while ((cursor = spn_hashmap_next(hm, cursor, &valkey[1], &valkey[0])) != 0) {
		SpnValue tmp;
		if (spn_ctx_callfunc(ctx, predicate, &tmp, COUNT(valkey), valkey) != 0) {
			spn_object_release(result);
			return -4;
		}

		if (!isbool(&tmp)) {
			spn_object_release(result);
			spn_value_release(&tmp);
			spn_ctx_runtime_error(ctx, "predicate must return a Boolean", NULL);
			return -5;
		}

		if (boolvalue(&tmp)) {
			spn_hashmap_set(result, &valkey[1], &valkey[0]);
		}
	}

	ret->type = SPN_TYPE_HASHMAP;
	ret->v.o = result;

	return 0;
}

/* if "getvals" is nonzero, this function will return an array composed
 * of the values of the hashmap passed as its argument.
 * Else, it will return an array of all the keys thereof.
 */
static int rtlb_aux_keyval(SpnValue *ret, int argc, SpnValue *argv, void *ctx, int getvals)
{
	size_t it = 0;
	SpnValue key, val;
	SpnHashMap *hm;
	SpnArray *result;

	if (argc != 1) {
		spn_ctx_runtime_error(ctx, "expecting one argument", NULL);
		return -1;
	}

	if (!ishashmap(&argv[0])) {
		spn_ctx_runtime_error(ctx, "argument must be a hashmap", NULL);
		return -2;
	}

	*ret = makearray();
	result = arrayvalue(ret);
	hm = hashmapvalue(&argv[0]);

	while ((it = spn_hashmap_next(hm, it, &key, &val)) != 0) {
		spn_array_push(result, getvals ? &val : &key);
	}

	return 0;
}

static int rtlb_keys(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{

	return rtlb_aux_keyval(ret, argc, argv, ctx, 0);
}

static int rtlb_values(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{

	return rtlb_aux_keyval(ret, argc, argv, ctx, 1);
}

static int rtlb_combine(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	size_t n, i;
	SpnArray *keys, *vals;
	SpnHashMap *result;

	if (argc != 2) {
		spn_ctx_runtime_error(ctx, "expecting two arguments", NULL);
		return -1;
	}

	if (!isarray(&argv[0]) || !isarray(&argv[1])) {
		spn_ctx_runtime_error(ctx, "arguments must be arrays", NULL);
		return -2;
	}

	*ret = makehashmap();
	result = hashmapvalue(ret);
	keys = arrayvalue(&argv[0]);
	vals = arrayvalue(&argv[1]);

	n = spn_array_count(keys);

	if (spn_array_count(vals) != n) {
		spn_ctx_runtime_error(ctx, "there must be exactly as many values as keys", NULL);
		return -3;
	}

	for (i = 0; i < n; i++) {
		SpnValue key, val;
		spn_array_get(keys, i, &key);
		spn_array_get(vals, i, &val);
		spn_hashmap_set(result, &key, &val);
	}

	return 0;
}

static void loadlib_hashmap(SpnVMachine *vm)
{
	/* Free functions */
	static const SpnExtFunc F[] = {
		{ "combine", rtlb_combine }
	};

	static const SpnExtFunc M[] = {
		{ "foreach", rtlb_hashmap_foreach },
		{ "map",     rtlb_hashmap_map     },
		{ "filter",  rtlb_hashmap_filter  },
		{ "keys",    rtlb_keys            },
		{ "values",  rtlb_values          }
	};

	spn_vm_addlib_cfuncs(vm, NULL, F, COUNT(F));
	load_methods(vm, SPN_TTAG_HASHMAP, M, COUNT(M));
}


/*****************
 * Maths library *
 *****************/

/* this is a little helper function to get a floating-point value
 * out of an SpnValue, even if it contains an integer.
 */
static double val2float(SpnValue *val)
{
	assert(isnum(val));
	return isfloat(val) ? floatvalue(val) : intvalue(val);
}

/* cube root function, because c89 doesn't define it */
static double rtlb_aux_cbrt(double x)
{
	double s = x < 0 ? -1.0 : +1.0;
	return s * pow(s * x, 1.0 / 3.0);
}

/* 2 ^ x and 10 ^ x (not in stdlib either) */
static double rtlb_aux_exp2(double x)
{
	return pow(2.0, x);
}

static double rtlb_aux_exp10(double x)
{
	return pow(10.0, x);
}

/* base-2 logarithm */
static double rtlb_aux_log2(double x)
{
	return log(x) / M_LN2;
}

/* round half away from zero correctly; stolen from glibc */
static double rtlb_aux_round(double x)
{
	double y;

	if (x >= 0) {
		y = floor(x);
		if (x - y >= 0.5) {
			y += 1.0;
		}
	} else {
		y = ceil(x);
		if (y - x >= 0.5) {
			y -= 1.0;
		}
	}

	return y;
}

static int rtlb_aux_intize(SpnValue *ret, int argc, SpnValue *argv, SpnContext *ctx, double (*fn)(double))
{
	double x;

	if (argc != 1) {
		spn_ctx_runtime_error(ctx, "exactly one argument is required", NULL);
		return -1;
	}

	if (!isnum(&argv[0])) {
		spn_ctx_runtime_error(ctx, "argument must be a number", NULL);
		return -2;
	}

	x = val2float(&argv[0]);

	if (x < LONG_MIN || x > LONG_MAX) {
		spn_ctx_runtime_error(ctx, "argument is out of range of integers", NULL);
		return -3;
	}

	*ret = makeint(fn(x));

	return 0;
}

static int rtlb_floor(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	return rtlb_aux_intize(ret, argc, argv, ctx, floor);
}

static int rtlb_ceil(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	return rtlb_aux_intize(ret, argc, argv, ctx, ceil);
}

static int rtlb_round(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	return rtlb_aux_intize(ret, argc, argv, ctx, rtlb_aux_round);
}

static int rtlb_sgn(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	if (argc != 1) {
		spn_ctx_runtime_error(ctx, "exactly one argument is required", NULL);
		return -1;
	}

	if (!isnum(&argv[0])) {
		spn_ctx_runtime_error(ctx, "argument must be a number", NULL);
		return -2;
	}

	if (isfloat(&argv[0])) {
		double x = floatvalue(&argv[0]);

		if (x > 0.0) {
			*ret = makefloat(+1.0);
		} else if (x < 0.0) {
			*ret = makefloat(-1.0);
		} else if (x == 0.0) {
			*ret = makefloat(0.0); /* yup, always +0 */
		} else {
			*ret = makefloat(0.0 / 0.0); /* sgn(NaN) = NaN */
		}
	} else {
		long x = intvalue(&argv[0]);
		*ret = makeint(x > 0 ? +1 : x < 0 ? -1 : 0);
	}

	return 0;
}

static int rtlb_aux_unmath(SpnValue *ret, int argc, SpnValue *argv, SpnContext *ctx, double (*fn)(double))
{
	double x;

	if (argc != 1) {
		spn_ctx_runtime_error(ctx, "exactly one argument is required", NULL);
		return -1;
	}

	if (!isnum(&argv[0])) {
		spn_ctx_runtime_error(ctx, "argument must be a number", NULL);
		return -2;
	}

	x = val2float(&argv[0]);

	*ret = makefloat(fn(x));

	return 0;
}

/* I've done my best refactoring this. Still utterly ugly. Any suggestions? */
static int rtlb_sqrt(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	return rtlb_aux_unmath(ret, argc, argv, ctx, sqrt);
}

static int rtlb_cbrt(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	return rtlb_aux_unmath(ret, argc, argv, ctx, rtlb_aux_cbrt);
}

static int rtlb_exp(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	return rtlb_aux_unmath(ret, argc, argv, ctx, exp);
}

static int rtlb_exp2(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	return rtlb_aux_unmath(ret, argc, argv, ctx, rtlb_aux_exp2);
}

static int rtlb_exp10(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	return rtlb_aux_unmath(ret, argc, argv, ctx, rtlb_aux_exp10);
}

static int rtlb_log(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	return rtlb_aux_unmath(ret, argc, argv, ctx, log);
}

static int rtlb_log2(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	return rtlb_aux_unmath(ret, argc, argv, ctx, rtlb_aux_log2);
}

static int rtlb_log10(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	return rtlb_aux_unmath(ret, argc, argv, ctx, log10);
}

static int rtlb_sin(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	return rtlb_aux_unmath(ret, argc, argv, ctx, sin);
}

static int rtlb_cos(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	return rtlb_aux_unmath(ret, argc, argv, ctx, cos);
}

static int rtlb_tan(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	return rtlb_aux_unmath(ret, argc, argv, ctx, tan);
}

static int rtlb_sinh(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	return rtlb_aux_unmath(ret, argc, argv, ctx, sinh);
}

static int rtlb_cosh(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	return rtlb_aux_unmath(ret, argc, argv, ctx, cosh);
}

static int rtlb_tanh(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	return rtlb_aux_unmath(ret, argc, argv, ctx, tanh);
}

static int rtlb_asin(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	return rtlb_aux_unmath(ret, argc, argv, ctx, asin);
}

static int rtlb_acos(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	return rtlb_aux_unmath(ret, argc, argv, ctx, acos);
}

static int rtlb_atan(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	return rtlb_aux_unmath(ret, argc, argv, ctx, atan);
}
/* end of horror */

static int rtlb_atan2(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	double x, y;

	if (argc != 2) {
		spn_ctx_runtime_error(ctx, "exactly two arguments are required", NULL);
		return -1;
	}

	if (!isnum(&argv[0]) || !isnum(&argv[1])) {
		spn_ctx_runtime_error(ctx, "arguments must be numbers", NULL);
		return -2;
	}

	y = val2float(&argv[0]);
	x = val2float(&argv[1]);
	*ret = makefloat(atan2(y, x));

	return 0;
}

static int rtlb_hypot(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	double h = 0.0;
	int i;

	for (i = 0; i < argc; i++) {
		double x;

		if (!isnum(&argv[i])) {
			spn_ctx_runtime_error(ctx, "arguments must be numbers", NULL);
			return -1;
		}

		x = val2float(&argv[i]);
		h += x * x;
	}

	*ret = makefloat(sqrt(h));

	return 0;
}

static int rtlb_deg2rad(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	if (argc != 1) {
		spn_ctx_runtime_error(ctx, "exactly one argument is required", NULL);
		return -1;
	}

	if (!isnum(&argv[0])) {
		spn_ctx_runtime_error(ctx, "argument must be a number", NULL);
		return -2;
	}

	*ret = makefloat(val2float(&argv[0]) / 180.0 * M_PI);

	return 0;
}

static int rtlb_rad2deg(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	if (argc != 1) {
		spn_ctx_runtime_error(ctx, "exactly one argument is required", NULL);
		return -1;
	}

	if (!isnum(&argv[0])) {
		spn_ctx_runtime_error(ctx, "argument must be a number", NULL);
		return -2;
	}

	*ret = makefloat(val2float(&argv[0]) / M_PI * 180.0);

	return 0;
}

static int rtlb_random(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	/* I am sorry for the skew, but no way I am putting an unbounded loop
	 * into this function. `rand()` is already pretty bad on its own, so
	 * it's pointless to try improving it for simple use cases. If one
	 * needs a decent PRNG, one will use a dedicated library anyway.
	 */
	*ret = makefloat(rand() * 1.0 / RAND_MAX);

	return 0;
}

static int rtlb_seed(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	if (argc != 1) {
		spn_ctx_runtime_error(ctx, "exactly one argument is required", NULL);
		return -1;
	}

	if (!isint(&argv[0])) {
		spn_ctx_runtime_error(ctx, "argument must be an integer", NULL);
		return -2;
	}

	srand(intvalue(&argv[0]));

	return 0;
}

/* C89 doesn't provide these, so here are implementations that work in general.
 * if they don't work on your platform, let me know and I'll do something
 * about it.
 */
static int rtlb_aux_isnan(double x)
{
	return x != x;
}

static int rtlb_aux_isfin(double x)
{
	double zero = x - x;
	return x == x && zero == zero;
}

static int rtlb_aux_isinf(double x)
{
	double zero = x - x;
	return x == x && zero != zero;
}

/* floating-point classification: double -> boolean */
static int rtlb_aux_fltclass(SpnValue *ret, int argc, SpnValue *argv, SpnContext *ctx, int (*fn)(double))
{
	if (argc != 1) {
		spn_ctx_runtime_error(ctx, "exactly one argument is required", NULL);
		return -1;
	}

	if (!isnum(&argv[0])) {
		spn_ctx_runtime_error(ctx, "argument must be a number", NULL);
		return -2;
	}

	*ret = makebool(fn(val2float(&argv[0])));

	return 0;
}

static int rtlb_isfin(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	return rtlb_aux_fltclass(ret, argc, argv, ctx, rtlb_aux_isfin);
}

static int rtlb_isinf(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	return rtlb_aux_fltclass(ret, argc, argv, ctx, rtlb_aux_isinf);
}

static int rtlb_isnan(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	return rtlb_aux_fltclass(ret, argc, argv, ctx, rtlb_aux_isnan);
}

static int rtlb_abs(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	if (argc != 1) {
		spn_ctx_runtime_error(ctx, "exactly one argument is required", NULL);
		return -1;
	}

	if (!isnum(&argv[0])) {
		spn_ctx_runtime_error(ctx, "argument must be a number", NULL);
		return -2;
	}

	*ret = argv[0]; /* don't need to retain a number */

	if (isfloat(ret)) {
		ret->v.f = fabs(ret->v.f);
	} else if (ret->v.i < 0) {
		ret->v.i = -ret->v.i;
	}

	return 0;
}

static int rtlb_pow(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	if (argc != 2) {
		spn_ctx_runtime_error(ctx, "exactly two arguments are required", NULL);
		return -1;
	}

	if (!isnum(&argv[0]) || !isnum(&argv[1])) {
		spn_ctx_runtime_error(ctx, "arguments must be numbers", NULL);
		return -2;
	}

	/* if either of the base or the exponent is real,
	 * then the result is real too.
	 * Furthermore, if both the base and the exponent are integers,
	 * but the exponent is negative, then the result is real.
	 * The result is an integer only when the base is an integer and
	 * the exponent is a non-negative integer at the same time.
	 */
	if (isfloat(&argv[0]) || isfloat(&argv[1]) || intvalue(&argv[1]) < 0) {
		double x = val2float(&argv[0]);
		double y = val2float(&argv[1]);
		*ret = makefloat(pow(x, y));
	} else {
		/* base, exponent, result */
		long b = intvalue(&argv[0]);
		long e = intvalue(&argv[1]);
		long r = 1;

		/* exponentation by squaring - http://stackoverflow.com/q/101439/ */
		while (e != 0) {
			if (e & 0x01) {
				r *= b;
			}

			b *= b;
			e >>= 1;
		}

		*ret = makeint(r);
	}

	return 0;
}

static int rtlb_min(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	int i;

	if (argc < 1) {
		spn_ctx_runtime_error(ctx, "at least one argument is required", NULL);
		return -1;
	}

	if (!isnum(&argv[0])) {
		spn_ctx_runtime_error(ctx, "arguments must be numbers", NULL);
		return -2;
	}

	*ret = argv[0]; /* again, no need to retain numbers */

	for (i = 1; i < argc; i++) {
		if (!isnum(&argv[i])) {
			spn_ctx_runtime_error(ctx, "arguments must be numbers", NULL);
			return -2;
		}

		if (isfloat(&argv[i])) {
			if (isfloat(ret)) {
				if (floatvalue(&argv[i]) < floatvalue(ret)) {
					*ret = argv[i];
				}
			} else {
				if (floatvalue(&argv[i]) < intvalue(ret)) {
					*ret = argv[i];
				}
			}
		} else {
			if (isfloat(ret)) {
				if (intvalue(&argv[i]) < floatvalue(ret)) {
					*ret = argv[i];
				}
			} else {
				if (intvalue(&argv[i]) < intvalue(ret)) {
					*ret = argv[i];
				}
			}
		}
	}

	return 0;
}

static int rtlb_max(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	int i;

	if (argc < 1) {
		spn_ctx_runtime_error(ctx, "at least one argument is required", NULL);
		return -1;
	}

	if (!isnum(&argv[0])) {
		spn_ctx_runtime_error(ctx, "arguments must be numbers", NULL);
		return -2;
	}

	*ret = argv[0];

	for (i = 1; i < argc; i++) {
		if (!isnum(&argv[i])) {
			spn_ctx_runtime_error(ctx, "arguments must be numbers", NULL);
			return -2;
		}

		if (isfloat(&argv[i])) {
			if (isfloat(ret)) {
				if (floatvalue(&argv[i]) > floatvalue(ret)) {
					*ret = argv[i];
				}
			} else {
				if (floatvalue(&argv[i]) > intvalue(ret)) {
					*ret = argv[i];
				}
			}
		} else {
			if (isfloat(ret)) {
				if (intvalue(&argv[i]) > floatvalue(ret)) {
					*ret = argv[i];
				}
			} else {
				if (intvalue(&argv[i]) > intvalue(ret)) {
					*ret = argv[i];
				}
			}
		}
	}

	return 0;
}

static int rtlb_range(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	int i;
	SpnArray *range;

	if (argc < 1 || argc > 3) {
		spn_ctx_runtime_error(ctx, "expecting 1, 2 or 3 arguments", NULL);
		return -1;
	}

	for (i = 0; i < argc; i++) {
		if (!isnum(&argv[i])) {
			spn_ctx_runtime_error(ctx, "arguments must be numbers", NULL);
			return -2;
		}
	}

	switch (argc) {
	case 1: {
		long i, n;

		if (!isint(&argv[0])) {
			spn_ctx_runtime_error(ctx, "argument must be an integer", NULL);
			return -3;
		}

		range = spn_array_new();

		for (i = 0, n = intvalue(&argv[0]); i < n; i++) {
			SpnValue v = makeint(i);
			spn_array_push(range, &v);
		}

		break;
	}
	case 2: {
		long i, begin, end;

		if (!isint(&argv[0]) || !isint(&argv[1])) {
			spn_ctx_runtime_error(ctx, "arguments must be integers", NULL);
			return -3;
		}

		range = spn_array_new();

		begin = intvalue(&argv[0]);
		end = intvalue(&argv[1]);

		for (i = begin; i < end; i++) {
			SpnValue v = makeint(i);
			spn_array_push(range, &v);
		}

		break;
	}
	case 3: {
#define FLOATVAL(f) (isint(f) ? intvalue(f) : floatvalue(f))

		double begin = FLOATVAL(&argv[0]);
		double end   = FLOATVAL(&argv[1]);
		double step  = FLOATVAL(&argv[2]);

#undef FLOATVAL

		long i;
		double x;

		range = spn_array_new();

		for (i = 0, x = begin; x <= end; x = begin + step * ++i) {
			SpnValue v = makefloat(x);
			spn_array_push(range, &v);
		}

		break;
	}
	default:
		SHANT_BE_REACHED();
		return -1; /* silence "used uninitialized" warning */
	}

	ret->type = SPN_TYPE_ARRAY;
	ret->v.o = range;

	return 0;
}

static int rtlb_isfloat(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	if (argc != 1) {
		spn_ctx_runtime_error(ctx, "exactly one argument is required", NULL);
		return -1;
	}

	*ret = makebool(isfloat(&argv[0]));

	return 0;
}

static int rtlb_isint(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	if (argc != 1) {
		spn_ctx_runtime_error(ctx, "exactly one argument is required", NULL);
		return -1;
	}

	*ret = makebool(isint(&argv[0]));

	return 0;
}

static int rtlb_fact(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	long i, n, r = 1;

	if (argc != 1) {
		spn_ctx_runtime_error(ctx, "exactly one argument is required", NULL);
		return -1;
	}

	if (!isint(&argv[0])) {
		spn_ctx_runtime_error(ctx, "argument must be an integer", NULL);
		return -2;
	}

	if (intvalue(&argv[0]) < 0) {
		spn_ctx_runtime_error(ctx, "argument must not be negative", NULL);
		return -3;
	}

	n = intvalue(&argv[0]);

	for (i = 2; i <= n; i++) {
		r *= i;
	}

	*ret = makeint(r);

	return 0;
}

static int rtlb_binom(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	long n, k, i, j, m, p;

	if (argc != 2) {
		spn_ctx_runtime_error(ctx, "exactly two arguments are required", NULL);
		return -1;
	}

	if (!isint(&argv[0]) || !isint(&argv[1])) {
		spn_ctx_runtime_error(ctx, "arguments must be integers", NULL);
		return -2;
	}

	n = intvalue(&argv[0]);
	k = intvalue(&argv[1]);

	if (n < 0 || k < 0 || n < k) {
		spn_ctx_runtime_error(ctx, "n >= k >= 0 is expected", NULL);
		return -3;
	}

	p = 1; /* accumulates the product */

	m = k < n - k ? k : n - k; /* min(k, n - k) */
	k = m; /* so that the multiplied numbers are large enough */
	i = n - k + 1;
	j = 1;
	while (m-- > 0) {
		/* not equivalent with p *= i++ / j++ due to precedence */
		p = p * i++ / j++;
	}

	*ret = makeint(p);

	return 0;
}

/*******************
 * Complex library *
 *******************/

/* helpers for getting and setting real and imaginary parts
 * the getter returns 0 if there are number (integer or floating-point)
 * values associated with the keys "re" and "im" or "r" and "theta",
 * and sets output arguments. If not (that's an error), returns nonzero
 */

static int rtlb_cplx_get(SpnValue *num, double *re_r, double *im_theta, int polar, SpnContext *ctx)
{
	SpnValue re_r_val, im_theta_val;
	SpnHashMap *hm = hashmapvalue(num);
	const char *re_r_key, *im_theta_key;

	re_r_key     = polar ? "r"     : "re";
	im_theta_key = polar ? "theta" : "im";

	spn_hashmap_get_strkey(hm, re_r_key, &re_r_val);
	spn_hashmap_get_strkey(hm, im_theta_key, &im_theta_val);

	if (!isnum(&re_r_val) || !isnum(&im_theta_val)) {
		spn_ctx_runtime_error(ctx, "keys 're' and 'im' or 'r' and 'theta' should correspond to numbers", NULL);
		return -1;
	}

	*re_r     = val2float(&re_r_val);
	*im_theta = val2float(&im_theta_val);

	return 0;
}

static void rtlb_cplx_set(SpnValue *num, double re_r, double im_theta, int polar)
{
	SpnValue re_r_val, im_theta_val;
	SpnHashMap *hm = hashmapvalue(num);
	const char *re_r_key, *im_theta_key;

	re_r_key     = polar ? "r"     : "re";
	im_theta_key = polar ? "theta" : "im";

	re_r_val     = makefloat(re_r);
	im_theta_val = makefloat(im_theta);

	spn_hashmap_set_strkey(hm, re_r_key, &re_r_val);
	spn_hashmap_set_strkey(hm, im_theta_key, &im_theta_val);
}


enum cplx_binop {
	CPLX_ADD,
	CPLX_SUB,
	CPLX_MUL,
	CPLX_DIV
};

static int rtlb_cplx_binop(SpnValue *ret, int argc, SpnValue *argv, enum cplx_binop op, SpnContext *ctx)
{
	double re1, im1, re2, im2, re, im;

	if (argc != 2) {
		spn_ctx_runtime_error(ctx, "exactly two arguments are required", NULL);
		return -1;
	}

	if (!ishashmap(&argv[0]) || !ishashmap(&argv[1])) {
		spn_ctx_runtime_error(ctx, "arguments must be hashmaps", NULL);
		return -2;
	}

	if (rtlb_cplx_get(&argv[0], &re1, &im1, 0, ctx) != 0) {
		return -3;
	}

	if (rtlb_cplx_get(&argv[1], &re2, &im2, 0, ctx) != 0) {
		return -3;
	}

	switch (op) {
	case CPLX_ADD:
		re = re1 + re2;
		im = im1 + im2;
		break;
	case CPLX_SUB:
		re = re1 - re2;
		im = im1 - im2;
		break;
	case CPLX_MUL:
		re = re1 * re2 - im1 * im2;
		im = re1 * im2 + re2 * im1;
		break;
	case CPLX_DIV: {
		double norm = re2 * re2 + im2 * im2;
		re = (re1 * re2 + im1 * im2) / norm;
		im = (re2 * im1 - re1 * im2) / norm;
		break;
	}
	default:
		SHANT_BE_REACHED();
		return -1;
	}

	*ret = makehashmap();
	rtlb_cplx_set(ret, re, im, 0);

	return 0;
}

static int rtlb_cplx_add(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	return rtlb_cplx_binop(ret, argc, argv, CPLX_ADD, ctx);
}

static int rtlb_cplx_sub(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	return rtlb_cplx_binop(ret, argc, argv, CPLX_SUB, ctx);
}

static int rtlb_cplx_mul(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	return rtlb_cplx_binop(ret, argc, argv, CPLX_MUL, ctx);
}

static int rtlb_cplx_div(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	return rtlb_cplx_binop(ret, argc, argv, CPLX_DIV, ctx);
}


enum cplx_trig_func {
	CPLX_SIN,
	CPLX_COS,
	CPLX_TAN
};

static int rtlb_aux_cplx_trig(SpnValue *ret, int argc, SpnValue *argv, enum cplx_trig_func fn, SpnContext *ctx)
{
	double re_in, im_in, re_out, im_out;

	if (argc != 1) {
		spn_ctx_runtime_error(ctx, "exactly one argument is required", NULL);
		return -1;
	}

	if (!ishashmap(&argv[0])) {
		spn_ctx_runtime_error(ctx, "argument must be a hashmap", NULL);
		return -2;
	}

	if (rtlb_cplx_get(&argv[0], &re_in, &im_in, 0, ctx) != 0) {
		return -3;
	}

	switch (fn) {
	case CPLX_SIN:
		re_out = sin(re_in) * cosh(im_in);
		im_out = cos(re_in) * sinh(im_in);
		break;
	case CPLX_COS:
		re_out = cos(re_in) * cosh(im_in);
		im_out = sin(re_in) * sinh(im_in) * -1;
		break;
	case CPLX_TAN: {
		double re_num = sin(re_in) * cosh(im_in);
		double im_num = cos(re_in) * sinh(im_in);
		double re_den = cos(re_in) * cosh(im_in);
		double im_den = sin(re_in) * sinh(im_in) * -1;
		double norm = re_den * re_den + im_den * im_den;
		re_out = (re_num * re_den + im_num * im_den) / norm;
		im_out = (re_den * im_num - re_num * im_den) / norm;
		break;
	}
	default:
		SHANT_BE_REACHED();
		return -1;
	}

	*ret = makehashmap();
	rtlb_cplx_set(ret, re_out, im_out, 0);

	return 0;
}

static int rtlb_cplx_sin(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	return rtlb_aux_cplx_trig(ret, argc, argv, CPLX_SIN, ctx);
}

static int rtlb_cplx_cos(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	return rtlb_aux_cplx_trig(ret, argc, argv, CPLX_COS, ctx);
}

static int rtlb_cplx_tan(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	return rtlb_aux_cplx_trig(ret, argc, argv, CPLX_TAN, ctx);
}

static int rtlb_cplx_conj(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	double re, im;

	if (argc != 1) {
		spn_ctx_runtime_error(ctx, "exactly one argument is required", NULL);
		return -1;
	}

	if (!ishashmap(&argv[0])) {
		spn_ctx_runtime_error(ctx, "argument must be a hashmap", NULL);
		return -2;
	}

	if (rtlb_cplx_get(&argv[0], &re, &im, 0, ctx) != 0) {
		return -3;
	}

	*ret = makehashmap();
	rtlb_cplx_set(ret, re, -im, 0);

	return 0;
}

static int rtlb_cplx_abs(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	double re, im;

	if (argc != 1) {
		spn_ctx_runtime_error(ctx, "exactly one argument is required", NULL);
		return -1;
	}

	if (!ishashmap(&argv[0])) {
		spn_ctx_runtime_error(ctx, "argument must be a hashmap", NULL);
		return -2;
	}

	if (rtlb_cplx_get(&argv[0], &re, &im, 0, ctx) != 0) {
		return -3;
	}

	*ret = makefloat(sqrt(re * re + im * im));

	return 0;
}

/* convert between the canonical and trigonometric (polar coordinate) forms */
static int rtlb_can2pol(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	double re, im, r, theta;

	if (argc != 1) {
		spn_ctx_runtime_error(ctx, "exactly one argument is required", NULL);
		return -1;
	}

	if (!ishashmap(&argv[0])) {
		spn_ctx_runtime_error(ctx, "argument must be a hashmap", NULL);
		return -2;
	}

	if (rtlb_cplx_get(&argv[0], &re, &im, 0, ctx) != 0) {
		return -3;
	}

	r = sqrt(re * re + im * im);
	theta = atan2(im, re);

	*ret = makehashmap();
	rtlb_cplx_set(ret, r, theta, 1);

	return 0;
}

static int rtlb_pol2can(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	double re, im, r, theta;

	if (argc != 1) {
		spn_ctx_runtime_error(ctx, "exactly one argument is required", NULL);
		return -1;
	}

	if (!ishashmap(&argv[0])) {
		spn_ctx_runtime_error(ctx, "argument must be a hashmap", NULL);
		return -2;
	}

	if (rtlb_cplx_get(&argv[0], &r, &theta, 1, ctx) != 0) {
		return -3;
	}

	re = r * cos(theta);
	im = r * sin(theta);

	*ret = makehashmap();
	rtlb_cplx_set(ret, re, im, 0);

	return 0;
}

static void loadlib_math(SpnVMachine *vm)
{
	/* Free functions */
	static const SpnExtFunc F[] = {
		{ "abs",       rtlb_abs         },
		{ "min",       rtlb_min         },
		{ "max",       rtlb_max         },
		{ "range",     rtlb_range       },
		{ "floor",     rtlb_floor       },
		{ "ceil",      rtlb_ceil        },
		{ "round",     rtlb_round       },
		{ "sgn",       rtlb_sgn         },
		{ "hypot",     rtlb_hypot       },
		{ "sqrt",      rtlb_sqrt        },
		{ "cbrt",      rtlb_cbrt        },
		{ "pow",       rtlb_pow         },
		{ "exp",       rtlb_exp         },
		{ "exp2",      rtlb_exp2        },
		{ "exp10",     rtlb_exp10       },
		{ "log",       rtlb_log         },
		{ "log2",      rtlb_log2        },
		{ "log10",     rtlb_log10       },
		{ "sin",       rtlb_sin         },
		{ "cos",       rtlb_cos         },
		{ "tan",       rtlb_tan         },
		{ "sinh",      rtlb_sinh        },
		{ "cosh",      rtlb_cosh        },
		{ "tanh",      rtlb_tanh        },
		{ "asin",      rtlb_asin        },
		{ "acos",      rtlb_acos        },
		{ "atan",      rtlb_atan        },
		{ "atan2",     rtlb_atan2       },
		{ "deg2rad",   rtlb_deg2rad     },
		{ "rad2deg",   rtlb_rad2deg     },
		{ "random",    rtlb_random      },
		{ "seed",      rtlb_seed        },
		{ "isfin",     rtlb_isfin       },
		{ "isinf",     rtlb_isinf       },
		{ "isnan",     rtlb_isnan       },
		{ "isfloat",   rtlb_isfloat     },
		{ "isint",     rtlb_isint       },
		{ "fact",      rtlb_fact        },
		{ "binom",     rtlb_binom       },
		{ "cplx_add",  rtlb_cplx_add    }, /* TODO: add square root, power and logarithm */
		{ "cplx_sub",  rtlb_cplx_sub    },
		{ "cplx_mul",  rtlb_cplx_mul    },
		{ "cplx_div",  rtlb_cplx_div    },
		{ "cplx_sin",  rtlb_cplx_sin    }, /* TODO: add inverse and hyperbolic functions */
		{ "cplx_cos",  rtlb_cplx_cos    },
		{ "cplx_tan",  rtlb_cplx_tan    },
		{ "cplx_conj", rtlb_cplx_conj   },
		{ "cplx_abs",  rtlb_cplx_abs    },
		{ "can2pol",   rtlb_can2pol     },
		{ "pol2can",   rtlb_pol2can     }
	};

	/* Constants */
	SpnExtValue C[6];

	C[0].name = "M_E";
	C[0].value = makefloat(M_E);

	C[1].name = "M_PI";
	C[1].value = makefloat(M_PI);

	C[2].name = "M_SQRT2";
	C[2].value = makefloat(M_SQRT2);

	C[3].name = "M_PHI";
	C[3].value = makefloat(M_PHI);

	C[4].name = "M_INF";
	C[4].value = makefloat(1.0 / 0.0);

	C[5].name = "M_NAN";
	C[5].value = makefloat(0.0 / 0.0);

	spn_vm_addlib_cfuncs(vm, NULL, F, COUNT(F));
	spn_vm_addlib_values(vm, NULL, C, COUNT(C));
}


/*************************************
 * OS/Shell access/Utilities library *
 *************************************/

static int rtlb_getenv(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	SpnString *name;
	const char *env;

	if (argc != 1) {
		spn_ctx_runtime_error(ctx, "exactly one argument is required", NULL);
		return -1;
	}

	if (!isstring(&argv[0])) {
		spn_ctx_runtime_error(ctx, "argument must be a string (name of an environment variable)", NULL);
		return -2;
	}

	name = stringvalue(&argv[0]);
	env = getenv(name->cstr);

	if (env != NULL) {
		*ret = makestring(env);
	}
	/* else implicitly return nil */

	return 0;
}

static int rtlb_system(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	SpnString *cmd;
	int code;

	if (argc != 1) {
		spn_ctx_runtime_error(ctx, "exactly one argument is required", NULL);
		return -1;
	}

	if (!isstring(&argv[0])) {
		spn_ctx_runtime_error(ctx, "argument must be a string (a command to execute)", NULL);
		return -2;
	}

	cmd = stringvalue(&argv[0]);
	code = system(cmd->cstr);

	*ret = makeint(code);

	return 0;
}

static int rtlb_assert(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	if (argc != 2) {
		spn_ctx_runtime_error(ctx, "exactly two arguments are required", NULL);
		return -1;
	}

	if (!isbool(&argv[0])) {
		spn_ctx_runtime_error(ctx, "assertion condition must be a boolean", NULL);
		return -2;
	}

	if (!isstring(&argv[1])) {
		spn_ctx_runtime_error(ctx, "error message must be a string", NULL);
		return -2;
	}

	/* actual assertion */
	if (boolvalue(&argv[0]) == 0) {
		SpnString *msg = stringvalue(&argv[1]);
		const void *args[1];
		args[0] = msg->cstr;
		spn_ctx_runtime_error(ctx, "assertion failed: %s", args);
		return -3;
	}

	return 0;
}

static int rtlb_time(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	/* XXX: is time_t guaranteed to be signed? Is it unsigned on any
	 * sensible implementation we should care about?
	 */
	*ret = makeint(time(NULL));
	return 0;
}

/* helper function that does the actual job filling the array from a struct tm.
 * `islocal` is a flag which is nonzero if localtime() is to be called, and
 * zero if gmtime() should be called. The other arguments and the return value
 * correspond exactly to that of the rtlb_gmtime() and rtlb_localtime().
 */
static int rtlb_aux_gettm(SpnValue *ret, int argc, SpnValue *argv, SpnContext *ctx, int islocal)
{
	time_t tmstp;
	struct tm *ts;

	SpnHashMap *hm;
	SpnValue val;

	if (argc != 1) {
		spn_ctx_runtime_error(ctx, "exactly one argument is required", NULL);
		return -1;
	}

	if (!isint(&argv[0])) {
		spn_ctx_runtime_error(ctx, "argument must be an integer", NULL);
		return -2;
	}

	/* the argument of this function is an integer returned by time() */
	tmstp = intvalue(&argv[0]);
	ts = islocal ? localtime(&tmstp) : gmtime(&tmstp);

	hm = spn_hashmap_new();

	/* make an SpnArray out of the returned struct tm */
	val = makeint(ts->tm_sec);
	spn_hashmap_set_strkey(hm, "sec", &val);

	val = makeint(ts->tm_min);
	spn_hashmap_set_strkey(hm, "min", &val);

	val = makeint(ts->tm_hour);
	spn_hashmap_set_strkey(hm, "hour", &val);

	val = makeint(ts->tm_mday);
	spn_hashmap_set_strkey(hm, "mday", &val);

	val = makeint(ts->tm_mon);
	spn_hashmap_set_strkey(hm, "month", &val);

	val = makeint(ts->tm_year);
	spn_hashmap_set_strkey(hm, "year", &val);

	val = makeint(ts->tm_wday);
	spn_hashmap_set_strkey(hm, "wday", &val);

	val = makeint(ts->tm_yday);
	spn_hashmap_set_strkey(hm, "yday", &val);

	val = makebool(ts->tm_isdst > 0);
	spn_hashmap_set_strkey(hm, "isdst", &val);

	/* return the array */
	ret->type = SPN_TYPE_HASHMAP;
	ret->v.o = hm;

	return 0;
}

static int rtlb_utctime(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	return rtlb_aux_gettm(ret, argc, argv, ctx, 0);
}

static int rtlb_localtime(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	return rtlb_aux_gettm(ret, argc, argv, ctx, 1);
}

#define RTLB_STRFTIME_BUFSIZE 0x100

/* helper function for converting an SpnArray representing the time
 * to a `struct tm'
 */
static int rtlb_aux_extract_time(SpnHashMap *hm, const char *str, int *outval, SpnContext *ctx)
{
	SpnValue val;

	spn_hashmap_get_strkey(hm, str, &val);

	if (!isint(&val)) {
		spn_ctx_runtime_error(ctx, "time components should be integers", NULL);
		return -1;
	}

	*outval = intvalue(&val);
	return 0;
}

static int rtlb_fmtdate(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	char *buf;
	struct tm ts;
	size_t len;

	SpnString *fmt;
	SpnHashMap *hm;
	SpnValue isdst;

	if (argc != 2) {
		spn_ctx_runtime_error(ctx, "exactly two arguments are required", NULL);
		return -1;
	}

	if (!isstring(&argv[0])) {
		spn_ctx_runtime_error(ctx, "first argument must be a format string", NULL);
		return -2;
	}

	if (!ishashmap(&argv[1])) {
		spn_ctx_runtime_error(ctx, "second argument must be a hashmap", NULL);
		return -2;
	}

	/* first argument is the format, second one is the hashmap
	 * that rtlb_aux_gettm() returned
	 */
	fmt = stringvalue(&argv[0]);
	hm = hashmapvalue(&argv[1]);

	/* convert array back to a struct tm */
	if (rtlb_aux_extract_time(hm, "sec",   &ts.tm_sec,   ctx) != 0
	 || rtlb_aux_extract_time(hm, "min",   &ts.tm_min,   ctx) != 0
	 || rtlb_aux_extract_time(hm, "hour",  &ts.tm_hour,  ctx) != 0
	 || rtlb_aux_extract_time(hm, "mday",  &ts.tm_mday,  ctx) != 0
	 || rtlb_aux_extract_time(hm, "month", &ts.tm_mon,   ctx) != 0
	 || rtlb_aux_extract_time(hm, "year",  &ts.tm_year,  ctx) != 0
	 || rtlb_aux_extract_time(hm, "wday",  &ts.tm_wday,  ctx) != 0
	 || rtlb_aux_extract_time(hm, "yday",  &ts.tm_yday,  ctx) != 0) {
		return -3;
	}

	/* treat isdst specially, since it's a boolean */
	spn_hashmap_get_strkey(hm, "isdst", &isdst);
	if (!isbool(&isdst)) {
		spn_ctx_runtime_error(ctx, "isdst must be a boolean", NULL);
		return -4;
	}

	ts.tm_isdst = boolvalue(&isdst);

	buf = spn_malloc(RTLB_STRFTIME_BUFSIZE);

	/* actually do the formatting */
	len = strftime(buf, RTLB_STRFTIME_BUFSIZE, fmt->cstr, &ts);

	/* set return value */
	*ret = makestring_nocopy_len(buf, len, 1);

	return 0;
}

static int rtlb_difftime(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	if (argc != 2) {
		spn_ctx_runtime_error(ctx, "exactly two arguments are required", NULL);
		return -1;
	}

	if (!isint(&argv[0]) || !isint(&argv[1])) {
		spn_ctx_runtime_error(ctx, "arguments must be integers", NULL);
		return -2;
	}

	*ret = makefloat(difftime(intvalue(&argv[0]), intvalue(&argv[1])));

	return 0;
}

static int rtlb_compile(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	SpnFunction *fn;
	const char *src;

	if (argc != 1) {
		spn_ctx_runtime_error(ctx, "exactly one argument is required", NULL);
		return -1;
	}

	if (!isstring(&argv[0])) {
		spn_ctx_runtime_error(ctx, "argument must be a string", NULL);
		return -2;
	}

	src = stringvalue(&argv[0])->cstr;
	fn = spn_ctx_loadstring(ctx, src);

	if (fn == NULL) {    /* return parser/compiler error message */
		const char *errmsg = spn_ctx_geterrmsg(ctx);
		*ret = makestring(errmsg);
		spn_ctx_clearerror(ctx);
	} else {             /* return function, make it owning */
		ret->type = SPN_TYPE_FUNC;
		ret->v.o = fn;
		spn_value_retain(ret);
	}

	return 0;
}

static int rtlb_require(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	const char *fname;

	if (argc != 1) {
		spn_ctx_runtime_error(ctx, "exactly one argument is required", NULL);
		return -1;
	}

	if (!isstring(&argv[0])) {
		spn_ctx_runtime_error(ctx, "argument must be a string (a filename)", NULL);
		return -2;
	}

	fname = stringvalue(&argv[0])->cstr;

	return spn_ctx_execsrcfile(ctx, fname, ret);
}

static int rtlb_exprtofn(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	SpnFunction *fn;

	if (argc != 1) {
		spn_ctx_runtime_error(ctx, "requiring exactly one argument", NULL);
		return -1;
	}

	if (!isstring(&argv[0])) {
		spn_ctx_runtime_error(ctx, "argument must be a string", NULL);
		return -2;
	}

	fn = spn_ctx_compile_expr(ctx, stringvalue(&argv[0])->cstr);
	if (fn == NULL) {
		const char *errmsg = spn_ctx_geterrmsg(ctx);
		*ret = makestring(errmsg);
		spn_ctx_clearerror(ctx);
	} else {
		ret->type = SPN_TYPE_FUNC;
		ret->v.o = fn;
		spn_value_retain(ret);
	}

	return 0;
}

static int rtlb_toint(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	SpnString *str;
	long base;

	if (argc < 1 || argc > 2) {
		spn_ctx_runtime_error(ctx, "one or two arguments are required", NULL);
		return -1;
	}

	if (!isstring(&argv[0])) {
		spn_ctx_runtime_error(ctx, "first argument must be a string", NULL);
		return -2;
	}

	if (argc == 2 && !isint(&argv[1])) {
		spn_ctx_runtime_error(ctx, "second argument must be an integer", NULL);
		return -3;
	}

	str = stringvalue(&argv[0]);
	base = argc == 2 ? intvalue(&argv[1]) : 0;

	if (base == 1 || base < 0 || base > 36) {
		spn_ctx_runtime_error(ctx, "second argument must be zero or between [2...36]", NULL);
		return -4;
	}

	*ret = makeint(strtol(str->cstr, NULL, base));

	return 0;
}

static int rtlb_tofloat(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	SpnString *str;

	if (argc != 1) {
		spn_ctx_runtime_error(ctx, "exactly one argument is required", NULL);
		return -1;
	}

	if (!isstring(&argv[0])) {
		spn_ctx_runtime_error(ctx, "argument must be a string", NULL);
		return -2;
	}

	str = stringvalue(&argv[0]);

	*ret = makefloat(strtod(str->cstr, NULL));

	return 0;
}

static int rtlb_tonumber(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	SpnString *str;

	if (argc != 1) {
		spn_ctx_runtime_error(ctx, "exactly one argument is required", NULL);
		return -1;
	}

	if (!isstring(&argv[0])) {
		spn_ctx_runtime_error(ctx, "argument must be a string", NULL);
		return -2;
	}

	str = stringvalue(&argv[0]);

	if (strpbrk(str->cstr, ".eE") != NULL) {
		return rtlb_tofloat(ret, argc, argv, ctx);
	} else {
		return rtlb_toint(ret, argc, argv, ctx);
	}
}

static int rtlb_call(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
#define MAX_AUTO_ARGC 16

	int callee_argc, status, i;
	SpnValue callee_argv_auto[MAX_AUTO_ARGC];
	SpnValue *callee_argv;
	SpnFunction *callee;
	SpnArray *arguments;

	if (argc != 2) {
		spn_ctx_runtime_error(ctx, "expecting 2 arguments", NULL);
		return -1;
	}

	if (!isfunc(&argv[0])) {
		spn_ctx_runtime_error(ctx, "first argument must be a function", NULL);
		return -2;
	}

	if (!isarray(&argv[1])) {
		spn_ctx_runtime_error(ctx, "second argument must be an array", NULL);
		return -3;
	}

	callee = funcvalue(&argv[0]);
	arguments = arrayvalue(&argv[1]);
	callee_argc = spn_array_count(arguments);

	/* if there aren't too many arguments, store them in
	 * an auto array. Only allocate dynamically when it's
	 * necessary. (I have already benchmarked this in the
	 * virtual machine (SPN_INS_CALL), and I found that
	 * when one is calling a function frequently, then
	 * the use of malloc slows down the mechanism a lot.)
	 */
	if (callee_argc > MAX_AUTO_ARGC) {
		callee_argv = spn_malloc(callee_argc * sizeof callee_argv[0]);
	} else {
		callee_argv = callee_argv_auto;
	}

	/* copy arguments */
	for (i = 0; i < callee_argc; i++) {
		spn_array_get(arguments, i, &callee_argv[i]);
	}

	/* perform actual call */
	status = spn_ctx_callfunc(ctx, callee, ret, callee_argc, callee_argv);

	if (callee_argc > MAX_AUTO_ARGC) {
		free(callee_argv);
	}

#undef MAX_AUTO_ARGC

	return status;
}

static int rtlb_backtrace(SpnValue *ret, int argc, SpnValue *argv, void *ctx)
{
	size_t n, i;
	const char **cnames = spn_ctx_stacktrace(ctx, &n);
	SpnArray *fnames = spn_array_new();

	/* 'i' starts at 1: exclude own stack frame */
	for (i = 1; i < n; i++) {
		SpnValue name = makestring(cnames[i]);
		spn_array_push(fnames, &name);
		spn_value_release(&name);
	}

	free(cnames);

	ret->type = SPN_TYPE_ARRAY;
	ret->v.o = fnames;
	return 0;
}

static void loadlib_sysutil(SpnVMachine *vm)
{
	/* Free functions */
	static const SpnExtFunc F[] = {
		{ "getenv",     rtlb_getenv     },
		{ "system",     rtlb_system     },
		{ "assert",     rtlb_assert     },
		{ "time",       rtlb_time       },
		{ "utctime",    rtlb_utctime    },
		{ "localtime",  rtlb_localtime  },
		{ "fmtdate",    rtlb_fmtdate    },
		{ "difftime",   rtlb_difftime   },
		{ "compile",    rtlb_compile    },
		{ "exprtofn",   rtlb_exprtofn   },
		{ "toint",      rtlb_toint      },
		{ "tofloat",    rtlb_tofloat    },
		{ "tonumber",   rtlb_tonumber   },
		{ "require",    rtlb_require    },
		{ "backtrace",  rtlb_backtrace  }
	};

	/* Methods */
	static const SpnExtFunc M[] = {
		{ "call", rtlb_call }
	};

	/* Constants */
	SpnExtValue C[4];
	SpnHashMap *classes = spn_vm_getclasses(vm);

	SpnValue stringindex  = makeint(SPN_TTAG_STRING),
	         arrayindex   = makeint(SPN_TTAG_ARRAY),
	         hashmapindex = makeint(SPN_TTAG_HASHMAP),
	         funcindex    = makeint(SPN_TTAG_FUNC);

	C[0].name = "String";
	spn_hashmap_get(classes, &stringindex, &C[0].value);

	C[1].name = "Array";
	spn_hashmap_get(classes, &arrayindex, &C[1].value);

	C[2].name = "HashMap";
	spn_hashmap_get(classes, &hashmapindex, &C[2].value);

	C[3].name = "Function";
	spn_hashmap_get(classes, &funcindex, &C[3].value);

	spn_vm_addlib_cfuncs(vm, NULL, F, COUNT(F));
	spn_vm_addlib_values(vm, NULL, C, COUNT(C));
	load_methods(vm, SPN_TTAG_FUNC, M, COUNT(M));
}


/* By default, only strings, arrays hashmaps and functions are considered
 * "object-like", while nil, booleans and numbers are not.
 * (Frankly, why would you ever call a method on a boolean?)
 * User info values can only have their methods and properties defined instance-wise.
 */
static void init_stdlib_classes(SpnVMachine *vm)
{
	SpnHashMap *classes = spn_vm_getclasses(vm);

	SpnValue stringclass  = makehashmap(),
	         arrayclass   = makehashmap(),
	         hashmapclass = makehashmap(),
	         funcclass    = makehashmap();

	SpnValue stringindex  = makeint(SPN_TTAG_STRING),
	         arrayindex   = makeint(SPN_TTAG_ARRAY),
	         hashmapindex = makeint(SPN_TTAG_HASHMAP),
	         funcindex    = makeint(SPN_TTAG_FUNC);

	spn_hashmap_set(classes, &stringindex,  &stringclass);
	spn_hashmap_set(classes, &arrayindex,   &arrayclass);
	spn_hashmap_set(classes, &hashmapindex, &hashmapclass);
	spn_hashmap_set(classes, &funcindex,    &funcclass);

	spn_value_release(&stringclass);
	spn_value_release(&arrayclass);
	spn_value_release(&hashmapclass);
	spn_value_release(&funcclass);
}

void spn_load_stdlib(SpnVMachine *vm)
{
	init_stdlib_classes(vm);

	loadlib_io(vm);
	loadlib_string(vm);
	loadlib_array(vm);
	loadlib_hashmap(vm);
	loadlib_math(vm);
	loadlib_sysutil(vm);
}
