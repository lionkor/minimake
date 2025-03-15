#include <errno.h>
#include <linux/limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef MINIMAKE_TESTS
#include "vendor/utest.h"
#else
/* these macros exist to turn all the utest code into dead code, and
   thus make the compiler eliminate it. */
#define UTEST(a, b) static void _test_##a_##b(void)
#define ASSERT_TRUE(...)
#define ASSERT_EQ(...)
#define ASSERT_STREQ(...)
#endif

typedef struct {
    _Bool ok;
    const char* message;
    const char* context;
} minimake_result;

/* a minimake string view ;) */
typedef struct {
    const char* data;
    size_t size;
} mm_sv;

/* creates a mm_sv from a c string, mostly useful for unit tests */
mm_sv minimake_cstr_stringview(const char* s) {
    mm_sv sv;
    sv.data = s;
    sv.size = strlen(s);
    return sv;
}

typedef struct {
    mm_sv target;
    mm_sv dependencies[64];
    size_t n_dependencies;
    mm_sv commands[32];
    size_t n_commands;
} minimake_rule;

typedef struct {
    minimake_rule* rules;
    size_t n_rules;
    void* (*alloc)(size_t);
    void (*free)(void*);
} minimake;

static const minimake_result minimake_result_ok = { .ok = 1, .message = "success", .context = "no context" };
static const minimake_result minimake_result_invalid_arguments = { .ok = 0, .message = "invalid arguments", .context = "no context" };

minimake minimake_init(void* (*alloc)(size_t), void (*dealloc)(void*)) {
    minimake m;
    m.alloc = alloc ? alloc : malloc;
    m.free = dealloc ? dealloc : free;
    m.rules = NULL;
    return m;
}

void minimake_free(minimake* m) {
    if (m) {
        m->free(m->rules);
        m->rules = NULL;
    }
}

static minimake_result minimake_read_makefile(minimake* m, const char* makefile, char** buffer, size_t* size) {
    FILE* file = NULL;
    *size = 0;
    *buffer = NULL;
    size_t rc = 0;
    minimake_result result = minimake_result_ok;

    if (!m || !makefile) {
        return minimake_result_invalid_arguments;
    }

    file = fopen(makefile, "r");
    if (!file) {
        result = (minimake_result) { .ok = 0, .message = strerror(errno), .context = makefile };
        goto cleanup;
    }

    /* seek to the end and back to get the full size */
    fseek(file, 0, SEEK_END);
    *size = ftell(file);
    fseek(file, 0, SEEK_SET);

    *buffer = m->alloc(*size + 1);
    if (!*buffer) {
        result = (minimake_result) { .ok = 0, .message = strerror(errno), .context = "allocating buffer" };
        goto cleanup;
    }
    memset(*buffer, 0, *size + 1);

    rc = fread(*buffer, 1, *size, file);
    if (rc != *size) {
        result = (minimake_result) { .ok = 0, .message = strerror(errno), .context = "reading file" };
        goto cleanup;
    }

cleanup:
    if (file) {
        fclose(file);
    }
    return result;
}

typedef enum {
    MINIMAKE_TOK_WORD,
    MINIMAKE_TOK_COLON,
    MINIMAKE_TOK_NEWLINE,
    MINIMAKE_TOK_COMMAND,
} minimake_token_type;

static const char* minimake_token_type_str[] = {
    "word",
    "colon",
    "newline",
    "command",
};

typedef struct {
    const char* start;
    const char* end;
    minimake_token_type type;
    uint32_t line;
    uint32_t column;
} minimake_token;

