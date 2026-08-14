# Runtime review fix — failure-atomic startup and production orchestration tests

## Review verdict

Both external findings were confirmed against `31a227c` before implementation.

1. `UserApp_TaskInit()` accepted null results from three semaphore creations,
   two queue creations, queue-set creation, both queue-set additions, and both
   task creations. The command queue was visible to public APIs before UiTask
   was known to exist or consume it.
2. The former `study_source` tests exercised boot/clear/local-result policy
   helpers that production did not call. Production catalog restore, remote
   commit, local-grade publication, and the KanjiTask source-change decision
   were separately embedded in `user_app.cpp`.

## Strict RED evidence

The production-runtime test was replaced before its new API existed. The first
focused run failed with the intended missing production symbols, beginning:

```text
error: unknown type name 'study_catalog_ops_t'
error: unknown type name 'study_runtime_t'
error: call to undeclared function 'study_runtime_restore'
```

After the production study runtime was GREEN, a source-switch assertion was
added first. Its RED was:

```text
error: call to undeclared function 'study_runtime_remote_grade_ready'
```

The lifecycle regression was also added before `task_lifecycle.{h,c}`. The
initial focused runner stopped at the study-runtime RED above, before it could
reach that target. The lifecycle source/header did not exist at that revision;
no production implementation preceded these tests.

## Production changes

### Failure-atomic initialization

`task_lifecycle.{h,c}` is the production transaction used by
`UserApp_TaskInit()`. It creates and validates, in order:

- state mutex;
- catalog mutex;
- poll-wake binary semaphore;
- UiTask-ready binary semaphore;
- button queue;
- command queue;
- queue set;
- both queue-set memberships;
- runtime preparation;
- UiTask creation;
- bounded UiTask readiness;
- KanjiTask creation.

The lifecycle publishes the command API only after UiTask has rendered its
first frame, signaled readiness immediately before its queue loop, and both
tasks exist. Failure deletes any
created tasks, removes added queue-set members, deletes every created queue or
semaphore, zeroes all handles, and never invokes publication. UiTask readiness
has a 30-second bound, so panel/task failure cannot hang boot forever.

`UserApp_TaskInit()` now returns `user_app_init_result_t`. `app_main()` returns
before provisioning, startup command enqueues, time sync, or `device_api_start`
when initialization is not `USER_APP_INIT_OK`. Every public command/snapshot
entry point additionally checks the final initialized gate rather than the
presence of an internal queue alone.

Buttons are initialized only after the complete lifecycle succeeds. This keeps
the ISR producer absent from all rollback paths, so deleted button queues
cannot retain a live producer.

### Production-used study transactions

The old unused boot/result policy structs were removed. `study_source.{h,c}`
now owns `study_runtime_t` and the exact functions called by production for:

- cold boot and URL-clear catalog restore with demo fallback;
- captured local versus remote grade routes and source generation;
- successful/stale remote commit and navigation reset;
- remote-grade validity after URL/source changes;
- catalog grade, current-card lookup, and final atomic state publication.

The catalog adapter contract preserves the previously reviewed store invariant:
`catalog_store_grade()` decodes the next card, persists and verifies the grade,
then pointer-publishes the store snapshot. The runtime calls that transaction
before retrieving the current snapshot and before acquiring the app-state lock
for its card/hash/ordinal/nav publication. Decode or state failure only clears
the occupied request; the answer, card, navigation, and ordinal remain intact.
A captured local grade may advance the local store after a remote takeover but
cannot replace the remote panel card; a captured remote grade cannot become a
local store write.

UiTask remains the only LVGL/panel owner. The transaction module performs no
LVGL, panel, network, allocation, or FreeRTOS work.

## Behavioral coverage

The focused user-app suite now exercises the production symbols used by
`user_app.cpp`, including:

- catalog cold boot at the restored ordinal;
- URL clear restoring catalog, then demo only when catalog is unavailable;
- local callback order `decode -> persist -> current -> publish`;
- decode failure and persistence failure preserving answer/card/ordinal;
- local capture surviving remote takeover without cross-publication;
- remote capture becoming invalid after source-generation change or URL clear;
- stale remote commit preserving the catalog card;
- every one of 13 initialization failure injection points cleaning all live
  resources/tasks, removing queue-set memberships, and never publishing;
- success publishing only after both task handles and UiTask readiness;
- the existing saturated startup queue delivering filler, network config, then
  overlay dismissal in exact order before API eligibility.

ASan+UBSan runs of both new host executables report zero failures.

## Verification evidence

Focused/component suites:

```sh
sh components/user_app/test/run.sh
sh components/provisioning/test/run.sh
```

Result: exit 0. User app reports source guard, production study runtime, task
lifecycle, and startup delivery at zero failures. Provisioning reports
`40 tests, 87 checks, 0 failures`.

Portable host suite:

```sh
cmake -S components/vault_core/test/host -B /tmp/offline-runtime-host
cmake --build /tmp/offline-runtime-host -j8
ctest --test-dir /tmp/offline-runtime-host --output-on-failure
```

Result: exit 0, `100% tests passed, 0 tests failed out of 9`.

