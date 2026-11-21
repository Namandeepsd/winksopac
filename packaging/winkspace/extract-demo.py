#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Extract the README's real screenshots from the creator's original recording.

Requires python3-gi, gir1.2-gstreamer-1.0, GStreamer H.264 support, and Pillow.
No model calls, generated artwork, or changes to the recorded interface.
"""

import argparse
from pathlib import Path

import gi
from PIL import Image, ImageDraw, ImageFont

gi.require_version("Gst", "1.0")
from gi.repository import Gst


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("video", type=Path)
    parser.add_argument("--output", type=Path, default=Path("doc/media"))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    Gst.init(None)
    pipeline = Gst.parse_launch(
        f'uridecodebin uri="{args.video.resolve().as_uri()}" ! videoconvert ! '
        "video/x-raw,format=RGB ! appsink name=frames sync=false "
        "max-buffers=1 drop=true"
    )
    sink = pipeline.get_by_name("frames")
    pipeline.set_state(Gst.State.PAUSED)
    pipeline.get_state(10 * Gst.SECOND)

    def frame(seconds):
        if not pipeline.seek_simple(
            Gst.Format.TIME, Gst.SeekFlags.FLUSH | Gst.SeekFlags.ACCURATE,
            seconds * Gst.SECOND,
        ):
            raise RuntimeError(f"Cannot seek to {seconds} seconds")
        pipeline.get_state(10 * Gst.SECOND)
        sample = sink.emit("try-pull-preroll", 10 * Gst.SECOND)
        if sample is None:
            raise RuntimeError(f"Cannot decode the frame at {seconds} seconds")
        caps = sample.get_caps().get_structure(0)
        width, height = caps.get_value("width"), caps.get_value("height")
        if (width, height) != (1920, 1080):
            raise RuntimeError("These crops require the original 1920 × 1080 recording")
        buffer = sample.get_buffer()
        ok, mapping = buffer.map(Gst.MapFlags.READ)
        if not ok:
            raise RuntimeError("Cannot read the video frame")
        try:
            return Image.frombytes("RGB", (width, height), bytes(mapping.data))
        finally:
            buffer.unmap(mapping)

    try:
        images = {second: frame(second) for second in (180, 200, 350, 380)}
    finally:
        pipeline.set_state(Gst.State.NULL)

    # Remove only the operating system's top bar and dock. Preserve the app UI.
    app_crop = (65, 38, 1920, 1080)
    for second, name in (
        (180, "generation-preview.jpg"),
        (350, "winkspace-editor.jpg"),
        (380, "editable-paths.jpg"),
    ):
        images[second].crop(app_crop).save(
            args.output / name, quality=93, optimize=True, progressive=True,
        )

    # An explicitly labelled comparison of two actual frames, with no retouching.
    comparison = Image.new("RGB", (1280, 780), "#11121f")
    draw = ImageDraw.Draw(comparison)
    font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
    title = ImageFont.truetype(font_path, 26)
    small = ImageFont.truetype(font_path, 17)
    for x, second, label, caption in (
        (24, 200, "01 / Generated from a reference", "3:20 in the original recording"),
        (652, 350, "02 / Edited with a prompt", "5:50 in the original recording"),
    ):
        crop = images[second].crop((300, 260, 1025, 1045))
        crop = crop.resize((604, 654), Image.Resampling.LANCZOS)
        comparison.paste(crop, (x, 70))
        draw.text((x, 22), label, font=title, fill="#eeeaf9")
        draw.text((x, 745), caption, font=small, fill="#aba7c0")
    comparison.save(args.output / "before-after.jpg", quality=93, optimize=True)
    print(f"Saved four images to {args.output}")


if __name__ == "__main__":
    main()
