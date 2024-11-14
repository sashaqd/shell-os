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

#define PORT 12345
#define BUFFER_SIZE 4096

void *handle_client(void *arg)
{
    int conn_fd = *(int *)arg;
    free(arg); // Free the allocated memory
    char buf[BUFFER_SIZE];

    // Handle client communication
    while (1)
    {
        // Read command from client
        memset(buf, 0, BUFFER_SIZE);
        int numbytes = recv(conn_fd, buf, BUFFER_SIZE - 1, 0);
        if (numbytes == -1)
        {
            perror("recv");
            break;
        }
        else if (numbytes == 0)
        {
            // Connection closed by client
            fprintf(stderr, "Server: Client disconnected\n");
            break;
        }

        buf[numbytes] = '\0'; // Null-terminate the received data

        fprintf(stderr, "Server: Received command: %s\n", buf);

        // Set up pipes for communication with myshell
        int pipe_stdin[2];  // For writing to myshell's stdin
        int pipe_stdout[2]; // For reading from myshell's stdout

        if (pipe(pipe_stdin) == -1 || pipe(pipe_stdout) == -1)
        {
            perror("pipe");
            break;
        }

        // Fork another child process to execute myshell
        pid_t pid2 = fork();
        if (pid2 == -1)
        {
            perror("fork");
            break;
        }
        else if (pid2 == 0)
        {
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
        }
        else
        {
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
            while ((read_bytes = read(pipe_stdout[0], output_buf, BUFFER_SIZE - 1)) > 0)
            {
                output_buf[read_bytes] = '\0'; // Null-terminate
                strcat(full_output, output_buf);
                total_bytes += read_bytes;
            }
            close(pipe_stdout[0]);

            // Wait for myshell process to finish
            waitpid(pid2, NULL, 0);

            // Create a new buffer for cleaned output
            char cleaned_output[BUFFER_SIZE * 10] = {0};
            char *read_ptr = full_output;
            char *write_ptr = cleaned_output;

            // Process the output line by line
            char *line;
            char *saveptr;
            char *temp = strdup(full_output);  // Create a copy for strtok_r

            line = strtok_r(temp, "\n", &saveptr);
            while (line != NULL) {
                // Skip lines that only contain the prompt
                if (strcmp(line, "myshell$ ") != 0) {
                    // Remove prompt if it exists at the start of the line
                    if (strncmp(line, "myshell$ ", 9) == 0) {
                        line += 9;
                    }
                    // Add the cleaned line to output
                    write_ptr += sprintf(write_ptr, "%s\n", line);
                }
                line = strtok_r(NULL, "\n", &saveptr);
            }
            free(temp);

            // Calculate final length
            total_bytes = strlen(cleaned_output);

            // Remove trailing newline if it exists
            if (total_bytes > 0 && cleaned_output[total_bytes - 1] == '\n') {
                cleaned_output[--total_bytes] = '\0';
            }

            // Send cleaned output back to client
            if (send(conn_fd, cleaned_output, total_bytes, 0) == -1)
            {
                perror("send");
            }
        }
    }

    close(conn_fd);
    return NULL;
}

int main()
{
    int listen_fd, conn_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t sin_size;
    int yes = 1;

    // Create a TCP socket
    if ((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
        perror("socket");
        exit(1);
    }

    // Set socket option to reuse address (useful for server restarts)
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1)
    {
        perror("setsockopt");
        exit(1);
    }

    // Configure server address structure
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);           // Convert port to network byte order
    server_addr.sin_addr.s_addr = INADDR_ANY;     // Bind to all available interfaces
    memset(&(server_addr.sin_zero), '\0', 8);     // Zero out the rest of the struct

    // Bind the socket to the specified address and port
    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(struct sockaddr)) == -1)
    {
        perror("bind");
        exit(1);
    }

    // Start listening for incoming connections
    if (listen(listen_fd, 10) == -1)  // Allow up to 10 pending connections
    {
        perror("listen");
        exit(1);
    }

    fprintf(stderr, "Server: Waiting for connections on port %d...\n", PORT);

    // Main server loop
    while (1)
    {
        // Accept a new client connection
        sin_size = sizeof(struct sockaddr_in);
        conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &sin_size);
        if (conn_fd == -1)
        {
            perror("accept");
            continue;  // Continue listening if accept fails
        }

        fprintf(stderr, "Server: Got connection from %s\n", inet_ntoa(client_addr.sin_addr));

        // Allocate memory for conn_fd to pass to the thread
        int *conn_fd_ptr = malloc(sizeof(int));
        if (conn_fd_ptr == NULL)
        {
            perror("malloc");
            close(conn_fd);
            continue;
        }
        *conn_fd_ptr = conn_fd;

        // Create a new thread to handle the client
        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, handle_client, conn_fd_ptr) != 0)
        {
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