Catalog-store transaction suite:

```sh
cmake -S components/catalog_store/test/host \
  -B /tmp/offline-runtime-catalog-store
cmake --build /tmp/offline-runtime-catalog-store -j8
ctest --test-dir /tmp/offline-runtime-catalog-store --output-on-failure
```

Result: exit 0, `catalog_store_core` passed 1/1. This is the real portable
decode-before-state-before-pointer-publication implementation below the runtime
adapter.

Simulator:

```sh
cmake -S sim -B /tmp/offline-runtime-sim
cmake --build /tmp/offline-runtime-sim -j8
```

Result: exit 0, `[100%] Built target kanji_sim`.

ESP-IDF 5.4.3 focused objects:

```sh
ninja -C /tmp/offline-runtime-idf \
  esp-idf/user_app/CMakeFiles/__idf_user_app.dir/user_app.cpp.obj \
  esp-idf/user_app/CMakeFiles/__idf_user_app.dir/study_source.c.obj \
  esp-idf/user_app/CMakeFiles/__idf_user_app.dir/startup_delivery.c.obj \
  esp-idf/user_app/CMakeFiles/__idf_user_app.dir/task_lifecycle.c.obj \
  esp-idf/main/CMakeFiles/__idf_main.dir/main.cpp.obj
```

Result: exit 0 under the real ESP32-S3 GCC/G++ commands.

The same commands were replayed with `-Wframe-larger-than=2048`,
`-Werror=frame-larger-than=2048`, and `-fstack-usage`. Relevant frame sizes are:

```text
study_runtime_restore                 64
study_runtime_capture_grade           96
study_runtime_commit_remote           32
study_runtime_process_local_grade     48
user_app_task_lifecycle_start         32
restore_catalog_or_demo               80
commit                                32
process_local_grade                   96
UserApp_TaskInit                      96
```

All are static byte counts and the hard 2 KiB frame gate exits 0.

An attempted dependency-wide `__idf_user_app __idf_main` build in the existing
external directory reached the already documented generated-config issue:
LVGL's unused `lv_example_style_21.c` references disabled Montserrat 14. Task
7's fresh external acceptance build at `33c15c9` uses its committed defaults,
disables unused examples/demos, and links successfully. This lane did not edit
`sdkconfig`, `sdkconfig.defaults`, or `managed_components`; its own production
objects and all requested independent suites compile/link as listed above.

`git diff --check` exits 0. No hardware was flashed.

## Review pass

The explicit no-subagent constraint overrode the normal external-review
dispatch. A separate post-implementation diff/invariant pass checked rollback
order, queue-set emptiness, ISR timing, task readiness, external API gating,
catalog/state lock order, source-capture behavior, frame use, and Task 7 file
isolation. That pass reported no open Critical or Important issue; the later
scoped re-review below found and closed one additional queue-order defect.

## Scoped re-review addendum

### Draw identity RED and fix

The scoped re-review reproduced a queue ordering that the first token policy
did not model: KanjiTask can publish a local next card while an already queued
`SET_URL(non-empty)` is ahead of its `CARD_ADVANCED` command. A source-only
generation check then rejected the local draw even though the runtime and
catalog had advanced, leaving the panel on the old revealed answer.

The production-module test was written first and failed to compile on the
missing `study_draw_token_t`, `study_runtime_capture_draw()`, and
`study_runtime_accepts_draw()` symbols. The fix separates two identities:

- `publication_revision` advances on restore, visible local grade publication,
  and changed/advanced/source-transition remote publication;
- `source_guard` remains the HTTP endpoint/fetch generation.

Queued local catalog draws carry `STUDY_DRAW_PUBLICATION_ONLY`, so a non-empty
URL edit alone cannot strand them. Remote card and remote status/error draws
carry `STUDY_DRAW_PUBLICATION_AND_SOURCE`, so a command committed before a
queued endpoint replacement is rejected. Restore/URL clear or any later card
publication changes the revision and invalidates either kind of older draw.
The production `handle_cmd()`, local-grade path, remote commit path, HTTP-error
path, and URL/network-config source changes all call these tested helpers.

The test also covers a same-payload fetch from the replacement endpoint. The
runtime records the generation of the last remote publication, so the first
valid commit for a new endpoint is a new publication even when its card hash is
identical. This guarantees a current-source draw follows a rejected old-source
command; later same-generation/same-card polls remain unchanged and do not
refresh.

### HTTP gate RED and fix

The lifecycle fake was extended first to acquire a prepare-owned gate. Before
the cleanup callback released it, failure injection at UI creation, UI-ready
wait, and KanjiTask creation produced six intended assertions (live gate plus
missing release for each stage). The HTTP seam test was then changed first and
failed on the former `void http_port_init(void)` declaration.

`http_port_init()` now returns whether the FreeRTOS TLS-connect mutex exists,
and `http_port_deinit()` releases it. A failed allocation makes lifecycle
prepare fail atomically. Every later startup failure deletes tasks first and
then releases the gate through `cleanup_complete`; a successful startup keeps
it for KanjiTask. The curl simulator and service fake implement the same
explicit API. The gate deinit contract requires all fetch tasks to have
stopped, which is true on every lifecycle rollback path.

