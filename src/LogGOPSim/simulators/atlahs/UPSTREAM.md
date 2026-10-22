# ATLAHS source and upstream pin

`upstream/` is the actual ATLAHS Git checkout, not generated output. Its local
`packed-trace-integration` branch starts at:

- Repository: https://github.com/spcl/atlahs.git
- Base commit: `fb51a99f908e550318056ebb3e084f3d2fff55bd`
- Machine-readable pin: [UPSTREAM_REVISION](UPSTREAM_REVISION)

The integration changes are maintained as one commit on that base in
[mgman03/atlahs](https://github.com/mgman03/atlahs), on branch
`packed-trace-integration`. The parent repository pins the exact integration
commit as a submodule; `UPSTREAM_REVISION` separately records the original
upstream base used by the reference tests.

Initialize this checkout from the LogGOPSim repository root with:

```sh
git submodule update --init src/LogGOPSim/simulators/atlahs/upstream
```

Our changes are normal source edits, readable directly under `upstream/`.
Original upstream history is retained for provenance and independent reference
tests; intermediate local development commits are not included.

## Read the implementation

| Responsibility | File under `upstream/` |
|---|---|
| Generator CLI, `--explicit-fields` | `goal_gen/ai/nccl_goal_generator/get_traced_events.py` |
| Emit the eight named attributes on every final operation | `goal_gen/ai/nccl_goal_generator/generator_modules/data_dependency_modules/inter_node_dependency.py` |
| Format explicit components or preserve legacy tags | `goal_gen/ai/nccl_goal_generator/generator_modules/data_dependency_modules/message_fields.py` |
| Recognize packed input and use `AtlahsAdapter` | `sim/htsim-backend/sim/lgs/Parser.hpp` |
| Message tuple and shared queue matcher | `sim/htsim-backend/sim/lgs/MessageIdentity.hpp`, `sim/htsim-backend/sim/lgs/MessageMatching.hpp` |
| Local/peer GPU, stream and message identity carried by scheduler/network records | `sim/htsim-backend/sim/lgs/LogGOPSim.hpp`, `sim/htsim-backend/sim/lgs/logsim.h` |
| GPU clocks, callbacks and endpoint mapping | `sim/htsim-backend/sim/datacenter/logsim-interface.cpp`, `sim/htsim-backend/sim/datacenter/atlahs_htsim_api.cpp` |

The adapter and its node type remain in `AtlahsAdapter.hpp/.cpp`, outside
upstream source. Original licenses and upstream documentation are retained.
Unrelated upstream submodules are not initialized: the NDP and NCCL-generator
checks do not need the external HPC/AI application repositories.

## Review changes with Git

From the workspace root:

```sh
git -C src/LogGOPSim/simulators/atlahs/upstream status --short
git -C src/LogGOPSim/simulators/atlahs/upstream diff fb51a99f908e550318056ebb3e084f3d2fff55bd -- goal_gen/ai/nccl_goal_generator sim/htsim-backend/sim/lgs 'sim/htsim-backend/sim/datacenter/*.cpp'
```

Open `upstream/` as a repository in the editor to inspect individual source diffs.
The upstream base pin remains unchanged when integration edits are committed on the
fork branch. Tests use `git archive` at the exact pin to obtain the original
generator for comparison; they do not compare the edited generator with itself.
Do not replace the pin with a moving branch name or advance it without rerunning
the integration and original NDP reference tests.

## Source versus build output

`integration/build.py` copies maintained backend source into the build tree
because upstream Makefiles write object files beside source. It applies no
patches and never edits the checkout. Run the Python generator directly from its
source directory; Python may create ignored bytecode caches there. C++ objects,
executables and the backend's disposable working copy live in
`build/atlahs-htsim`. The older generated `build/atlahs-generator` copy
and patch/preparation files are no longer used.
