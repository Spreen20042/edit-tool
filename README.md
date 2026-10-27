# Editor Tools (Geode)

Free helpers for the Geometry Dash editor, written from scratch. Four tools live in a small
button column inside the editor (position adjustable in the mod settings):

| Button  | What it does |
|---------|--------------|
| Scatter | Brush that sprays the **selected object** with random size / rotation while you drag. |
| Polygon | Tap to place points (drag to nudge the last one), then **Done** (or Enter). Builds an outline from stretched blocks; optional rounded corners and fill. |
| Lasso   | Draw a closed outline with your finger/mouse; the inside is filled with blocks. |
| Shot    | Saves a PNG of the level **without the editor UI** (desktop key: F2, configurable). |

Tap a tool again to turn it off. If nothing is selected, tools use the basic block (ID 1).
Every action is registered in the editor's undo list. All numbers (radius, thickness, fill size,
object limit, ...) are in the mod settings.

> Works best with square-ish blocks, since lines/fills are made by scaling the picked object.

## Build

No local setup needed: push to GitHub and open **Actions → Build Mod**. When it finishes,
download the `.geode` file from **Artifacts → Build Output** and drop it in your Geode `mods` folder.

Local build: install the Geode SDK + CLI and run `geode build`.
