/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * tlsf-bsd is freely redistributable under the BSD License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/* Implementation of simplest crossplatform getopt analogue */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct {
    bool is_have_param;
    char option;
} tlsf_command_option;

typedef struct {
    int argc;
    char **argv;
    tlsf_command_option *options;
    int num_options;
    char *optarg;
    int counter;

    /* Offset of the next option character inside argv[counter]. Zero means
     * "start a fresh element"; a non-zero value means a clustered element such
     * as "-cq" is still being consumed. Callers leave this zero-initialized.
     */
    int char_pos;
} tlsf_getop_state;

static inline tlsf_command_option *tlsf_scan_options(tlsf_getop_state *state,
                                                     char c)
{
    for (int i = 0; i < state->num_options; i++) {
        if (state->options[i].option == c) {
            return &state->options[i];
        }
    }
    return NULL;
}

/* Returns the option character, '?' on a malformed option, or -1 once the
 * option list is exhausted. Mirrors getopt(3) closely enough for the test
 * harnesses: clustered flags ("-cq") and attached arguments ("-s64") both work,
 * and "--" terminates option processing.
 */
static inline int tlsf_getopt(tlsf_getop_state *state)
{
    if (!state || !state->argv || state->argc <= 1 || state->num_options == 0)
        return -1;
    if (state->counter >= state->argc)
        return -1;

    state->optarg = NULL;

    char *arg = state->argv[state->counter];

    /* A bare "-" or a non-option element ends the scan. */
    if (arg[0] != '-' || arg[1] == '\0')
        return -1;
    /* "--" ends the scan and is consumed. */
    if (arg[1] == '-' && arg[2] == '\0') {
        state->counter++;
        return -1;
    }

    if (state->char_pos == 0)
        state->char_pos = 1;

    char c_option = arg[state->char_pos++];
    tlsf_command_option *option = tlsf_scan_options(state, c_option);

    /* Consumed the last character of this element, so move to the next one. */
    if (arg[state->char_pos] == '\0') {
        state->counter++;
        state->char_pos = 0;
    }

    if (!option) {
        fprintf(stderr, "%s: unknown option -- '%c'\n", state->argv[0],
                c_option);
        return '?';
    }

    if (!option->is_have_param)
        return c_option;

    if (state->char_pos != 0) {
        /* Attached argument: the rest of this element, as in "-s64". */
        state->optarg = arg + state->char_pos;
        state->counter++;
        state->char_pos = 0;
    } else if (state->counter < state->argc) {
        /* Separate argument: the following element, as in "-s 64". */
        state->optarg = state->argv[state->counter++];
    } else {
        fprintf(stderr, "%s: option requires an argument -- '%c'\n",
                state->argv[0], c_option);
        return '?';
    }

    return c_option;
}
