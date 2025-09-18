Simple Unix-like shell:
 - builtins: cd, exit
 - pipelines, redirection: > >> <, |
 - background jobs with &
 - basic signal handling (SIGINT, SIGCHLD)
 
Compile: gcc -Wall -Wextra -std=gnu11 -o myshell myshell.c
Run: ./myshell
