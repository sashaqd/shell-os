// server.c

// Include standard input/output headers for basic I/O functionality (printf, fprintf, etc.)
#include <stdio.h>
// Include standard library for general-purpose functions (malloc, exit, etc.)
#include <stdlib.h>
// Include string library for string manipulation functions (strcpy, strncpy, etc.)
#include <string.h>
// Include unistd.h for close(), fork(), pipe(), sleep(), etc.
#include <unistd.h>
// Include errno.h to access the global variable errno for error reporting
#include <errno.h>    
// Include sys/types.h for data types used in sockets and other sys calls
#include <sys/types.h>
// Include sys/socket.h for socket-related functions and structures
#include <sys/socket.h>
// Include netinet/in.h for sockaddr_in and related definitions
#include <netinet/in.h>
// Include arpa/inet.h for inet_ functions like inet_ntoa
#include <arpa/inet.h>
// Include pthread.h for multi-threading support
#include <pthread.h>
// Include semaphore.h for semaphores (sem_t, sem_init, sem_wait, etc.)
#include <semaphore.h>
// Include signal.h for signal handling (SIGKILL, SIGSTOP, etc.)
#include <signal.h>
// Include sys/wait.h for waitpid() function
#include <sys/wait.h>
// Include time.h for time-related functions (time())
#include <time.h>
// Include stdarg.h for variable argument functions (va_list, va_start, etc.)
#include <stdarg.h>
// Include limits.h for INT_MAX and other limit macros
#include <limits.h>

// Define the port number on which the server will listen
#define PORT 12345
// Define the buffer size for reading/writing data
#define BUFFER_SIZE 4096
// Define the maximum number of clients that can connect
#define MAX_CLIENTS 100

// Define some ANSI color codes for pretty printing messages in different colors
#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_RESET   "\x1b[0m"

// Define a process structure that holds information about a client command execution
typedef struct process {
    int pid;                // Process ID of the forked child process
    int client_id;          // ID of the client who initiated this process
    char command[BUFFER_SIZE]; // The command string (program and arguments)
    int burst_time;         // Estimated "burst time" (execution time)
    int remaining_time;     // Remaining time for the process to complete
    int arrival_time;       // Time when the process was queued
    int last_executed_time; // Last time the process was executed
    int first_round_completed; // Flag indicating if the process has completed its first scheduling round
    int round_completed;    // Flag indicating if the process has completed the current round
    int is_running;         // Flag indicating if the process is currently running
    struct process *next;   // Pointer to next process in the queue
    int client_fd;          // The file descriptor to communicate back to the client
} process_t;

// A global pointer to the head of the process queue
process_t *process_queue = NULL;

// A mutex to protect the process queue from concurrent access
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
// A semaphore used to signal when new processes arrive for scheduling
sem_t queue_sem;

// Global counter for clients
int client_count = 0;
// A mutex to protect the client_count variable
pthread_mutex_t client_count_mutex = PTHREAD_MUTEX_INITIALIZER;

// Forward declarations of functions handling client and scheduling logic
void *handle_client(void *arg);
void *scheduler_function(void *arg);
void add_process(process_t *proc);
void remove_process(process_t *proc);
process_t *get_next_process();
void log_server(const char *format, ...);

