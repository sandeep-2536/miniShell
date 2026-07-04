# minishell

A simplified Unix shell written in C, built from scratch as an OS/Linux
learning project. It implements a REPL, a lexer/parser, `$VAR`/`$?`
expansion, pipes, redirections (`<`, `>`, `>>`, `<<`), signal handling,
and a set of builtins — the same core ideas real shells like `bash` are
built on.

> This is a **learning project**, not a production shell. Scope was
> deliberately capped (see `knowledge.md`) once the core OS concepts —
> processes, file descriptors, and signals — were solidly covered.

## Features

- **REPL** with `readline` (arrow-key history, line editing)
- **Lexer** that tokenizes input, correctly handling `'single'` and
  `"double"` quotes
- **Parser** that builds a pipeline of commands, each with its own
  argument list and redirections
- **Expansion** of `$VAR` and `$?`, respecting quoting rules (single
  quotes suppress expansion, double quotes don't)
- **Execution** via `fork` + `execve` + `$PATH` resolution
- **Builtins** (run in-process, not forked): `cd`, `pwd`, `echo`,
  `export`, `unset`, `env`, `exit`
- **Pipes** — any number of commands chained with `|`
- **Redirections** — `<`, `>`, `>>`, and heredocs (`<<`)
- **Signal handling** — `Ctrl-C` and `Ctrl-\` behave like in `bash`
  (the shell survives; a running child is what gets interrupted/killed)
- **Exit status propagation** — `$?` and process exit codes match
  bash's conventions (e.g. `128 + signal` when a command is killed)
- **Error handling** — command not found, bad redirection targets,
  failed `fork`/`pipe`, and redirection-only lines (`> file.txt` with
  no command) are all handled without crashing

## Build

```bash
make
```

Produces a `minishell` binary. Requires `libreadline-dev` (or your
distro's equivalent readline development package).

Other targets:

```bash
make clean   # remove object files
make fclean  # remove object files + binary
make re      # fclean + all
```

## Run

```bash
./minishell
```

You'll get a `minishell$ ` prompt. Type commands as you would in
`bash`:

```
minishell$ echo hello | wc -c
6
minishell$ echo "user is $USER, last status was $?"
minishell$ cat << EOF
heredoc line with $HOME expanded
EOF
minishell$ ls > out.txt
minishell$ export NAME=world
minishell$ exit
```

Exit with `exit`, or `Ctrl-D` (EOF), just like a real shell.

## Project structure

```
minishell/
├── Makefile
└── src/
    └── main.c
```

Everything currently lives in one file by design (see `knowledge.md`
for why) — it was kept single-file deliberately while the project
scope stayed at this size, to avoid header/linkage indirection with no
payoff yet.

## What's intentionally out of scope

These were identified as low resume-value / high effort during
planning, and skipped in favor of hardening what's already built:

- `&&` / `||` logical operators
- Wildcards (`*`)
- Subshells (`(...)`)
- A fully custom `envp` array (currently uses the process environment
  directly via `getenv`/`setenv`/`unsetenv`)

See `knowledge.md` for the full reasoning, the build-up of the project
step by step, and what each step was meant to teach.
