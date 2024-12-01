// server.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>    // For errno
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <stdarg.h>
#include <limits.h>

#define PORT 12345
#define BUFFER_SIZE 4096
#define MAX_CLIENTS 100

// Process structure
typedef struct process {
    int pid;                // Process ID (from fork)
    int client_id;          // Client ID
    char command[BUFFER_SIZE]; // Command to execute
    int burst_time;         // Burst time (e.g., N for demo N)
    int remaining_time;     // Remaining execution time
    int arrival_time;       // Arrival time
    int last_executed_time; // Time when the process was last executed
    int first_round_completed; // 0 if first round not completed, 1 otherwise
    int is_running;         // Whether the process is currently running
    struct process *next;   // Next process in the queue
} process_t;

// Global variables
process_t *process_queue = NULL;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
sem_t queue_sem;

int client_count = 0;
pthread_mutex_t client_count_mutex = PTHREAD_MUTEX_INITIALIZER;

// Function declarations
void *handle_client(void *arg);
void *scheduler_function(void *arg);
void add_process(process_t *proc);
void remove_process(process_t *proc);
process_t *get_next_process();
void log_server(const char *format, ...);

// Main function
int main() {
    int listen_fd, conn_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t sin_size;
    int yes = 1;

    // Initialize semaphore
    sem_init(&queue_sem, 0, 0);

    // Create a TCP socket
    if ((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        exit(1);
    }

    // Set socket option to reuse address
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
        perror("setsockopt");
        exit(1);
    }

    // Configure server address structure
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);           // Convert port to network byte order
    server_addr.sin_addr.s_addr = INADDR_ANY;     // Bind to all available interfaces
    memset(&(server_addr.sin_zero), '\0', 8);     // Zero out the rest of the struct

    // Bind the socket to the specified address and port
    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(struct sockaddr)) == -1) {
        perror("bind");
        exit(1);
    }

    // Start listening for incoming connections
    if (listen(listen_fd, 10) == -1)  // Allow up to 10 pending connections
    {
        perror("listen");
        exit(1);
    }

    log_server("Server: Waiting for connections on port %d...", PORT);

    // Create scheduler thread
    pthread_t scheduler_thread;
    if (pthread_create(&scheduler_thread, NULL, scheduler_function, NULL) != 0) {
        perror("pthread_create");
        exit(1);
    }

    // Main server loop
    while (1) {
        // Accept a new client connection
        sin_size = sizeof(struct sockaddr_in);
        conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &sin_size);
        if (conn_fd == -1) {
            perror("accept");
            continue;  // Continue listening if accept fails
        }

        // Allocate memory for conn_fd to pass to the thread
        int *conn_fd_ptr = malloc(sizeof(int));
        if (conn_fd_ptr == NULL) {
            perror("malloc");
            close(conn_fd);
            continue;
        }
        *conn_fd_ptr = conn_fd;

        // Increment client count and assign client ID
        pthread_mutex_lock(&client_count_mutex);
        int client_id = ++client_count;
        pthread_mutex_unlock(&client_count_mutex);

        log_server("Client %d connected.", client_id);

        // Create a new thread to handle the client
        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, handle_client, conn_fd_ptr) != 0) {
            perror("pthread_create");
            free(conn_fd_ptr);
            close(conn_fd);
            continue;
        }

        // Detach the thread to reclaim resources when it exits
        pthread_detach(thread_id);
    }

    close(listen_fd);
    return 0;
}

