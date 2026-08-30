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
  };
  napi_define_properties(env, exports, sizeof(props)/sizeof(props[0]), props);
  return exports;
}
NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
