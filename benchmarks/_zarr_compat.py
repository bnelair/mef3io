"""NWB-Zarr codec configuration shared by the benchmarks.

One definition, so `mef_benchmark.py` and `compression_test.py` cannot drift
apart as zarr / hdmf-zarr evolve.
"""
from __future__ import annotations

import re


def zarr_major(version: str) -> int:
    """Major version from a zarr version string.

    Reads the leading digits only, so pre-release and dev builds ("3.0.0rc1",
    "3.0.0.dev0", "3.1.0+local") parse too. Deliberately not `packaging`: zarr
    v2 does not depend on it, so it is not guaranteed to be installed.
    """
    m = re.match(r"\s*v?(\d+)", version)
    if not m:
        raise ValueError(f"cannot read a major version from zarr {version!r}")
    return int(m.group(1))


def blosc_zstd_kwargs() -> dict:
    """Blosc/zstd level 3 with byte shuffle — the same codec either way — in the
    form the installed hdmf-zarr accepts: zarr v3 codecs under zarr >= 3
    (`compressors=`), a numcodecs codec before that (`compressor=`)."""
    import zarr

    if zarr_major(zarr.__version__) >= 3:
        from zarr.codecs import BloscCodec

        return {"compressors": BloscCodec(cname="zstd", clevel=3, shuffle="shuffle")}
    import numcodecs

    return {"compressor": numcodecs.Blosc(cname="zstd", clevel=3, shuffle=numcodecs.Blosc.SHUFFLE)}