void *handle_client(void *arg) {
    int conn_fd = *(int *)arg;
    free(arg); // Free the allocated memory
    char buf[BUFFER_SIZE];

    // Assign client ID
    pthread_mutex_lock(&client_count_mutex);
    int client_id = client_count;
    pthread_mutex_unlock(&client_count_mutex);

    // Handle client communication
    while (1) {
        // Read command from client
        memset(buf, 0, BUFFER_SIZE);
        int numbytes = recv(conn_fd, buf, BUFFER_SIZE - 1, 0);
        if (numbytes == -1) {
            perror("recv");
            break;
        } else if (numbytes == 0) {
            // Connection closed by client
            log_server("Client %d disconnected.", client_id);
            break;
        }

        buf[numbytes] = '\0'; // Null-terminate the received data

        log_server("Client %d: Received command `%s`.", client_id, buf);

        // Check if the command is a shell command or program command
        if (strncmp(buf, "demo", 4) == 0) {
            // Program command
            // Parse the burst time (N)
            int N;
            if (sscanf(buf, "demo %d", &N) != 1) {
                // Invalid command format
                char *msg = "Invalid command format. Use `demo N`.\n";
                send(conn_fd, msg, strlen(msg), 0);
                continue;
            }

            // Create a new process
            process_t *proc = malloc(sizeof(process_t));
            if (proc == NULL) {
                perror("malloc");
                continue;
            }
            proc->pid = -1;
            proc->client_id = client_id;
            strcpy(proc->command, buf);
            proc->burst_time = N;
            proc->remaining_time = N;
            proc->arrival_time = time(NULL);
            proc->last_executed_time = 0;
            proc->first_round_completed = 0;
            proc->is_running = 0;
            proc->next = NULL;

            // Add process to the queue
            pthread_mutex_lock(&queue_mutex);
            add_process(proc);
            pthread_mutex_unlock(&queue_mutex);

            // Signal the scheduler
            sem_post(&queue_sem);

            // Send acknowledgment to client
            char *msg = "Process added to queue successfully.\n";
            send(conn_fd, msg, strlen(msg), 0);

            log_server("Client %d: Added process `%s` to the queue.", client_id, buf);
        } else {
            // Shell command
            log_server("Executing shell command: `%s`.", buf);

            // Execute the shell command immediately
            // Set up pipes for communication with myshell
            int pipe_stdin[2];  // For writing to myshell's stdin
            int pipe_stdout[2]; // For reading from myshell's stdout

            if (pipe(pipe_stdin) == -1 || pipe(pipe_stdout) == -1) {
                perror("pipe");
                break;
            }

            // Fork another child process to execute myshell
            pid_t pid2 = fork();
            if (pid2 == -1) {
                perror("fork");
                break;
            } else if (pid2 == 0) {
                // Child process to execute myshell

                // Redirect stdin to read from pipe
                dup2(pipe_stdin[0], STDIN_FILENO);
                close(pipe_stdin[1]);
                close(pipe_stdin[0]);

                // Redirect stdout to write to pipe
                dup2(pipe_stdout[1], STDOUT_FILENO);
                close(pipe_stdout[0]);
                close(pipe_stdout[1]);

                // Execute myshell
                execl("./myshell", "myshell", (char *)NULL);

                // If execl fails
                perror("execl");
                exit(1);
            } else {
                // Parent process (client handler)

                // Close unused pipe ends
                close(pipe_stdin[0]);
                close(pipe_stdout[1]);

                // Write command to myshell's stdin
                write(pipe_stdin[1], buf, strlen(buf));
                write(pipe_stdin[1], "\n", 1); // Ensure command is terminated
                write(pipe_stdin[1], "exit\n", 5); // Send exit command to terminate myshell
                close(pipe_stdin[1]); // Close writing end to signal EOF

                // Read all output from myshell's stdout
                char output_buf[BUFFER_SIZE];
                char full_output[BUFFER_SIZE * 10] = {0}; // Larger buffer for full output
                int total_bytes = 0;
                int read_bytes;
                while ((read_bytes = read(pipe_stdout[0], output_buf, BUFFER_SIZE - 1)) > 0) {
                    output_buf[read_bytes] = '\0'; // Null-terminate
                    strcat(full_output, output_buf);
                    total_bytes += read_bytes;
                }
                close(pipe_stdout[0]);

                // Wait for myshell process to finish
                waitpid(pid2, NULL, 0);

                // Send output back to client
                if (send(conn_fd, full_output, strlen(full_output), 0) == -1) {
                    perror("send");
                }

                log_server("Client %d: Command executed successfully.", client_id);
            }
        }
    }

    close(conn_fd);

    // On client disconnection, remove all associated processes from the queue
    pthread_mutex_lock(&queue_mutex);
    process_t *prev = NULL;
    process_t *curr = process_queue;
    while (curr != NULL) {
        if (curr->client_id == client_id) {
            // Remove this process
            if (prev == NULL) {
                process_queue = curr->next;
            } else {
                prev->next = curr->next;
            }
            process_t *to_free = curr;
            curr = curr->next;
            free(to_free);
        } else {
            prev = curr;
            curr = curr->next;
        }
    }
    pthread_mutex_unlock(&queue_mutex);

    return NULL;
}

