# pj_qdags

Header-only C++17 library for solving joins (BGP queries over binary relations) with qdags, plus the
executables and scripts to measure each strategy. Namespace `gdj::`.

## Contents

```
include/qdags/        library (header-only)
  index/              quadtree and qdag over 6 rank backends, qdag_pin (constants), wide_base_config
  join/               join engine (AND, multiJoin*), wide path, resumable
  query/              patterns, dataset/query loading, metadata builders, plans, topology classifier
  strategy/           the execution strategies (see below)
  ccq/                alternative backend: ccqQDAGS classes and AND, isolated in namespace ccq
  detail/             bit utilities
examples/             executables: build datasets and profile each strategy
scripts/              runs driven by .conf files (run_*_stats.sh); example configurations in scripts/confs/
queries/              example and benchmark queries
```

Each layer is a CMake `INTERFACE` library (`qdags::join`, `qdags::query`, `qdags::strategy`).

### Backends

A single `template<class RankBackend>` implements `se_quadtree` and `qdag`, with a thin facade per backend. The six names accepted by the executables (`--variant`) are:

| Name | Rank structure |
|---|---|
| `bv64` | `rank_bv_64_p` (the reference one) |
| `v5`, `v` | `sdsl::rank_support_v5` / `rank_support_v` |
| `sd` | `sdsl::sd_vector` |
| `rrr`, `rrr15` | `sdsl::rrr_vector` with blocks of 63 and 15 |

Each backend serializes in its own format: a dataset must be built (or converted) for the backend it will be read with.

### Strategies (`include/qdags/strategy/`)

| Strategy | File | When it applies |
|---|---|---|
| `strategy_orig` | `generic_strategies.hpp` | any query; a single join over all patterns |
| `strategy_non_lonely` (**One-Shot**) | `generic_strategies.hpp` | queries with non-lonely variables; exact prejoin |
| `strategy_weak_kernel` (+ `_adaptive`) | `generic_strategies.hpp` | unknown topology; retries with a growing bound |
| `strategy_kernel` | `generic_strategies_kernel.hpp` | queries with both lonely and non-lonely variables (others are skipped) |
| `strategy_leapfrog` (+ `_adaptive`) | `generic_strategies_leapfrog.hpp` | any query: chain of prejoins over a variable order |
| `strategy_plan` (+ `_adaptive`) | `generic_strategies_plan.hpp` | known topology: runs a plan written in a text DSL |
| `strategy_matrix_multiply` / `_wp` | `generic_strategies_mxm.hpp` | product of two binary relations (the first 2 patterns) |

Variants of the above:

- **Resumable** (`strategy_weak_kernel_resumable`, `strategy_leapfrog_resumable`, `strategy_plan_resumable`): each retry extends the previous prejoin instead of recomputing it (`ResumableJoinAuto`).
- **Raced** (`strategy_non_lonely_raced`, `strategy_weak_kernel_raced(_adaptive)`, `strategy_leapfrog_raced(_adaptive)`, `strategy_kernel_raced`, `strategy_plan_raced(_adaptive)`): run sequential and parallel at once and keep whichever finishes first (`raceStrategies`).
- One-Shot and Kernel have no adaptive or resumable variants: their prejoin is exact, so with `upper_bound = L` a single pass yields `min(L, total)` results.

Joins of 7 to 10 attributes use the wide path (`joins_wide.hpp`; `multiJoinAuto` picks the tier).

### Constants in patterns

`(?x, p, c)` and `(c, p, ?y)` are solved with `qdag_pin` (`index/qdag_pin.hpp`): a unary table over the variable that walks the relation only along the constant's path. It is a separate class; variables-only queries go through the same code as before. Every per-strategy `run_*` supports them (the code is in `examples/run_*_const.cpp`). A pattern with both a constant subject *and* object is not supported.

## Dependencies and build

