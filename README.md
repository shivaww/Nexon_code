# nexon_code

**A single-file C++17 binary that turns any chatbot into a coding agent — no API keys, no subscriptions, no setup. You paste JSON; the model drives your project.**

```
┌──────────────────────────────────────────────────────────┐
│ nexon_code - local multi-tool bridge for LLMs            │
│ no API keys - no subscriptions                           │
│ paste is all you need                                    │
└──────────────────────────────────────────────────────────┘
```

nexon_code runs on your machine, reads JSON commands from stdin, and executes them against your project directory — read files, patch code, run builds, manage git. Your LLM (any chat: ChatGPT free tier, Claude, Gemini, DeepSeek, a local model — anything that can emit JSON) is the brain; nexon_code is the hands. You relay between them by copy-paste. That's the whole system.

It was built and debugged on Android (Termux), which means it compiles and runs anywhere: phones, Raspberry Pis, laptops, servers.

## Why

Every agent framework today assumes you can pay: an API key, a subscription, tokens per request. nexon_code assumes only that you have *a chat window*. If you can copy and paste, you have a coding agent — with the same tools the paid products use: file reads and edits, patching, building, git, multi-file search.

- **Zero cost, zero keys** — works with free chatbot tiers and local models
- **Zero telemetry** — nothing leaves your machine except what you choose to paste into a chat
- **One file, one dependency** — a single ~4,000-line C++17 source; `clang++`/`g++` and you're done
- **Built for phones** — large-paste handling, raw-mode input, echo suppression, and `--plain` output for chat apps that mangle ANSI

## Install

One copy-paste. On Termux it installs a C++ compiler if needed, downloads the source, compiles, and puts `nexon_code` on your PATH:

```bash
curl -fsSL https://raw.githubusercontent.com/shivaww/Nexon_code/main/install.sh | bash
```

Prefer building manually? No cmake, no vcpkg, no dependencies — one file:

```bash
curl -fsSL https://raw.githubusercontent.com/shivaww/Nexon_code/main/nexon_code.cpp -o nexon_code.cpp
clang++ -std=c++17 -O2 -o nexon_code nexon_code.cpp   # or g++
```