static minimake_result minimake_tokenize(minimake* m, const char* buffer, size_t size, minimake_token** ptokens, size_t* n_tokens) {
    size_t max_tokens = 1024;
    minimake_token* new_tokens = NULL;
    minimake_result result = minimake_result_ok;
    const char* p = buffer;
    *ptokens = m->alloc(sizeof(minimake_token) * max_tokens);
    if (!*ptokens) {
        result = (minimake_result) { .ok = 0, .message = strerror(errno), .context = "allocating tokens" };
        goto cleanup;
    }
    memset(*ptokens, 0, sizeof(minimake_token) * max_tokens);

    /*
     * parsing the grammar, which is:
     *
     * recipe       = target ':' dependencies '\n' '\t' commands '\n'
     * target       = +ALPHANUM
     * dependencies = *ALPHANUM
     * commands     = +ALPHANUM
     */

    /* tokenize from buffer */

    uint32_t line = 1;
    uint32_t column = 1;

    for (; *p; ++p) {
        (*ptokens)[*n_tokens].line = line;
        (*ptokens)[*n_tokens].column = column;
        ++column;
        if (*p == '\n') {
            column = 1;
            ++line;
        }
        if (*p == '\n') {
            (*ptokens)[*n_tokens].type = MINIMAKE_TOK_NEWLINE;
            (*ptokens)[*n_tokens].start = p;
            (*ptokens)[*n_tokens].end = p + 1;
            ++(*n_tokens);
        } else if (*p == ':') {
            (*ptokens)[*n_tokens].type = MINIMAKE_TOK_COLON;
            (*ptokens)[*n_tokens].start = p;
            (*ptokens)[*n_tokens].end = p + 1;
            ++(*n_tokens);
        } else if (*p == '#') {
            /* skip comments */
            --column;
            while (*p && *p != '\n') {
                ++p;
                ++column;
            }
            // unskip the newline so we can tokenize it
            if (*p == '\n') {
                --p;
            }
        } else if (*p == ' ') {
            /* skip whitespace */
        } else if (*p == '\t') {
            /* command */
            (*ptokens)[*n_tokens].type = MINIMAKE_TOK_COMMAND;
            (*ptokens)[*n_tokens].start = p + 1;
            while (*p != '\n') {
                ++p;
                ++column;
            }
            (*ptokens)[*n_tokens].end = p;
            --p;
            ++(*n_tokens);
        } else {
            /* target or dependency or part of a command */
            (*ptokens)[*n_tokens].type = MINIMAKE_TOK_WORD;
            (*ptokens)[*n_tokens].start = p;
            --column;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != ':') {
                ++p;
                ++column;
            }
            (*ptokens)[*n_tokens].end = p;
            --p;
            ++(*n_tokens);
        }

        if (*n_tokens == max_tokens) {
            max_tokens = max_tokens * 1.5;
            new_tokens = m->alloc(sizeof(minimake_token) * max_tokens);
            if (!new_tokens) {
                result = (minimake_result) { .ok = 0, .message = strerror(errno), .context = "allocating tokens" };
                goto cleanup;
            }
            memcpy(new_tokens, *ptokens, sizeof(minimake_token) * max_tokens);
            m->free(*ptokens);
            *ptokens = new_tokens;
            new_tokens = NULL;
        }
    }

cleanup:
    if (new_tokens) {
        m->free(new_tokens);
    }
    return result;
}

UTEST(tokenize, empty) {
    minimake m = minimake_init(NULL, NULL);
    const char* buffer = "";
    size_t size = 0;
    minimake_token* tokens = NULL;
    size_t n_tokens = 0;
    minimake_result result = minimake_tokenize(&m, buffer, size, &tokens, &n_tokens);
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(n_tokens, 0);
    minimake_free(&m);
}

UTEST(tokenize, target_only) {
    minimake m = minimake_init(NULL, NULL);
    const char* buffer = "target:\n";
    size_t size = strlen(buffer);
    minimake_token* tokens = NULL;
    size_t n_tokens = 0;
    minimake_result result = minimake_tokenize(&m, buffer, size, &tokens, &n_tokens);
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(n_tokens, 3);
    ASSERT_EQ(tokens[0].type, MINIMAKE_TOK_WORD);
    ASSERT_EQ(tokens[1].type, MINIMAKE_TOK_COLON);
    ASSERT_EQ(tokens[2].type, MINIMAKE_TOK_NEWLINE);
    minimake_free(&m);
}