- C++17 and threads.
- [sdsl-lite](https://github.com/simongog/sdsl-lite) with `libdivsufsort`. `CMakeLists.txt` looks for it in `~/include`  and `~/lib` and in the standard system paths.

```sh
cmake -S . -B build
cmake --build build -j
```

(or `./buildme.sh`, which deletes `build/` and rebuilds it). `CMakeLists.txt` sets `-O3 -march=native -DNDEBUG -g` unconditionally, without using `CMAKE_BUILD_TYPE`; if `-march=native` is changed, reconfigure.
`-DQDAGS_BUILD_EXAMPLES=OFF` skips the executables.

## Trying it with synthetic data

```sh
./build/examples/generate_demo_dataset /tmp/demo          # 5 predicates, side=24, seed=42
./build/examples/run_bgp_query /tmp/demo/relations /tmp/demo/queries.txt        # One-Shot on each query
./build/examples/run_nl /tmp/demo/relations /tmp/demo/queries.txt 0 0           # profiler for one strategy
./build/examples/run_plan /tmp/demo/relations queries/tr1_example.txt 0 0 --plan_group normal   # expected: 6
```

`generate_demo_dataset` writes the indexes to `<dir>/relations/` and the queries to `<dir>/queries.txt`, in separate folders on purpose: `resource_manager::load_directory` tries to read **every** file in the directory it is given as a `se_quadtree`. It only generates `bv64` data.

## Datasets

The format is the `resource_manager::load_directory` convention: for each predicate `<pid>`, a file `<pid>` (S-O relation) and the unary lists `<pid>s` (subjects) and `<pid>o` (objects).

`examples/build_dataset` builds or converts that format in any of the six backends:

```sh
# from "s p o" triples (ids already enumerated), one per line
./build/examples/build_dataset triples triples.txt out/ --variant all --grid_side 296008193
./build/examples/build_dataset triples triples.txt out/ --variant bv64,sd --only_so

# from a folder with one "s o" relation per file, named <prefix><id>
./build/examples/build_dataset pairs data/ out/ --variant bv64 --grid_side 52000000

# from an already built dataset
./build/examples/build_dataset convert out/bv64 out_conv/ --from bv64 --variant v5,rrr15
```

- With a single backend the files go straight into the output folder; with several, into `<output>/<backend>/`.
- `--grid_side` must be greater than every id.
- `pairs` reads `<prefix><id>` (`--prefix`, default `prop-direct-P`). Files with any other name get new ids
  (`max_id+1`, ...) with `--others assign` (default) or are skipped with `--others skip`; the mapping is written to `<output>.predicados_extra.tsv` (one backend) or `<output>/predicados_extra.tsv` (several), never inside a dataset folder.
<!-- - `convert` uses `k^height` of each tree as the side, which reproduces the original height. -->
- At the end it prints time, memory and disk space per backend.

`scripts/build_dataset_variants.sh <file.conf>` runs it from a `.conf` (`MODE` = `triples`|`pairs`|`convert`, `INPUT`, `OUT`, `VARIANTS`, `GRID_SIDE`, `ONLY_SO`, `FROM`, `PREFIX`, `FORCE`, `BIN`, `MEMLIMIT_KB`); example in `scripts/confs/wiki/build_dataset_wiki.conf`. Each backend ends up in `<OUT>/<backend>/`, with a marker `<OUT>/build_<backend>.done` (outside the dataset folder) that makes already built ones be skipped; `FORCE=true` rebuilds them. The paths in the `.conf` files are placeholders: adjust them.

`examples/classify_queries <queries> <output>` classifies the topology of each query (useful to split a mixed file, since several `run_*` expect one topology per file).

## Profilers (`examples/run_*`)

One executable per strategy, built to **measure**: they run the strategy on every query of a file and report cardinality and time per query, plus total, mean and median. All have the form

```
run_<x> <dir_dataset> <queries_file> <upper_bound> <parallel_level[,level...]> [options]
```

| Executable | Strategy |
|---|---|
| `run_orig` | Orig |
| `run_nl`, `run_nl_race` | One-Shot, and its raced version |
| `run_wk`, `run_wk_mixed`, `run_wk_resumable`, `run_wk_race` | WeakKernel: generic + adaptive, with independent output backend, resumable, raced |
| `run_leapfrog`, `run_leapfrog_resumable`, `run_leapfrog_race` | Leapfrog |
| `run_kernel`, `run_kernel_race` | Kernel |
| `run_plan`, `run_plan_resumable`, `run_plan_race` | Plan |
| `run_plan_hybrid` | Plan and, for queries without a plan, other strategies |
| `run_mxm` | matrix product (`--mode basic\|wp\|both`) |
| `run_ccq` | the strategies on the ccq backend (see below) |
| `run_bgp_query` | end-to-end demo with One-Shot |
| `bfs_prelude_bench` | measures only the prelude (enumeration of cut-level roots), DFS vs BFS |

Common options:

- `--variant bv64|v5|v|sd|rrr|rrr15`: dataset backend (default `bv64`; must match how it was built).
  `--variant_out`: backend of the final result, if different (`run_nl*`, `run_wk_mixed`, `run_wk_resumable`,
  `run_wk_race`, `run_leapfrog*`, `run_kernel*`; `run_wk_mixed` uses `--variant_in`/`--variant_out`; `run_wk`,
  `run_orig` and `run_plan*` do not have it).
- `--grid_side <n>` (default 52000000), `--time_out <s>` per query, `--reps <n>`.
- `--results_file`/`--times_file`: one line per non-skipped query (`query_id;generic;adaptive`; cardinality or milliseconds). They let you check that the result does not change across levels, backends or runs.
- `--parallel_phases first_only|last_only|all`: which phases of the plan use the parallel cut level.
- Strategies with retries (WeakKernel, Leapfrog, Plan; the resumable ones have no `--mode`):
  `--mode generic|adaptive|both`, `--coef_bound`, `--init_coef`, `--growth`, `--max_retries` and
  `--growth_mode fixed|proportional` (with `proportional` the coefficient grows by `clamp(L / r × 1.5, growth, 64)`, where `r` is the number of results of the attempt that did not reach `L`).
- Leapfrog: `--lf_order proj_size|degree`, `--lf_depth full|non_lonely`, `--lf_max_atts <n>` (0 = automatic).
- Plan: `--plan_group normal|best|wk_k|leapfrog` (per-topology plan from the catalog) **or** `--plan_file <path> --plan_id "<key>"` (one fixed plan for all queries); the topology of each query is classified.
- `--mxm` (`run_nl`, `run_wk`, `run_leapfrog`, `run_kernel`): the query is a matrix chain and
  `π_{endpoints}(result)` is measured.
- Wide path: `--wide_base automatic|tables|scatter`, `--wide_min_d`, `--wide_min_dim` (see below).

### Several levels in one execution

`<parallel_level>` accepts a list (`0,2,4,6`); the dataset is loaded **once**. `--reps N` repeats each level. `{L}` and `{R}` in `--results_file`/`--times_file` are replaced by the level and the repetition (required if there is more than one pass). Before each pass `LEVEL;<level>;<rep>` is printed, and afterwards the `RESULT;...` line:

```sh
./build/examples/run_wk <dataset> <queries> 1000 0,2,4,6 --reps 2 \
    --results_file out/results_p{L}_r{R}.txt --times_file out/times_p{L}_r{R}.txt
```

### Plan + fallback (`run_plan_hybrid`)

Per query: if the catalog gives a plan for its topology and the builder accepts it, only the plan runs; otherwise **all** the strategies in `--fallback` run (`one_shot`, `wk`, `leapfrog`, `kernel`, `orig`; `nl` = alias of `one_shot`). With `--plan_file/--plan_id` the fixed plan applies only to the topologies in its file (`--plan_topologies` changes that). `--mode` is a list of variants (`generic`, `adaptive`, `resumable`; `both`, `all`); each strategy runs the ones it has. With `UPPER_BOUND=0` and `COEF_BOUND=0` all must give the same count: otherwise `MISMATCH` is printed and counted in `n_mismatch`. Queries with constants always go to the fallback.

```sh
./build/examples/run_plan_hybrid <dataset> <queries> 1000 0,2,4,6 --plan_group normal \
    --fallback wk,leapfrog,one_shot,kernel --results_file out/results_p{L}_r{R}.txt
```

### Plans by topology

`include/qdags/query/topology_plan_selection.hpp` resolves `(topology, group)` → plan from two CSV files
(`query/plans/topology_plan_files.csv` and `topology_plan_selection.csv`) and the seven DSL files in
`query/plans/*.txt`; they can be edited without recompiling. `default_plans_dir()` locates that folder with `__FILE__`; `QDAGS_PLANS_DIR` relocates it. `plan_parser.hpp` defines the DSL, `classify_query.hpp` the 18 topologies, and `topology_mapping.hpp` / `plan_builder.hpp` translate them into concrete quadtrees. The DSL does not allow comments. `scripts/confs/plans/example_triangle.plan.txt` + `queries/tr1_example.txt` are a runnable `--plan_file`/`--plan_id` pair (`--plan_id "{a}, {a,b}"`).

## Run scripts (`scripts/`)

One script per family: `run_one_shot_stats.sh`, `run_wk_stats.sh`, `run_leapfrog_stats.sh`, `run_kernel_stats.sh`, `run_orig_stats.sh`, `run_mxm_stats.sh`, `run_plan_stats.sh`, `run_plan_hybrid_stats.sh`, `run_ccq_stats.sh`, and the `*_resumable_stats.sh` and `*_race_stats.sh` variants. All take a `.conf` (bash-sourceable; commented template in `scripts/confs/example.conf.example`, which lists each script's variables):

```sh
cp scripts/confs/example.conf.example scripts/confs/my_run.conf    # edit DATASET, QUERIES, OUT
./scripts/run_wk_stats.sh scripts/confs/my_run.conf
```

- They run the executable once with all the `PARALLEL_LEVELS` (`scripts/lib_levels.sh`) and build a table +
  `summary.csv` with total/mean time per level and the speedup against the first.
- In `OUT` they leave `results_p<N>_r<R>.txt`, `times_p<N>_r<R>.txt`, `summary.csv` and `run.conf` (a copy of the `.conf`; `summary.csv` and `run.conf` are overwritten on every run).
- If the process dies at a level (e.g. rc=137 from memory), that level is recorded as `FAILED_rc<N>` and the process is relaunched with the remaining ones. `TIME_OUT` is per query; `TIMEOUT` limits the whole execution (the level in progress becomes `FAILED_rc124` and the rest `NOT_RUN`).
- The paths in the example `.conf` files (`DATASET`, `QUERIES`, `OUT`) are placeholders: adjust them.

### qdag scatter path (`wide_base_config`)

A `qdag` extends each node of its base (`d` attributes) to the join space (`dim` attributes), in two ways:
**tables** of 16 bits (`tab_extend_*`, one `rank_vector` per node) or **scatter** (one mask per base child and lazy rank, computed only for the children that survive the AND). A base with `d = 9` inside a join of `dim = 10` exists only through scatter.

The library does not read environment variables for this: a `gdj::wide_base_config` (`index/wide_base_config.hpp`) is passed to the metadata builders (`build_nl_metadata(query, res, wcfg)`, `build_wk_metadata`,`build_leapfrog_metadata`, `build_qdags_from_query`, the `_pin` ones; in plans, `query_metadata_plan::wide`). Without it the value is `automatic`.

| `mode` | What it does |
|---|---|
| `automatic` | scatter for `d >= 4` in joins of 7 to 10 attributes; tables otherwise |
| `tables` | always tables, except `d = 9` in `dim = 10` |
| `scatter` | scatter for `d >= min_d` and `dim >= min_dim` (defaults 4 and 7) |

The resumable engines (`wk_resumable`, `plan_resumable`, `leapfrog_resumable`) use `materialize_node_256`: with `automatic` they build the `qdag`s with tables; with `scatter`, or with `d = 9` in `dim = 10`, they fail with a clear message. WeakKernel does not change (its bases have 2 attributes); Kernel, One-Shot and Leapfrog do.

### Parallel join: root cap

With `parallel_level > 0` the engine enumerates the cut-level subtree roots serially, and their number grows by `2^d` per level. The enumeration has a cap (default 65536 roots; `QDAGS_JOIN_MAX_ROOTS` or `set_join_max_roots()` change it, 0 = no cap): if exceeded, it retries one level higher with the same result. `partialAND*` and the sequential phase read the timeout flag.
