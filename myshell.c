/* myshell.c
 *
 * Simple Unix-like shell:
 *  - builtins: cd, exit
 *  - pipelines, redirection: > >> <, |
 *  - background jobs with &
 *  - basic signal handling (SIGINT, SIGCHLD)
 *
 * Compile: gcc -Wall -Wextra -std=gnu11 -o myshell myshell.c
 * Run: ./myshell
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>

#define MAX_TOKENS 256
#define MAX_JOBS 128
#define PROMPT "myshell$ "

typedef struct {
    pid_t pid;
    char cmdline[512];
    int running;
} job_t;

static job_t jobs[MAX_JOBS];

static void add_job(pid_t pid, const char *cmdline) {
    for (int i = 0; i < MAX_JOBS; ++i) {
        if (!jobs[i].running) {
            jobs[i].pid = pid;
            strncpy(jobs[i].cmdline, cmdline, sizeof(jobs[i].cmdline)-1);
            jobs[i].running = 1;
            printf("[%d] %d\n", i+1, pid);
            return;
        }
    }
    fprintf(stderr, "job list full\n");
}

static void mark_job_done(pid_t pid, int status) {
    for (int i = 0; i < MAX_JOBS; ++i) {
        if (jobs[i].running && jobs[i].pid == pid) {
            jobs[i].running = 0;
            if (WIFEXITED(status)) {
                printf("\nJob [%d] %d finished (exit %d): %s\n", i+1, pid, WEXITSTATUS(status), jobs[i].cmdline);
            } else if (WIFSIGNALED(status)) {
                printf("\nJob [%d] %d killed by signal %d: %s\n", i+1, pid, WTERMSIG(status), jobs[i].cmdline);
            }
            fflush(stdout);
            return;
        }
    }
}

/* SIGCHLD handler: reap children without blocking */
static void sigchld_handler(int sig) {
    (void)sig;
    int saved_errno = errno;
    while (1) {
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0) break;
        mark_job_done(pid, status);
    }
    errno = saved_errno;
}

/* Ignore SIGINT in shell; children inherit default so Ctrl-C kills them */
static void sigint_handler(int sig) {
    (void)sig;
    /* do nothing: avoid exiting the shell on Ctrl-C */
    write(STDOUT_FILENO, "\n", 1);
}

/* Simple tokenizer: splits input into tokens separated by whitespace,
   but treats > >> < | & as separate tokens even when adjacent */
static int tokenize(const char *line, char *tokens[], int max_tokens) {
    char *s = strdup(line);
    if (!s) return 0;
    int n = 0;
    char *p = s;
    while (*p && n < max_tokens-1) {
        while (*p && (*p == ' ' || *p == '\t' || *p == '\n')) p++;
        if (!*p) break;
        if (*p == '>' || *p == '<' || *p == '|' || *p == '&') {
            if (*p == '>' && *(p+1) == '>') {
                tokens[n++] = strdup(">>"); p += 2;
            } else {
                char tmp[3] = {*p, 0, 0};
                tokens[n++] = strdup(tmp); p++;
            }
            continue;
        }
        /* regular word */
        char *start = p;
        int inquote = 0;
        char quotechar = 0;
        while (*p) {
            if (!inquote && (*p == ' ' || *p == '\t' || *p == '\n')) break;
            if (!inquote && (*p == '\'' || *p == '"')) {
                inquote = 1; quotechar = *p; p++; continue;
            }
            if (inquote && *p == quotechar) { inquote = 0; quotechar = 0; p++; continue; }
            if (!inquote && (*p == '>' || *p == '<' || *p == '|' || *p == '&')) break;
            p++;
        }
        int len = p - start;
        char *tok = malloc(len+1);
        int ti = 0;
        for (int i = 0; i < len; ++i) {
            char c = start[i];
            if (c == '\'' || c == '"') continue; /* strip quotes */
            tok[ti++] = c;
        }
        tok[ti] = 0;
        tokens[n++] = tok;
    }
    tokens[n] = NULL;
    free(s);
    return n;
}

/* Free tokens */
static void free_tokens(char *tokens[], int n) {
    for (int i = 0; i < n; ++i) free(tokens[i]);
}

/* Structure describing a single command in a pipeline */
typedef struct {
    char *argv[MAX_TOKENS];
    char *infile;
    char *outfile;
    int append; /* for >> */
} cmd_t;

/* Parse tokens into cmd_t array (pipeline), and detect background flag */
static int parse_commands(char *tokens[], int ntok, cmd_t cmds[], int *ncmds, int *background) {
    int ci = 0;
    int ai = 0;
    cmds[ci].infile = NULL;
    cmds[ci].outfile = NULL;
    cmds[ci].append = 0;
    for (int i = 0; i < MAX_TOKENS; ++i) cmds[ci].argv[i] = NULL;

    *background = 0;
    *ncmds = 0;

    for (int i = 0; i < ntok; ++i) {
        char *t = tokens[i];
        if (strcmp(t, "&") == 0) {
            *background = 1;
            continue;
        } else if (strcmp(t, "|") == 0) {
            cmds[ci].argv[ai] = NULL;
            ci++;
            if (ci >= MAX_TOKENS) { fprintf(stderr, "too many pipeline segments\n"); return -1; }
            ai = 0;
            cmds[ci].infile = NULL; cmds[ci].outfile = NULL; cmds[ci].append = 0;
            for (int j = 0; j < MAX_TOKENS; ++j) cmds[ci].argv[j] = NULL;
            continue;
        } else if (strcmp(t, "<") == 0) {
            if (i+1 >= ntok) { fprintf(stderr, "syntax error: < needs file\n"); return -1; }
            cmds[ci].infile = tokens[++i];
            continue;
        } else if (strcmp(t, ">") == 0 || strcmp(t, ">>") == 0) {
            int app = (strcmp(t, ">>") == 0);
            if (i+1 >= ntok) { fprintf(stderr, "syntax error: > needs file\n"); return -1; }
            cmds[ci].outfile = tokens[++i];
            cmds[ci].append = app;
            continue;
        } else {
            cmds[ci].argv[ai++] = t;
        }
    }
    cmds[ci].argv[ai] = NULL;
    *ncmds = ci + 1;
    return 0;
}