UTEST(tokenize, target_dependency) {
    minimake m = minimake_init(NULL, NULL);
    const char* buffer = "target: dependency\n";
    size_t size = strlen(buffer);
    minimake_token* tokens = NULL;
    size_t n_tokens = 0;
    minimake_result result = minimake_tokenize(&m, buffer, size, &tokens, &n_tokens);
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(n_tokens, 4);
    ASSERT_EQ(tokens[0].type, MINIMAKE_TOK_WORD);
    ASSERT_EQ(tokens[1].type, MINIMAKE_TOK_COLON);
    ASSERT_EQ(tokens[2].type, MINIMAKE_TOK_WORD);
    ASSERT_EQ(tokens[3].type, MINIMAKE_TOK_NEWLINE);
    minimake_free(&m);
}

UTEST(tokenize, target_dependency_command) {
    minimake m = minimake_init(NULL, NULL);
    const char* buffer = "target: dependency\n\tcommand\n";
    size_t size = strlen(buffer);
    minimake_token* tokens = NULL;
    size_t n_tokens = 0;
    minimake_result result = minimake_tokenize(&m, buffer, size, &tokens, &n_tokens);
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(n_tokens, 6);
    ASSERT_EQ(tokens[0].type, MINIMAKE_TOK_WORD);
    ASSERT_EQ(tokens[1].type, MINIMAKE_TOK_COLON);
    ASSERT_EQ(tokens[2].type, MINIMAKE_TOK_WORD);
    ASSERT_EQ(tokens[3].type, MINIMAKE_TOK_NEWLINE);
    ASSERT_EQ(tokens[4].type, MINIMAKE_TOK_COMMAND);
    ASSERT_EQ(tokens[5].type, MINIMAKE_TOK_NEWLINE);
    minimake_free(&m);
}

UTEST(tokenize, words) {
    minimake m = minimake_init(NULL, NULL);
    const char* buffer = "these are words\nand these are too";
    size_t size = strlen(buffer);
    minimake_token* tokens = NULL;
    size_t n_tokens = 0;
    minimake_result result = minimake_tokenize(&m, buffer, size, &tokens, &n_tokens);
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(n_tokens, 8);
    ASSERT_EQ(tokens[0].type, MINIMAKE_TOK_WORD);
    ASSERT_EQ(tokens[1].type, MINIMAKE_TOK_WORD);
    ASSERT_EQ(tokens[2].type, MINIMAKE_TOK_WORD);
    ASSERT_EQ(tokens[3].type, MINIMAKE_TOK_NEWLINE);
    ASSERT_EQ(tokens[4].type, MINIMAKE_TOK_WORD);
    ASSERT_EQ(tokens[5].type, MINIMAKE_TOK_WORD);
    ASSERT_EQ(tokens[6].type, MINIMAKE_TOK_WORD);
    minimake_free(&m);
}

UTEST(tokenize, specials_only) {
    minimake m = minimake_init(NULL, NULL);
    const char* buffer = ":\n\n\n:::";
    size_t size = strlen(buffer);
    minimake_token* tokens = NULL;
    size_t n_tokens = 0;
    minimake_result result = minimake_tokenize(&m, buffer, size, &tokens, &n_tokens);
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(n_tokens, 7);
    ASSERT_EQ(tokens[0].type, MINIMAKE_TOK_COLON);
    ASSERT_EQ(tokens[1].type, MINIMAKE_TOK_NEWLINE);
    ASSERT_EQ(tokens[2].type, MINIMAKE_TOK_NEWLINE);
    ASSERT_EQ(tokens[3].type, MINIMAKE_TOK_NEWLINE);
    ASSERT_EQ(tokens[4].type, MINIMAKE_TOK_COLON);
    ASSERT_EQ(tokens[5].type, MINIMAKE_TOK_COLON);
    ASSERT_EQ(tokens[6].type, MINIMAKE_TOK_COLON);
    minimake_free(&m);
}

