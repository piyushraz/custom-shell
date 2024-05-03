/* 
 * tsh - A tiny shell program with job control
 * 
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */

/* 
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Global variables */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* Per-job data */
    pid_t pid;              /* job PID */
    int jid;                /* job ID [1, 2, ...] */
    int state;              /* UNDEF, FG, BG, or ST */
    char cmdline[MAXLINE];  /* command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */

volatile sig_atomic_t ready; /* Is the newest child in its own process group? */

/* End global variables */


/* Function prototypes */

/* Here are the functions that you will implement */
void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);
void sigchld_handler(int sig);
void sigint_handler(int sig);
void sigtstp_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, char **argv); 
void sigquit_handler(int sig);
void sigusr1_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int freejid(struct job_t *jobs); 
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid); 
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid); 
int pid2jid(pid_t pid); 
void listjobs(struct job_t *jobs);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/*
 * main - The shell's main routine 
 */
int main(int argc, char **argv) {
    char c;
    char cmdline[MAXLINE];
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(STDOUT_FILENO, STDERR_FILENO);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != -1) {
        switch (c) {
            case 'h':             /* print help message */
                usage();
                break;
            case 'v':             /* emit additional diagnostic info */
                verbose = 1;
                break;
            case 'p':             /* don't print a prompt */
                emit_prompt = 0;  /* handy for automatic testing */
                break;
            default:
                usage();
        }
    }

    /* Install the signal handlers */

    Signal(SIGUSR1, sigusr1_handler); /* Child is ready */

    /* These are the ones you will need to implement */
    Signal(SIGINT,  sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */

    /* This one provides a clean way to kill the shell */
    Signal(SIGQUIT, sigquit_handler); 

    /* Initialize the job list */
    initjobs(jobs);

    /* Execute the shell's read/eval loop */
    while (1) {

        /* Read command line */
        if (emit_prompt) {
            printf("%s", prompt);
            fflush(stdout);
        }
        if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
            app_error("fgets error");
        if (feof(stdin)) { /* End of file (ctrl-d) */
            fflush(stdout);
            exit(0);
        }

        /* Evaluate the command line */
        eval(cmdline);
        fflush(stdout);
    } 
    exit(0); /* control never reaches here */
}
  
