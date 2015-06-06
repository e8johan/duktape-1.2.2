/*
 *  Plugin based execution tool.  Useful for building generic, extendable run-times.
 *
 *  Based on the command line tool. Grep for DUK_CMDLINE_ for configuration options.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef DUK_CMDLINE_ALLOC_LOGGING
#include "duk_alloc_logging.h"
#endif
#ifdef DUK_CMDLINE_ALLOC_TORTURE
#include "duk_alloc_torture.h"
#endif
#ifdef DUK_CMDLINE_ALLOC_HYBRID
#include "duk_alloc_hybrid.h"
#endif
#include "duktape.h"

#ifdef DUK_CMDLINE_AJSHEAP
/* Defined in duk_cmdline_ajduk.c or alljoyn.js headers. */
void ajsheap_init(void);
void ajsheap_dump(void);
void ajsheap_register(duk_context *ctx);
void ajsheap_start_exec_timeout(void);
void ajsheap_clear_exec_timeout(void);
void *AJS_Alloc(void *udata, duk_size_t size);
void *AJS_Realloc(void *udata, void *ptr, duk_size_t size);
void AJS_Free(void *udata, void *ptr);
#endif

#ifdef DUK_CMDLINE_DEBUGGER_SUPPORT
#include "duk_debug_trans_socket.h"
#endif

#define  MEM_LIMIT_NORMAL   (128*1024*1024)   /* 128 MB */
#define  MEM_LIMIT_HIGH     (2047*1024*1024)  /* ~2 GB */
#define  LINEBUF_SIZE       65536

static int interactive_mode = 0;

static int get_stack_raw(duk_context *ctx) {
	if (!duk_is_object(ctx, -1)) {
		return 1;
	}
	if (!duk_has_prop_string(ctx, -1, "stack")) {
		return 1;
	}
	if (!duk_is_error(ctx, -1)) {
		/* Not an Error instance, don't read "stack". */
		return 1;
	}

	duk_get_prop_string(ctx, -1, "stack");  /* caller coerces */
	duk_remove(ctx, -2);
	return 1;
}

/* Print error to stderr and pop error. */
static void print_pop_error(duk_context *ctx, FILE *f) {
	/* Print error objects with a stack trace specially.
	 * Note that getting the stack trace may throw an error
	 * so this also needs to be safe call wrapped.
	 */
	(void) duk_safe_call(ctx, get_stack_raw, 1 /*nargs*/, 1 /*nrets*/);
	fprintf(f, "%s\n", duk_safe_to_string(ctx, -1));
	fflush(f);
	duk_pop(ctx);
}

static int wrapped_compile_execute(duk_context *ctx) {
	const char *src_data;
	duk_size_t src_len;
	int comp_flags;

	/* XXX: Here it'd be nice to get some stats for the compilation result
	 * when a suitable command line is given (e.g. code size, constant
	 * count, function count.  These are available internally but not through
	 * the public API.
	 */

	/* Use duk_compile_lstring_filename() variant which avoids interning
	 * the source code.  This only really matters for low memory environments.
	 */

	/* [ ... src_data src_len filename ] */

	comp_flags = 0;
	src_data = (const char *) duk_require_pointer(ctx, -3);
	src_len = (duk_size_t) duk_require_uint(ctx, -2);
	duk_compile_lstring_filename(ctx, comp_flags, src_data, src_len);

	/* [ ... src_data src_len function ] */

#if defined(DUK_CMDLINE_AJSHEAP)
	ajsheap_start_exec_timeout();
#endif

	duk_push_global_object(ctx);  /* 'this' binding */
	duk_call_method(ctx, 0);

#if defined(DUK_CMDLINE_AJSHEAP)
	ajsheap_clear_exec_timeout();
#endif

	if (interactive_mode) {
		/*
		 *  In interactive mode, write to stdout so output won't
		 *  interleave as easily.
		 *
		 *  NOTE: the ToString() coercion may fail in some cases;
		 *  for instance, if you evaluate:
		 *
		 *    ( {valueOf: function() {return {}},
		 *       toString: function() {return {}}});
		 *
		 *  The error is:
		 *
		 *    TypeError: failed to coerce with [[DefaultValue]]
		 *            duk_api.c:1420
		 *
		 *  These are handled now by the caller which also has stack
		 *  trace printing support.  User code can print out errors
		 *  safely using duk_safe_to_string().
		 */

		fprintf(stdout, "= %s\n", duk_to_string(ctx, -1));
		fflush(stdout);
	} else {
		/* In non-interactive mode, success results are not written at all.
		 * It is important that the result value is not string coerced,
		 * as the string coercion may cause an error in some cases.
		 */
	}

	duk_pop(ctx);
	return 0;
}

