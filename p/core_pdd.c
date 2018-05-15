/* radare - LGPL - Copyright 2018 - pancake, deroad */
#if 0
gcc -o core_test.so -fPIC `pkg-config --cflags --libs r_core` core_test.c -shared
mkdir -p ~/.config/radare2/plugins
mv core_test.so ~/.config/radare2/plugins
#endif

#include <r_types.h>
#include <r_lib.h>
#include <r_cmd.h>
#include <r_core.h>
#include <r_cons.h>
#include <string.h>
#include <r_anal.h>
#include <duktape.h>
#include <duk_console.h>

#undef R_API
#define R_API static
#undef R_IPI
#define R_IPI static

/* for compatibility. */
#ifndef R2_HOME_DATADIR
#define R2_HOME_DATADIR R2_HOMEDIR
#endif
#include "long_js.c"

#define REQUIRE_JS "var require = function(x) {try {if (arguments.callee.loaded[x]) {return arguments.callee.loaded[x];}var module = {exports: null};eval(___internal_require(x));arguments.callee.loaded[x] = module.exports;return module.exports;} catch (ee) {console.log('Exception from ' + x);console.log(ee.stack);}}; require.loaded = {};"

typedef struct {
	bool hidecasts;
	bool assembly;
} e_r2dec_t;

static RCore *core_link = 0;
static e_r2dec_t config = {0};

static char* r2dec_read_file(const char* file) {
	if (!file) {
		return 0;
	}
	char *r2dec_home = r_str_home (R2_HOME_DATADIR R_SYS_DIR "r2pm" R_SYS_DIR "git" R_SYS_DIR "r2dec-js" R_SYS_DIR);
	int len = 0;
	char filepath[1024];
	if (!r2dec_home) {
		return 0;
	}
	snprintf (filepath, sizeof(filepath), "%s%s", r2dec_home, file);
	free (r2dec_home);
	char* text = r_file_slurp (filepath, &len);
	if (text && len > 0) {
		return text;
	}
	return 0;
}

static duk_ret_t duk_r2cmd(duk_context *ctx) {
	if (duk_is_string (ctx, 0)) {
		char* output = r_core_cmd_str (core_link, duk_safe_to_string (ctx, 0));
		duk_push_string (ctx, output);
		free (output);
		return 1;
	}
	return DUK_RET_TYPE_ERROR;
}

static duk_ret_t duk_internal_require(duk_context *ctx) {
	char fullname[256];
	if (duk_is_string (ctx, 0)) {
		snprintf (fullname, sizeof(fullname), "%s.js", duk_safe_to_string (ctx, 0));
		char* text = r2dec_read_file (fullname);
		if (text) {
			duk_push_string (ctx, text);
			free (text);
		} else {
			printf("Error: '%s' not found.\n", fullname);
			return DUK_RET_TYPE_ERROR;
		}
		return 1;
	}
	return DUK_RET_TYPE_ERROR;
}

static void duk_r2_init(duk_context* ctx) {
	duk_push_c_function (ctx, duk_internal_require, 1);
	duk_put_global_string (ctx, "___internal_require");
	duk_push_c_function (ctx, duk_r2cmd, 1);
	duk_put_global_string (ctx, "r2cmd");
	duk_eval_string_noresult (ctx, REQUIRE_JS);
}

static void duk_eval_file(duk_context* ctx, const char* file) {
	char* text = r2dec_read_file (file);
	if (text) {
		duk_eval_string_noresult (ctx, text);
		free (text);
	}
}

static void r2dec_fatal_function (void *udata, const char *msg) {
    fprintf (stderr, "*** FATAL ERROR: %s\n", (msg ? msg : "no message"));
    fflush (stderr);
    abort ();
}