int main() {
    int listen_fd, conn_fd;  // Socket file descriptors for listening and accepted connections
    struct sockaddr_in server_addr, client_addr; // Address structures for server and client
    socklen_t sin_size;     // Size of the client address structure
    int yes = 1;            // For setsockopt to allow reuse of the port

    // Initialize the semaphore to 0, indicating no processes initially
    sem_init(&queue_sem, 0, 0);

    // Print a message indicating the server has started
    fprintf(stderr, "| Hello, Server Started |\n");

    // Create a TCP socket
    if ((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        exit(1);
    }
    // Allow the socket to reuse the address immediately after the server stops
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
        perror("setsockopt");
        exit(1);
    }

    // Set up the server_addr structure for binding
    server_addr.sin_family = AF_INET;         // IPv4
    server_addr.sin_port = htons(PORT);       // Host-to-network byte order for port
    server_addr.sin_addr.s_addr = INADDR_ANY; // Bind to any local IP address
    memset(&(server_addr.sin_zero), '\0', 8); // Zero out the rest of the struct

    // Bind the listening socket to the specified port
    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(struct sockaddr)) == -1) {
        perror("bind");
        exit(1);
    }

    // Start listening on the socket with a backlog of 10
    if (listen(listen_fd, 10) == -1) {
        perror("listen");
        exit(1);
    }

    // Create the scheduler thread that handles process scheduling
    pthread_t scheduler_thread;
    if (pthread_create(&scheduler_thread, NULL, scheduler_function, NULL) != 0) {
        perror("pthread_create");
        exit(1);
    }

    // Main server loop to accept new client connections
    while (1) {
        sin_size = sizeof(struct sockaddr_in);
        // Accept a new client connection
        conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &sin_size);
        if (conn_fd == -1) {
            perror("accept");
            continue; // If accept fails, continue to next iteration
        }

        // Allocate memory to store the connection file descriptor pointer
        int *conn_fd_ptr = malloc(sizeof(int));
        if (conn_fd_ptr == NULL) {
            perror("malloc");
            close(conn_fd);
            continue;
        }
        *conn_fd_ptr = conn_fd;

        // Increase the client count in a thread-safe manner
        pthread_mutex_lock(&client_count_mutex);
        int client_id = ++client_count;
        pthread_mutex_unlock(&client_count_mutex);

        // Log that a client has connected
        fprintf(stderr, "[%d]<<< client connected\n", client_id);

        // Create a thread to handle this client
        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, handle_client, conn_fd_ptr) != 0) {
            perror("pthread_create");
            free(conn_fd_ptr);
            close(conn_fd);
            continue;
        }

        // Detach the thread so that its resources are freed automatically when it exits
        pthread_detach(thread_id);
    }

    // Close the listening socket (unreachable code if infinite loop doesn't break)
    close(listen_fd);
    return 0;
}