static int handle_fh(duk_context *ctx, FILE *f, const char *filename) {
	char *buf = NULL;
	int len;
	int got;
	int rc;
	int retval = -1;

	if (fseek(f, 0, SEEK_END) < 0) {
		goto error;
	}
	len = (int) ftell(f);
	if (fseek(f, 0, SEEK_SET) < 0) {
		goto error;
	}
	buf = (char *) malloc(len);
	if (!buf) {
		goto error;
	}

	got = fread((void *) buf, (size_t) 1, (size_t) len, f);

	duk_push_pointer(ctx, (void *) buf);
	duk_push_uint(ctx, (duk_uint_t) got);
	duk_push_string(ctx, filename);

	interactive_mode = 0;  /* global */

	rc = duk_safe_call(ctx, wrapped_compile_execute, 3 /*nargs*/, 1 /*nret*/);

#if defined(DUK_CMDLINE_AJSHEAP)
	ajsheap_clear_exec_timeout();
#endif

	free(buf);
	buf = NULL;

	if (rc != DUK_EXEC_SUCCESS) {
		print_pop_error(ctx, stderr);
		goto error;
	} else {
		duk_pop(ctx);
		retval = 0;
	}
	/* fall thru */

 cleanup:
	if (buf) {
		free(buf);
	}
	return retval;

 error:
	fprintf(stderr, "error in executing file %s\n", filename);
	fflush(stderr);
	goto cleanup;
}

static int handle_file(duk_context *ctx, const char *filename) {
	FILE *f = NULL;
	int retval;

	f = fopen(filename, "rb");
	if (!f) {
		fprintf(stderr, "failed to open source file: %s\n", filename);
		fflush(stderr);
		goto error;
	}

	retval = handle_fh(ctx, f, filename);

	fclose(f);
	return retval;

 error:
	return -1;
}

#ifdef DUK_CMDLINE_DEBUGGER_SUPPORT
static void debugger_detached(void *udata) {
	fprintf(stderr, "Debugger detached, udata: %p\n", (void *) udata);
	fflush(stderr);
}
#endif

#define  ALLOC_DEFAULT  0
#define  ALLOC_LOGGING  1
#define  ALLOC_TORTURE  2
#define  ALLOC_HYBRID   3
#define  ALLOC_AJSHEAP  4

