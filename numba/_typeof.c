#include "_pymodule.h"

#include <string.h>
#include <time.h>
#include <assert.h>

#include "_typeof.h"
#include "_hashtable.h"

#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION
#include <numpy/ndarrayobject.h>


/* Cached typecodes for basic scalar types */
static int tc_int8;
static int tc_int16;
static int tc_int32;
static int tc_int64;
static int tc_uint8;
static int tc_uint16;
static int tc_uint32;
static int tc_uint64;
static int tc_float32;
static int tc_float64;
static int tc_complex64;
static int tc_complex128;
static int BASIC_TYPECODES[12];

static int tc_intp;

static PyObject* typecache;
static PyObject* ndarray_typecache;

static PyObject *str_typeof_pyval = NULL;


/*
 * Type fingerprint computation.
 */

typedef struct {
    char *buf;
    size_t n;
    size_t allocated;
    char static_buf[40];
} string_writer_t;

static void
string_writer_init(string_writer_t *w)
{
    w->buf = w->static_buf;
    w->n = 0;
    w->allocated = sizeof(w->static_buf) / sizeof(unsigned char);
}

static void
string_writer_clear(string_writer_t *w)
{
    if (w->buf != w->static_buf)
        free(w->buf);
}

static inline int
string_writer_ensure(string_writer_t *w, size_t bytes)
{
    size_t newsize;
    if (w->n + bytes <= w->allocated)
        return 0;
    newsize = (w->allocated << 2) + 1;
    if (w->buf == w->static_buf)
        w->buf = malloc(newsize);
    else
        w->buf = realloc(w->buf, newsize);
    if (w->buf) {
        return 0;
    }
    else {
        PyErr_NoMemory();
        return -1;
    }
}

static inline int
string_writer_put_char(string_writer_t *w, unsigned char c)
{
    if (string_writer_ensure(w, 1))
        return -1;
    w->buf[w->n++] = c;
    return 0;
}

static inline int
string_writer_put_int32(string_writer_t *w, unsigned int v)
{
    if (string_writer_ensure(w, 4))
        return -1;
    w->buf[w->n] = v & 0xff;
    w->buf[w->n + 1] = (v >> 8) & 0xff;
    w->buf[w->n + 2] = (v >> 16) & 0xff;
    w->buf[w->n + 3] = (v >> 24) & 0xff;
    w->n += 4;
    return 0;
}

static inline int
string_writer_put_intp(string_writer_t *w, npy_intp v)
{
    const int N = sizeof(npy_intp);
    if (string_writer_ensure(w, N))
        return -1;
    w->buf[w->n] = v & 0xff;
    w->buf[w->n + 1] = (v >> 8) & 0xff;
    w->buf[w->n + 2] = (v >> 16) & 0xff;
    w->buf[w->n + 3] = (v >> 24) & 0xff;
    if (N > 4) {
        w->buf[w->n + 4] = (v >> 32) & 0xff;
        w->buf[w->n + 5] = (v >> 40) & 0xff;
        w->buf[w->n + 6] = (v >> 48) & 0xff;
        w->buf[w->n + 7] = (v >> 56) & 0xff;
    }
    w->n += N;
    return 0;
}

static inline int
string_writer_put_string(string_writer_t *w, const char *s)
{
    if (s == NULL) {
        return string_writer_put_char(w, 0);
    }
    else {
        size_t N = strlen(s) + 1;
        if (string_writer_ensure(w, N))
            return -1;
        memcpy(w->buf + w->n, s, N);
        w->n += N;
        return 0;
    }
}

enum opcode {
    OP_START_TUPLE = '(',
    OP_END_TUPLE = ')',
    OP_INT = 'i',
    OP_FLOAT = 'f',
    OP_COMPLEX = 'c',
    OP_BOOL = '?',
    OP_BYTEARRAY = 'a',
    OP_BYTES = 'b',
    OP_NONE = 'n',

    OP_BUFFER = 'B',
    OP_NP_SCALAR = 'S',
    OP_NP_ARRAY = 'A',
    OP_NP_DTYPE = 'D'
};

