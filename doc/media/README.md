# Demo media

These images are extracted from the creator's recording,
`Screencast From 2026-11-14 01-17-26.mp4` (1920 × 1080, 6:29).
They show the application running; the interface and artwork are not mockups.

| File | Recording position | What it shows |
| --- | --- | --- |
| [generation-preview.jpg](generation-preview.jpg) | 3:00 | Reference attachment, generation prompt, SVG preview, and Insert |
| [winkspace-editor.jpg](winkspace-editor.jpg) | 5:50 | The portrait after the instruction to add sunglasses |
| [editable-paths.jpg](editable-paths.jpg) | 6:20 | The sunglasses group and individual paths in the object tree |
| [before-after.jpg](before-after.jpg) | 3:20 and 5:50 | Labelled canvas crops before and after the edit |

The screenshots remove the operating system dock and top bar. The comparison
adds captions around two canvas crops. Neither changes the recorded artwork.
The decorative [README banner](winkspace-banner.svg) uses the existing
WinkSpace application mark.

The original MP4 is available as `winkspace-demo.mp4` in the
[Linux beta release](https://github.com/Namandeepsd/winksopac/releases/tag/v0.1.0-beta.1).
The recording was made before the release packaging; some interface details can
differ from the current build.

To recreate the screenshots from the original recording:

```sh
python3 packaging/winkspace/extract-demo.py /path/to/recording.mp4
```
