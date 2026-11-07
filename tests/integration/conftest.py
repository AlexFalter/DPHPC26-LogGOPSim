"""Paths to the built programs and test data, as fixtures.

By default the programs come from the build directory next to the tests, so
`pytest tests/integration` runs from the repository root after a build; ctest passes the
paths of its own build. Missing core programs fail. Optional ATLAHS inputs may skip unless CTest requires them.
"""

from pathlib import Path
import shutil

import pytest

from helpers import Converter, existing

REPOSITORY = Path(__file__).resolve().parents[2]
DATA = REPOSITORY / "tests" / "data"


def pytest_addoption(parser):
    group = parser.getgroup("loggopsim")
    group.addoption("--build-dir", type=Path, default=REPOSITORY / "build",
                    help="CMake build directory with txt2bin, LogGOPSim and atlahs-htsim")
    group.addoption("--atlahs-dir", type=Path, default=REPOSITORY.parent / "atlahs",
                    help="Checkout of the ATLAHS fork")
    group.addoption("--require-orc", action="store_true", help="Require the configured LLVM ORC runner")
    group.addoption("--require-atlahs", action="store_true",
                    help="Fail if configured ATLAHS inputs or executables are missing")
    group.addoption("--mpi-exec", type=Path, default=None,
                    help="MPI launcher for the liballprof integration test")
    group.addoption("--mpi-numproc-flag", default="-n")
    group.addoption("--mpi-preflag", action="append", default=[])
    group.addoption("--mpi-postflag", action="append", default=[])
    group.addoption("--recorded-traces", type=Path, default=None,
                    help="Nsight SQLite exports for the recorded-trace comparison")
    group.addoption("--recorded-nodes", type=int, default=16,
                    help="GPU count of the recorded traces")


def pytest_collection_modifyitems(config, items):
    # Old backend binaries may remain after BUILD_ATLAHS is switched off. Their
    # existence alone must not opt the optional integration back into the run.
    enabled = config.getoption("--require-atlahs")
    cache = config.getoption("--build-dir").resolve() / "CMakeCache.txt"
    if cache.is_file():
        enabled |= any(line.startswith("BUILD_ATLAHS:BOOL=") and
                       line.split("=", 1)[1].strip().upper() in {"ON", "TRUE", "YES", "Y", "1"}
                       for line in cache.read_text().splitlines())
    if not enabled:
        disabled = pytest.mark.skip(reason="ATLAHS tests disabled; configure BUILD_ATLAHS=ON to enable them")
        for item in items:
            if item.get_closest_marker("atlahs"):
                item.add_marker(disabled)


@pytest.fixture(scope="session")
def build_dir(request):
    return request.config.getoption("--build-dir").resolve()


@pytest.fixture(scope="session")
def plain_fields():
    """No custom fields, for plain GOAL traces."""
    return DATA / "empty_custom_fields.toml"


@pytest.fixture(scope="session")
def custom_fields():
    """gpu_id and priority."""
    return DATA / "converter_custom_fields.toml"


@pytest.fixture(scope="session")
def atlahs_fields():
    """The eight ATLAHS message-identity fields."""
    return DATA / "atlahs_custom_fields.toml"


@pytest.fixture(scope="session")
def reference_case():
    """binomialtreebcast_8 with its saved legacy binary and host times."""
    return DATA / "reference" / "binomialtreebcast_8"


@pytest.fixture(scope="session")
def converter(build_dir, plain_fields):
    return Converter(existing(build_dir / "txt2bin", "build the txt2bin target"), plain_fields)


@pytest.fixture(scope="session")
def atlahs_converter(converter, atlahs_fields):
    return Converter(converter.executable, atlahs_fields)


@pytest.fixture(scope="session")
def loggopsim(build_dir):
    return existing(build_dir / "LogGOPSim", "build the LogGOPSim target")


@pytest.fixture(scope="session")
def atlahs_dir(request):
    directory = request.config.getoption("--atlahs-dir").resolve()
    return existing(directory / "goal_gen", "pass --atlahs-dir with a checkout of the ATLAHS fork",
                    required=request.config.getoption("--require-atlahs")).parent


@pytest.fixture(scope="session")
def atlahs_generator(atlahs_dir):
    return atlahs_dir / "goal_gen" / "ai" / "nccl_goal_generator"


@pytest.fixture(scope="session")
def atlahs_upstream_revision():
    """The upstream ATLAHS commit the fork branched from."""
    return (REPOSITORY / "src/LogGOPSim/simulators/atlahs/UPSTREAM_REVISION").read_text().split()[0]


@pytest.fixture(scope="session")
def htsim_dir(build_dir, request):
    directory = existing(build_dir / "atlahs-htsim", "build the atlahs_htsim target (BUILD_ATLAHS=ON)",
                         required=request.config.getoption("--require-atlahs"))
    existing(directory / "datacenter" / "htsim_ndp", "build the atlahs_htsim target")
    return directory


@pytest.fixture(scope="session")
def htsim(htsim_dir):
    return existing(htsim_dir / "datacenter" / "htsim_ndp", "build the atlahs_htsim target")


@pytest.fixture(scope="session")
def schedgen(build_dir):
    return existing(build_dir / "schedgen", "build the schedgen target")


@pytest.fixture(scope="session")
def mpi_exec(request):
    configured = request.config.getoption("--mpi-exec")
    launcher = configured or shutil.which("mpiexec")
    if launcher is None:
        pytest.skip("MPI is not installed")
    return existing(Path(launcher), "configure --mpi-exec with an MPI launcher")