#define TRY(func, w, arg) \
    do { \
        if (func(w, arg)) return -1; \
    } while (0)


static int
fingerprint_unrecognized(PyObject *val)
{
    PyErr_SetString(PyExc_NotImplementedError,
                    "cannot compute type fingerprint for value");
    return -1;
}

static int
compute_dtype_fingerprint(string_writer_t *w, PyArray_Descr *descr)
{
    int typenum = descr->type_num;
    if (typenum < NPY_OBJECT)
        return string_writer_put_char(w, (char) typenum);
    if (typenum == NPY_VOID) {
        /* Structured dtype, serialize the dtype pointer.  In most cases,
         * people reuse the same dtype instance; otherwise the cache
         * will just be less efficient (but as correct).
         */
        TRY(string_writer_put_char, w, (char) typenum);
        return string_writer_put_intp(w, (npy_intp) descr);
    }
#if NPY_API_VERSION >= 0x00000007
    if (PyTypeNum_ISDATETIME(typenum)) {
        PyArray_DatetimeMetaData *md;
        md = &(((PyArray_DatetimeDTypeMetaData *)descr->c_metadata)->meta);
        TRY(string_writer_put_char, w, (char) typenum);
        TRY(string_writer_put_char, w, (char) md->base);
        return string_writer_put_int32(w, (char) md->num);
    }
#endif

    return fingerprint_unrecognized((PyObject *) descr);
}

static int
compute_fingerprint(string_writer_t *w, PyObject *val)
{
    if (val == Py_None)
        return string_writer_put_char(w, OP_NONE);
    if (PyBool_Check(val))
        return string_writer_put_char(w, OP_BOOL);
    if (PyInt_Check(val) || PyLong_Check(val))
        return string_writer_put_char(w, OP_INT);
    if (PyFloat_Check(val))
        return string_writer_put_char(w, OP_FLOAT);
    if (PyComplex_CheckExact(val))
        return string_writer_put_char(w, OP_COMPLEX);
    if (PyTuple_Check(val)) {
        Py_ssize_t i, n;
        n = PyTuple_GET_SIZE(val);
        TRY(string_writer_put_char, w, OP_START_TUPLE);
        for (i = 0; i < n; i++)
            TRY(compute_fingerprint, w, PyTuple_GET_ITEM(val, i));
        TRY(string_writer_put_char, w, OP_END_TUPLE);
        return 0;
    }
    if (PyBytes_Check(val))
        return string_writer_put_char(w, OP_BYTES);
    if (PyByteArray_Check(val))
        return string_writer_put_char(w, OP_BYTEARRAY);
    if (PyArray_IsScalar(val, Generic)) {
        /* Note: PyArray_DescrFromScalar() may be a bit slow on
           non-trivial types. */
        PyArray_Descr *descr = PyArray_DescrFromScalar(val);
        if (descr == NULL)
            return -1;
        TRY(string_writer_put_char, w, OP_NP_SCALAR);
        TRY(compute_dtype_fingerprint, w, descr);
        Py_DECREF(descr);
        return 0;
    }
    if (PyArray_Check(val)) {
        PyArrayObject *ary = (PyArrayObject *) val;
        int ndim = PyArray_NDIM(ary);

        TRY(string_writer_put_char, w, OP_NP_ARRAY);
        TRY(string_writer_put_int32, w, ndim);
        if (PyArray_IS_C_CONTIGUOUS(ary))
            TRY(string_writer_put_char, w, 'C');
        else if (PyArray_IS_F_CONTIGUOUS(ary))
            TRY(string_writer_put_char, w, 'F');
        else
            TRY(string_writer_put_char, w, 'A');
        if (PyArray_ISWRITEABLE(ary))
            TRY(string_writer_put_char, w, 'W');
        else
            TRY(string_writer_put_char, w, 'R');
        return compute_dtype_fingerprint(w, PyArray_DESCR(ary));
    }
    if (PyObject_CheckBuffer(val)) {
        Py_buffer buf;
        int flags = PyBUF_ND | PyBUF_STRIDES | PyBUF_FORMAT;
        char contig;
        int ndim;
        char readonly;

        /* Attempt to get a writable buffer, then fallback on read-only */
        if (PyObject_GetBuffer(val, &buf, flags | PyBUF_WRITABLE)) {
            PyErr_Clear();
            if (PyObject_GetBuffer(val, &buf, flags))
                goto _unrecognized;
        }
        if (PyBuffer_IsContiguous(&buf, 'C'))
            contig = 'C';
        else if (PyBuffer_IsContiguous(&buf, 'F'))
            contig = 'F';
        else
            contig = 'A';
        ndim = buf.ndim;
        readonly = buf.readonly ? 'R' : 'W';
        if (string_writer_put_char(w, OP_BUFFER) ||
            string_writer_put_int32(w, ndim) ||
            string_writer_put_char(w, contig) ||
            string_writer_put_char(w, readonly) ||
            string_writer_put_string(w, buf.format) ||
            /* We serialize the object's Python type as well, to
               distinguish between types which have Numba specializations
               (e.g. array.array() vs. memoryview)
            */
            string_writer_put_intp(w, (npy_intp) Py_TYPE(val))) {
            PyBuffer_Release(&buf);
            return -1;
        }
        PyBuffer_Release(&buf);
        return 0;
    }
    if (PyArray_DescrCheck(val)) {
        TRY(string_writer_put_char, w, OP_NP_DTYPE);
        return compute_dtype_fingerprint(w, (PyArray_Descr *) val);
    }

_unrecognized:
    /* Type not recognized */
    return fingerprint_unrecognized(val);
}