/* 
 * eval - Evaluate the command line that the user has just typed in
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
*/
void eval(char *cmdline) {
    char *argument[MAXARGS]; // Command Line Arguments
    int numargs; // Contains number of arguments for argument
    pid_t pid, pid2; // Process ID for the children
    int bgorfg; // 1 for background, 0 for foreground
    sigset_t mask_all, prev_all; // For blocking and containing a signal
    int pipefd[2]; // Create 2 File Descriptors
    int fd_input, fd_output; // Input and Output File Redirection
    char *argument2[MAXARGS]; // Second command line argument after | operator
    int is_pipe = 0; // Check if pipe is created
    numargs = parseline(cmdline, argument); // Parse command line with argument

    sigemptyset(&mask_all); // No Signal Set, Block Nothing As of Now 
    sigaddset(&mask_all, SIGCHLD); // Signal Mask for SIGCHILD, Blocked 
    sigaddset(&mask_all, SIGINT); // CTRL + C, Signal Mask for SIGINT, Blocked
    sigaddset(&mask_all, SIGTSTP); // CTRL + Z, Signal Mask for SIGTSTP, Blocked
    sigprocmask(SIG_BLOCK, &mask_all, &prev_all); // Saves previous masks in prev, applies block to all masked members in set

    if (!builtin_cmd(argument)) { // Chekcing to see if command is built-in or not. (0 - not built-in, 1 - built-in)
        if (argument[numargs - 1] != NULL && strcmp(argument[numargs - 1], "&") == 0) { // Checks to see if & exists at end (For BG)
            argument[numargs - 1] = NULL; // Remove & before execution, error prevention
            bgorfg = 1; // Run command in background
        } else {
            bgorfg = 0; // Run command in foreground
        }
        int input_redirection = 0, output_redirection = 0; // Checks if input (<) or output (>) is called
        for (int i = 0; argument[i] != NULL; i++) { // Loops over arguments till NULL reached
            if (strcmp(argument[i], "|") == 0) { // Check if pipe logic symbol exists
                is_pipe = 1; // If it does, set is_pipe to 1 indicating pipe exists
                argument[i] = NULL; // Set that pipe logic symbol to NULL
                int j; // New int j for nested for loop
                for (i = i + 1, j = 0; argument[i] != NULL; i++, j++) { // Loops over command after pipe operator
                    argument2[j] = argument[i]; // Sets argument2 to argument to copy command into second array
                }
                argument2[j] = NULL; // End of argument2 reached
                break;
            } else if (strcmp(argument[i], "<") == 0) { // Checks if argument contains input redirection
                input_redirection = 1; // Set input redirection value to 1
                argument[i] = NULL; // Remove < from argument
                fd_input = open(argument[i+1], O_RDONLY, 0); // Open file at argument[i + 1] with read only, storing file drescriptor
                if (fd_input < 0) { // If less than 0, error encountered
                    unix_error("Error opening file for input-redirect"); // Unix error and std-error
                    return; // End execution due to error
                }
            } else if (strcmp(argument[i], ">") == 0) { // Checks if argument contains output redirection
                output_redirection = 1; // Set output redirection value to 1
                argument[i] = NULL; // Remove > from argument
                fd_output = open(argument[i+1], O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH); 
                // fd_output: Opens or creates file for reading with read/write permissions to enable it
                if (fd_output == -1) { // If less than 0, error encountered
                    perror("Error opening file for output-redirect"); // Unix error and std-error
                    return; // End execution due to error
                }
            }
        }

        if (is_pipe && pipe(pipefd) < 0) { // Checks if command contains pipe operator, and creates a pipe if true.
            unix_error("Error: Pipe Error"); // Pipe error if less than 1 or -1.
            return; // Ends execution for command handle
        }

        if ((pid = fork()) == 0) { // Creates a child process, sets pid to fork, if pid/fork = 0 then enter child process
            setpgid(0, 0); // Sets process group id of child process to pid.
            if (is_pipe) { // Check if pipe exists.
                close(pipefd[0]); // Closes read end for child process pipe
                dup2(pipefd[1], STDOUT_FILENO); // Redirection of child STDOUT_FILE to write end
                close(pipefd[1]); // Close write end for child process pipe, once STDOUT is established
            } 
            if (input_redirection) { // Checks if input redirection exists
                dup2(fd_input, STDIN_FILENO); // Redirection of STDIN to fd_input
                close(fd_input); // Closes fd_input file descriptor
            }
            if (output_redirection) { // Checks if output redirection exists
                dup2(fd_output, STDOUT_FILENO); // Redirection of STDOUT to fd_output
                close(fd_output); // Closes fd_output file descriptor
            }
            sigprocmask(SIG_SETMASK, &prev_all, NULL);  // Brings back signals to old state, unblocking blocked signals
            if (execve(argument[0], argument, environ) < 0) { // Executes command, check if error exists or less than 0
                printf("%s: Command not found\n", argument[0]); // If less than 0 then failure command doesnt exist
                exit(0); // Exit the child process upon failure to avoid running shell on failure.
            }
        }

        if (is_pipe) { // Checks if pipe exists
            if ((pid2 = fork()) == 0) { // Fork again for interprocess communication between 2 pipes, second command
                close(pipefd[1]); // Close write-end of child pipe, only need to read.
                dup2(pipefd[0], STDIN_FILENO); // Redirection of read end to STDIN, second command reads from output of first
                close(pipefd[0]); // Close read-end of pipe after duplicated

                if (execve(argument2[0], argument2, environ) < 0) { // Executes command2, check if error exists or less than 0
                    printf("%s: Command not found\n", argument2[0]); // If less than 0 then failure command doesnt exist
                    exit(0); //Exit the child process upon failure to avoid running shell on failure.
                }
            }
            close(pipefd[0]); // Close read ends of pipe for the parent process
            close(pipefd[1]); // Close write ends of pipe for the parent process
        }

        sigprocmask(SIG_SETMASK, &prev_all, NULL); // Unblock signals for parent process, ensure signals work again 

        if (!bgorfg) { // Foreground Job, Checks if job is in background or foreground (1 for bg, 0 for fg)
            addjob(jobs, pid, FG, cmdline); // Foreground job Added, Add command to job if no pipe exists (first command if pipe exists)
            if (is_pipe) { // Check if pipe exists
                addjob(jobs, pid2, FG, cmdline);  // Foreground job Added, If pipe exists add second command as well
            }
            waitfg(pid); // Wait for fg job to finish
            if (is_pipe) { // Check if pipe exists 
                waitfg(pid2); // If pipeo exists, wait for second fg job too
            }
        } else {
            addjob(jobs, pid, BG, cmdline); // Adds Background Job
            printf("[%d] (%d) %s", pid2jid(pid), pid, cmdline); // Prints bg job inforamtion, Job ID, process ID, and command line
            if (is_pipe) { // Check if pipe exists
                addjob(jobs, pid2, BG, cmdline); // Add second argument to Background Job, with pid2.
                printf("[%d] (%d) %s", pid2jid(pid2), pid2, cmdline); // Prints bg job inforamtion, Job ID, process ID, and command line
            }
        }

        if (input_redirection) { // If input redirection open
            close(fd_input); // Close input direction if set.
        }

        if (output_redirection) { // If output redirection open
            close(fd_output); // Close output direction if set.
        }
    }
}