void *handle_client(void *arg) {
    int conn_fd = *(int *)arg; // Extract the file descriptor from the argument
    free(arg); // Free the allocated memory for the file descriptor pointer
    char buf[BUFFER_SIZE]; // Buffer for receiving data from client

    // Retrieve the current client ID (as assigned in main)
    pthread_mutex_lock(&client_count_mutex);
    int client_id = client_count;
    pthread_mutex_unlock(&client_count_mutex);

    // Infinite loop to receive and handle commands from this client
    while (1) {
        memset(buf, 0, BUFFER_SIZE); // Clear the buffer
        int numbytes = recv(conn_fd, buf, BUFFER_SIZE - 1, 0); // Receive data from the client
        if (numbytes == -1) {
            perror("recv");
            break; // If error, break from the loop
        } else if (numbytes == 0) {
            // If recv returns 0, client disconnected
            fprintf(stderr, "[%d]<<< client disconnected\n", client_id);
            break;
        }

        buf[numbytes] = '\0'; // Null-terminate the received string

        // Log the command received from the client
        fprintf(stderr, "[%d]>>> %s\n", client_id, buf);

        if (buf[0] == '.' && buf[1] == '/') {
            // This indicates a program command (like "./demo 12")

            // Create a new process structure
            process_t *proc = malloc(sizeof(process_t));
            if (proc == NULL) {
                perror("malloc");
                continue;
            }
            proc->pid = -1; // Not started yet
            proc->client_id = client_id;
            strncpy(proc->command, buf, BUFFER_SIZE - 1); 
            proc->command[BUFFER_SIZE - 1] = '\0';

            // For demo, assume burst_time is the last argument of the command
            int burst = 3; 
            char *copy_cmd = strdup(buf);
            if (copy_cmd) {
                char *token = strtok(copy_cmd, " ");
                while (token != NULL) {
                    int val = atoi(token);
                    if (val > 0) {
                        burst = val; // If last token is a positive number, use it as burst time
                    }
                    token = strtok(NULL, " ");
                }
                free(copy_cmd);
            }

            // Set initial process timings
            proc->burst_time = burst;
            proc->remaining_time = proc->burst_time;
            proc->arrival_time = time(NULL);
            proc->last_executed_time = 0;
            proc->first_round_completed = 0;
            proc->round_completed = 0;
            proc->is_running = 0;
            proc->next = NULL;
            proc->client_fd = conn_fd;

            // Add the process to the queue
            pthread_mutex_lock(&queue_mutex);
            add_process(proc);
            pthread_mutex_unlock(&queue_mutex);

            // Notify the scheduler that a new process is available
            sem_post(&queue_sem);

            // Log that the process was created
            fprintf(stderr, "[%d]---- " ANSI_COLOR_BLUE "created" ANSI_COLOR_RESET " (%d)\n", client_id, proc->remaining_time);
        } else {
            // The received command is a shell command (not starting with "./")

            // Log that a shell command process is created
            fprintf(stderr, "[%d]---- " ANSI_COLOR_BLUE "created" ANSI_COLOR_RESET " (-1)\n", client_id);

            // Set up pipes to run the shell command through an external shell (myshell)
            int pipe_stdin[2];  
            int pipe_stdout[2]; 

            if (pipe(pipe_stdin) == -1 || pipe(pipe_stdout) == -1) {
                perror("pipe");
                break;
            }

            pid_t pid2 = fork();
            if (pid2 == -1) {
                perror("fork");
                break;
            } else if (pid2 == 0) {
                // Child process: set up I/O redirection and execute myshell
                dup2(pipe_stdin[0], STDIN_FILENO);
                close(pipe_stdin[1]);
                close(pipe_stdin[0]);

                dup2(pipe_stdout[1], STDOUT_FILENO);
                close(pipe_stdout[0]);
                close(pipe_stdout[1]);

                execl("./myshell", "myshell", (char *)NULL);
                perror("execl");
                exit(1);
            } else {
                // Parent process: send the command to myshell and read its output

                // Log that shell command started
                fprintf(stderr, "[%d]---- " ANSI_COLOR_GREEN "started" ANSI_COLOR_RESET " (-1)\n", client_id);

                close(pipe_stdin[0]);
                close(pipe_stdout[1]);

                // Write the received command and an "exit" command to terminate myshell
                write(pipe_stdin[1], buf, strlen(buf));
                write(pipe_stdin[1], "\n", 1);
                write(pipe_stdin[1], "exit\n", 5); 
                close(pipe_stdin[1]);

                // Read all output from myshell
                char output_buf[BUFFER_SIZE];
                char full_output[BUFFER_SIZE * 10] = {0};
                int total_bytes = 0;
                int read_bytes;
                while ((read_bytes = read(pipe_stdout[0], output_buf, BUFFER_SIZE - 1)) > 0) {
                    output_buf[read_bytes] = '\0';
                    strcat(full_output, output_buf);
                    total_bytes += read_bytes;
                }
                close(pipe_stdout[0]);

                // Wait for the child to finish
                waitpid(pid2, NULL, 0);

                // Send the output back to the client
                if (send(conn_fd, full_output, strlen(full_output), 0) == -1) {
                    perror("send");
                }

                // Log how many bytes were sent
                fprintf(stderr, "[%d]<<< %d bytes sent\n", client_id, total_bytes);

                // Log that the shell command ended
                fprintf(stderr, "[%d]---- " ANSI_COLOR_RED "ended" ANSI_COLOR_RESET " (-1)\n", client_id);
            }
        }
    }

    // Client disconnected, close the socket
    close(conn_fd);

    // Remove any remaining processes belonging to this client
    pthread_mutex_lock(&queue_mutex);
    process_t *prev = NULL;
    process_t *curr = process_queue;
    while (curr != NULL) {
        process_t *next = curr->next; // Save next pointer
        if (curr->client_id == client_id) {
            // If process is running, kill it
            if (curr->is_running && curr->pid > 0) {
                kill(curr->pid, SIGKILL);
                waitpid(curr->pid, NULL, 0);
            }

            // Remove it from the queue
            if (prev == NULL) {
                process_queue = next;
            } else {
                prev->next = next;
            }
            free(curr);
            curr = next;
        } else {
            prev = curr;
            curr = next;
        }
    }
    pthread_mutex_unlock(&queue_mutex);

    return NULL; // Thread exits
}

