# WinkSpace Linux distribution

Build and launch from the source tree:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
cmake --install build
./run-winkspace
```

Choose a local install prefix at configure time if needed; do not install over a
system Inkscape installation. The `winkspace` launcher runs the editor with a
separate application instance tag. Existing Inkscape-format preferences are
currently shared; this is not profile isolation.

The default workspace has essential tools plus a More tools toggle. Advanced
contains filters, extensions, XML editing, and tiled clones. New profiles open
Layers, Fill and Stroke, Align, Export, and AI Assistant. Saved panel layouts
remain respected. Help distinguishes upstream support links from WinkSpace.

Offline upstream tutorials and examples are omitted from a fresh install by
default. To include them, configure with:

```sh
cmake -S . -B build -DWINKSPACE_BUNDLE_LEARNING_RESOURCES=ON
```

Changing this option does not remove files from a previous install. Use a fresh
staging directory when measuring or producing a reduced distribution. All
extensions remain included because they also implement import/export formats;
no dependency-based extension pruning has been performed.

The source tree retains upstream tutorials, examples, credits, licenses, and
notices. When distributing binaries, also supply the matching complete source
and build scripts under the applicable license. Preserve WINKSPACE-NOTICES.txt,
COPYING, AUTHORS, TRANSLATORS, LICENSES/, and component-specific notices. Review
all actually bundled libraries/extensions/fonts before a public release; the
notice file is not a completed third-party license audit or trademark clearance.

The current Desktop beta archive is a separate GTK3 build. This GTK4 source
checkout and its build do not update that archive. A Linux bundle needs compatible
system libraries; this does not create a universal AppImage or a Windows build.

## Generate inside selection

1. Select one closed vector shape or closed compound path (rectangles, ellipses,
   stars, or a closed Bezier outline). Groups, open paths, and boundaries with
   existing clipping/masking are not supported in this first version.
2. Open AI Assistant and choose **Generate inside selection** in the mode picker.
3. Describe the contents and Send using a configured connector. Optionally attach
   an image using **Use as reference**. Local tracing remains a Create workflow.
4. Review the clipped preview and choose Insert. The original boundary remains;
   new vector artwork is grouped above it with a locally generated clipping path.
5. Ctrl+Z undoes the insertion; the saved SVG retains editable contents and clip.

The captured boundary, its holes, and parent transforms determine the clip.
Changing the document while waiting invalidates the preview; generate again.
Restored conversation artwork does not retain a live boundary snapshot and must
be regenerated before shape-fill insertion. Output is fitted to the selected
shape's bounding rectangle; the model receives its outline and is asked to keep
important content inside it. Generated composition quality depends on the model.
