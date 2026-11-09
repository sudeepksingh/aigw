# Model back-ends: running AIGW's models on your NPU

AIGW's on-device models (today: the domain-risk model, 150 gradient-boosted
trees over 12 features of a DNS name) are computed by a **back-end**. Two are
built in; a router maker can add its own for an NPU, a DSP or a tuned CPU
library, as one shared library and no change to AIGW.

| Back-end | What it is |
|----------|------------|
| `interp` | The reference interpreter. Every other back-end is checked against it. |
| `flat` | Built in: the same trees packed into one array, thresholds stored as floats rounded down so every decision, and every score, is identical to the reference. Faster on small CPU caches. |
| yours | A library in `/usr/lib/aigw/backends`. |

## How agd uses a back-end

1. **Loading.** At start, agd loads every `*.so` in `models.backend_dir`
   (`/usr/lib/aigw/backends`). The directory and each file must belong to root
   and be writable by nobody else, so only the package manager can put code
   there. Anything else is skipped with a note (`agctl models`).
2. **Checking.** Whenever a model is loaded (at start, or when a signed update
   arrives), every back-end prepares it and scores 300 probe inputs:
   - known names: popular sites, generated names, generated names under `www.`;
   - names generated at random of every kind.

   A back-end passes only when, on every probe:
   - it gives the reference's verdict (alert or not, at the alert threshold);
   - its probability is within 0.005 of the reference's.

   Each back-end is also timed.
3. **Choosing.** `models.backend auto` uses the fastest back-end that passed.
   A name uses that one if it passed, and the reference otherwise.

A back-end never sees traffic, only feature vectors.

## Writing one

The interface is one header, [`include/ag/backend.h`](../include/ag/backend.h).
On OpenWRT it is installed for other packages: build with `DEPENDS:=+aigw` and
`#include <ag/backend.h>`.

```c
#include <ag/backend.h>

/* "compile" the model for your hardware; NULL + reason if you can't take it */
static void *prepare(const struct ag_model_view *m, char *err, size_t n);
/* raw scores (log-odds) of `count` feature vectors, each m->n_features floats */
static int predict(void *handle, const float *x, int count, double *raw);
static void release(void *handle);

static const struct ag_backend me = {
	.abi = AG_BACKEND_ABI, .name = "acme-npu",
	.prepare = prepare, .predict = predict, .release = release,
};
const struct ag_backend *ag_backend_entry(void) { return &me; }
```

The model view gives you every tree:
- an inner node goes left when `x[feature] <= value`;
- a leaf has `feature == -1` and adds `value`;
- the score is `base` plus the leaves reached, one per tree.

You may keep pointers into the view until `release()`. `predict()` is called
from one thread.

A complete example, computing in 32-bit floats as an accelerator might, is
[`plugins/examples/backend/example_backend.c`](../plugins/examples/backend/example_backend.c):

```sh
cc -O2 -shared -fPIC -I include -o acme.so plugins/examples/backend/example_backend.c
install -m 0644 -o root acme.so /usr/lib/aigw/backends/
/etc/init.d/aigw restart
agctl models
```

```
Computed by (models.backend auto; libraries in /usr/lib/aigw/backends)
  interp       1.98 µs per score, same verdicts as the reference, built in (reference)
  flat         IN USE, 1.80 µs per score, same verdicts as the reference, built in
  example-npu  2.00 µs per score, same verdicts as the reference, /usr/lib/aigw/backends/acme.so
```

`tests/backend-test.sh` runs this example, and a broken build of it that agd
must refuse.

## Trust

A back-end runs inside agd, as root, so it is trusted like any package:
- it is loaded only from root's directory;
- it is never downloaded by AIGW;
- the check protects the verdicts, not the system.

If your NPU's SDK needs a separate process, have `predict()` talk to it.
