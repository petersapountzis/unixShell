/* CMPS2300 Shell Lab
 * tsh - A tiny shell program with job control
 *
 * Put your name and Tulane email address here
 * psapountzis@tulane.edu
 * Peter Sapountzis
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
#define MAXJID    1<<16   /* max job ID */

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
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* The job struct */
    pid_t pid;              /* job PID */
    int jid;                /* job ID [1, 2, ...] */
    int state;              /* UNDEF, BG, FG, or ST */
    char cmdline[MAXLINE];  /* command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */
/* End global variables */


/* Function prototypes */

/* Here are the functions that you will implement */
void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void do_redirect(char **argv);
void waitfg(pid_t pid);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, char **argv);
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs);
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
int main(int argc, char **argv)
{
    char c;
    char cmdline[MAXLINE];
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(1, 2);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
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

    /* These are the ones you will need to implement */
    Signal(SIGINT,  sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */

    /* Ignoring these signals simplifies reading from stdin/stdout */
    Signal(SIGTTIN, SIG_IGN);          /* ignore SIGTTIN */
    Signal(SIGTTOU, SIG_IGN);          /* ignore SIGTTOU */


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
void eval(char *cmdline)
{
    char *argv[MAXLINE];
    int bg = parseline(cmdline, argv); /* set to true if parseline determines the command shld be running in background, false if foreground */
    pid_t pid;

    /* gets the SIGCHLD mask ready to avoid race conditions */
    sigset_t mask, prev_mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);


    /* In order to ignore whitespace, we just do nothing when white space (null values). */
    if (argv[0]== NULL){
        return;
    }

    /* Check if the command is a built-in command. If not, fork a child process and execute the specified program. */
    if (!builtin_cmd(argv)){

        /* Block the SIGCHLD signal while we are adding the job to the job list and executing the command. */
        sigprocmask(SIG_BLOCK, &mask, &prev_mask);
        // fork & exec specified program
        pid = fork();

        if (pid == 0){ // pid = 0 means child process
            setpgid(0,0); // set child process group PID to 0 to prevent race conditions
            /* We are in the child process. Unblock the SIGCHLD signal and execute the command. */
            sigprocmask(SIG_UNBLOCK, &mask, &prev_mask);
            do_redirect(argv);
            if (execve(argv[0], argv, environ) < 0){ // execve will return -1 if there was an error.
                /* If there was an error executing the command, print an error message and exit. */
                printf("%s: Command not found\n", argv[0]);
                exit(0);
            }
        }
        else {
             /* We are in the parent process. Add the job to the job list and either wait for it to finish
                or print a message indicating that it is running in the background. */
            if (!bg){ // Foreground
              /* Add the job to the job list, unblock the SIGCHLD signal, and wait for the job to finish. */
                addjob(jobs, pid, FG, cmdline);
                sigprocmask(SIG_UNBLOCK, &mask, &prev_mask);
                waitfg(pid); // wait for any previous foreground jobs to finish

            }
            else {
                /* Add the job to the job list, unblock the SIGCHLD signal,
                and print a message indicating that it is running in the background. */
                addjob(jobs, pid, BG, cmdline);
                sigprocmask(SIG_UNBLOCK, &mask, &prev_mask);
                int jobid = pid2jid(pid);
                printf("[%d] (%d) %s", jobid, pid, cmdline);
            }
        }
    }
    return;
}

/*
 * parseline - Parse the command line and build the argv array.
 *
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested a FG job.
 */
int parseline(const char *cmdline, char **argv)  // DONE
{
    static char array[MAXLINE]; /* holds local copy of command line */
    char *buf = array;          /* ptr that traverses command line */
    char *delim;                /* points to first space delimiter */
    int argc;                   /* number of args */
    int bg;                     /* background job? */

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

    if (argc == 0)  /* ignore blank line */
	return 1;

    /* should the job run in the background? */
    if ((bg = (*argv[argc-1] == '&')) != 0) {
	argv[--argc] = NULL;
    }
    return bg;
}

/*
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately.
 */
int builtin_cmd(char **argv) // DONE
{
    if (strcmp(argv[0], "quit") == 0) { /* if command entered is "quit", immediately exit the process. */
        exit(0);
    }
    else if (strcmp(argv[0], "jobs") == 0){ /*if "jobs", call the listjobs function which is predefined. */
        listjobs(jobs);
        return 1;
    }
    /* if either bg or fg, enter the do_bgfg function which has been implemented. */
    else if (strcmp(argv[0], "bg") == 0){
        do_bgfg(argv);
        return 1;
    }
    else if (strcmp(argv[0], "fg") == 0){
        do_bgfg(argv);
        return 1;

    }
    return 0;
}


/*
 * do_redirect - scans argv for any use of < or > which indicate input or output redirection
 *
 */
void do_redirect(char **argv)
{
    int i;
    for(i=0; argv[i]; i++){
        if (!strcmp(argv[i],"<")) {
            /* add code for input redirection below */

            //parse argv to get file name
            char *input_file = argv[i+1];
            int input_f = open(input_file, O_RDWR | O_TRUNC | O_CREAT, 0);
            // read, write, truncate, or create with edit permissions to all
            if (input_f < 0){
                printf("error");
                exit(1);
            }

            //file descriptor
            if (dup2(input_f, STDIN_FILENO) < 0){
                printf("error");
                exit(1);
            }
            /* the line below cuts argv short. This
                removes the < and whatever follows from argv */
            argv[i]=NULL;
            close(input_f);
        }
        else if (!strcmp(argv[i],">")) {
            /* add code for output redirection here */

            //parse argv to get file name
            char *output_file = argv[i + 1];
            int output_f = open(output_file, O_RDWR | O_TRUNC | O_CREAT, 0);
            // read, write, truncate, or create with edit permissions to all
            if (output_f < 0){
                printf("error");
                exit(1);
            }
            // file descriptor
            if (dup2(output_f, STDOUT_FILENO) < 0){
                printf("error");
                exit(1);
            }

            /* the line below cuts argv short. This removes the > and whatever follows from argv */
            argv[i]=NULL;
            close(output_f);
        }
    }
}

/*
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv)
{
    int jid_call;
    struct job_t *currentJob;

    if (argv[1] == NULL) { //if there is no second index in argv, it means no info was placed after bg/fg.
        printf("%s command requires PID or %%jobid argument\n", argv[0]);
        return;
    }

    if (!isdigit(argv[1][0]) && argv[1][0] != '%') {
        /* it is either an int (pid) or % (JID), if neither there is an error*/
        /* this checks to make sure we are either using a pid for jid after the fg/bg */
        printf("%s: argument must be a PID or %%jobid\n", argv[0]);
        return;
    }

    /* to check whether there is a % (meaning they are going to be using JID not PID) */
    if (argv[1][0] == '%'){
        jid_call = 1; // true
    }
    else{
        jid_call = 0; // false
    }


    if (jid_call) { // if % present, look for JID value
    /* get the job by parsing argv to get JID #. Will be second index of second arg, hence [1][1]*/
        currentJob = getjobjid(jobs, atoi(&argv[1][1]));
        /*if there is no job with that JID, return error statement*/
        if (currentJob == NULL) {
            printf("%s: No such job\n", argv[1]);
            return;
        }
    } else {  //if we are going to be using PID
        pid_t pid_1 = atoi(argv[1]); // get the PID by turning second arg into integer, then into pid_t type.
        currentJob = getjobpid(jobs, pid_1); // retrieve job
        /* if job DNE, return error statement */
        if (currentJob == NULL) {
            printf("(%d): No such process\n", pid_1);
            return;
        }
    }


    if(strcmp(argv[0], "bg") == 0) {  // BACKGROUND CALL
        currentJob->state = BG;  // change state to BG
        printf("[%d] (%d) %s", currentJob->jid, currentJob->pid, currentJob->cmdline);
        kill(-currentJob->pid, SIGCONT);  // send SIGCONT to processes telling it to resume
    }
     else if (strcmp(argv[0], "fg") == 0){ // FOREGROUND CALL
        currentJob->state = FG; // change state to FG
        kill(-currentJob->pid, SIGCONT); // send SIGCONT to processes telling it to resume
        waitfg(currentJob->pid); // make sure all current FG processes are done
    }

    return;

}