void *scheduler_function(void *arg) {
    process_t *current_process = NULL; // Keep track of the currently scheduled process

    while (1) {
        // Wait until there is at least one process to schedule
        sem_wait(&queue_sem);

        pthread_mutex_lock(&queue_mutex);
        if (process_queue == NULL) {
            // If no processes, unlock and continue waiting
            pthread_mutex_unlock(&queue_mutex);
            continue;
        }

        // Select the next process to run based on scheduling logic
        current_process = get_next_process();
        if (current_process == NULL) {
            pthread_mutex_unlock(&queue_mutex);
            continue;
        }

        time_t current_start_time = time(NULL);

        if (current_process->pid == -1) {
            // If process is not started yet, fork a new process
            int pipe_stdout[2];  // Pipe to capture output if needed
            if (pipe(pipe_stdout) == -1) {
                perror("pipe");
                pthread_mutex_unlock(&queue_mutex);
                continue;
            }

            pid_t pid = fork();
            if (pid == -1) {
                perror("fork");
                close(pipe_stdout[0]);
                close(pipe_stdout[1]);
                pthread_mutex_unlock(&queue_mutex);
                continue;
            } else if (pid == 0) {
                // Child process: redirect stdout and execute the command
                close(pipe_stdout[0]);
                dup2(pipe_stdout[1], STDOUT_FILENO);
                close(pipe_stdout[1]);

                char *args[64];
                int arg_count = 0;
                char *token = strtok(current_process->command, " ");
                while (token != NULL && arg_count < 64) {
                    args[arg_count++] = token;
                    token = strtok(NULL, " ");
                }
                args[arg_count] = NULL;

                execvp(args[0], args);
                perror("execvp");
                exit(1);
            } else {
                // Parent process: we have the pid now
                close(pipe_stdout[1]);
                current_process->pid = pid;
                current_process->is_running = 1;

                // If it's a "./demo" command, just log that it started
                if (strstr(current_process->command, "./demo") == current_process->command) {
                    fprintf(stderr, "[%d]---- " ANSI_COLOR_GREEN "started" ANSI_COLOR_RESET " (%d)\n", 
                            current_process->client_id, current_process->remaining_time);
                } else {
                    // For other commands, capture output
                    char output_buf[BUFFER_SIZE];
                    char full_output[BUFFER_SIZE * 10] = {0};
                    int total_bytes = 0;
                    int read_bytes;

                    while ((read_bytes = read(pipe_stdout[0], output_buf, BUFFER_SIZE - 1)) > 0) {
                        output_buf[read_bytes] = '\0';
                        strcat(full_output, output_buf);
                        total_bytes += read_bytes;
                    }
                    close(pipe_stdout[0]);

                    // Send output to client
                    send(current_process->client_fd, full_output, strlen(full_output), 0);
                    fprintf(stderr, "[%d]<<< %d bytes sent\n", current_process->client_id, total_bytes);
                }
            }
        } else {
            // Process already started before, resume it
            kill(current_process->pid, SIGCONT);
            current_process->is_running = 1;
            fprintf(stderr, "[%d]---- " ANSI_COLOR_GREEN "running" ANSI_COLOR_RESET " (%d)\n", current_process->client_id, current_process->remaining_time);
        }
        pthread_mutex_unlock(&queue_mutex);

        // Determine the time slice (quantum) based on whether the process completed its first round
        int quantum = current_process->first_round_completed ? 7 : 3;
        int slice = (current_process->remaining_time < quantum) ? current_process->remaining_time : quantum;

        int i;
        int preempted = 0;
        for (i = 0; i < slice; i++) {
            sleep(1); // Simulate one second of execution time

            // If this is a demo command, send progress updates to the client
            if (strstr(current_process->command, "./demo") == current_process->command) {
                char progress_msg[BUFFER_SIZE];
                snprintf(progress_msg, BUFFER_SIZE, "Demo %d/%d\n", 
                         current_process->burst_time - current_process->remaining_time + i + 1,
                         current_process->burst_time);
                int client_fd = current_process->client_fd;
                send(client_fd, progress_msg, strlen(progress_msg), 0);
            }

            pthread_mutex_lock(&queue_mutex);
            int remaining_after_this_second = current_process->remaining_time - (i + 1);

            // Check if a new shorter job arrived after current_start_time
            process_t *check_proc = process_queue;
            int found_shorter_new_job = 0;
            while (check_proc != NULL) {
                // If there is a newly arrived process (after we started current process)
                // with remaining_time shorter than what's left for the current process,
                // we preempt the current one.
                if (check_proc != current_process && 
                    check_proc->arrival_time > current_start_time && 
                    check_proc->remaining_time < remaining_after_this_second) {
                    found_shorter_new_job = 1;
                    break;
                }
                check_proc = check_proc->next;
            }

            if (found_shorter_new_job) {
                // Preempt current process
                kill(current_process->pid, SIGSTOP);
                current_process->remaining_time = remaining_after_this_second;
                current_process->last_executed_time = time(NULL);
                current_process->is_running = 0;
                current_process->first_round_completed = 1;
                fprintf(stderr, "[%d]---- " ANSI_COLOR_YELLOW "waiting" ANSI_COLOR_RESET " (%d)\n", current_process->client_id, current_process->remaining_time);
                sem_post(&queue_sem);
                pthread_mutex_unlock(&queue_mutex);
                preempted = 1;
                break;
            }

            pthread_mutex_unlock(&queue_mutex);
        }

        if (!preempted) {
            // If the process was not preempted by a shorter job
            pthread_mutex_lock(&queue_mutex);
            current_process->remaining_time -= i;
            current_process->last_executed_time = time(NULL);
            current_process->first_round_completed = 1;
            current_process->round_completed = 1;

            if (current_process->remaining_time <= 0) {
                // Process finished execution
                waitpid(current_process->pid, NULL, 0);

                // Calculate total bytes "sent" for demo messages (a rough guess)
                int total_bytes = current_process->burst_time * strlen("Demo XX/XX\n");
                fprintf(stderr, "[%d]<<< %d bytes sent\n", current_process->client_id, total_bytes);

                fprintf(stderr, "[%d]---- " ANSI_COLOR_RED "ended" ANSI_COLOR_RESET " (0)\n", current_process->client_id);
                remove_process(current_process);
                current_process = NULL;
            } else {
                // Process still has remaining time, preempt normally
                kill(current_process->pid, SIGSTOP);
                current_process->is_running = 0;
                fprintf(stderr, "[%d]---- " ANSI_COLOR_YELLOW "waiting" ANSI_COLOR_RESET " (%d)\n", 
                        current_process->client_id, 
                        current_process->remaining_time);
                sem_post(&queue_sem);
            }

            pthread_mutex_unlock(&queue_mutex);
        } else {
            pthread_mutex_lock(&queue_mutex);
            current_process->first_round_completed = 1;
            pthread_mutex_unlock(&queue_mutex);
        }
    }
    return NULL; // Scheduler thread exits (unreachable in this infinite loop)
}

