/*
 * ctx.c
 * Sparkling, a lightweight C-style scripting language
 *
 * Created by Árpád Goretity on 12/10/2013
 * Licensed under the 2-clause BSD License
 *
 * A convenience context API
 */

#include "ctx.h"
#include "private.h"


struct SpnContext {
	SpnParser *parser;
	SpnCompiler *cmp;
	SpnVMachine *vm;
	struct spn_bc_list *bclist; /* holds all bytecodes ever compiled */

	enum spn_error_type errtype; /* type of the last error */
	const char *errmsg; /* last error message */

	void *info; /* context info initialized to NULL, use freely */
};

static void prepend_bytecode_list(SpnContext *ctx, spn_uword *bc, size_t len);
static void free_bytecode_list(struct spn_bc_list *head);

SpnContext *spn_ctx_new()
{
	SpnContext *ctx = spn_malloc(sizeof(*ctx));

	ctx->parser  = spn_parser_new();
	ctx->cmp     = spn_compiler_new();
	ctx->vm      = spn_vm_new();
	ctx->bclist  = NULL;
	ctx->errtype = SPN_ERROR_OK;
	ctx->errmsg  = NULL;
	ctx->info    = NULL;

	spn_vm_setcontext(ctx->vm, ctx);
	spn_load_stdlib(ctx->vm);

	return ctx;
}

void spn_ctx_free(SpnContext *ctx)
{
	spn_parser_free(ctx->parser);
	spn_compiler_free(ctx->cmp);
	spn_vm_free(ctx->vm);

	free_bytecode_list(ctx->bclist);
	free(ctx);
}

enum spn_error_type spn_ctx_geterrtype(SpnContext *ctx)
{
	return ctx->errtype;
}

const char *spn_ctx_geterrmsg(SpnContext *ctx)
{
	switch (ctx->errtype) {
	case SPN_ERROR_OK:		return NULL;
	case SPN_ERROR_SYNTAX:		return ctx->parser->errmsg;
	case SPN_ERROR_SEMANTIC:	return spn_compiler_errmsg(ctx->cmp);
	case SPN_ERROR_RUNTIME:		return spn_vm_geterrmsg(ctx->vm);
	case SPN_ERROR_GENERIC:		return ctx->errmsg;
	default:			return NULL;
	}
}

const struct spn_bc_list *spn_ctx_getbclist(SpnContext *ctx)
{
	return ctx->bclist;
}

void *spn_ctx_getuserinfo(SpnContext *ctx)
{
	return ctx->info;
}

void spn_ctx_setuserinfo(SpnContext *ctx, void *info)
{
	ctx->info = info;
}


/* the essence */

spn_uword *spn_ctx_loadstring(SpnContext *ctx, const char *str)
{
	SpnAST *ast;
	spn_uword *bc;
	size_t len;

	ctx->errtype = SPN_ERROR_OK;

	/* attempt parsing, handle error */
	ast = spn_parser_parse(ctx->parser, str);
	if (ast == NULL) {
		ctx->errtype = SPN_ERROR_SYNTAX;
		return NULL;
	}

	/* attempt compilation, handle error */
	bc = spn_compiler_compile(ctx->cmp, ast, &len);
	spn_ast_free(ast);

	if (bc == NULL) {
		ctx->errtype = SPN_ERROR_SEMANTIC;
		return NULL;
	}

	/* prepend bytecode to the link list */
	prepend_bytecode_list(ctx, bc, len);
	return bc;
}

spn_uword *spn_ctx_loadsrcfile(SpnContext *ctx, const char *fname)
{
	char *src;
	spn_uword *bc;

	ctx->errtype = SPN_ERROR_OK;

	src = spn_read_text_file(fname);
	if (src == NULL) {
		ctx->errtype = SPN_ERROR_GENERIC;
		ctx->errmsg = "Sparkling: I/O error: could not read source file";
		return NULL;
	}

	bc = spn_ctx_loadstring(ctx, src);
	free(src);

	return bc;
}