UTEST(tokenize, comments) {
    minimake m = minimake_init(NULL, NULL);
    const char* buffer = "target: # comment\n";
    size_t size = strlen(buffer);
    minimake_token* tokens = NULL;
    size_t n_tokens = 0;
    minimake_result result = minimake_tokenize(&m, buffer, size, &tokens, &n_tokens);
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(n_tokens, 3);
    ASSERT_EQ(tokens[0].type, MINIMAKE_TOK_WORD);
    ASSERT_EQ(tokens[1].type, MINIMAKE_TOK_COLON);
    ASSERT_EQ(tokens[2].type, MINIMAKE_TOK_NEWLINE);
    minimake_free(&m);
}

UTEST(tokenize, comments_and_words) {
    minimake m = minimake_init(NULL, NULL);
    const char* buffer = "target: # comment\nword\n";
    size_t size = strlen(buffer);
    minimake_token* tokens = NULL;
    size_t n_tokens = 0;
    minimake_result result = minimake_tokenize(&m, buffer, size, &tokens, &n_tokens);
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(n_tokens, 5);
    ASSERT_EQ(tokens[0].type, MINIMAKE_TOK_WORD);
    ASSERT_EQ(tokens[1].type, MINIMAKE_TOK_COLON);
    ASSERT_EQ(tokens[2].type, MINIMAKE_TOK_NEWLINE);
    ASSERT_EQ(tokens[3].type, MINIMAKE_TOK_WORD);
    ASSERT_EQ(tokens[4].type, MINIMAKE_TOK_NEWLINE);
    minimake_free(&m);
}

UTEST(tokenize, complex_case) {
    const char* makefile = "simple_rule: test-dep\n"
                           "\ttouch simple_rule\n"
                           "test-dep: foo bar\n";
    minimake m = minimake_init(NULL, NULL);
    minimake_token* tokens = NULL;
    size_t n_tokens = 0;
    minimake_result result = minimake_tokenize(&m, makefile, strlen(makefile), &tokens, &n_tokens);
    if (!result.ok) {
        printf("error: %s\n", result.message);
    }
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(n_tokens, 11);
    ASSERT_EQ(tokens[0].type, MINIMAKE_TOK_WORD);
    ASSERT_EQ(tokens[1].type, MINIMAKE_TOK_COLON);
    ASSERT_EQ(tokens[2].type, MINIMAKE_TOK_WORD);
    ASSERT_EQ(tokens[3].type, MINIMAKE_TOK_NEWLINE);
    ASSERT_EQ(tokens[4].type, MINIMAKE_TOK_COMMAND);
    ASSERT_EQ(tokens[5].type, MINIMAKE_TOK_NEWLINE);
    ASSERT_EQ(tokens[6].type, MINIMAKE_TOK_WORD);
    ASSERT_EQ(tokens[7].type, MINIMAKE_TOK_COLON);
    ASSERT_EQ(tokens[8].type, MINIMAKE_TOK_WORD);
    ASSERT_EQ(tokens[9].type, MINIMAKE_TOK_WORD);
    ASSERT_EQ(tokens[10].type, MINIMAKE_TOK_NEWLINE);
    minimake_free(&m);
}

static mm_sv tok_expect(minimake_token* tokens, size_t n_tokens, size_t* i, minimake_token_type type) {
    mm_sv sv = { .data = NULL, .size = 0 };
    if (*i >= n_tokens) {
        return sv;
    }
    if (tokens[*i].type == type) {
        sv.data = tokens[*i].start;
        sv.size = tokens[*i].end - tokens[*i].start;
        ++(*i);
    }
    return sv;
}

