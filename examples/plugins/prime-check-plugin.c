#include "duktape.h"

static duk_ret_t native_prime_check(duk_context *ctx) {
    int val = duk_require_int(ctx, 0);
    int lim = duk_require_int(ctx, 1);
    int i;

    for (i = 2; i <= lim; i++) {
        if (val % i == 0) {
            duk_push_false(ctx);
            return 1;
        }
    }

    duk_push_true(ctx);
    return 1;
}

void duk_plugin_init(duk_context *ctx) {
    duk_push_global_object(ctx);
    duk_push_c_function(ctx, native_prime_check, 2 /*nargs*/);
    duk_put_prop_string(ctx, -2, "primeCheckNative");
}

void duk_plugin_cleanup(void) {
    // do nothing
}