Or skip the terminal entirely for downloading: hit the green **Code** button on the [repo page](https://github.com/shivaww/Nexon_code) → **Download ZIP**. Unzip it and you have everything — `nexon_code.cpp`, `prompt.md`, this README, the installer. This is also the easiest way to get `prompt.md` as a clean file: it comes down inside the zip, ready to attach to your chat without any copy-paste.

```bash
cd Nexon_code-main && clang++ -std=c++17 -O2 -o nexon_code nexon_code.cpp   # build from the zip
```

## Quickstart

**Step 1 — give your chat the prompt.** Download [`prompt.md`](https://raw.githubusercontent.com/shivaww/Nexon_code/main/prompt.md) and attach it to your AI chat as a file (system prompt / custom instructions / attachment — wherever your chat accepts it). **Download it as a file; do not copy-paste the text** — 300 lines pasted through a terminal clipboard can arrive mangled with broken line endings, and a slightly corrupted prompt makes the model emit malformed commands that waste your round-trips. A direct file download gives your chat exactly what we wrote. On Termux: `curl -fsSL https://raw.githubusercontent.com/shivaww/Nexon_code/main/prompt.md -o prompt.md` downloads it intact.

**Step 2 — launch on your project.**

```bash
nexon_code /path/to/project        # interactive (up to 3 project folders)
nexon_code /path/to/project < req.json   # piped (recommended for big payloads)
```

Paste a JSON command into the session; it executes the moment its braces balance:

```json
{
  "t": "read",
  "a": {
    "r": [
      {
        "f": "main.cpp",
        "s": 1,
        "e": 80
      }
    ]
  }
}
```

The result comes back as JSON you copy to your model. Slash commands inside a session: `/help` `/clear` `/roots` `/plain` `/stop` `/exit`; `Ctrl+L` redraws the header.

> **The prompt is the other half of the system** — it teaches the model the JSON dialect, verification discipline, and multi-agent orchestration. If you used the installer, it's already on your device at `~/.nexon_code/prompt.md`, downloaded as a file. Attach it to your chat as described in Quickstart.

## Usage

```
Usage: nexon_code [PROJECT_DIR ...] [--plain] [--max-output=N] [--max-input=MB] < commands.json
  PROJECT_DIR     - 1-3 project directories; the first is the primary workspace
                    (skips interactive prompt); add_dir registers more mid-session
  --plain         - no ANSI colors / box-drawing decoration, raw JSON only
  --max-output=N  - cap a single result at N chars, wrapped with paging guidance
  --max-input=MB  - cap one pasted/piped JSON payload at MB megabytes (default 64)
  If PROJECT_DIR is omitted, prompts interactively for up to 3 directories.
```

## Tools

| Tool | What it does |
|------|--------------|
| `sh` | run a shell command (timeout, output cap) |
| `read` | read line ranges from files |
| `search` | multi-query text/regex search across the project |
| `patch` | batched edits: search/replace, line ranges, unified diffs (offset-tracked) |
| `edit` | create/append/insert/delete file content |
| `git` | status / diff / log / commit / revert |
| `list` | directory tree (binary files tagged `bin`) |
| `find` | glob file search |
| `outline` | extract function/class signatures with line numbers |
| `recent` | files modified in the last N minutes |
| `undo` | restore any file from automatic backup snapshots |
| `create_file` | single or multi-file creation |
| `cut` / `extract` | move code blocks between files — by line range or by symbol name |
| `fileops` | copy / move / rename / delete / stat |
| `diagnostics` | run a build, parse GCC/Clang errors into structured output |
| `add_dir` | register another workspace root mid-session |

Every write tool snapshots the pre-change file to `.mpt_backups/` first — `undo` walks that history, and undoing is itself undoable (redo).

## Multiple workspaces

Point nexon_code at up to 3 project directories at launch (or up to 6 via `add_dir` mid-session). The sandbox becomes the **union of all registered roots** — the model can read, patch, build, and commit across projects in a single session, batching calls that touch different roots in one round-trip. Relative paths resolve to whichever workspace actually holds the file; `sh`/`git`/`diagnostics` accept a `dir` argument to run in a specific root.

## Agent orchestration (via the prompt)

`prompt.md` also teaches the model to orchestrate *other* models it has access to:

- **Media/research agents** (image, video, web search) get self-contained dispatch prompts in their own fenced JSON blocks
- **Read-only subagents** investigate a scoped part of the codebase and write a report file — the main agent reads the report and does the coding itself
- A dispatch ledger, verification-before-trust rules, and recovery paths for confused subagents

The binary enforces nothing here — it's all convention in the prompt layer, which is exactly where it belongs.

## Security model

Read honestly:

- **Path-taking tools are sandboxed.** Every `read`/`patch`/`edit`/`find`/… path must resolve inside a registered workspace root; anything else is rejected.
- **`sh` is an escape hatch by design.** It runs real shell commands with real consequences. That's what makes the tool useful — and it means you should read commands before pasting them, the same way you'd read a command before running it yourself.
- **You are the security boundary.** nexon_code executes only what you paste. Nothing runs on its own; nothing phones home.

## Repo layout

```
nexon_code.cpp   - the entire tool (single file, C++17)
prompt.md        - the agent system prompt (the other half of the product)
README.md        - this file
LICENSE          - MIT
```

## Status

Battle-tested over one long session on a phone: three reported bugs fixed, two discovered and fixed along the way, a multi-workspace feature designed, built, and verified live, and a rename — all through the tool's own copy-paste loop. The tool debugged itself into existence.

## License

MIT — see [LICENSE](LICENSE).
