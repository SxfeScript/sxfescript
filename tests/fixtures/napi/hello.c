#include <node_api.h>
#include <string.h>
#include <stdlib.h>

static napi_value Hello(napi_env env, napi_callback_info info) {
  napi_value out;
  napi_create_string_utf8(env, "hello from C", NAPI_AUTO_LENGTH, &out);
  return out;
}

static napi_value Add(napi_env env, napi_callback_info info) {
  size_t argc = 2; napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  if (argc < 2) { napi_throw_type_error(env, "ERR_ARGS", "add(a, b)"); return NULL; }
  double a, b;
  napi_get_value_double(env, argv[0], &a);
  napi_get_value_double(env, argv[1], &b);
  napi_value out; napi_create_double(env, a + b, &out);
  return out;
}

static napi_value Concat(napi_env env, napi_callback_info info) {
  size_t argc = 1; napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  size_t len = 0;
  napi_get_value_string_utf8(env, argv[0], NULL, 0, &len);
  char *buf = malloc(len + 8);
  napi_get_value_string_utf8(env, argv[0], buf, len + 1, NULL);
  memcpy(buf + len, "!", 2);
  napi_value out; napi_create_string_utf8(env, buf, NAPI_AUTO_LENGTH, &out);
  free(buf);
  return out;
}

static napi_value MakeObject(napi_env env, napi_callback_info info) {
  napi_value obj, n, arr, s;
  napi_create_object(env, &obj);
  napi_create_int32(env, 42, &n);
  napi_set_named_property(env, obj, "answer", n);
  napi_create_array(env, &arr);
  for (uint32_t i = 0; i < 3; i++) { napi_value e; napi_create_uint32(env, i * i, &e); napi_set_element(env, arr, i, e); }
  napi_set_named_property(env, obj, "squares", arr);
  napi_create_string_utf8(env, "nested", NAPI_AUTO_LENGTH, &s);
  napi_set_named_property(env, obj, "label", s);
  return obj;
}

static napi_value CallBack(napi_env env, napi_callback_info info) {
  size_t argc = 1; napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  napi_value undef, arg, result;
  napi_get_undefined(env, &undef);
  napi_create_int32(env, 7, &arg);
  napi_call_function(env, undef, argv[0], 1, &arg, &result);
  return result;
}

static napi_value Thrower(napi_env env, napi_callback_info info) {
  napi_throw_range_error(env, "ERR_RANGE", "out of range on purpose");
  return NULL;
}

static napi_value MakeBuffer(napi_env env, napi_callback_info info) {
  void *data; napi_value buf;
  napi_create_buffer(env, 4, &data, &buf);
  memcpy(data, "abcd", 4);
  return buf;
}

static napi_value ReadTyped(napi_env env, napi_callback_info info) {
  size_t argc = 1; napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  void *data = NULL; size_t len = 0;
  napi_get_typedarray_info(env, argv[0], NULL, &len, &data, NULL, NULL);
  int sum = 0;
  for (size_t i = 0; i < len; i++) sum += ((unsigned char *)data)[i];
  napi_value out; napi_create_int32(env, sum, &out);
  return out;
}

typedef struct { int value; } Counter;
static void CounterFinalize(napi_env env, void *data, void *hint) { free(data); }
static napi_value MakeCounter(napi_env env, napi_callback_info info) {
  napi_value obj; napi_create_object(env, &obj);
  Counter *c = malloc(sizeof(Counter)); c->value = 100;
  napi_wrap(env, obj, c, CounterFinalize, NULL, NULL);
  return obj;
}
static napi_value ReadCounter(napi_env env, napi_callback_info info) {
  size_t argc = 1; napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  Counter *c = NULL;
  napi_unwrap(env, argv[0], (void **)&c);
  napi_value out; napi_create_int32(env, c ? c->value : -1, &out);
  return out;
}

/* A thread-safe function driven from a real worker thread: the one part of
   this that cannot be exercised without threads. The worker calls in three
   times and releases; the JS side counts the calls and resolves. */
#include <pthread.h>

typedef struct { napi_threadsafe_function tsfn; } Job;

static void *worker(void *arg) {
  Job *j = arg;
  for (intptr_t i = 1; i <= 3; i++)
    napi_call_threadsafe_function(j->tsfn, (void *)i, napi_tsfn_blocking);
  napi_release_threadsafe_function(j->tsfn, napi_tsfn_release);
  free(j);
  return NULL;
}

static void CallIntoJs(napi_env env, napi_value js_cb, void *context, void *data) {
  if (env == NULL || js_cb == NULL) return;
  napi_value undef, arg, ignored;
  napi_get_undefined(env, &undef);
  napi_create_int32(env, (int32_t)(intptr_t)data, &arg);
  napi_call_function(env, undef, js_cb, 1, &arg, &ignored);
}

