/**
 * main.c
 *
 * Copyright (C) 2020 zznop, zznop0x90@gmail.com
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <ctype.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <unistd.h>
#include <elf.h>
#include <sys/uio.h>
#include "utils.h"

////////////////////////////// PREPROCESSOR //////////////////////////////

#define ELF_MAGIC 0x464c457f
#define HOOK_LIB_PATH "/tmp/libich.so"


////////////////////////////// EXTERNALS /////////////////////////////////

extern char **environ;
extern uint8_t _hook_library[0];
extern int _hook_library_size;
static bool _continue = 1;

////////////////////////////// TYPES  ////////////////////////////////////

typedef enum {
    EXITED = 0,
    CRASHED,
    UNKNOWN
} estat_t;

////////////////////////// END TYPES /////////////////////////////////////

/**
 * Drop libich.so to a tmp location
 *
 * @return 0 for success, 1 for failure
 */
static int init_crash_harness(void)
{
    int nb, total_nb;
    info("Dropping instrumentation library to %s ...", HOOK_LIB_PATH);
    FILE *hook_lib_file = fopen(HOOK_LIB_PATH, "wb");
    if (!hook_lib_file) {
        err("Failed to initialize crash harness\n");
        return 1;
    }

    total_nb = 0;
    while (nb > 0) {
        nb = fwrite(_hook_library, 1, _hook_library_size, hook_lib_file);
        total_nb += nb;
        if (total_nb == _hook_library_size)
            break;
    }

    if (total_nb != _hook_library_size) {
        err("Failed to write hook library to tmp directory\n");
        return 1;
    }

    fclose(hook_lib_file);
    chmod(HOOK_LIB_PATH, 0700);
    return 0;
}

/**
 * Resolve base address of the ELF that RIP is within
 *
 * @param pid Debugee process ID
 * @param addr RIP address
 */
static void dump_elf_base(pid_t pid, uint64_t addr)
{
    uint64_t qword;
    addr = addr & ~(PAGE_SIZE - 1);
    while (_continue) {
        qword = ptrace(PTRACE_PEEKDATA, pid, addr, 0);
        if (errno)
            return;

        if ((qword & 0x00000000ffffffff) == ELF_MAGIC) {
            printf("ELF base address: %016lx\n\n", addr);
            break;
        }

        addr -= PAGE_SIZE;
    }
}

/**
 * Dump virtual memory content
 *
 * @param pid Debuggee process ID
 * @param addr Base address
 */
static void dump_reg_memory(pid_t pid, uint64_t addr)
{
    uint8_t buf[16];
    size_t i;
    uint64_t *ptr;

    memset(buf, '\0', sizeof(buf));
    ptr = (uint64_t *)buf;
    *ptr = ptrace(PTRACE_PEEKDATA, pid, addr, NULL);
    if (errno) {
        printf("\n");
        return;
    }

    ptr += 1;
    *ptr = ptrace(PTRACE_PEEKDATA, pid, addr, NULL);

    printf(" -> ");
    for (i = 0; i < sizeof(buf); i++) {
        printf("%02x ", buf[i]);
    }

    printf("    ");
    for (i = 0; i < sizeof(buf); i++) {
        if (isprint(buf[i]))
            printf("%c", buf[i]);
        else
            printf(".");
    }
    printf("\n");
}

/**
 * Display a crash dump
 *
 * @param pid Debuggee process ID
 */
