# Editor Tools (Geode)

Free helpers for the Geometry Dash editor, written from scratch. Four tools live in a small
button column inside the editor (position adjustable in the mod settings):

| Button  | What it does |
|---------|--------------|
| Scatter | Brush that sprays the **selected object** with random size / rotation while you drag. |
| Polygon | Tap to place points (drag to nudge the last one), then **Done** (or Enter). Builds an outline from stretched blocks; optional rounded corners and fill. |
| Lasso   | Draw a closed outline with your finger/mouse; the inside is filled with blocks. |
| Shot    | Saves a PNG of the level **without the editor UI** (desktop key: F2, configurable). |

| Art AI  | **Free AI Art Agent.** Select blocks, press it, describe the art in any language. |

The tools live in a drop-down: press **Tools** to open the list, pick one, and it closes again.

### Art AI (free)

No payments and no server of ours: it talks to free third-party AI services directly from your game.
You need a free API key (no credit card) from one of these:

| Service | Where | Notes |
|---------|-------|-------|
| **Gemini** | <https://aistudio.google.com> | Free tier on Flash models. Best for drawing. Google may use free-tier prompts for training outside the EU/UK. |
| **Groq** | <https://console.groq.com> | Fast, ~1000 requests/day free. Model `openai/gpt-oss-120b`. |
| **OpenRouter** | <https://openrouter.ai> | Models ending in `:free`, ~50 requests/day free. |
| **Cerebras / Mistral** | their sites | Also OpenAI-compatible, free tiers. |

Choose the service in the mod settings. For anything except Gemini use **OpenAI-compatible** and fill
*Service URL*, *API Key* and *Model*. Bigger models draw better.

**Modes:** *Shapes as real blocks* (default) turns the AI's drawing into stretched blocks: crisp and few
objects. *Recolor selected blocks* and *Draw new blocks in area* use a pixel grid instead.
Free services have limits and can change; your prompt is sent to the service you choose.

Tap a tool again to turn it off. If nothing is selected, tools use the basic block (ID 1).
Every action is registered in the editor's undo list. All numbers (radius, thickness, fill size,
object limit, ...) are in the mod settings.

> Works best with square-ish blocks, since lines/fills are made by scaling the picked object.

## Build

No local setup needed: push to GitHub and open **Actions → Build Mod**. When it finishes,
download the `.geode` file from **Artifacts → Build Output** and drop it in your Geode `mods` folder.

Local build: install the Geode SDK + CLI and run `geode build`.