/*
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid) // DONE
{
    while (fgpid(jobs) == pid){ // while fgpid is the pid passed in, sleep and keep checking until it is removed
        sleep(1);
    }
    return;
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
void sigchld_handler(int sig) // DONE
{
    pid_t pid;
    int stat; // need this for waitpid to return status id

    // Use waitpid to reap all available zombie children
    /* WAITPID PARAMETER EXPLANATION:
    -1: the process ID to wait for (in this case, -1 indicates any child process)
    &stat: pointer to int variable to store exit status of child process
    WNOHANG: tells waitpid() to return immediately if there are no child processes that have changed state. Prevents sig handler from blocking main process
    WUNTRACED: return if a child process has been stopped (but not necessarily terminated)
    */
    while ((pid = waitpid(-1, &stat, WNOHANG|WUNTRACED)) > 0) {
        // Check if the child process terminated normally or due to a signa
        if (WIFSIGNALED(stat)) { //SIGINT catcher
            int jobid = pid2jid(pid); //jid needed for return message
            printf("Job [%d] (%d) terminated by signal 2\n", jobid, pid); // standard message to be called based on traces
            deletejob(jobs, pid); // delete the job if terminated

        }
        else if (WIFSTOPPED(stat)) {  //SIGSTP catcher
            getjobpid(jobs, pid)->state = ST; // change job state to STOPPED so we get the proper message when `jobs` is called.
            int jobid = pid2jid(pid); // jid needed for return message
            printf("Job [%d] (%d) stopped by signal 20\n", jobid, pid); // standard message to be called based on traces


        }
        else { /* if the child completed with no issues, just remove it from the job list. */
            deletejob(jobs, pid); // Delete the child from the job list
        }

    return;
    }
}