static void display_crash_dump(pid_t pid)
{
    struct user_regs_struct regs;
    struct iovec iov;

    memset(&regs, 0, sizeof(regs));
    iov.iov_len = sizeof(regs);
    iov.iov_base = &regs;

    ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &iov);
    dump_elf_base(pid, regs.rip);
    printf("rax    : %016llx", regs.rax);
    dump_reg_memory(pid, regs.rax);
    printf("rbx    : %016llx", regs.rbx);
    dump_reg_memory(pid, regs.rbx);
    printf("rcx    : %016llx", regs.rcx);
    dump_reg_memory(pid, regs.rcx);
    printf("rdx    : %016llx", regs.rdx);
    dump_reg_memory(pid, regs.rdx);
    printf("rsp    : %016llx", regs.rsp);
    dump_reg_memory(pid, regs.rsp);
    printf("rbp    : %016llx", regs.rbp);
    dump_reg_memory(pid, regs.rbp);
    printf("rsi    : %016llx", regs.rsi);
    dump_reg_memory(pid, regs.rsi);
    printf("rdi    : %016llx", regs.rdi);
    dump_reg_memory(pid, regs.rdi);
    printf("rip    : %016llx", regs.rip);
    dump_reg_memory(pid, regs.rip);
    printf("r8     : %016llx", regs.r8);
    dump_reg_memory(pid, regs.r8);
    printf("r9     : %016llx", regs.r9);
    dump_reg_memory(pid, regs.r9);
    printf("r10    : %016llx", regs.r10);
    dump_reg_memory(pid, regs.r10);
    printf("r11    : %016llx", regs.r11);
    dump_reg_memory(pid, regs.r11);
    printf("r12    : %016llx", regs.r12);
    dump_reg_memory(pid, regs.r12);
    printf("r13    : %016llx", regs.r13);
    dump_reg_memory(pid, regs.r13);
    printf("r14    : %016llx", regs.r14);
    dump_reg_memory(pid, regs.r14);
    printf("r15    : %016llx", regs.r15);
    dump_reg_memory(pid, regs.r15);
    printf("\neflags : %016llx\n", regs.eflags);
    printf("ss: %04x cs: %04x ds: %04x gs: %04x es: %04x fs: %04x\n",
        (uint32_t)regs.ss, (uint32_t)regs.cs, (uint32_t)regs.ds,
        (uint32_t)regs.gs, (uint32_t)regs.es, (uint32_t)regs.fs
    );
}

/**
 * Copies the default environment and adds a LD_PRELOAD line for the
 * instrumentation library. Then, it starts and ptrace attaches to the
 * debuggee.
 *
 * @param argv Argument values (the command line)
 */
static void spawn_process(char **argv)
{
    char **env = NULL;
    char preload_env[256];
    size_t i = 0;

    memset(preload_env, '\0', sizeof(preload_env));
    snprintf(preload_env, sizeof(preload_env), "LD_PRELOAD=%s", HOOK_LIB_PATH);
    info("Setting up the environment: %s", preload_env);

    /* Get count */
    while (environ[i] != NULL)
        i++;
    env = (char **)malloc(i * sizeof(char *));

    /* Copy the environment variables */
    i = 0;
    while (environ[i] != NULL) {
        env[i] = environ[i];
        i++;
    }

    /* Append LD_PRELOAD */
    env[i] = preload_env;
    env[i+1] = NULL;

    info("Executing process (%s) ...\n", argv[0]);
    ptrace(PTRACE_TRACEME, 0, NULL, NULL);
    kill(getpid(), SIGSTOP);
    execve(argv[0], argv, env);

    /* execve only returns on failure */
    err("Failed to execute binary");
    exit(1);
}

/**
 * Parse status integer
 *
 * @param status Status integer
 * @return Integer representing crashed, exited, or unknown/unhandled status
 */
static estat_t status_type(int status)
{
    if (WIFSTOPPED(status)) {
        /* Process crashed! */
        if (WSTOPSIG(status) == SIGSEGV)
            return CRASHED;
    }

    /* Process exited */
    if (WIFEXITED(status))
        return EXITED;

    return UNKNOWN;
}

/**
 * Wait on process and handle signals
 *
 * @param pid Debuggee process ID
 * @return 1 if the process exited, 0 if it crashed
 */
static int monitor_execution(pid_t pid)
{
    int ret = 1;
    int status, st;

    waitpid(pid, &status, 0);
    while (_continue) {
        ptrace(PTRACE_CONT, pid, 0, 0);
        waitpid(pid, &status, 0);

        st = status_type(status);
        if (st == CRASHED) {
            info("Process has crashed with SIGSEGV");
            return 0;
        }

        if (st == EXITED) {
            info("Process has exited");
            return 1;
        }
    }
    return ret;
}

/**
 * Main
 *
 * @param argc Argument count
 * @param argv Argument values
 * @return 0 for success, 1 for failure
 */
int main(int argc, char **argv)
{
    int ret = 1;
    int pid;

    if (argc < 2) {
        printf("./ich [cmd]\n");
        goto done;
    }

    if (init_crash_harness())
        goto done;

    pid = fork();
    if (!pid) {
        /* This won't return */
        spawn_process(&argv[1]);
    } else {
        if (!monitor_execution(pid)) {
            display_crash_dump(pid);
        }
        ptrace(PTRACE_DETACH, pid, 0, 0);
    }

    ret = 0;
done:
    return ret;
}
