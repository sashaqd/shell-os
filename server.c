// server.c (modified portions only)

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

typedef struct process {
    int pid;                
    int client_id;          
    char command[BUFFER_SIZE]; 
    int burst_time;         
    int remaining_time;     
    int arrival_time;       
    int last_executed_time; 
    int first_round_completed; 
    int is_running;         
    struct process *next;   
    int client_fd;          
} process_t;

process_t *process_queue = NULL;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
sem_t queue_sem;

int client_count = 0;
pthread_mutex_t client_count_mutex = PTHREAD_MUTEX_INITIALIZER;

void *handle_client(void *arg);
void *scheduler_function(void *arg);
void add_process(process_t *proc);
void remove_process(process_t *proc);
process_t *get_next_process();
void log_server(const char *format, ...);

int main() {
    int listen_fd, conn_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t sin_size;
    int yes = 1;

    sem_init(&queue_sem, 0, 0);

    // Print server start message
    fprintf(stderr, "| Hello, Server Started |\n");

    if ((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        exit(1);
    }
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
        perror("setsockopt");
        exit(1);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);           
    server_addr.sin_addr.s_addr = INADDR_ANY;     
    memset(&(server_addr.sin_zero), '\0', 8);     

    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(struct sockaddr)) == -1) {
        perror("bind");
        exit(1);
    }

    if (listen(listen_fd, 10) == -1) {
        perror("listen");
        exit(1);
    }

    // No need to log here with old format, we already printed server start

    pthread_t scheduler_thread;
    if (pthread_create(&scheduler_thread, NULL, scheduler_function, NULL) != 0) {
        perror("pthread_create");
        exit(1);
    }

    while (1) {
        sin_size = sizeof(struct sockaddr_in);
        conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &sin_size);
        if (conn_fd == -1) {
            perror("accept");
            continue;
        }

        int *conn_fd_ptr = malloc(sizeof(int));
        if (conn_fd_ptr == NULL) {
            perror("malloc");
            close(conn_fd);
            continue;
        }
        *conn_fd_ptr = conn_fd;

        pthread_mutex_lock(&client_count_mutex);
        int client_id = ++client_count;
        pthread_mutex_unlock(&client_count_mutex);

        // Log client connected
        fprintf(stderr, "[%d]<<< client connected\n", client_id);

        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, handle_client, conn_fd_ptr) != 0) {
            perror("pthread_create");
            free(conn_fd_ptr);
            close(conn_fd);
            continue;
        }

        pthread_detach(thread_id);
    }

    close(listen_fd);
    return 0;
}