PyObject *
typeof_compute_fingerprint(PyObject *val)
{
    PyObject *res;
    string_writer_t w;

    string_writer_init(&w);

    if (compute_fingerprint(&w, val))
        goto error;
    res = PyBytes_FromStringAndSize(w.buf, w.n);

    string_writer_clear(&w);
    return res;

error:
    string_writer_clear(&w);
    return NULL;
}


/* When we want to cache the type's typecode for later lookup, we need to
   keep a reference to the returned type object so that it cannot be
   deleted. This is because of the following events occurring when first
   using a @jit function for a given set of types:

    1. typecode_fallback requests a new typecode for an arbitrary Python value;
       this implies creating a Numba type object (on the first dispatcher call);
       the typecode cache is then populated.
    2. matching of the typecode list in _dispatcherimpl.cpp fails, since the
       typecode is new.
    3. we have to compile: compile_and_invoke() is called, it will invoke
       Dispatcher_Insert to register the new signature.

   The reference to the Numba type object returned in step 1 is deleted as
   soon as we call Py_DECREF() on it, since we are holding the only
   reference. If this happens and we use the typecode we got to populate the
   cache, then the cache won't ever return the correct typecode, and the
   dispatcher will never successfully match the typecodes with those of
   some already-compiled instance. So we need to make sure that we don't
   call Py_DECREF() on objects whose typecode will be used to populate the
   cache. This is ensured by calling _typecode_fallback with
   retain_reference == 0.

   Note that technically we are leaking the reference, since we do not continue
   to hold a pointer to the type object that we get back from typeof_pyval.
   However, we don't need to refer to it again, we just need to make sure that
   it is never deleted.
*/
static
int _typecode_fallback(PyObject *dispatcher, PyObject *val,
                       int retain_reference) {
    PyObject *tmptype, *tmpcode;
    int typecode;

    // Go back to the interpreter
    tmptype = PyObject_CallMethodObjArgs((PyObject *) dispatcher,
                                         str_typeof_pyval, val, NULL);
    if (!tmptype) {
        return -1;
    }

    tmpcode = PyObject_GetAttrString(tmptype, "_code");
    if (!retain_reference) {
        Py_DECREF(tmptype);
    }
    if (tmpcode == NULL) {
        return -1;
    }
    typecode = PyLong_AsLong(tmpcode);
    Py_DECREF(tmpcode);
    return typecode;
}

/* Variations on _typecode_fallback for convenience */