/*
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.
 */
void sigint_handler(int sig) // DONE
{
    pid_t pid = fgpid(jobs);
    if (pid != 0){ // if pid == 0, there is no process currently running in the foreground.
        kill(-pid, sig);
    }
    return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.
 */
void sigtstp_handler(int sig) // DONE
{
    pid_t pid = fgpid(jobs);
    if (pid != 0){ // if pid == 0, there is no process currently running in the foreground.
        kill(-pid, sig); /*use -PID because a PID is always a positive integer, and the process group ID is always a negative integer. */
    }

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

/* maxjid - Returns largest allocated job ID */
int maxjid(struct job_t *jobs)
{
    int i, max=0;

    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].jid > max)
	    max = jobs[i].jid;
    return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline)
{
    int i;

    if (pid < 1)
	return 0;

    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid == 0) {
	    jobs[i].pid = pid;
	    jobs[i].state = state;
	    jobs[i].jid = nextjid++;
	    if (nextjid > MAXJOBS)
		nextjid = 1;
	    strcpy(jobs[i].cmdline, cmdline);
  	    if(verbose){
	        printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
            }
            return 1;
	}
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid)
{
    int i;

    if (pid < 1)
	return 0;

    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid == pid) {
	    clearjob(&jobs[i]);
	    nextjid = maxjid(jobs)+1;
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
int pid2jid(pid_t pid)
{
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
void listjobs(struct job_t *jobs)
{
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
 * usage - print a help message
 */
void usage(void)
{
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg)
{
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg)
{
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler)
{
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
void sigquit_handler(int sig)
{
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}
