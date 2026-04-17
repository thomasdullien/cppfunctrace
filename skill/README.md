# cppfunctrace skill

Drop-in skill for coding agents (Claude Code, OpenCode, or any tool
that can `exec` scripts) to capture and analyze function-level traces
of arbitrary C/C++ programs.

## Install

### Claude Code

User-scope (available in every project):

```bash
mkdir -p ~/.claude/skills
cp -r /path/to/cppfunctrace/skill ~/.claude/skills/cppfunctrace
```

Project-scope (available only in this checkout):

```bash
mkdir -p .claude/skills
cp -r /path/to/cppfunctrace/skill .claude/skills/cppfunctrace
```

On the next session the agent discovers the skill through
`.claude/skills/*/SKILL.md` and can invoke it via the `Skill` tool.

### OpenCode / other agents

Point the agent at
[`skill/SKILL.md`](SKILL.md) as a prompt prefix, or have it run the
scripts directly — they work outside any particular agent runtime.

## What the agent gets

| File                         | Purpose                                                  |
|------------------------------|----------------------------------------------------------|
| `SKILL.md`                   | Activation triggers, step-by-step workflow, SQL snippets |
| `scripts/install.sh`         | Clone + `make` cppfunctrace into `~/.cache/cppfunctrace` |
| `scripts/enable-build.sh`    | Emit compile/link flags for Makefile, CMake, autoconf    |
| `scripts/trace.sh`           | Run a command with tracing, emit `.perfetto-trace`       |
| `scripts/analyze.py`         | Canned `trace_processor` SQL summaries                   |

Together these let an agent, in four commands:

```bash
eval "$(./skill/scripts/install.sh)"                   # install
./skill/scripts/enable-build.sh --env > flags.env      # get flags
# (agent edits the user's build system)
./skill/scripts/trace.sh --out /tmp/tr -- ./target     # record
./skill/scripts/analyze.py /tmp/tr/trace.perfetto-trace  # report
```

## Trying it manually

```bash
# from this repo:
make
eval "$(./skill/scripts/install.sh ~/.cache/cppfunctrace-test)"
eval "$(./skill/scripts/enable-build.sh --env)"
# (rebuild your target with the flags)
./skill/scripts/trace.sh --out /tmp/ex -- /path/to/your/binary
./skill/scripts/analyze.py /tmp/ex/trace.perfetto-trace
```