void *scheduler_function(void *arg) {
    process_t *current_process = NULL;
    process_t *last_process = NULL;

    while (1) {
        // Wait until there is a process in the queue
        sem_wait(&queue_sem);

        pthread_mutex_lock(&queue_mutex);
        if (process_queue == NULL) {
            pthread_mutex_unlock(&queue_mutex);
            continue;
        }

        // Select next process based on scheduling algorithm
        current_process = get_next_process();

        if (current_process == NULL) {
            pthread_mutex_unlock(&queue_mutex);
            continue;
        }

        // Run the process
        if (current_process->pid == -1) {
            // Process has not been started yet
            // Fork and exec the process
            pid_t pid = fork();
            if (pid == -1) {
                perror("fork");
                pthread_mutex_unlock(&queue_mutex);
                continue;
            } else if (pid == 0) {
                // Child process
                // Parse the command to get N
                int N;
                sscanf(current_process->command, "demo %d", &N);

                // Convert N to string
                char N_str[10];
                sprintf(N_str, "%d", N);

                execl("./demo", "demo", N_str, NULL);
                // If execl fails
                perror("execl");
                exit(1);
            } else {
                // Parent process
                current_process->pid = pid;
                current_process->is_running = 1;
                log_server("Scheduler: Started process `%s` with PID %d.", current_process->command, pid);
            }
        } else {
            // Process was previously running and stopped
            kill(current_process->pid, SIGCONT);
            current_process->is_running = 1;
            log_server("Scheduler: Resumed process `%s` with PID %d.", current_process->command, current_process->pid);
        }
        pthread_mutex_unlock(&queue_mutex);

        // Run the process for the quantum time or until completion
        int quantum = current_process->first_round_completed ? 7 : 3;

        // Sleep for the quantum or remaining time, whichever is smaller
        int sleep_time = current_process->remaining_time < quantum ? current_process->remaining_time : quantum;

        sleep(sleep_time);

        // Update remaining time
        current_process->remaining_time -= sleep_time;
        current_process->last_executed_time = time(NULL);

        log_server("Scheduler: Process `%s` ran for %d seconds. Remaining time: %d seconds.", current_process->command, sleep_time, current_process->remaining_time);

        if (current_process->remaining_time <= 0) {
            // Process has finished
            // Wait for the process to finish
            waitpid(current_process->pid, NULL, 0);
            log_server("Scheduler: Process `%s` with PID %d finished.", current_process->command, current_process->pid);

            // Remove the process from the queue
            pthread_mutex_lock(&queue_mutex);
            remove_process(current_process);
            pthread_mutex_unlock(&queue_mutex);

            current_process = NULL;
        } else {
            // Preempt the process
            kill(current_process->pid, SIGSTOP);
            current_process->is_running = 0;
            log_server("Scheduler: Process `%s` with PID %d preempted.", current_process->command, current_process->pid);

            // Mark first round as completed
            current_process->first_round_completed = 1;
            last_process = current_process;

            // Re-add the process to the semaphore
            sem_post(&queue_sem);
        }
    }
    return NULL;
}

void add_process(process_t *proc) {
    // Add process to the end of the queue
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
    // Remove process from the queue
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
    // Implement the scheduling algorithm here

    static process_t *last_process = NULL; // To prevent selecting the same process consecutively

    process_t *curr = process_queue;
    process_t *selected_process = NULL;

    // Find the process with the shortest remaining time that is not the same as last_process
    int min_remaining_time = INT_MAX;
    while (curr != NULL) {
        if (curr != last_process && curr->remaining_time < min_remaining_time) {
            min_remaining_time = curr->remaining_time;
            selected_process = curr;
        }
        curr = curr->next;
    }

    if (selected_process == NULL) {
        // If only one process is remaining or all have the same remaining time
        curr = process_queue;
        while (curr != NULL) {
            if (curr != last_process) {
                selected_process = curr;
                break;
            }
            curr = curr->next;
        }
    }

    if (selected_process == NULL) {
        // If only one process is remaining
        selected_process = process_queue;
    }

    last_process = selected_process;

    return selected_process;
}

void log_server(const char *format, ...) {
    va_list args;
    va_start(args, format);

    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");

    va_end(args);
}
