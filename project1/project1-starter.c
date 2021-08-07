#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>

#define MAX_LINE 80
#define MAX_ARGS (MAX_LINE/2 + 1)
#define REDIRECT_OUT_OP '>'
#define REDIRECT_IN_OP '<'
#define PIPE_OP '|'
#define BG_OP '&'

/* Holds a single command. */
typedef struct Cmd {
	/* The command as input by the user. */
	char line[MAX_LINE + 1];
	/* The command as null terminated tokens. */
	char tokenLine[MAX_LINE + 1];
	/* Pointers to each argument in tokenLine, non-arguments are NULL. */
	char* args[MAX_ARGS];
	/* Pointers to each symbol in tokenLine, non-symbols are NULL. */
	char* symbols[MAX_ARGS];
	/* The process id of the executing command. */
	pid_t pid;

	/* Additional fields may be helpful. */
	int done;
	int printed;
	int stopped;

} Cmd;

/* The process of the currently executing foreground command, or 0. */
pid_t foregroundPid = 0;

int bgJobSize = 0;

Cmd** jobs;
Cmd* temp;
/* Parses the command string contained in cmd->line.
 * * Assumes all fields in cmd (except cmd->line) are initailized to zero.
 * * On return, all fields of cmd are appropriatly populated. */
void parseCmd(Cmd* cmd) {
	char* token;
	int i=0;
	strcpy(cmd->tokenLine, cmd->line);
	strtok(cmd->tokenLine, "\n");
	token = strtok(cmd->tokenLine, " ");
	while (token != NULL) {
		if (*token == '\n') {
			cmd->args[i] = NULL;
		} else if (*token == REDIRECT_OUT_OP || *token == REDIRECT_IN_OP
				|| *token == PIPE_OP || *token == BG_OP) {
			cmd->symbols[i] = token;
			cmd->args[i] = NULL;
		} else {
			cmd->args[i] = token;
		}
		token = strtok(NULL, " ");
		i++;
	}
	cmd->args[i] = NULL;
}

/* Finds the index of the first occurance of symbol in cmd->symbols.
 * * Returns -1 if not found. */
int findSymbol(Cmd* cmd, char symbol) {
	for (int i = 0; i < MAX_ARGS; i++) {
		if (cmd->symbols[i] && *cmd->symbols[i] == symbol) {
			return i;
		}
	}
	return -1;
}

/* Signal handler for SIGTSTP (SIGnal - Terminal SToP),
 * which is caused by the user pressing control+z. */
void sigtstpHandler(int sig_num) {
	/* Reset handler to catch next SIGTSTP. */
	if (foregroundPid > 0) {
		/* Foward SIGTSTP to the currently running foreground process. */
		kill(foregroundPid, SIGTSTP);
		/* Add foreground command to the list of jobs. */
		bgJobSize++;
		jobs = (Cmd**) realloc(jobs, bgJobSize * sizeof(Cmd*));
		jobs[bgJobSize-1] = temp;
		jobs[bgJobSize-1]->stopped = 1;
	}
}

/* Executes commands passed. Called by foregroundCmd() and backgroundCmd().
 * Checks for redirects first. 
 * Then checks for pipes.
 * Finally, executes command provided. */
void executeCmd(Cmd *cmd) {
	//child process calls execvp() to run cmd
	//checks for redirects first
	// <
	if(findSymbol(cmd, REDIRECT_IN_OP) != -1){
		int in = findSymbol(cmd, REDIRECT_IN_OP);
		int fdIn = open(cmd->args[in+1], O_RDONLY);
		dup2(fdIn, 0);
	}
	// >
	if(findSymbol(cmd, REDIRECT_OUT_OP) != -1){
		int out = findSymbol(cmd, REDIRECT_OUT_OP);
		int fdOut = open(cmd->args[out+1], O_WRONLY|O_CREAT);
		dup2(fdOut, fileno(stdout));
	}

	//then checks for pipes
	if(findSymbol(cmd, PIPE_OP) != -1){
		int pipePos = findSymbol(cmd, PIPE_OP); //ls | grep txt >> pipePos = 1
		int num = pipePos + 1;
		while(cmd->args[num] != NULL){num++;}
		num = num - pipePos - 1; // ls | grep txt >> num = 2
		
		//populating argsLeft
		char* argsLeft[pipePos * sizeof(char*)];
		for(int i = 0; i < pipePos; i++){argsLeft[i] = cmd->args[i];}

		//populating argsRight
		char* argsRight[num * sizeof(char*)];
		int parser = pipePos + 1;
		for(int i = 0; i < num; i++){
			argsRight[i] = cmd->args[parser];
			parser++;
		}

		//making pipes
		int fd[2];
		pipe(fd);
		pid_t pid = fork();
		if(pid < 0) return;
		if(pid == 0){ //child
			dup2(fd[0], 0);
			close(fd[1]);
			execvp(argsRight[0], argsRight);
			perror("child exe failed\n");
			exit(1);
		}
		else { //parent
			dup2(fd[1], 1);
			close(fd[0]);
			execvp(argsLeft[0], argsLeft);
			perror("parent exe failed\n");
			exit(1);
		}
		return; //return here because we don't want to execute it twice
	}

	//Execute the command
	execvp(cmd->args[0], cmd->args);
	//perror("command incorrect\n");

}

/* Uses fork() to run a separate child process. 
 * Does not put the parent process (shell) on hold/wait while the child process executes. 
 * Child process uses executeCmd() method to execute command. 
 */
