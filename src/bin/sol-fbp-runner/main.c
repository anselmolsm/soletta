/*
 * This file is part of the Soletta Project
 *
 * Copyright (C) 2015 Intel Corporation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Intel Corporation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

#include "sol-arena.h"
#include "sol-conffile.h"
#include "sol-file-reader.h"
#include "sol-flow-buildopts.h"
#include "sol-json.h"
#include "sol-log.h"
#include "sol-mainloop.h"
#include "sol-util.h"
#include "sol-vector.h"

#ifdef USE_MEMMAP
#include "sol-memmap-storage.h"
#endif

#include "runner.h"

#define MAX_OPTS 64

static struct {
    const char *name;

    const char *memory_map_file;

    const char *options[MAX_OPTS + 1];
    int options_count;

    bool check_only;
    bool provide_sim_nodes;
    bool execute_type;
    struct sol_ptr_vector fbp_search_paths;
} args;

static struct runner *the_runner;
static struct sol_arena *str_arena;

#ifdef SOL_FLOW_INSPECTOR_ENABLED
/* defined in inspector.c */
extern void inspector_init(void);
#endif

static void
usage(const char *program)
{
    fprintf(stderr,
        "usage: %s [options] input_file [-- flow_arg1 flow_arg2 ...]\n"
        "\n"
        "Executes the flow described in input_file.\n\n"
        "Options:\n"
        "    -c            Check syntax only. The program will exit as soon as the flow\n"
        "                  is built and the syntax is verified.\n"
        "    -s            Provide simulation nodes for flows with exported ports.\n"
        "    -t            Instead of reading a file, execute a node type with the name\n"
        "                  passed as first argument. Implies -s.\n"
        "    -o name=value Provide option when creating the root node, can have multiple.\n"
#ifdef SOL_FLOW_INSPECTOR_ENABLED
        "    -D            Debug the flow by printing connections and packets to stdout.\n"
#endif
        "    -I            Define search path for FBP files\n"
        "\n",
        program);
}

static bool
parse_args(int argc, char *argv[])
{
    int opt, err;
    const char known_opts[] = "cho:stI:"
#ifdef SOL_FLOW_INSPECTOR_ENABLED
        "D"
#endif
    ;

    sol_ptr_vector_init(&args.fbp_search_paths);

    while ((opt = getopt(argc, argv, known_opts)) != -1) {
        switch (opt) {
        case 'c':
            args.check_only = true;
            break;
        case 's':
            args.provide_sim_nodes = true;
            break;
        case 'h':
            usage(argv[0]);
            exit(EXIT_SUCCESS);
            break;
        case 'o':
            if (args.options_count == MAX_OPTS) {
                printf("Too many options.\n");
                exit(EXIT_FAILURE);
            }
            args.options[args.options_count++] = optarg;
            break;
        case 't':
            args.execute_type = true;
            break;
#ifdef SOL_FLOW_INSPECTOR_ENABLED
        case 'D':
            inspector_init();
            break;
#endif
        case 'I':
            err = sol_ptr_vector_append(&args.fbp_search_paths, optarg);
            if (err < 0) {
                printf("Out of memory\n");
                exit(1);
            }
            break;
        default:
            return false;
        }
    }

    if (optind == argc)
        return false;

    args.name = argv[optind];
    if (args.execute_type) {
        args.provide_sim_nodes = true;
    }

    sol_args_set(argc - optind, &argv[optind]);

    return true;
}

static bool
load_memory_maps(const struct sol_ptr_vector *maps)
{
#ifdef USE_MEMMAP
    struct sol_memmap_map *map;
    int i;

    SOL_PTR_VECTOR_FOREACH_IDX (maps, map, i) {
        if (sol_memmap_add_map(map) < 0)
            return false;
    }

    return true;
#else
    SOL_WRN("Memory map defined on config file, but Solleta was built without support to it");
    return true;
#endif
}

static bool
startup(void *data)
{
    bool finished = true;
    int result = EXIT_FAILURE;
    struct sol_ptr_vector *memory_maps;

    str_arena = sol_arena_new();
    if (!str_arena) {
        fprintf(stderr, "Cannot create str arena\n");
        goto end;
    }

    if (args.execute_type) {
        the_runner = runner_new_from_type(args.name, args.options);
    } else {
        the_runner = runner_new_from_file(args.name, args.options,
            &args.fbp_search_paths);
    }

    if (!the_runner)
        goto end;

    if (args.check_only) {
        printf("'%s' - Syntax OK\n", args.name);
        result = EXIT_SUCCESS;
        goto end;
    }

    if (args.provide_sim_nodes) {
        int err;
        err = runner_attach_simulation(the_runner);
        if (err < 0) {
            fprintf(stderr, "Cannot attach simulation nodes\n");
            goto end;
        }
    }

    if (sol_conffile_resolve_memmap(&memory_maps)) {
        SOL_ERR("Couldn't resolve memory mappings on config file");
        goto end;
    }
    if (memory_maps)
        load_memory_maps(memory_maps);

    if (runner_run(the_runner) < 0) {
        fprintf(stderr, "Failed to run\n");
        goto end;
    }

    finished = false;

end:
    if (finished)
        sol_quit_with_code(result);

    return false;
}

static void
shutdown(void)
{
    if (the_runner)
        runner_del(the_runner);

    if (str_arena)
        sol_arena_del(str_arena);

    sol_ptr_vector_clear(&args.fbp_search_paths);
}

int
main(int argc, char *argv[])
{
    int r;

    if (!parse_args(argc, argv)) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (sol_init() < 0) {
        fprintf(stderr, "Cannot initialize soletta.\n");
        return EXIT_FAILURE;
    }

    sol_idle_add(startup, NULL);

    r = sol_run();

    shutdown();
    sol_shutdown();

    return r;
}
