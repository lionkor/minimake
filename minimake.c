#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef MINIMAKE_TESTS
#include "vendor/utest.h"
#else
/* these macros exist to turn all the utest code into dead code, and
   thus make the compiler eliminate it. */
#define UTEST(a, b) static void _test_##a_##b(void)
#define ASSERT_TRUE(...)
#define ASSERT_EQ(...)
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
    char* buffer;
    void* (*alloc)(size_t);
    void (*free)(void*);
} minimake;

/* initializes minimake with the given allocator and deallocator, which default to malloc/free if NULL */
minimake minimake_init(void* (*alloc)(size_t), void (*free)(void*));
void minimake_free(minimake* m);
minimake_result minimake_parse(minimake* m, const char* makefile);

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
        m->free(m->buffer);
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
    MINIMAKE_TOK_EOF
} minimake_token_type;

static const char* minimake_token_type_str[] = {
    "word",
    "colon",
    "newline",
    "eof"
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
     * recipe       = target ':' dependencies '\n' commands '\n'
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
        if (*p == '\n') {
            column = 1;
            ++line;
        } else {
            ++column;
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
            while (*p && *p != '\n') {
                ++p;
            }
            // unskip the newline so we can tokenize it
            if (*p == '\n') {
                --p;
            }
        } else if (*p == ' ' || *p == '\t') {
            /* skip whitespace */
        } else {
            /* target or dependency or part of a command */
            (*ptokens)[*n_tokens].type = MINIMAKE_TOK_WORD;
            (*ptokens)[*n_tokens].start = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != ':') {
                ++p;
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
    ASSERT_EQ(tokens[4].type, MINIMAKE_TOK_WORD);
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

minimake_result minimake_parse(minimake* m, const char* makefile) {
    char* buffer = NULL;
    size_t size = 0;
    minimake_token* tokens = NULL;
    size_t n_tokens = 0;

    minimake_result result = minimake_read_makefile(m, makefile, &buffer, &size);
    if (!result.ok) {
        goto cleanup;
    }

    result = minimake_tokenize(m, buffer, size, &tokens, &n_tokens);
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

        while (tokens[i].type == MINIMAKE_TOK_WORD && i < n_tokens) {
            if (rule->n_commands == 128) {
                result = (minimake_result) { .ok = 0, .message = "too many commands", .context = "no context" };
                goto cleanup;
            }
            /* can't fail */
            rule->commands[rule->n_commands] = tok_expect(tokens, n_tokens, &i, MINIMAKE_TOK_WORD);
            ++rule->n_commands;
        }

        newline = tok_expect(tokens, n_tokens, &i, MINIMAKE_TOK_NEWLINE);
        if (!newline.data) {
            MINIMAKE_ERR_EXPECTED(tokens[i], "newline");
            result = (minimake_result) { .ok = 0, .message = ERR_BUF, .context = "no context" };
            goto cleanup;
        }
    }

    m->buffer = buffer;
    buffer = NULL;

cleanup:
    if (buffer) {
        m->free(buffer);
    }
    if (tokens) {
        m->free(tokens);
    }
    return result;
}

minimake_result minimake_resolve(minimake* m) {
    if (!m) {
        return minimake_result_invalid_arguments;
    }
    minimake_result result = minimake_result_ok;



    return result;
}

#ifdef MINIMAKE_TESTS
UTEST_MAIN()
#else

int main(void) {
    memset(ERR_BUF, 0, sizeof(ERR_BUF));
    minimake m = minimake_init(NULL, NULL);
    minimake_result result = minimake_parse(&m, "Minimakefile");
    if (!result.ok) {
        printf("ERROR: %s\n", result.message);
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

    result = minimake_resolve(&m);
    if (!result.ok) {
        printf("ERROR: %s\n", result.message);
    }

    minimake_free(&m);
    return 0;
}

#endif