static char ERR_BUF[256];
#define MINIMAKE_ERR_EXPECTED(token, expected)                          \
    do {                                                                \
        memset(ERR_BUF, 0, sizeof(ERR_BUF));                            \
        if (token.line == 0) {                                          \
            sprintf(ERR_BUF, "%s: unexpected end of file, expected %s", \
                makefile,                                               \
                expected);                                              \
        } else {                                                        \
            sprintf(ERR_BUF, "%s:%u:%u: expected %s, got %s: \"%.*s\"", \
                makefile,                                               \
                token.line,                                             \
                token.column,                                           \
                expected,                                               \
                minimake_token_type_str[token.type],                    \
                (int)(token.end - token.start), token.start);           \
        }                                                               \
    } while (0)

minimake_result minimake_parse(minimake* m, const char* makefile, char* buffer, size_t size) {
    minimake_token* tokens = NULL;
    size_t n_tokens = 0;

    minimake_result result = minimake_tokenize(m, buffer, size, &tokens, &n_tokens);
    if (!result.ok) {
        goto cleanup;
    }

    size_t rules_capacity = 5;
    m->rules = m->alloc(sizeof(minimake_rule) * rules_capacity);
    if (!m->rules) {
        result = (minimake_result) { .ok = 0, .message = strerror(errno), .context = "allocating rules" };
        goto cleanup;
    }
    memset(m->rules, 0, sizeof(minimake_rule) * rules_capacity);
    size_t i = 0;

    for (m->n_rules = 0; i < n_tokens; ++m->n_rules) {
        // quick and dirty realloc :^)
        if (m->n_rules > rules_capacity) {
            rules_capacity *= 2;
            minimake_rule* new_rules = m->alloc(sizeof(minimake_rule) * rules_capacity);
            if (!new_rules) {
                result = (minimake_result) { .ok = 0, .message = strerror(errno), .context = "allocating rules" };
                goto cleanup;
            }
            memcpy(new_rules, m->rules, sizeof(minimake_rule) * rules_capacity);
            m->free(m->rules);
            m->rules = new_rules;
        }

        minimake_rule* rule = &m->rules[m->n_rules];

        /* special case where we have multiple newlines between rules */
        while (tokens[i].type == MINIMAKE_TOK_NEWLINE && i < n_tokens) {
            ++i;
        }
        if (i >= n_tokens) {
            break;
        }

        rule->target = tok_expect(tokens, n_tokens, &i, MINIMAKE_TOK_WORD);
        if (!rule->target.data) {
            MINIMAKE_ERR_EXPECTED(tokens[i], "target");
            result = (minimake_result) { .ok = 0, .message = ERR_BUF, .context = "no context" };
            goto cleanup;
        }
        mm_sv colon = tok_expect(tokens, n_tokens, &i, MINIMAKE_TOK_COLON);
        if (!colon.data) {
            MINIMAKE_ERR_EXPECTED(tokens[i], "colon");
            result = (minimake_result) { .ok = 0, .message = ERR_BUF, .context = "no context" };
            goto cleanup;
        }

        while (tokens[i].type == MINIMAKE_TOK_WORD && i < n_tokens) {
            if (rule->n_dependencies == 64) {
                result = (minimake_result) { .ok = 0, .message = "too many dependencies", .context = "no context" };
                goto cleanup;
            }
            /* can't fail */
            rule->dependencies[rule->n_dependencies] = tok_expect(tokens, n_tokens, &i, MINIMAKE_TOK_WORD);
            ++rule->n_dependencies;
        }

        mm_sv newline = tok_expect(tokens, n_tokens, &i, MINIMAKE_TOK_NEWLINE);
        if (!newline.data) {
            MINIMAKE_ERR_EXPECTED(tokens[i], "newline");
            result = (minimake_result) { .ok = 0, .message = ERR_BUF, .context = "no context" };
            goto cleanup;
        }

        if (tokens[i].type == MINIMAKE_TOK_NEWLINE) {
            MINIMAKE_ERR_EXPECTED(tokens[i], "command(s)");
            result = (minimake_result) { .ok = 0, .message = ERR_BUF, .context = "no context" };
            goto cleanup;
        }
        while (tokens[i].type == MINIMAKE_TOK_COMMAND && i < n_tokens) {
            if (rule->n_commands == 128) {
                result = (minimake_result) { .ok = 0, .message = "too many commands", .context = "no context" };
                goto cleanup;
            }
            rule->commands[rule->n_commands] = tok_expect(tokens, n_tokens, &i, MINIMAKE_TOK_COMMAND);
            if (!rule->commands[rule->n_commands].data) {
                MINIMAKE_ERR_EXPECTED(tokens[i], "command");
                result = (minimake_result) { .ok = 0, .message = ERR_BUF, .context = "no context" };
                goto cleanup;
            }
            newline = tok_expect(tokens, n_tokens, &i, MINIMAKE_TOK_NEWLINE);
            /* ignore if this fails */
            ++rule->n_commands;
        }
    }

cleanup:
    if (tokens) {
        m->free(tokens);
    }
    return result;
}

