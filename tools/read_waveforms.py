#!/usr/bin/env python3
"""Read one icxe-daq wf_*.bin file.

Events are concatenated with no file-level header. Each event is little-endian:

    4s  magic b'ICX1'
    u4  event counter
    u4  trigger time tag
    u4  channel mask
    u4  sample count
    i2  samples[n_channels, n_samples]

The channel axis is the enabled channels in ascending channel number, which is
the set bits of the channel mask. metadata.json records that mask and the
enabled_channels list.
"""

from __future__ import annotations

import argparse
import json
import sys
from typing import Dict, Tuple

import numpy as np

MAGIC = b"ICX1"
HEADER_BYTES = 20


def enabled_channels(mask: int):
    return [i for i in range(16) if mask & (1 << i)]


def load_waveforms(path: str) -> Tuple[np.ndarray, Dict[str, np.ndarray]]:
    """Memmap one wf_*.bin file.

    Returns
    -------
    waveforms : ndarray, shape (n_events, n_channels, n_samples), dtype int16
        A read-only view of the memmap. The channel axis is enabled channels
        in ascending order.
    headers : dict of ndarrays
        event_counter, trigger_time_tag, channel_mask, sample_count.
    """
    with open(path, "rb") as handle:
        prefix = handle.read(HEADER_BYTES)
    if len(prefix) < HEADER_BYTES:
        raise ValueError("file is shorter than one event header: %s" % path)
    if prefix[:4] != MAGIC:
        raise ValueError("bad magic in %s (expected ICX1)" % path)
    sample_count = int(np.frombuffer(prefix[16:20], dtype="<u4")[0])
    mask = int(np.frombuffer(prefix[12:16], dtype="<u4")[0])
    n_channels = len(enabled_channels(mask))
    event_size = HEADER_BYTES + n_channels * sample_count * 2
    file_size = _file_size(path)
    if event_size <= 0 or file_size % event_size != 0:
        raise ValueError(
            "file size %d is not a whole number of %d-byte events: %s"
            % (file_size, event_size, path)
        )
    dtype = np.dtype(
        [
            ("magic", "S4"),
            ("event_counter", "<u4"),
            ("trigger_time_tag", "<u4"),
            ("channel_mask", "<u4"),
            ("sample_count", "<u4"),
            ("samples", "<i2", (n_channels, sample_count)),
        ],
        align=False,
    )
    if dtype.itemsize != event_size:
        raise RuntimeError("event dtype size %d != %d" % (dtype.itemsize, event_size))
    mapped = np.memmap(path, dtype=dtype, mode="r")
    if not np.all(mapped["magic"] == MAGIC):
        raise ValueError("an event in %s does not start with ICX1" % path)
    if not np.all(mapped["sample_count"] == sample_count):
        raise ValueError("mixed sample counts in %s" % path)
    if not np.all(mapped["channel_mask"] == mask):
        raise ValueError("mixed channel masks in %s" % path)
    headers = {
        "event_counter": mapped["event_counter"],
        "trigger_time_tag": mapped["trigger_time_tag"],
        "channel_mask": mapped["channel_mask"],
        "sample_count": mapped["sample_count"],
    }
    return mapped["samples"], headers


def _file_size(path: str) -> int:
    with open(path, "rb") as handle:
        handle.seek(0, 2)
        return handle.tell()


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Read an icxe-daq wf_*.bin file")
    parser.add_argument("file", help="wf_XXXXX.bin")
    parser.add_argument("--metadata", help="metadata.json written beside the waveforms")
    parser.add_argument(
        "--head",
        type=int,
        default=0,
        help="print this many samples from event 0, channel axis 0",
    )
    args = parser.parse_args(argv)
    waveforms, headers = load_waveforms(args.file)
    mask = int(headers["channel_mask"][0]) if len(headers["channel_mask"]) else 0
    print(
        "events=%d channels=%d samples=%d mask=%s"
        % (waveforms.shape[0], waveforms.shape[1], waveforms.shape[2], hex(mask))
    )
    print("channel_axis=%s" % enabled_channels(mask))
    print("channel_axis_note=enabled channels in ascending order")
    if args.metadata:
        with open(args.metadata, "r", encoding="utf-8") as handle:
            meta = json.load(handle)
        print("metadata_enabled_channels=%s" % meta.get("enabled_channels"))
        print("metadata_channel_mask=%s" % meta.get("channel_mask"))
    if args.head > 0 and waveforms.shape[0] > 0 and waveforms.shape[1] > 0:
        n = min(args.head, waveforms.shape[2])
        print("event0_ch0=%s" % waveforms[0, 0, :n].tolist())
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        sys.stderr.write("read_waveforms: %s\n" % exc)
        sys.exit(1)