### Re-review verification

- focused user-app: source guard, study runtime/draw queue ordering, lifecycle,
  and saturated startup delivery all report zero failures;
- ASan+UBSan with both halt-on-error options: study runtime and lifecycle report
  zero failures (the first sanitizer pass found and fixed an undersized fake
  event log before final evidence);
- provisioning: `40 tests, 87 checks, 0 failures`;
- portable host: 9/9, including the explicit HTTP lifecycle contract;
- catalog-store transaction: 1/1;
- simulator: `[100%] Built target kanji_sim` with the new HTTP seam;
- ESP-IDF 5.4.3 objects: `user_app.cpp`, `study_source.c`,
  `task_lifecycle.c`, `http_port_esp.c`, and `main.cpp` compile with the real
  ESP32-S3 toolchain.

The hard frame replay remains GREEN. Updated/new relevant static frames are:

```text
study_runtime_capture_draw             64
study_runtime_accepts_draw             32
study_runtime_commit_remote            48
process_local_grade                   112
http_port_init                         32
http_port_deinit                       32
UserApp_TaskInit                       96
```

No hardware was flashed.

## Remote provenance scoped re-review

The next scoped review found that a token captured after a URL switch could
combine the old remote card's still-current publication revision with the new
source generation. A failed URL B fetch could therefore redraw URL A's card,
and a button press could capture A's remote card ID as a grade for B.

The exact production-helper sequence was added before the fix:

1. A publishes a remote card and records generation A;
2. switching to B advances the source and rejects A's queued draw;
3. B fails and captures a status token while A is still the runtime card;
4. the B status token must be rejected and remote grade capture must refuse;
5. B succeeds with byte-identical card data, creating a new B publication;
6. the B draw is accepted and the card becomes gradable.

The RED reported four assertions: the B failure token was accepted, grade
capture returned remote, the pending slot became occupied, and the subsequent
B card could not be graded. `study_runtime_accepts_draw()` now accepts a remote
publication-and-source token only when the token matches the current source
and the runtime card's recorded publication generation also matches that
source. `study_runtime_capture_grade()` applies the same provenance gate before
forming a remote request. Catalog capture remains unchanged.

The existing remote-epoch rule makes B's first valid same-card response a new
publication, updating the recorded provenance; subsequent same-generation,
same-card polls remain unchanged. Until that response, A stays available only
as stale, ungradable internal state and cannot be sent to B.

Final evidence after the fix:

- focused user-app: all four executables report zero failures;
- ASan+UBSan study-runtime test with halt-on-error: zero failures;
- provisioning: `40 tests, 87 checks, 0 failures`;
- ESP-IDF 5.4.3 `study_source.c` and dependent `user_app.cpp` objects compile;
- the hard 2 KiB frame replay exits zero; provenance helper and draw/grade
  functions remain at 32/32/96-byte static frames.

No hardware was flashed.

## Final nav-epoch and catalog-release review

### Remote epoch navigation reset

The first valid response from a replacement remote endpoint was already forced
into a new publication, even for byte-identical card data, but its navigation
reset only covered `advanced || transitioned`. With remote URL A and URL B both
producing a remote card, `transitioned` is false; A's revealed answer or open
sheet could therefore survive onto B's first card.

The production-module test first put A on a revealed description sheet at page
2 with the Easy cursor, switched to B, and committed B's byte-identical card.
RED reported four failures: revealed, sheet, page, and grade all retained A's
state. `remote_epoch_changed` is now also a `kanji_nav_reset()` condition. The
same sequence returns to the question, no sheet, page zero, and Good cursor.

### Catalog ownership rollback and retry

Lifecycle prepare creates the catalog workspace before it creates the HTTP
gate or either task. The public, idempotent `catalog_store_release()` API is now
consumed by `user_app`: prepare marks catalog ownership before init, successful
startup retains it, and every rollback after prepare releases it after tasks
have been deleted. Failed catalog init is safe because release is idempotent.

The lifecycle fake acquired a catalog allocation during prepare before cleanup
was changed. RED reported 12 assertions across prepare/HTTP, UI creation,
UI-ready, KanjiTask creation, and retry: missing releases, live stores after
failure, and a retry peak of two live stores. GREEN now proves:

- failures before prepare allocate/release zero stores;
- prepare and every later injected failure allocate once, release once, and
  leave zero live stores;
- successful startup keeps one live store and performs no release;
- failure followed by successful retry yields two lifetime allocations, one
  release, one live store, and a maximum simultaneous live count of one.

Focused user-app and ASan+UBSan study/lifecycle tests report zero failures;
provisioning remains `40 tests, 87 checks, 0 failures`. ESP-IDF 5.4.3 compiles
`study_source.c`, `task_lifecycle.c`, and `user_app.cpp` with the new public
catalog API. The hard 2 KiB frame gate passes; remote commit remains 48 bytes,
prepare/cleanup 32 bytes each, and `UserApp_TaskInit` 96 bytes.

No hardware was flashed.

Commit identity is reported in the parent handoff because a commit cannot
contain its own final hash.