/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return number of arguments parsed.
 */
int parseline(const char *cmdline, char **argv) {
    static char array[MAXLINE]; /* holds local copy of command line */
    char *buf = array;          /* ptr that traverses command line */
    char *delim;                /* points to space or quote delimiters */
    int argc;                   /* number of args */

    strcpy(buf, cmdline);
    buf[strlen(buf)-1] = ' ';  /* replace trailing '\n' with space */
    while (*buf && (*buf == ' ')) /* ignore leading spaces */
        buf++;

    /* Build the argv list */
    argc = 0;
    if (*buf == '\'') {
        buf++;
        delim = strchr(buf, '\'');
    }
    else {
        delim = strchr(buf, ' ');
    }

    while (delim) {
        argv[argc++] = buf;
        *delim = '\0';
        buf = delim + 1;
        while (*buf && (*buf == ' ')) /* ignore spaces */
            buf++;

        if (*buf == '\'') {
            buf++;
            delim = strchr(buf, '\'');
        }
        else {
            delim = strchr(buf, ' ');
        }
    }
    argv[argc] = NULL;
    
    return argc;
}

/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately.  
 */
int builtin_cmd(char **argv) {

    if (strcmp(argv[0], "quit") == 0) { // Check if quit called
        exit(0); // exit if quit is called
    } else if (strcmp(argv[0], "jobs") == 0) { //  Check if jobs called
        listjobs(jobs); // List jobs 
        return 1; // builtin command
    } else if (strcmp(argv[0], "bg") == 0) { // Check if bg is called on job
        do_bgfg(argv); // Execute bg commands
        return 1; // builtin command
    } else if (strcmp(argv[0], "fg") == 0) { // Check if fg is called on job
        do_bgfg(argv); // Execute fg commands
        return 1; // builtin command
    } else if (!strcmp("&", argv[0])){ // Check if command is & only.
        return 1; // builtin command
    }
    return 0;     /* not a builtin command */
}

/* 
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv) {
    struct job_t *job = NULL; // Create job pointer
    char *id = argv[1]; // Get job ID
    char *end; // Parsing for strtol
    long jid; // Job ID long for strtol

    if (argv[1] == NULL) {
            printf("%s command requires PID or %%jid argument\n", argv[0]);
            return;
        }
    if (id == NULL) { // If id is NULL, does not exist
        return; // Exit function
    }

    if (id[0] == '%') { // Job ID starts with % check
        jid = strtol(&id[1], &end, 10);  // Get Job ID from strtol
        if (*end != '\0') { // Check if argument is number (contains)
            printf("%s: [jid] Must be a valid number\n", argv[0]); // Print if not number
            return; // Exit function
        }
        job = getjobjid(jobs, (int)jid); // Get job ID (convert long to int), set to job
        if (job == NULL) { // Check if job is NULL with given id
            printf("%%%ld: No such job\n", jid); // There is no job that exists
            return; // Exit function
        }
    } else if (isdigit(id[0])) { // If no % check if PID is valid digit 
        pid_t pid = strtol(id, &end, 10); // Get pid from strtol
        if (*end != '\0') { // Print if strtol didn't convert properly.
            printf("%s: [pid] Must be a number\n", argv[0]); // Print error if conversion error
            return; // Exit function
        }
        job = getjobpid(jobs, pid); // Get job from PID
        if (job == NULL) { // If Job is NULL or doesnt exists with PID
            printf("(%d): No such process\n", pid); // No process with PID exists
            return; // Exit function
        }
    } else { // If argument is neither PID or JID
        printf("%s: argument must be a PID or %%jid \n", argv[0]); // Print error
        return; // Exit function
    }

    kill(-(job->pid), SIGCONT); // Send SIGCONT signal to the job, continuing it

    if (strcmp("fg", argv[0]) == 0) { // If command is fg, make job a fg job, otherwise bg
        job->state = FG; // Make job a foreground
        waitfg(job->pid); // Wait for fg to complete
    } else {
        job->state = BG; // Otherwise, Make job a background 
        printf("[%d] (%d) %s", job->jid, job->pid, job->cmdline); // Print background job
    }
}

/* 
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid) {
    struct job_t *job = getjobpid(jobs, pid); // Get Job from jobpid 
    if (!job) { // If job doesnt exist
        return; // Exit function
    } 
    sigset_t mask; // Create signal mask set
    sigemptyset(&mask); // Set signal to empty so all signals are unblocked

    while (job->state == FG) { // If job state is fg
        sigsuspend(&mask); // Suspend process until unblocked signal is caught
        job = getjobpid(jobs, pid); // Get job from jobspid
        if (job == NULL) { // Check if job exists
            break; // If job no longer exists after signal is caught, exit while loop
        } 
    }
}


/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.  
 */
