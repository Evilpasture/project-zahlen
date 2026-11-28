# Documentation Rules (`*.md`, `CMakeLists.txt` / workflow user comments)

All documentation in this repository must read like a POSIX man page or compiler specification: declarative, imperative, and strictly factual.

## 1. Hard Content Rules
- **State only what is, what is required, what happens, and what fails.** Never state *why* a design is good, clever, or superior to an alternative.
- **Evergreen present tense only.** Describe current behavior. Never write changelog-style transitions (*"formerly"*, *"replaces the old X"*, *"no longer does Y"*, *"now supports"*).
- **No external comparisons.** Do not analogize mechanisms to `pacman`, `apt`, `cargo`, `ubuntu-keyring`, browsers, or other ecosystems.
- **No unsolicited tutorials on basics.** Assume the reader understands standard C++, CMake, Git, and cryptographic signing. State the verification behavior; do not lecture on why out-of-band trust or checksums matter.
- **No defensive disclaimers.** State preconditions and limits directly.
- **No explanatory clauses.** Never use connector phrases like "which ensures", "in order to", "allowing you to", or "because it". 
- If a sentence requires an explanation of *how* or *why* a background mechanism works, delete the explanation entirely. State only the imperative command or the raw fact.

## 2. Banned Syntactic Patterns & Phrases
Before finalizing any documentation edit, reject and rewrite any sentence matching these LLM prose patterns:
- **Contrastive antithesis / "X, not Y" framing:**
  - *"is a refusal, not an instruction to..."*
  - *"proves nothing"*
  - *"the whole point of..."*
  - *"instead of silently..."*
  - *"by design, not by accident"*
- **Self-congratulatory / marketing qualifiers:**
  - *"nothing to trust by hand"*, *"never has to"*, *"out of the box"*, *"first-class"*, *"seamless"*, *"hermetic"*, *"honest"*, *"cleanly"*, *"baked into"* (use *"embedded in"* or *"compiled into"*).
- **Maintainer rationalization:**
  - *"deliberately"*, *"intentionally"*, *"for the same reason"*, *"this is why"*.

## 3. Audience Boundaries (Strict Partitioning)
Never mix maintainer internals into consumer documentation.

| Paths | Audience | Forbidden Content |
|---|---|---|
| `docs/CONSUMING.md`, `docs/UPGRADING.md`, `docs/FIRST_GAME.md`, `docs/REQUIREMENTS.md`, `docs/Image.md`, `docs/COM*.md` | External game developer using released packages | CI jobs, `scripts/test-*.sh`, `scripts/check-*.sh`, `USE_SANITIZERS`, internal CMake governance checks, maintainer rationales, removed legacy flags/packages |
| `packaging/*.md`, `packaging/keys/README.md`, `utils/zpkg/README.md`, `utils/README.md` | Packagers, release maintainers, tool users | Editorializing on trust models, internal CI test assertions, legacy bash/python rewrites, sanitizer CI lanes |
| `docs/DEVELOPMENT.md`, `include/ARCHITECTURE.md` | Engine contributors | Conversational storytelling; state invariants, ownership rules, file paths, and enforcement scripts directly |

## 4. Required Structure by Section Type
- **CLI / Config Setup:** 1-sentence behavior summary -> code block -> flag/option table or list -> failure condition.
- **API / Subsystem Contract:** Ownership -> valid preconditions/thread rules -> error/failure behavior -> minimal usage snippet.
- **Workflow / Pipeline:** Inputs -> exact command -> outputs -> failure modes.

## 5. Tone Calibration (Before -> After)

**Bad (rejected — lecturing, analogies, rhetorical antithesis):**
> The project key is long-lived and pinned in `packaging/keys/zahlen-release.pub`. It is baked into `zpkg` at build time, so no manual trust step is needed — the binary you already trust carries the key it verifies with, the same model as `pacman` (`archlinux-keyring`) and `apt` (`ubuntu-keyring`). A release signed by an unknown key is a refusal, not an instruction to trust it.

**Good (accepted — factual contract only):**
> Official release archives are signed with the key at `packaging/keys/zahlen-release.pub`, which is embedded in `zpkg` at build time. `zpkg install` verifies archive signatures and checksums automatically. Use `zpkg key add` to trust keys from other publishers.

**Bad (rejected — CI dogma leaked into user tutorial):**
> Copy `samples/FirstGameSample.cpp` to `main.cpp`. It is documentation only and is deliberately not a CMake target in this tree; examples must never be inputs to CI jobs.

**Good (accepted — actionable instructions only):**
> Copy `samples/FirstGameSample.cpp` from the release tag matching your installed engine to your game directory as `main.cpp`.

**Bad (rejected — legacy transition trivia & sanitizer CI notes):**
> On Arch it comes from `zpkg` (which replaces the former `zahlen-zpkg` package). Release archives use `USE_SANITIZERS=OFF`; sanitizer-enabled in-tree builds still configure the package targets for testing.

**Good (accepted — current state only):**
> On Arch the manager is packaged as `zpkg`.

## 6. Mandatory Self-Audit Gate
Before outputting or committing edits to any `.md` file or user-facing comment:
1. Delete every clause explaining *why* an engineering choice was made unless the document's scope is explicitly architectural rationale.
2. Delete every mention of `scripts/`, CI workflows, `USE_SANITIZERS`, or historical tool versions from consumer guides (`docs/CONSUMING.md`, `docs/UPGRADING.md`, `docs/FIRST_GAME.md`, `docs/REQUIREMENTS.md`).
3. Verify every remaining sentence states a testable fact, path, command, precondition, or failure mode.