static void duk_r2dec(RCore *core, const char *input) {
	char args[1024] = {0};
	core_link = core;
	duk_context *ctx = duk_create_heap (0, 0, 0, 0, r2dec_fatal_function);
	duk_console_init (ctx, 0);
//	Long_init (ctx);
	duk_r2_init (ctx);
	duk_eval_file (ctx, "r2dec-duk.js");
	if (*input) {
		snprintf (args, sizeof(args), "if(typeof r2dec_main == 'function'){r2dec_main(\"%s\".split(/\\s+/));}else{console.log('Fatal error. Cannot use R2_HOME_DATADIR.');}", input);
	} else {
		snprintf (args, sizeof(args), "if(typeof r2dec_main == 'function'){r2dec_main(\"\".split(/\\s+/));}else{console.log('Fatal error. Cannot use R2_HOME_DATADIR.');}");
	}
	duk_eval_string_noresult (ctx, args);
	duk_destroy_heap (ctx);
	core_link = 0;
}

static void usage(void) {
	eprintf ("Usage: pdd [args] - core plugin for r2dec\n");
	eprintf (" pdd   - decompile current function\n");
	eprintf (" pdd?  - show this help\n");
	eprintf (" pdda  - decompile current function with side assembly\n");
	eprintf (" pddu  - install/upgrade r2dec via r2pm\n");
	eprintf (" pddi  - generates the issue data\n");
}

static void _cmd_pdd(RCore *core, const char *input) {
	switch (*input) {
	case '\0':
		duk_r2dec(core, input);
		break;
	case ' ':
		duk_r2dec(core, input);
		break;
	case 'u':
		// update
		r_core_cmd0 (core, "!r2pm -ci r2dec");
		break;
	case 'i':
		// --issue
		duk_r2dec(core, "--issue");
		break;
	case 'a':
		// --assembly
		duk_r2dec(core, "--assembly");
		break;
	case '?':
	default:
		usage();
		break;
	}
}

static void custom_config(bool *p, const char* input) {
	if (strchr (input, '=')) {
		if(strchr (input, 't')) {
			*p = true;
		} else if(strchr (input, 'f')) {
			*p = false;
		}
	} else {
		r_cons_printf ("%s\n", ((*p) ? "true" : "false"));
	}
}

static bool is_option(const char* input) {
	if (!strncmp (input, "e ", 2)) {
		input += 2;
		if (!strncmp (input, "r2dec.casts", 11)) {
			custom_config (&config.hidecasts, input + 11);
			return true;
		} else if (!strncmp (input, "r2dec.asm", 9)) {
			custom_config (&config.assembly, input + 9);
			return true;
		}
	} else if (!strncmp (input, "e! ", 3)) {
		input += 3;
		if (!strncmp (input, "r2dec.casts", 11)) {
			config.hidecasts = !config.hidecasts;
			return true;
		} else if (!strncmp (input, "r2dec.asm", 9)) {
			config.assembly = !config.assembly;
			return true;
		}
	} else if (!strncmp (input, "e? ", 3)) {
		input += 3;
		if (!strncmp (input, "r2dec.casts", 11)) {
			r_cons_printf ("         r2dec.casts: if true, hides all casts in the pseudo code\n");
			return true;
		} else if (!strncmp (input, "r2dec.asm", 9)) {
			r_cons_printf ("           r2dec.asm: if true, shows pseudo next to the assembly\n");
			return true;
		}
	}
	return false;
}

static int r_cmd_pdd(void *user, const char *input) {
	RCore *core = (RCore *) user;
	if (!strncmp (input, "e cmd.pdc", 9)) {
		if (strchr (input, '=') && strchr (input, '?')) {
			r_cons_printf ("r2dec\n");
			return false;
		}
	}
	if (!strncmp (input, "pdd", 3)) {
		_cmd_pdd (core, input + 3);
		return true;
	} else if (!strncmp (input, "r2dec", 5)) {
		duk_r2dec(core, input + 5);
		return true;
	} else if (is_option (input)) {
		return true;
	}
	return false;
}

RCorePlugin r_core_plugin_test = {
	.name = "r2dec",
	.desc = "experimental pseudo-C decompiler for radare2",
	.license = "Apache",
	.call = r_cmd_pdd,
};

#ifndef CORELIB
RLibStruct radare_plugin = {
	.type = R_LIB_TYPE_CORE,
	.data = &r_core_plugin_test,
	.version = R2_VERSION
};
#endif