static
int typecode_fallback(PyObject *dispatcher, PyObject *val) {
    return _typecode_fallback(dispatcher, val, 0);
}

static
int typecode_fallback_keep_ref(PyObject *dispatcher, PyObject *val) {
    return _typecode_fallback(dispatcher, val, 1);
}


/* A cache mapping fingerprints (string_writer_t *) to typecodes (int). */
static _Py_hashtable_t *fingerprint_hashtable = NULL;

static Py_uhash_t
hash_writer(const void *key)
{
    string_writer_t *writer = (string_writer_t *) key;
    Py_uhash_t x = 0;

    /* The old FNV algorithm used by Python 2 */
    if (writer->n > 0) {
        unsigned char *p = (unsigned char *) writer->buf;
        Py_ssize_t len = writer->n;
        x ^= *p << 7;
        while (--len >= 0)
            x = (1000003*x) ^ *p++;
        x ^= writer->n;
        if (x == -1)
            x = -2;
    }
    return x;
}

static int
compare_writer(const void *key, const _Py_hashtable_entry_t *entry)
{
    string_writer_t *v = (string_writer_t *) key;
    string_writer_t *w = (string_writer_t *) entry->key;
    if (v->n != w->n)
        return 0;
    return memcmp(v->buf, w->buf, v->n) == 0;
}

static int
typecode_using_fingerprint(PyObject *dispatcher, PyObject *val)
{
    int typecode;
    string_writer_t w;

    if (fingerprint_hashtable == NULL) {
        fingerprint_hashtable = _Py_hashtable_new(sizeof(int),
                                                  hash_writer,
                                                  compare_writer);
        if (fingerprint_hashtable == NULL) {
            PyErr_NoMemory();
            return -1;
        }
    }

    string_writer_init(&w);

    if (compute_fingerprint(&w, val)) {
        string_writer_clear(&w);
        if (PyErr_ExceptionMatches(PyExc_NotImplementedError)) {
            /* Can't compute a type fingerprint for the given value,
               fall back on typeof() without caching. */
            PyErr_Clear();
            return typecode_fallback(dispatcher, val);
        }
        return -1;
    }
    if (_Py_HASHTABLE_GET(fingerprint_hashtable, &w, typecode) > 0) {
        /* Cache hit */
        string_writer_clear(&w);
        return typecode;
    }

    /* Not found in cache: invoke pure Python typeof() and cache result.
     * Note we have to keep the type alive forever as explained
     * above in _typecode_fallback().
     */
    typecode = typecode_fallback_keep_ref(dispatcher, val);
    if (typecode >= 0) {
        string_writer_t *key = (string_writer_t *) malloc(sizeof(string_writer_t));
        if (key == NULL) {
            string_writer_clear(&w);
            PyErr_NoMemory();
            return -1;
        }
        memcpy(key, &w, sizeof(string_writer_t));
        if (_Py_HASHTABLE_SET(fingerprint_hashtable, key, typecode)) {
            string_writer_clear(&w);
            PyErr_NoMemory();
            return -1;
        }
    }
    return typecode;
}


/*
 * Direct lookup table for extra-fast typecode resolution of simple array types.
 */

#define N_DTYPES 12
#define N_NDIM 5    /* Fast path for up to 5D array */
#define N_LAYOUT 3
static int cached_arycode[N_NDIM][N_LAYOUT][N_DTYPES];

/* Convert a Numpy dtype number to an internal index into cached_arycode.
   The returned value must also be a valid index into BASIC_TYPECODES. */
static int dtype_num_to_typecode(int type_num) {
    int dtype;
    switch(type_num) {
    case NPY_INT8:
        dtype = 0;
        break;
    case NPY_INT16:
        dtype = 1;
        break;
    case NPY_INT32:
        dtype = 2;
        break;
    case NPY_INT64:
        dtype = 3;
        break;
    case NPY_UINT8:
        dtype = 4;
        break;
    case NPY_UINT16:
        dtype = 5;
        break;
    case NPY_UINT32:
        dtype = 6;
        break;
    case NPY_UINT64:
        dtype = 7;
        break;
    case NPY_FLOAT32:
        dtype = 8;
        break;
    case NPY_FLOAT64:
        dtype = 9;
        break;
    case NPY_COMPLEX64:
        dtype = 10;
        break;
    case NPY_COMPLEX128:
        dtype = 11;
        break;
    default:
        /* Type not included in the global lookup table */
        dtype = -1;
    }
    return dtype;
}