minimake_result minimake_resolve(minimake* m, mm_sv target, mm_sv** result_chain, size_t* result_chain_len) {
    *result_chain = NULL;
    *result_chain_len = 0;
    minimake_result result = minimake_result_ok;

    /* We initialize a chain (array) of targets, which will be our *inverted* todo list of targets to check.
    "Inverted" meaning that it starts with the target we want, and the following elements are sort of a flat
    tree structure with all the elements required, like a 1-D representation of the dependency tree.
    If multiple elements in the tree depend on the same dependency, we don't really mind that; we keep inserting
    that dependency every time we see that we need it. This means that, at the very end, we may have multiple
    copies of the same dependency in the tree. When we later process all the dependencies, we can keep track of
    which are already confirmed, and then we can skip those extra elements. This also means, though, that we
    will not easily detect circular dependencies. We need to find a way to fix that.
    We deliberately don't check if we already resolved a dependency, because that way we can properly handle multiple
    dependencies. */

    size_t chain_capacity = 16;
    mm_sv* chain = m->alloc(sizeof(mm_sv) * chain_capacity);
    if (!chain) {
        result = (minimake_result) { .ok = 0, .message = strerror(errno), .context = "allocating chain" };
        goto cleanup;
    }
    memset(chain, 0, sizeof(mm_sv) * chain_capacity);
    size_t n_chain = 0;

    chain[n_chain++] = target;

    /* iterate over all existing make-rules */
    for (size_t i = 0; i < n_chain; ++i) {
        for (size_t j = 0; j < m->n_rules; ++j) {
            /* we'll call make rules a "recipe" */
            minimake_rule* recipe = &m->rules[j];
            if (recipe->target.size == chain[i].size && memcmp(recipe->target.data, chain[i].data, chain[i].size) == 0) {
                /* we found the rule we need */
                /* now add all dependencies of this rule to the chain */
                for (size_t k = 0; k < recipe->n_dependencies; ++k) {
                    mm_sv dependency = recipe->dependencies[k];
                    chain[n_chain++] = dependency;
                    /* realloc if there's no more space */
                    if (n_chain >= chain_capacity) {
                        chain_capacity *= 2;
                        mm_sv* new_chain = m->alloc(sizeof(mm_sv) * chain_capacity);
                        if (!new_chain) {
                            result = (minimake_result) { .ok = 0, .message = strerror(errno), .context = "allocating chain" };
                            goto cleanup;
                        }
                        memcpy(new_chain, chain, sizeof(mm_sv) * chain_capacity);
                        m->free(chain);
                        chain = new_chain;
                    }
                }
            }
        }
    }

    for (size_t i = 0; i < n_chain; ++i) {
        printf("node: %.*s\n", (int)chain[i].size, chain[i].data);
    }

    *result_chain = chain;
    *result_chain_len = n_chain;
    chain = NULL;

cleanup:
    if (chain) {
        m->free(chain);
    }

    return result;
}