/* Check and run builtin; return 1 if builtin executed, 0 otherwise */
static int run_builtin(cmd_t *c) {
    if (!c->argv[0]) return 0;
    if (strcmp(c->argv[0], "cd") == 0) {
        char *dir = c->argv[1] ? c->argv[1] : getenv("HOME");
        if (chdir(dir) < 0) perror("cd");
        return 1;
    }
    if (strcmp(c->argv[0], "exit") == 0) {
        exit(0);
    }
    if (strcmp(c->argv[0], "jobs") == 0) {
        for (int i = 0; i < MAX_JOBS; ++i) {
            if (jobs[i].running) {
                printf("[%d] %d  %s\n", i+1, jobs[i].pid, jobs[i].cmdline);
            }
        }
        return 1;
    }
    return 0;
}

/* Execute pipeline of ncmds commands in cmds[]. background flag determines wait behavior.
   cmdline is supplied for job bookkeeping. */
static void execute_pipeline(cmd_t cmds[], int ncmds, int background, const char *cmdline) {
    int pipe_fd[2];
    int prev_fd = -1; /* read end of previous pipe */
    pid_t last_pid = -1;

    /* If single command and builtin -> run in parent (unless background?) */
    if (ncmds == 1 && run_builtin(&cmds[0])) return;

    for (int i = 0; i < ncmds; ++i) {
        if (i < ncmds - 1) {
            if (pipe(pipe_fd) < 0) { perror("pipe"); return; }
        } else {
            pipe_fd[0] = pipe_fd[1] = -1;
        }

        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return; }

        if (pid == 0) {
            /* Child */
            /* restore default SIGINT so Ctrl-C kills child */
            signal(SIGINT, SIG_DFL);

            if (prev_fd != -1) {
                dup2(prev_fd, STDIN_FILENO);
                close(prev_fd);
            }
            if (pipe_fd[1] != -1) {
                dup2(pipe_fd[1], STDOUT_FILENO);
                close(pipe_fd[1]);
            }

            /* Input redir */
            if (cmds[i].infile) {
                int fd = open(cmds[i].infile, O_RDONLY);
                if (fd < 0) { perror("open infile"); exit(1); }
                dup2(fd, STDIN_FILENO);
                close(fd);
            }
            /* Output redir */
            if (cmds[i].outfile) {
                int flags = O_CREAT | O_WRONLY | (cmds[i].append ? O_APPEND : O_TRUNC);
                int fd = open(cmds[i].outfile, flags, 0644);
                if (fd < 0) { perror("open outfile"); exit(1); }
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }

            /* Close unused fds in child */
            if (pipe_fd[0] != -1) close(pipe_fd[0]);

            /* Exec */
            if (!cmds[i].argv[0]) exit(0);
            execvp(cmds[i].argv[0], cmds[i].argv);
            perror("execvp");
            exit(127);
        } else {
            /* Parent */
            last_pid = pid;
            if (prev_fd != -1) close(prev_fd);
            if (pipe_fd[1] != -1) close(pipe_fd[1]);
            prev_fd = pipe_fd[0];
        }
    }

    if (background) {
        /* record last child as job */
        add_job(last_pid, cmdline);
    } else {
        /* wait for last child (simple behavior) */
        int status;
        while (waitpid(last_pid, &status, 0) < 0) {
            if (errno == EINTR) continue;
            perror("waitpid");
            break;
        }
    }
}

int main(void) {
    /* initialize jobs */
    for (int i = 0; i < MAX_JOBS; ++i) jobs[i].running = 0;

    /* setup signal handlers */
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    struct sigaction sa2;
    sa2.sa_handler = sigint_handler;
    sigemptyset(&sa2.sa_mask);
    sa2.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa2, NULL);

    char *line = NULL;
    size_t len = 0;

    while (1) {
        /* print prompt */
        if (isatty(STDIN_FILENO)) {
            printf(PROMPT);
            fflush(stdout);
        }

        ssize_t nread = getline(&line, &len, stdin);
        if (nread < 0) {
            if (feof(stdin)) { putchar('\n'); break; }
            perror("getline");
            continue;
        }
        /* trim newline */
        if (nread > 0 && line[nread-1] == '\n') line[nread-1] = '\0';

        /* skip empty */
        char *trim = line;
        while (*trim == ' ' || *trim == '\t') trim++;
        if (*trim == '\0') continue;

        /* Tokenize */
        char *tokens[MAX_TOKENS];
        int ntok = tokenize(trim, tokens, MAX_TOKENS);
        if (ntok <= 0) continue;

        /* Parse into commands */
        cmd_t cmds[MAX_TOKENS];
        int ncmds = 0;
        int background = 0;
        if (parse_commands(tokens, ntok, cmds, &ncmds, &background) < 0) {
            free_tokens(tokens, ntok);
            continue;
        }

        /* If single builtin, handle in parent */
        if (ncmds == 1 && run_builtin(&cmds[0])) {
            free_tokens(tokens, ntok);
            continue;
        }

        /* Execute pipeline */
        execute_pipeline(cmds, ncmds, background, trim);

        free_tokens(tokens, ntok);
    }

    free(line);
    return 0;
}