static
int typecode_ndarray(PyObject *dispatcher, PyArrayObject *ary) {
    int typecode;
    int dtype;
    int ndim = PyArray_NDIM(ary);
    int layout = 0;

    /* The order in which we check for the right contiguous-ness is important.
       The order must match the order by numba.numpy_support.map_layout.
    */
    if (PyArray_ISCARRAY(ary)){
        layout = 1;
    } else if (PyArray_ISFARRAY(ary)) {
        layout = 2;
    }

    if (ndim <= 0 || ndim > N_NDIM) goto FALLBACK;

    dtype = dtype_num_to_typecode(PyArray_TYPE(ary));
    if (dtype == -1) goto FALLBACK;

    /* Fast path, using direct table lookup */
    assert(layout < N_LAYOUT);
    assert(ndim <= N_NDIM);
    assert(dtype < N_DTYPES);

    typecode = cached_arycode[ndim - 1][layout][dtype];
    if (typecode == -1) {
        /* First use of this table entry, so it requires populating */
        typecode = typecode_fallback_keep_ref(dispatcher, (PyObject*)ary);
        cached_arycode[ndim - 1][layout][dtype] = typecode;
    }
    return typecode;

FALLBACK:
    /* Slower path, for non-trivial array types */
    return typecode_using_fingerprint(dispatcher, (PyObject *) ary);
}

int
typeof_typecode(PyObject *dispatcher, PyObject *val)
{
    /* Fast array handling */
    if (PyType_IsSubtype(Py_TYPE(val), &PyArray_Type)) {
        return typecode_ndarray(dispatcher, (PyArrayObject*)val);
    }

    return typecode_using_fingerprint(dispatcher, val);
}

PyObject *
typeof_init(PyObject *self, PyObject *args)
{
    PyObject *tmpobj;
    PyObject* dict = PySequence_Fast_GET_ITEM(args, 0);
    int index = 0;

    /* Initialize Numpy API */
    import_array();

    #define UNWRAP_TYPE(S)                                              \
        if(!(tmpobj = PyDict_GetItemString(dict, #S))) return NULL;     \
        else {  tc_##S = PyLong_AsLong(tmpobj);                         \
                BASIC_TYPECODES[index++] = tc_##S;  }

    UNWRAP_TYPE(int8)
    UNWRAP_TYPE(int16)
    UNWRAP_TYPE(int32)
    UNWRAP_TYPE(int64)

    UNWRAP_TYPE(uint8)
    UNWRAP_TYPE(uint16)
    UNWRAP_TYPE(uint32)
    UNWRAP_TYPE(uint64)

    UNWRAP_TYPE(float32)
    UNWRAP_TYPE(float64)

    UNWRAP_TYPE(complex64)
    UNWRAP_TYPE(complex128)

    #undef UNWRAP_TYPE

    switch(sizeof(void*)) {
    case 4:
        tc_intp = tc_int32;
        break;
    case 8:
        tc_intp = tc_int64;
        break;
    default:
        PyErr_SetString(PyExc_AssertionError, "sizeof(void*) != {4, 8}");
        return NULL;
    }

    typecache = PyDict_New();
    ndarray_typecache = PyDict_New();
    if (typecache == NULL || ndarray_typecache == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "failed to create type cache");
        return NULL;
    }

    /* initialize cached_arycode to all ones (in bits) */
    memset(cached_arycode, 0xFF, sizeof(cached_arycode));

    str_typeof_pyval = PyString_InternFromString("typeof_pyval");
    if (str_typeof_pyval == NULL)
        return NULL;

    Py_RETURN_NONE;
}