UTEST(resolve, simple_rule) {
    minimake m = minimake_init(malloc, free);

    char* makefile = "simple_rule: test-dep\n"
                     "\ttouch simple_rule\n"
                     "test-dep: foo bar\n";

    minimake_result result = minimake_parse(&m, "Not A Real Makefile", makefile, strlen(makefile));
    ASSERT_TRUE(result.ok);

    mm_sv* chain;
    size_t chain_len;
    result = minimake_resolve(&m, minimake_cstr_stringview("test-test"), &chain, &chain_len);
    ASSERT_TRUE(result.ok);

    minimake_free(&m);
}

minimake_result minimake_make(minimake* m, mm_sv* target, char** cmd, size_t* cmd_capacity) {
    int found = 0;
    for (size_t j = 0; j < m->n_rules; ++j) {
        minimake_rule* rule = &m->rules[j];
        if (rule->target.size == target->size && memcmp(rule->target.data, target->data, target->size) == 0) {
            found = 1;
            for (size_t k = 0; k < rule->n_commands; ++k) {
                if (*cmd_capacity < rule->commands[k].size + 1) {
                    *cmd_capacity = rule->commands[k].size + 1;
                    m->free(*cmd);
                    *cmd = m->alloc(*cmd_capacity);
                    memset(*cmd, 0, *cmd_capacity);
                }
                memcpy(*cmd, rule->commands[k].data, rule->commands[k].size);
                printf("%s\n", *cmd);
                if (system(*cmd) != 0) {
                    sprintf(ERR_BUF, "command \"%.*s\" failed", (int)rule->commands[k].size, rule->commands[k].data);
                    return (minimake_result) { .ok = 0, .message = ERR_BUF, .context = "command" };
                }
            }
        }
    }
    if (!found) {
        sprintf(ERR_BUF, "no rule to make \"%.*s\"", (int)target->size, target->data);
        return (minimake_result) { .ok = 0, .message = ERR_BUF, .context = "no context" };
    }
    return minimake_result_ok;
}