void *handle_client(void *arg) {
    int conn_fd = *(int *)arg;
    free(arg); 
    char buf[BUFFER_SIZE];

    pthread_mutex_lock(&client_count_mutex);
    int client_id = client_count;
    pthread_mutex_unlock(&client_count_mutex);

    while (1) {
        memset(buf, 0, BUFFER_SIZE);
        int numbytes = recv(conn_fd, buf, BUFFER_SIZE - 1, 0);
        if (numbytes == -1) {
            perror("recv");
            break;
        } else if (numbytes == 0) {
            // client disconnected
            fprintf(stderr, "[%d]<<< client disconnected\n", client_id);
            break;
        }

        buf[numbytes] = '\0'; 

        // Log command received
        fprintf(stderr, "[%d]>>> %s\n", client_id, buf);

        if (buf[0] == '.' && buf[1] == '/') {
            // Program command
            process_t *proc = malloc(sizeof(process_t));
            if (proc == NULL) {
                perror("malloc");
                continue;
            }
            proc->pid = -1;
            proc->client_id = client_id;
            strncpy(proc->command, buf, BUFFER_SIZE - 1);
            proc->command[BUFFER_SIZE - 1] = '\0';
            // For the demo, assume burst_time extracted from command (e.g. "./demo 12")
            // Let's say the last argument is the burst time:
            int burst = 10; 
            char *copy_cmd = strdup(buf);
            if (copy_cmd) {
                char *token = strtok(copy_cmd, " ");
                while (token != NULL) {
                    // try to parse last token as int
                    int val = atoi(token);
                    if (val > 0) {
                        burst = val;
                    }
                    token = strtok(NULL, " ");
                }
                free(copy_cmd);
            }
            proc->burst_time = burst;
            proc->remaining_time = proc->burst_time;
            proc->arrival_time = time(NULL);
            proc->last_executed_time = 0;
            proc->first_round_completed = 0;
            proc->is_running = 0;
            proc->next = NULL;
            proc->client_fd = conn_fd;

            pthread_mutex_lock(&queue_mutex);
            add_process(proc);
            pthread_mutex_unlock(&queue_mutex);
            sem_post(&queue_sem);

            // Process created log
            fprintf(stderr, "[%d]---- created (%d)\n", client_id, proc->remaining_time);
        } else {
            // Shell command
            // Add created log for shell command
            fprintf(stderr, "[%d]---- created (-1)\n", client_id);

            // We execute and then send bytes back:
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
                // Add started log for shell command
                fprintf(stderr, "[%d]---- started (-1)\n", client_id);

                close(pipe_stdin[0]);
                close(pipe_stdout[1]);

                write(pipe_stdin[1], buf, strlen(buf));
                write(pipe_stdin[1], "\n", 1);
                write(pipe_stdin[1], "exit\n", 5); 
                close(pipe_stdin[1]);

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

                waitpid(pid2, NULL, 0);

                if (send(conn_fd, full_output, strlen(full_output), 0) == -1) {
                    perror("send");
                }

                // Log bytes sent
                fprintf(stderr, "[%d]<<< %d bytes sent\n", client_id, total_bytes);

                // After sending output, add ended log
                fprintf(stderr, "[%d]---- ended (-1)\n", client_id);
            }
        }
    }

    close(conn_fd);

    // Remove processes of this client
    pthread_mutex_lock(&queue_mutex);
    process_t *prev = NULL;
    process_t *curr = process_queue;
    while (curr != NULL) {
        if (curr->client_id == client_id) {
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

    while (1) {
        sem_wait(&queue_sem);

        pthread_mutex_lock(&queue_mutex);
        if (process_queue == NULL) {
            pthread_mutex_unlock(&queue_mutex);
            continue;
        }

        current_process = get_next_process();
        if (current_process == NULL) {
            pthread_mutex_unlock(&queue_mutex);
            continue;
        }

        time_t current_start_time = time(NULL);

        if (current_process->pid == -1) {
            // Start new process
            pid_t pid = fork();
            if (pid == -1) {
                perror("fork");
                pthread_mutex_unlock(&queue_mutex);
                continue;
            } else if (pid == 0) {
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
                current_process->pid = pid;
                current_process->is_running = 1;
                fprintf(stderr, "[%d]---- started (%d)\n", current_process->client_id, current_process->remaining_time);
            }
        } else {
            // Resuming existing process
            kill(current_process->pid, SIGCONT);
            current_process->is_running = 1;
            fprintf(stderr, "[%d]---- running (%d)\n", current_process->client_id, current_process->remaining_time);
        }
        pthread_mutex_unlock(&queue_mutex);

        int quantum = current_process->first_round_completed ? 7 : 3;
        int slice = (current_process->remaining_time < quantum) ? current_process->remaining_time : quantum;

        int i;
        int preempted = 0;
        for (i = 0; i < slice; i++) {
            sleep(1);

            // Send progress update to client
            char progress_msg[BUFFER_SIZE];
            snprintf(progress_msg, BUFFER_SIZE, "Demo %d/%d\n", 
                    current_process->burst_time - current_process->remaining_time + i + 1,
                    current_process->burst_time);
            
            // Find the client's connection fd (you'll need to store this in the process_t struct)
            int client_fd = current_process->client_fd;  // Add this field to process_t
            send(client_fd, progress_msg, strlen(progress_msg), 0);

            // After 1 second, check if a new, shorter job arrived
            pthread_mutex_lock(&queue_mutex);
            int remaining_after_this_second = current_process->remaining_time - (i + 1);

            process_t *check_proc = process_queue;
            int found_shorter_new_job = 0;
            while (check_proc != NULL) {
                if (check_proc != current_process && 
                    check_proc->arrival_time > current_start_time && 
                    check_proc->remaining_time < remaining_after_this_second) {
                    found_shorter_new_job = 1;
                    break;
                }
                check_proc = check_proc->next;
            }

            if (found_shorter_new_job) {
                // Preempt current process due to a newly arrived shorter job
                kill(current_process->pid, SIGSTOP);
                current_process->remaining_time = remaining_after_this_second;
                current_process->last_executed_time = time(NULL);
                current_process->is_running = 0;
                current_process->first_round_completed = 1;
                fprintf(stderr, "[%d]---- waiting (%d)\n", current_process->client_id, current_process->remaining_time);
                sem_post(&queue_sem);
                pthread_mutex_unlock(&queue_mutex);
                preempted = 1;
                break;
            }

            pthread_mutex_unlock(&queue_mutex);
        }

        if (!preempted) {
            // If we completed the allotted slice or the process finished
            pthread_mutex_lock(&queue_mutex);
            current_process->remaining_time -= i;  // account for the elapsed seconds
            current_process->last_executed_time = time(NULL);

            if (current_process->remaining_time <= 0) {
                // Process finished
                waitpid(current_process->pid, NULL, 0);
                fprintf(stderr, "[%d]---- ended (0)\n", current_process->client_id);
                remove_process(current_process);
                current_process = NULL;
            } else {
                // Time slice completed without finishing, so preempt normally
                kill(current_process->pid, SIGSTOP);
                current_process->is_running = 0;
                current_process->first_round_completed = 1;
                fprintf(stderr, "[%d]---- waiting (%d)\n", current_process->client_id, current_process->remaining_time);
                sem_post(&queue_sem);
            }

            pthread_mutex_unlock(&queue_mutex);
        }
    }
    return NULL;
}

void add_process(process_t *proc) {
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
    
    // First, check if all processes have completed their round
    process_t *curr = process_queue;
    int all_completed = 1;
    while (curr != NULL) {
        if (!curr->first_round_completed) {
            all_completed = 0;
            break;
        }
        curr = curr->next;
    }

    // If all processes completed their round, reset their round completion status
    if (all_completed) {
        curr = process_queue;
        while (curr != NULL) {
            curr->first_round_completed = 0;
            curr = curr->next;
        }
    }

    // Find the shortest remaining time process that hasn't completed its round
    // and isn't the same as the last process that ran (if possible)
    process_t *selected_process = NULL;
    int min_remaining_time = INT_MAX;
    
    // First try to find a process different from the last one
    curr = process_queue;
    while (curr != NULL) {
        if (!curr->first_round_completed && 
            curr->remaining_time < min_remaining_time && 
            curr != last_process) {
            min_remaining_time = curr->remaining_time;
            selected_process = curr;
        }
        curr = curr->next;
    }

    // If no different process found, then try including the last process
    if (selected_process == NULL) {
        min_remaining_time = INT_MAX;
        curr = process_queue;
        while (curr != NULL) {
            if (!curr->first_round_completed && 
                curr->remaining_time < min_remaining_time) {
                min_remaining_time = curr->remaining_time;
                selected_process = curr;
            }
            curr = curr->next;
        }
    }

    // If still no process found (all completed their round),
    // start new round with shortest remaining time (different from last if possible)
    if (selected_process == NULL) {
        // First try to find a different process
        curr = process_queue;
        min_remaining_time = INT_MAX;
        while (curr != NULL) {
            if (curr->remaining_time < min_remaining_time && 
                curr != last_process) {
                min_remaining_time = curr->remaining_time;
                selected_process = curr;
            }
            curr = curr->next;
        }

        // If no different process found, then include all processes
        if (selected_process == NULL) {
            curr = process_queue;
            min_remaining_time = INT_MAX;
            while (curr != NULL) {
                if (curr->remaining_time < min_remaining_time) {
                    min_remaining_time = curr->remaining_time;
                    selected_process = curr;
                }
                curr = curr->next;
            }
        }
    }

    // If still no process found (shouldn't happen if queue not empty)
    if (selected_process == NULL) {
        selected_process = process_queue;
    }

    last_process = selected_process;
    return selected_process;
}

void log_server(const char *format, ...) {
    // Not using this anymore for the main logs, 
    // but you can still use it for debugging if needed
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    va_end(args);
}
