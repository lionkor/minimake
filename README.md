# minimake

Minimake is a build system which implements a primitive subset of Make[[1](https://www.gnu.org/software/make/),[2](https://wiki.netbsd.org/tutorials/bsd_make/)].

It aims to be so simple that the `minimake.c` can be distributed with a program's source code, such that only a C compiler is required to bootstrap the entire build system. Alternatively, an existing `make` command can be used due to the [compatibility with make](#compatibility).

I was inspired to build this by the [no-build ("nob") build system](https://github.com/tsoding/nob.h). I find this approach meaningful, but I'd rather write simple Makefile rules.

## Compatibility

*All* Minimake make-files are *valid GNU/BSD Makefiles*.

## Example

See the contained [Makefile](./Makefile) for a complete example.

## Grammar

### Quick & Dirty

Like GNU/BSD Make, but:
- No variables (neither `${...}` nor `...=...` nor `$...`)
- No functions
- No automatic rules, like *.o from *.c

### Full Grammar

```
makefile   ::= rule*
rule       ::= target ':' dependency* '\n' command*
target     ::= [^: \n]+
dependency ::= [^: \n]+
command    ::= '\t' [^\n]* '\n'
```