void add_process(process_t *proc) {
    // Add a process at the end of the queue
    if (process_queue == NULL) {
        process_queue = proc;
    } else {
        process_t *curr = process_queue;
        while (curr->next != NULL) {
            curr = curr->next;
        }
        curr->next = proc;
    }
}

void remove_process(process_t *proc) {
    // Remove the given process from the queue
    if (process_queue == proc) {
        process_queue = proc->next;
    } else {
        process_t *curr = process_queue;
        while (curr != NULL && curr->next != proc) {
            curr = curr->next;
        }
        if (curr != NULL) {
            curr->next = proc->next;
        }
    }
    free(proc);
}

process_t *get_next_process() {
    static process_t *last_process = NULL;
    
    // First, check if all processes have completed their current round
    process_t *curr = process_queue;
    int all_completed = 1;
    while (curr != NULL) {
        if (!curr->round_completed) { 
            all_completed = 0;
            break;
        }
        curr = curr->next;
    }

    // If all processes completed their round, reset the round_completed flag for all
    if (all_completed) {
        curr = process_queue;
        while (curr != NULL) {
            curr->round_completed = 0; 
            curr = curr->next;
        }
    }

    // Select the shortest remaining time process that hasn't completed the current round
    process_t *selected_process = NULL;
    int min_remaining_time = INT_MAX;
    
    curr = process_queue;
    while (curr != NULL) {
        if (!curr->round_completed && curr->remaining_time < min_remaining_time && curr != last_process) {
            min_remaining_time = curr->remaining_time;
            selected_process = curr;
        }
        curr = curr->next;
    }

    // If no different process found, consider the last process as well
    if (selected_process == NULL) {
        curr = process_queue;
        min_remaining_time = INT_MAX;
        while (curr != NULL) {
            if (!curr->round_completed && curr->remaining_time < min_remaining_time) {
                min_remaining_time = curr->remaining_time;
                selected_process = curr;
            }
            curr = curr->next;
        }
    }

    // If still no process found (unlikely), just select the head
    if (selected_process == NULL) {
        selected_process = process_queue;
    }
    
    last_process = selected_process;
    return selected_process;
}

void log_server(const char *format, ...) {
    // A logging function that can be used for debugging
    // Currently not used for the main logs
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    va_end(args);
}
