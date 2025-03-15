# minimake

Minimake is a build system which implements a primitive subset of Make[[1](https://www.gnu.org/software/make/),[2](https://wiki.netbsd.org/tutorials/bsd_make/)]. This means that it can produce targets by running commands when dependencies change.

It aims to be so simple that the `minimake.c` can be distributed with a program's source code, such that only a C compiler is required to bootstrap the entire build system. Alternatively, an existing `make` command can be used due to the [compatibility with make](#compatibility).

I was inspired to build this by the [no-build ("nob") build system](https://github.com/tsoding/nob.h). I find this approach meaningful, but I'd rather write simple Makefile rules.

## What does it do?

It **does**:
- Run one or more rules to create a target
- Use "last modified" file metadata to determine if something is outdated
- Rebuild when dependencies change

Or, in terms of differences from existing tools:

It's like GNU/BSD Make, but:
- No variables (neither `${...}` nor `...=...` nor `$...`)
- No functions
- No automatic rules, like *.o from *.c
- No .PHONY targets

## Compatibility

*All* Minimake make-files are *valid GNU/BSD Makefiles*.

## Example

See the contained [Makefile](./Makefile) for a complete example.

## Grammar

```
makefile   ::= rule*
rule       ::= target ':' dependency* '\n' command*
target     ::= [^: \n]+
dependency ::= [^: \n]+
command    ::= '\t' [^\n]* '\n'
```

### How to use

Simply copy the `minimake.c` into your project and write a minimake makefile.

**If you want to contribute to minimake**, here are a few important details:
- I'm very happy to increase the amount of supported makefile syntax
- There are unit-tests, which you can run by compiling with `-DMINIMAKE_TESTS`
