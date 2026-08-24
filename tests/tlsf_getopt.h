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

int tlsf_getopt(tlsf_getop_state *state)
{
    if (!state || state->argc <= 1 || state->num_options == 0 || !state->argv)
        return -1;
    if (state->counter >= state->argc)
        return -1;

    state->optarg = NULL;

    char *current_arg = state->argv[state->counter];

    if (current_arg[0] != '-' || current_arg[1] == '\0') {
        return -1;
    }

    char c_option = current_arg[1];
    tlsf_command_option *option = tlsf_scan_options(state, c_option);

    if (option == NULL) {
        fprintf(stderr, "%s: unknown option -- '%c'\n", state->argv[0],
                c_option);
        state->counter++;
        return '?';
    }

    if (option->is_have_param) {
        if (state->counter + 1 < state->argc) {
            state->optarg = state->argv[state->counter + 1];
            state->counter += 2;
        } else {
            fprintf(stderr, "%s: option requires an argument -- '%c'\n",
                    state->argv[0], c_option);
            state->counter++;
            return '?';
        }
    } else {
        state->counter++;
    }

    return c_option;
}