int main(int argc, char *argv[]) {
	duk_context *ctx = NULL;
	int retval = 0;
	int alloc_provider = ALLOC_DEFAULT;
	int debugger = 0;
	int i;

#ifdef DUK_CMDLINE_AJSHEAP
	alloc_provider = ALLOC_AJSHEAP;
#endif

	/*
	 *  Parse options
	 */

	for (i = 1; i < argc; i++) {
		char *arg = argv[i];
		if (!arg) {
			goto usage;
		}
                if (strcmp(arg, "--alloc-default") == 0) {
			alloc_provider = ALLOC_DEFAULT;
		} else if (strcmp(arg, "--alloc-logging") == 0) {
			alloc_provider = ALLOC_LOGGING;
		} else if (strcmp(arg, "--alloc-torture") == 0) {
			alloc_provider = ALLOC_TORTURE;
		} else if (strcmp(arg, "--alloc-hybrid") == 0) {
			alloc_provider = ALLOC_HYBRID;
		} else if (strcmp(arg, "--alloc-ajsheap") == 0) {
			alloc_provider = ALLOC_AJSHEAP;
		} else if (strcmp(arg, "--debugger") == 0) {
			debugger = 1;
		} else if (strlen(arg) >= 1 && arg[0] == '-') {
			goto usage;
		} else {
			/* This is probably a file name */
		}
	}
	
	/*
	 *  Create context
	 */

	ctx = NULL;
	if (!ctx && alloc_provider == ALLOC_LOGGING) {
#ifdef DUK_CMDLINE_ALLOC_LOGGING
		ctx = duk_create_heap(duk_alloc_logging,
		                      duk_realloc_logging,
		                      duk_free_logging,
		                      (void *) 0xdeadbeef,
		                      NULL);
#else
		fprintf(stderr, "Warning: option --alloc-logging ignored, no logging allocator support\n");
		fflush(stderr);
#endif
	}
	if (!ctx && alloc_provider == ALLOC_TORTURE) {
#ifdef DUK_CMDLINE_ALLOC_TORTURE
		ctx = duk_create_heap(duk_alloc_torture,
		                      duk_realloc_torture,
		                      duk_free_torture,
		                      (void *) 0xdeadbeef,
		                      NULL);
#else
		fprintf(stderr, "Warning: option --alloc-torture ignored, no torture allocator support\n");
		fflush(stderr);
#endif
	}
	if (!ctx && alloc_provider == ALLOC_HYBRID) {
#ifdef DUK_CMDLINE_ALLOC_HYBRID
		void *udata = duk_alloc_hybrid_init();
		if (!udata) {
			fprintf(stderr, "Failed to init hybrid allocator\n");
			fflush(stderr);
		} else {
			ctx = duk_create_heap(duk_alloc_hybrid,
			                      duk_realloc_hybrid,
			                      duk_free_hybrid,
			                      udata,
			                      NULL);
		}
#else
		fprintf(stderr, "Warning: option --alloc-hybrid ignored, no hybrid allocator support\n");
		fflush(stderr);
#endif
	}
	if (!ctx && alloc_provider == ALLOC_AJSHEAP) {
#ifdef DUK_CMDLINE_AJSHEAP
		ajsheap_init();

		ctx = duk_create_heap(AJS_Alloc,
		                      AJS_Realloc,
		                      AJS_Free,
		                      (void *) 0xdeadbeef,  /* heap_udata: ignored by AjsHeap, use as marker */
		                      NULL);                /* fatal_handler */
#else
		fprintf(stderr, "Warning: option --alloc-ajsheap ignored, no ajsheap allocator support\n");
		fflush(stderr);
#endif
	}
	if (!ctx && alloc_provider == ALLOC_DEFAULT) {
		ctx = duk_create_heap_default();
	}

	if (!ctx) {
		fprintf(stderr, "Failed to create Duktape heap\n");
		fflush(stderr);
		exit(-1);
	}

#ifdef DUK_CMDLINE_AJSHEAP
	if (alloc_provider == ALLOC_AJSHEAP) {
		fprintf(stdout, "Pool dump after heap creation\n");
		ajsheap_dump();
	}
#endif

#ifdef DUK_CMDLINE_AJSHEAP
	if (alloc_provider == ALLOC_AJSHEAP) {
		ajsheap_register(ctx);
	}
#endif

	if (debugger) {
#ifdef DUK_CMDLINE_DEBUGGER_SUPPORT
		fprintf(stderr, "Debugger enabled, create socket and wait for connection\n");
		fflush(stderr);
		duk_debug_trans_socket_init();
		duk_debug_trans_socket_waitconn();
		fprintf(stderr, "Debugger connected, call duk_debugger_attach() and then execute requested file(s)/eval\n");
		fflush(stderr);
		duk_debugger_attach(ctx,
		                    duk_debug_trans_socket_read,
		                    duk_debug_trans_socket_write,
		                    duk_debug_trans_socket_peek,
		                    duk_debug_trans_socket_read_flush,
		                    duk_debug_trans_socket_write_flush,
		                    debugger_detached,
		                    (void *) 0xbeef1234);
#else
		fprintf(stderr, "Warning: option --debugger ignored, no debugger support\n");
		fflush(stderr);
#endif
	}

#if 0
	/* Manual test for duk_debugger_cooperate() */
	{
		for (i = 0; i < 60; i++) {
			printf("cooperate: %d\n", i);
			usleep(1000000);
			duk_debugger_cooperate(ctx);
		}
	}
#endif

	/*
	 *  Execute any argument file(s)
	 */

	for (i = 1; i < argc; i++) {
		char *arg = argv[i];
		if (!arg) {
			continue;
		} else if (strlen(arg) >= 1 && arg[0] == '-') {
			continue;
		}

		if (handle_file(ctx, arg) != 0) {
			retval = 1;
			goto cleanup;
		}
	}

	/*
	 *  Cleanup and exit
	 */

 cleanup:

#ifdef DUK_CMDLINE_AJSHEAP
	if (alloc_provider == ALLOC_AJSHEAP) {
		fprintf(stdout, "Pool dump before duk_destroy_heap(), before forced gc\n");
		ajsheap_dump();

		duk_gc(ctx, 0);

		fprintf(stdout, "Pool dump before duk_destroy_heap(), after forced gc\n");
		ajsheap_dump();
	}
#endif

	if (ctx) {
		duk_destroy_heap(ctx);
	}

#ifdef DUK_CMDLINE_AJSHEAP
	if (alloc_provider == ALLOC_AJSHEAP) {
		fprintf(stdout, "Pool dump after duk_destroy_heap() (should have zero allocs)\n");
		ajsheap_dump();
	}
#endif

	return retval;

	/*
	 *  Usage
	 */

 usage:
	fprintf(stderr, "Usage: duk [options] [<filenames>]\n"
	                "\n"
	                "   --alloc-default    use Duktape default allocator\n"
#ifdef DUK_CMDLINE_ALLOC_LOGGING
	                "   --alloc-logging    use logging allocator (writes to /tmp)\n"
#endif
#ifdef DUK_CMDLINE_ALLOC_TORTURE
	                "   --alloc-torture    use torture allocator\n"
#endif
#ifdef DUK_CMDLINE_ALLOC_HYBRID
	                "   --alloc-hybrid     use hybrid allocator\n"
#endif
#ifdef DUK_CMDLINE_AJSHEAP
	                "   --alloc-ajsheap    use ajsheap allocator (enabled by default with 'ajduk')\n"
#endif
#ifdef DUK_CMDLINE_DEBUGGER_SUPPORT
			"   --debugger         start example debugger\n"
#endif
	                "\n"
	                "If <filename> is omitted, interactive mode is started automatically.\n");
	fflush(stderr);
	exit(1);
}