spn_uword *spn_ctx_loadobjfile(SpnContext *ctx, const char *fname)
{
	spn_uword *bc;
	size_t filesize, nwords;

	ctx->errtype = SPN_ERROR_OK;

	bc = spn_read_binary_file(fname, &filesize);
	if (bc == NULL) {
		ctx->errtype = SPN_ERROR_GENERIC;
		ctx->errmsg = "Sparkling: I/O error: could not read object file";
		return NULL;
	}

	/* the size of the object file is not the same
	 * as the number of machine words in the bytecode
	 */
	nwords = filesize / sizeof(*bc);
	prepend_bytecode_list(ctx, bc, nwords);

	return bc;
}

int spn_ctx_execstring(SpnContext *ctx, const char *str, SpnValue *ret)
{
	spn_uword *bc = spn_ctx_loadstring(ctx, str);
	if (bc == NULL) {
		return -1;
	}

	return spn_ctx_execbytecode(ctx, bc, ret);
}

int spn_ctx_execsrcfile(SpnContext *ctx, const char *fname, SpnValue *ret)
{
	spn_uword *bc = spn_ctx_loadsrcfile(ctx, fname);
	if (bc == NULL) {
		return -1;
	}

	return spn_ctx_execbytecode(ctx, bc, ret);
}

int spn_ctx_execobjfile(SpnContext *ctx, const char *fname, SpnValue *ret)
{
	spn_uword *bc = spn_ctx_loadobjfile(ctx, fname);
	if (bc == NULL) {
		return -1;
	}

	return spn_ctx_execbytecode(ctx, bc, ret);
}

/* NB: this does **not** add the bytecode to the linked list */
int spn_ctx_execbytecode(SpnContext *ctx, spn_uword *bc, SpnValue *ret)
{
	int status;

	ctx->errtype = SPN_ERROR_OK;

	status = spn_vm_exec(ctx->vm, bc, ret);
	if (status != 0) {
		ctx->errtype = SPN_ERROR_RUNTIME;
	}

	return status;
}


/* abstraction (well, sort of) of the virtual machine API */

int spn_ctx_callfunc(SpnContext *ctx, SpnValue *func, SpnValue *ret, int argc, SpnValue argv[])
{
	int status;

	ctx->errtype = SPN_ERROR_OK;

	status = spn_vm_callfunc(ctx->vm, func, ret, argc, argv);
	if (status != 0) {
		ctx->errtype = SPN_ERROR_RUNTIME;
	}

	return status;
}

void spn_ctx_runtime_error(SpnContext *ctx, const char *fmt, const void *args[])
{
	spn_vm_seterrmsg(ctx->vm, fmt, args);
}

const char **spn_ctx_stacktrace(SpnContext *ctx, size_t *size)
{
	return spn_vm_stacktrace(ctx->vm, size);
}

void spn_ctx_addlib_cfuncs(SpnContext *ctx, const char *libname, const SpnExtFunc fns[], size_t n)
{
	spn_vm_addlib_cfuncs(ctx->vm, libname, fns, n);
}

void spn_ctx_addlib_values(SpnContext *ctx, const char *libname, SpnExtValue vals[], size_t n)
{
	spn_vm_addlib_values(ctx->vm, libname, vals, n);
}

SpnArray *spn_ctx_getglobals(SpnContext *ctx)
{
	return spn_vm_getglobals(ctx->vm);
}


/* private bytecode link list functions */

static void prepend_bytecode_list(SpnContext *ctx, spn_uword *bc, size_t len)
{
	struct spn_bc_list *node = spn_malloc(sizeof(*node));

	node->bc = bc;
	node->len = len;
	node->next = ctx->bclist;
	ctx->bclist = node;
}

static void free_bytecode_list(struct spn_bc_list *head)
{
	while (head != NULL) {
		struct spn_bc_list *tmp = head->next;
		free(head->bc);
		free(head);
		head = tmp;
	}
}

