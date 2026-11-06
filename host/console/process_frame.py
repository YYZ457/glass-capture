"""Bounded P5 worker. Numerical modules remain unchanged; no hardware access."""
import argparse
from dataclasses import asdict
import hashlib
import json
from pathlib import Path
import sys
import time

import numpy as np

MAX_PIXELS = 8 * 1024 * 1024
MAX_JSONL_BYTES = 4 * 1024 * 1024


class CandidateLimitExceeded(RuntimeError):
    pass


def write_components(file, items, byte_count):
    """Fail before a write exceeds the cap; never emit a truncated success."""
    count = 0
    for component in items:
        raw = (json.dumps(asdict(component), ensure_ascii=False) + "\n").encode("utf-8")
        if byte_count + len(raw) > MAX_JSONL_BYTES:
            raise CandidateLimitExceeded("候选文件超过4MiB上限；请减小图像尺寸或提高阈值。结果未完成。")
        file.write(raw)
        byte_count += len(raw)
        count += 1
    return count, byte_count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--module-root", required=True, type=Path)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--native-library", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--radius", type=int, choices=range(8), default=7)
    parser.add_argument("--threshold", type=int, choices=range(1, 256), default=25)
    parser.add_argument("--polarity", choices=("both", "bright", "dark"), default="both")
    args = parser.parse_args()
    sys.path.insert(0, str(args.module_root / "python"))
    from pgm_stream import PgmStream
    from reference import decode_words
    from stream_components import StreamComponents
    from stream_pipeline import process_chunks
    from tile_runtime import NativeEngine

    with PgmStream(args.input) as source:
        width, height = source.width, source.height
        if width * height > MAX_PIXELS:
            raise ValueError("图像超过8Mi像素上限。")
        engine = NativeEngine(args.native_library)
        cc = StreamComponents(width, frame_id=0, min_area=2)
        args.output_dir.mkdir(parents=True, exist_ok=False)
        state = dict(status="running", source=str(args.input), width=width, height=height,
                     radius=args.radius, threshold=args.threshold, polarity=args.polarity,
                     implementation="native C++ shared with FPGA IP", board_tested=False, accuracy_evaluated=False)
        state_path = args.output_dir / "run.json"
        state_path.write_text(json.dumps(state, ensure_ascii=False, indent=2), encoding="utf-8")
        started, digest = time.perf_counter(), hashlib.sha256()
        count = highwater = stripes = candidate_bytes = 0
        try:
            with (args.output_dir / "mask.pgm.partial").open("xb") as mask_file, \
                 (args.output_dir / "corrected.pgm.partial").open("xb") as corrected_file, \
                 (args.output_dir / "components.jsonl.partial").open("xb") as candidates:
                header = f"P5\n{width} {height}\n255\n".encode("ascii")
                mask_file.write(header)
                corrected_file.write(header)
                for stripe in process_chunks(source.chunks(), width, height, engine, radius=args.radius,
                        polarity=("both", "bright", "dark").index(args.polarity), threshold=args.threshold):
                    decoded = decode_words(stripe.words)
                    digest.update(stripe.words.astype("<u4", copy=False).tobytes())
                    mask_file.write((decoded["mask"].astype(np.uint8) * 255).tobytes())
                    corrected_file.write(decoded["corrected"].tobytes())
                    for signed, mask in zip(decoded["signed"], decoded["mask"]):
                        added, candidate_bytes = write_components(candidates, cc.consume_row(signed, mask), candidate_bytes)
                        count += added
                    highwater = max(highwater, stripe.input_cache_high_water_bytes)
                    stripes += 1
                added, candidate_bytes = write_components(candidates, cc.finish(), candidate_bytes)
                count += added
            for name in ("mask.pgm", "corrected.pgm", "components.jsonl"):
                (args.output_dir / (name + ".partial")).rename(args.output_dir / name)
            state.update(status="complete", components=count, stripes=stripes,
                         retained_input_cache_peak_bytes=highwater, output_words_sha256=digest.hexdigest(),
                         native_wall_seconds=time.perf_counter() - started)
        except BaseException as error:
            state.update(status="failed", error=str(error))
            raise
        finally:
            state_path.write_text(json.dumps(state, ensure_ascii=False, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