static napi_value FromThread(napi_env env, napi_callback_info info) {
  size_t argc = 1; napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  napi_value name;
  napi_create_string_utf8(env, "sxn-tsfn-test", NAPI_AUTO_LENGTH, &name);
  Job *j = malloc(sizeof(Job));
  if (napi_create_threadsafe_function(env, argv[0], NULL, name, 0, 1,
                                      NULL, NULL, NULL, CallIntoJs, &j->tsfn) != napi_ok) {
    free(j);
    napi_throw_error(env, "ERR_TSFN", "cannot create a thread-safe function");
    return NULL;
  }
  /* The process must stay awake until the worker is done. */
  napi_ref_threadsafe_function(env, j->tsfn);
  pthread_t th;
  pthread_create(&th, NULL, worker, j);
  pthread_detach(th);
  napi_value undef; napi_get_undefined(env, &undef);
  return undef;
}

/* A class, which is the shape that needs new_target to work: a constructor
   built the wrong way never learns it was called with `new`. */
typedef struct { int32_t n; } Box;
static void BoxFree(napi_env env, void *data, void *hint) { free(data); }

static napi_value BoxNew(napi_env env, napi_callback_info info) {
  napi_value target;
  napi_get_new_target(env, info, &target);
  if (target == NULL) { napi_throw_type_error(env, "ERR_CTOR", "Box requires new"); return NULL; }
  size_t argc = 1; napi_value argv[1], self;
  napi_get_cb_info(env, info, &argc, argv, &self, NULL);
  int32_t n = 0;
  if (argc > 0) napi_get_value_int32(env, argv[0], &n);
  Box *b = malloc(sizeof(Box)); b->n = n;
  napi_wrap(env, self, b, BoxFree, NULL, NULL);
  return self;
}
static napi_value BoxValue(napi_env env, napi_callback_info info) {
  napi_value self; size_t argc = 0;
  napi_get_cb_info(env, info, &argc, NULL, &self, NULL);
  Box *b = NULL; napi_unwrap(env, self, (void **)&b);
  napi_value out; napi_create_int32(env, b ? b->n : -1, &out);
  return out;
}
static napi_value BoxAdd(napi_env env, napi_callback_info info) {
  size_t argc = 2; napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  int32_t a = 0, b = 0;
  napi_get_value_int32(env, argv[0], &a); napi_get_value_int32(env, argv[1], &b);
  napi_value out; napi_create_int32(env, a + b, &out);
  return out;
}

/* More handles than one scope block holds. A scope that relocated its slots
   would leave every one of these pointing at the wrong place. */
static napi_value ManyHandles(napi_env env, napi_callback_info info) {
  napi_value held[200];
  for (int i = 0; i < 200; i++) napi_create_int32(env, i, &held[i]);
  int32_t sum = 0;
  for (int i = 0; i < 200; i++) { int32_t v = -1; napi_get_value_int32(env, held[i], &v); sum += v; }
  napi_value out; napi_create_int32(env, sum, &out);
  return out;
}

static napi_value Init(napi_env env, napi_value exports) {
  napi_property_descriptor props[] = {
    { "hello", NULL, Hello, NULL, NULL, NULL, napi_default, NULL },
    { "add", NULL, Add, NULL, NULL, NULL, napi_default, NULL },
    { "concat", NULL, Concat, NULL, NULL, NULL, napi_default, NULL },
    { "makeObject", NULL, MakeObject, NULL, NULL, NULL, napi_default, NULL },
    { "callBack", NULL, CallBack, NULL, NULL, NULL, napi_default, NULL },
    { "thrower", NULL, Thrower, NULL, NULL, NULL, napi_default, NULL },
    { "makeBuffer", NULL, MakeBuffer, NULL, NULL, NULL, napi_default, NULL },
    { "readTyped", NULL, ReadTyped, NULL, NULL, NULL, napi_default, NULL },
    { "makeCounter", NULL, MakeCounter, NULL, NULL, NULL, napi_default, NULL },
    { "readCounter", NULL, ReadCounter, NULL, NULL, NULL, napi_default, NULL },
    { "fromThread", NULL, FromThread, NULL, NULL, NULL, napi_default, NULL },
    { "manyHandles", NULL, ManyHandles, NULL, NULL, NULL, napi_default, NULL },
  };
  napi_define_properties(env, exports, sizeof(props)/sizeof(props[0]), props);

  napi_property_descriptor boxprops[] = {
    { "value", NULL, NULL, BoxValue, NULL, NULL, napi_enumerable, NULL },
    { "get", NULL, BoxValue, NULL, NULL, NULL, napi_default, NULL },
    { "add", NULL, BoxAdd, NULL, NULL, NULL, (napi_property_attributes)(napi_static | napi_default), NULL },
  };
  napi_value klass;
  napi_define_class(env, "Box", NAPI_AUTO_LENGTH, BoxNew, NULL,
                    sizeof(boxprops)/sizeof(boxprops[0]), boxprops, &klass);
  napi_set_named_property(env, exports, "Box", klass);
  return exports;
}
NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