void sigchld_handler(int sig) {
    int status; // Status variable for signals
    pid_t pid; // PID for child that was exited or stopped
    
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) { // Loop through zombie child
    struct job_t *job = getjobpid(jobs, pid); // Create struct that get jobs with child process
    if (WIFSTOPPED(status)) { // If Child stopped by signal
        if (job != NULL) { // If job exists with PID 
            job->state = ST; // Change job state to stopped
            printf("Job [%d] (%d) stopped by signal %d\n", job->jid, pid, WSTOPSIG(status)); // Print signal that stopped job
        }
    } else if (WIFSIGNALED(status)) { // If child terminated by signal
        if (job != NULL) { // If job exists with PID
            printf("Job [%d] (%d) terminated by signal %d\n", job->jid, pid, WTERMSIG(status)); // Print signal that stopped
            deletejob(jobs, pid); // Remove  job from the jobs list
        }
    } else if (WIFEXITED(status)) { // If child terminated normally  
        if (job != NULL) { // If job exists with PID
            deletejob(jobs, pid); // Remove job from the jobs list
        }
    }
}

}


/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenever the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void sigint_handler(int sig) {

    pid_t fg_pid = fgpid(jobs);
    if (fg_pid != 0) {
        kill(-fg_pid, SIGINT);
    }
}


/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void sigtstp_handler(int sig) {
    pid_t fg_pid = fgpid(jobs);
    if (fg_pid != 0) {
        kill(-fg_pid, SIGTSTP);
    }
}

/*
 * sigusr1_handler - child is ready
 */
void sigusr1_handler(int sig) {
    ready = 1;
}


/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) {
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
        clearjob(&jobs[i]);
}

/* freejid - Returns smallest free job ID */
int freejid(struct job_t *jobs) {
    int i;
    int taken[MAXJOBS + 1] = {0};
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].jid != 0) 
        taken[jobs[i].jid] = 1;
    for (i = 1; i <= MAXJOBS; i++)
        if (!taken[i])
            return i;
    return 0;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) {
    int i;
    
    if (pid < 1)
        return 0;
    int free = freejid(jobs);
    if (!free) {
        printf("Tried to create too many jobs\n");
        return 0;
    }
    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == 0) {
            jobs[i].pid = pid;
            jobs[i].state = state;
            jobs[i].jid = free;
            strcpy(jobs[i].cmdline, cmdline);
            if(verbose){
                printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
            }
            return 1;
        }
    }
    return 0; /*suppress compiler warning*/
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid) {
    int i;

    if (pid < 1)
        return 0;

    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == pid) {
            clearjob(&jobs[i]);
            return 1;
        }
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].state == FG)
            return jobs[i].pid;
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
    int i;

    if (pid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].pid == pid)
            return &jobs[i];
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid) 
{
    int i;

    if (jid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].jid == jid)
            return &jobs[i];
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid) {
    int i;

    if (pid < 1)
        return 0;
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].pid == pid) {
            return jobs[i].jid;
    }
    return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs) {
    int i;
    
    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid != 0) {
            printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
            switch (jobs[i].state) {
                case BG: 
                    printf("Running ");
                    break;
                case FG: 
                    printf("Foreground ");
                    break;
                case ST: 
                    printf("Stopped ");
                    break;
                default:
                    printf("listjobs: Internal error: job[%d].state=%d ", 
                       i, jobs[i].state);
            }
            printf("%s", jobs[i].cmdline);
        }
    }
}
/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message and terminate
 */
void usage(void) {
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg) {
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg) {
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler) {
    struct sigaction action, old_action;

    action.sa_handler = handler;  
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
        unix_error("Signal error");
    return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig) {
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}