minimake_result minimake_execute_chain(minimake* m, mm_sv* chain, size_t chain_len) {
    char filename[PATH_MAX];
    size_t cmd_capacity = 0;
    char* cmd = NULL;
    minimake_result result = minimake_result_ok;
    memset(ERR_BUF, 0, sizeof(ERR_BUF));
    memset(filename, 0, sizeof(filename));
    for (ssize_t i = chain_len - 1; i > -1; --i) {
        /* 1. check if that file exists */
        if (chain[i].size >= PATH_MAX) {
            result = (minimake_result) { .ok = 0, .message = "path too long", .context = "target" };
            goto cleanup;
        }
        memcpy(filename, chain[i].data, chain[i].size);
        filename[chain[i].size] = 0;
        struct stat st;
        if (stat(filename, &st) < 0) {
            switch (errno) {
            case ENOENT: {
                /* 2a. if it doesn't exist, execute the commands */
                minimake_result make_result = minimake_make(m, &chain[i], &cmd, &cmd_capacity);
                if (!make_result.ok) {
                    result = make_result;
                    goto cleanup;
                }
                /* check that the rule succeeded by doing another stat */
                if (stat(filename, &st) < 0) {
                    sprintf(ERR_BUF, "rule \"%.*s\" should have created \"%s\", but after running the rule, minimake checked, and got the error: %s", (int)chain[i].size, chain[i].data, filename, strerror(errno));
                    make_result = (minimake_result) { .ok = 0, .message = ERR_BUF, .context = "stat" };
                    goto cleanup;
                }
                break;
            }
            default:
                sprintf(ERR_BUF, "error determining if \"%s\" exists: %s", filename, strerror(errno));
                result = (minimake_result) { .ok = 0, .message = ERR_BUF, .context = "stat" };
                goto cleanup;
            }
        } else {
            char dep_filename[PATH_MAX];
            /* 2b. does exist, check that the modified time of all dependencies is older than the target's modification time */
            /* at this point, all dependencies are guaranteed to exist */
            for (size_t rule_i = 0; rule_i < m->n_rules; ++rule_i) {
                if (m->rules[rule_i].target.size == chain[i].size && memcmp(m->rules[rule_i].target.data, chain[i].data, chain[i].size) == 0) {
                    int rebuilt = 0;
                    /* check dependencies */
                    for (size_t k = 0; !rebuilt && k < m->rules[rule_i].n_dependencies; ++k) {
                        if (m->rules[i].dependencies[k].size >= PATH_MAX) {
                            result = (minimake_result) { .ok = 0, .message = "path too long", .context = "dependency" };
                            goto cleanup;
                        }
                        memcpy(dep_filename, m->rules[rule_i].dependencies[k].data, m->rules[rule_i].dependencies[k].size);
                        dep_filename[m->rules[rule_i].dependencies[k].size] = 0;
                        struct stat dep_st;
                        if (stat(dep_filename, &dep_st) < 0) {
                            result = (minimake_result) { .ok = 0, .message = "dependency not satisfied when it should be guaranteed, is something else modifying the filesystem?", .context = "dependency" };
                            goto cleanup;
                        }
                        /* compare dependency mtime to target mtime, if target mtime < dependency mtime, make target again */
                        if (st.st_mtim.tv_sec < dep_st.st_mtim.tv_sec) {
                            minimake_result make_result = minimake_make(m, &chain[i], &cmd, &cmd_capacity);
                            if (!make_result.ok) {
                                make_result.context = "rebuild due to mtime";
                                result = make_result;
                                goto cleanup;
                            }
                            rebuilt = 1;
                            break;
                        }
                    }
                    break;
                }
            }
        }
    }
cleanup:
    if (cmd) {
        m->free(cmd);
    }
    return result;
}

#ifdef MINIMAKE_TESTS
UTEST_MAIN()
#else

int main(int argc, char** argv) {
    memset(ERR_BUF, 0, sizeof(ERR_BUF));
    minimake m = minimake_init(NULL, NULL);

    char* buffer = NULL;
    size_t size = 0;
    minimake_result result = minimake_read_makefile(&m, "Minimakefile", &buffer, &size);
    if (!result.ok) {
        printf("ERROR: %s\n", result.message);
        return 1;
    }
    result = minimake_parse(&m, "Minimakefile", buffer, size);
    if (!result.ok) {
        printf("ERROR: %s\n", result.message);
        return 1;
    }

    /* print entire makefile and all rules */
    for (size_t i = 0; i < m.n_rules; ++i) {
        minimake_rule* rule = &m.rules[i];
        printf("rule: %.*s\n", (int)rule->target.size, rule->target.data);
        for (size_t j = 0; j < rule->n_dependencies; ++j) {
            printf("  dependency: %.*s\n", (int)rule->dependencies[j].size, rule->dependencies[j].data);
        }
        for (size_t j = 0; j < rule->n_commands; ++j) {
            printf("  command: %.*s\n", (int)rule->commands[j].size, rule->commands[j].data);
        }
    }

    mm_sv* chain;
    size_t chain_len;

    mm_sv target;
    if (argc > 1) {
        target = minimake_cstr_stringview(argv[1]);
    } else {
        target = m.rules[0].target;
    }

    result = minimake_resolve(&m, target, &chain, &chain_len);
    if (!result.ok) {
        printf("ERROR: %s\n", result.message);
        return 1;
    }

    /* now we have the chain, so we can start walking it */
    result = minimake_execute_chain(&m, chain, chain_len);
    if (!result.ok) {
        printf("ERROR: %s\n", result.message);
        return 1;
    }

    m.free(chain);
    m.free(buffer);
    minimake_free(&m);
    return 0;
}

#endif