int bgCmd(Cmd *cmd){
	pid_t pid = fork();
	if (pid < 0) return -1;
	if(pid == 0) {
		executeCmd(cmd);
		exit(0);		
	}
	if(pid > 0){
		if(setpgid(pid, 0) != 0) perror("setpgid() error"); //to help with sigstpHandler()
		bgJobSize++;
		jobs = (Cmd**) realloc(jobs, bgJobSize * sizeof(Cmd*));
		jobs[bgJobSize-1] = cmd;
		jobs[bgJobSize-1]->pid = pid;
		jobs[bgJobSize-1]->done = 0;
		jobs[bgJobSize-1]->printed = 0;
		jobs[bgJobSize-1]->stopped = 0;
	}

	return 0;
}

/*  Uses fork() to run a separate child process. 
 *  Puts the parent process (shell) on hold/wait while child process executes. 
 * Child process uses executeCmd() method to execute command. 
 */
int fgCmd(Cmd *cmd){
	pid_t pid = fork();
	if (pid < 0) return -1;
	if(pid == 0){
		executeCmd(cmd);
		exit(0);
	}
	if(pid > 0) {
		temp = (Cmd*) malloc(1 * sizeof(Cmd));
		temp = cmd;
		foregroundPid = pid; //used in sigstpHandler()
		waitpid(pid, NULL, WUNTRACED);
	}
	return 0;
}

/* loops through the jobs array and checks if the processes in it are done. */
int processCheck(int exitCode){
	int i = 0;
	while(i < bgJobSize){
		int status;
		int amp = findSymbol(jobs[i], BG_OP);
		pid_t checker = waitpid(jobs[i]->pid, &status, WNOHANG);
		if(checker > 0){ //this specific job is terminated
			if(WIFEXITED(status)){
				//printf("exit status is %d\n", WEXITSTATUS(status));
				status = WEXITSTATUS(status);
				jobs[i]->done = 1;
			}
			else if(WIFSIGNALED(status)){
				status = WTERMSIG(status);
				jobs[i]->done = 1;
				printf("[%d]\tTerminated\t", i+1);
				int n = 0; 
				if(jobs[i]->printed == 0){
					while(1){
						if(amp == n){break;}
						if(jobs[i]->args[n] == NULL && jobs[i]->symbols[n] == NULL){break;}
                               	 		else if(jobs[i]->args[n] == NULL){printf("%s ", jobs[i]->symbols[n]);}
                                		else{ printf("%s ", jobs[i]->args[n]);}
                                		n++;
                        		}
					jobs[i]->printed = 1;
					printf("\n");
				}
				return 0;
			}

			if(status != 0){printf("[%d]\tExit\t%d ", i+1, status);}
			else {printf("[%d]\tDone\t\t", i+1); }
			//loop to print args
			int n = 0;
			if(jobs[i]->printed == 0){
				while(1){
					if(amp == n){break;}
					if(jobs[i]->args[n] == NULL && jobs[i]->symbols[n] == NULL){break;}
					else if(jobs[i]->args[n] == NULL){printf("%s ", jobs[i]->symbols[n]);}
					else{ printf("%s ", jobs[i]->args[n]);}
					n++;
				}
				jobs[i]->printed = 1;
				printf("\n");
			}
		}
		i++;
	}
	return 0;
}

int main(void) {
	/* Listen for control+z (suspend process). */
	signal(SIGTSTP, sigtstpHandler);
	jobs = (Cmd**) malloc (bgJobSize * sizeof(Cmd*));
	int bg = 0;
	while (1) {
		printf("352> ");
		fflush(stdout);
		Cmd *cmd = (Cmd*) calloc(1, sizeof(Cmd));
		fgets(cmd->line, MAX_LINE, stdin);
		parseCmd(cmd);

		/*checks on the status of background processes. */
		int pCh = processCheck(bg);

		if (!cmd->args[0]) {
			free(cmd);
		} else if (strcmp(cmd->args[0], "exit") == 0) {
			free(cmd);
			exit(0);

		/* Add built-in commands: jobs and bg. */

		}
		/* handles jobs using the done and printed fields added in the Cmd struct. */
		else if(strcmp(cmd->args[0], "jobs") == 0){
			for(int i = 0; i < bgJobSize; i++){
				if(jobs[i]->done == 0 && jobs[i]->printed == 0 && jobs[i]->stopped != 1){printf("[%d]\tRunning\t\t%s", i, jobs[i]->line);}
				else if(jobs[i]->done == 1 && jobs[i]->printed == 0 && jobs[i]->stopped != 1){
					printf("[%d]\tDone\t\t%s", i, jobs[i]->line);
					jobs[i]->printed = 1;
				}
				else if(jobs[i]->stopped == 1){printf("[%d]\tStopped\t\t%s", i, jobs[i]->line);}
			}
		}

		else if(strcmp(cmd->args[0], "bg") == 0){
			int ind = atoi(cmd->args[1]);
			int jobPid = jobs[ind]->pid;
			kill(jobPid, SIGCONT);
			jobs[ind]->stopped = 0;
		}

		else {
			if (findSymbol(cmd, BG_OP) != -1) {
				/* Run command in background. */
				bg = bgCmd(cmd);
				printf("[%d] %d\n", bgJobSize, jobs[bgJobSize-1]->pid);

			} else {
				/* Run command in foreground. */
				int fg = fgCmd(cmd);

			}
		}

	}

	//free jobs
	for(int i = 0; i < bgJobSize; i++){
		free(jobs[i]);
	}
	free(jobs);
	return 0;
}
