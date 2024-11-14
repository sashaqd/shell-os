/* myshell.c
 * A simple shell implementation in C that supports:
 * - Execution of commands with and without arguments
 * - Input and output redirection
 * - Piping up to three commands
 * - Error handling for various invalid inputs
 * - Built-in commands: exit
 * Extensive comments are provided to explain every single line and logical block.
 */

#include <stdio.h>      // For standard I/O functions
#include <stdlib.h>     // For general purpose functions
#include <string.h>     // For string handling functions
#include <unistd.h>     // For POSIX API (fork, exec, pipe, etc.)
#include <sys/wait.h>   // For waitpid()
#include <fcntl.h>      // For file control options (open flags)
#include <errno.h>      // For error handling

#define MAX_COMMAND_LENGTH 1024    // Maximum length of a command line
#define MAX_NUM_TOKENS 128         // Maximum number of tokens per command
#define MAX_NUM_PIPES 3            // Maximum number of pipes (supports up to 4 commands)

#define STDIN_FD 0     // File descriptor for standard input
#define STDOUT_FD 1    // File descriptor for standard output
#define STDERR_FD 2    // File descriptor for standard error

/* Function Declarations */
void display_prompt();                                              // Function to display the shell prompt
char *read_command();                                               // Function to read a command from the user
int parse_command(char *input, char **commands);                    // Function to parse the input into commands based on pipes
int parse_tokens(char *command, char **args);                       // Function to parse a command into tokens (arguments)
int execute_command(char **commands, int num_commands);             // Function to execute the parsed commands
int is_builtin(char *command);                                      // Function to check if a command is built-in
void execute_builtin(char **args);                                  // Function to execute built-in commands
int handle_redirection(char **args, int *in_fd, int *out_fd, int *err_fd); // Function to handle I/O redirection
void handle_error(const char *msg);                                 // Function to display error messages

int main() {
    while (1) {
        // Display the shell prompt
        display_prompt();

        // Read the command from the user
        char *input = read_command();
        if (input == NULL) {
            continue; // If input is NULL, display the prompt again
        }

        // Array to hold the individual commands separated by pipes
        char *commands[MAX_NUM_PIPES + 1] = {NULL};

        // Parse the input into separate commands based on pipes
        int num_commands = parse_command(input, commands);

        if (num_commands == -1) {
            // Error parsing command, free input and continue
            free(input);
            continue;
        }

        // Execute the parsed commands
        if (execute_command(commands, num_commands) == -1) {
            // Error executing command
            // Error messages are already displayed in execute_command
        }

        // Free allocated memory
        free(input);
        for (int i = 0; i < num_commands; i++) {
            free(commands[i]);
        }
    }

    return 0; // Return 0 to indicate successful execution (this line will never be reached)
}

/* Function to display the shell prompt */
void display_prompt() {
    printf("myshell$ ");    // Print the prompt
    fflush(stdout);         // Ensure the prompt is displayed immediately
}

/* Function to read a command line from the user */
char *read_command() {
    // Allocate memory for the input string
    char *input = (char *)malloc(sizeof(char) * MAX_COMMAND_LENGTH);
    if (input == NULL) {
        perror("Allocation error"); // Print error message if memory allocation fails
        exit(EXIT_FAILURE);         // Exit the program with failure status
    }

    // Read a line of input from the user
    if (fgets(input, MAX_COMMAND_LENGTH, stdin) == NULL) {
        free(input); // Free the allocated memory
        // Handle Ctrl+D (EOF)
        if (feof(stdin)) {
            printf("\n");    // Print a newline for neatness
            exit(EXIT_SUCCESS); // Exit the shell gracefully
        }
        return NULL; // Return NULL to indicate no input
    }

    // Remove trailing newline character
    size_t len = strlen(input);        // Get the length of the input
    if (len > 0 && input[len - 1] == '\n') {
        input[len - 1] = '\0';         // Replace newline with null terminator
    }

    return input; // Return the input string
}

/* Function to parse the command line into separate commands based on pipes */
int parse_command(char *input, char **commands) {
    int num_commands = 0;            // Number of commands found
    char *ptr = input;               // Pointer to iterate over the input string
    char *command_start = ptr;       // Pointer to the start of the current command

    while (*ptr != '\0') {
        // If we find a pipe symbol
        if (*ptr == '|') {
            // Extract the command between command_start and ptr
            int command_length = ptr - command_start;

            // Skip leading spaces
            while (*command_start == ' ' && command_length > 0) {
                command_start++;
                command_length--;
            }
            // Skip trailing spaces
            while (command_length > 0 && *(command_start + command_length - 1) == ' ') {
                command_length--;
            }

            // Check if the command is empty after trimming
            if (command_length == 0) {
                handle_error("Empty command between pipes.");
                return -1; // Return -1 to indicate an error
            }

            // Allocate memory and copy the command into the commands array
            commands[num_commands] = (char *)malloc(command_length + 1);
            strncpy(commands[num_commands], command_start, command_length);
            commands[num_commands][command_length] = '\0'; // Null-terminate the command
            num_commands++; // Increment the number of commands

            // Check if the maximum number of pipes is exceeded
            if (num_commands > MAX_NUM_PIPES + 1) {
                handle_error("Too many pipes. Maximum of 3 pipes supported.");
                return -1; // Return -1 to indicate an error
            }

            ptr++; // Move past the pipe symbol
            command_start = ptr; // Set the start of the next command
        } else {
            ptr++; // Move to the next character
        }
    }

    // Handle the last command after the last pipe (or the whole command if no pipes)
    int command_length = ptr - command_start;

    // Skip leading spaces
    while (*command_start == ' ' && command_length > 0) {
        command_start++;
        command_length--;
    }
    // Skip trailing spaces
    while (command_length > 0 && *(command_start + command_length - 1) == ' ') {
        command_length--;
    }

    // Check if the last command is empty
    if (command_length == 0) {
        // Check if input ends with a pipe (e.g., "ls |")
        if (*(ptr - 1) == '|') {
            handle_error("Command missing after pipe.");
            return -1; // Return -1 to indicate an error
        } else {
            // No command entered
            handle_error("Empty command.");
            return -1; // Return -1 to indicate an error
        }
    }

    // Allocate memory and copy the last command into the commands array
    commands[num_commands] = (char *)malloc(command_length + 1);
    strncpy(commands[num_commands], command_start, command_length);
    commands[num_commands][command_length] = '\0'; // Null-terminate the command
    num_commands++; // Increment the number of commands

    return num_commands; // Return the total number of commands parsed
}

/* Function to parse a command into tokens (arguments), handling quotes */
int parse_tokens(char *command, char **args) {
    int num_args = 0;             // Number of arguments found
    char *ptr = command;          // Pointer to iterate over the command string
    int in_quotes = 0;            // Flag to indicate if we are inside quotes
    char quote_char = '\0';       // The type of quote (' or ")
    char token[MAX_COMMAND_LENGTH]; // Buffer to hold the current token
    int token_index = 0;          // Index in the token buffer

    while (*ptr != '\0') {
        // Skip leading whitespace
        while (*ptr == ' ' || *ptr == '\t') ptr++;

        // Reset token
        token_index = 0;

        // Check if end of command
        if (*ptr == '\0') break;

        // Check for quotes
        if (*ptr == '\'' || *ptr == '"') {
            in_quotes = 1;        // Set the in_quotes flag
            quote_char = *ptr;    // Store the quote character
            ptr++;                // Move past the quote
        } else {
            in_quotes = 0;        // Not in quotes
        }

        // Collect characters into token
        while (*ptr != '\0' && ((in_quotes && *ptr != quote_char) || (!in_quotes && *ptr != ' ' && *ptr != '\t'))) {
            token[token_index++] = *ptr++; // Add character to token and move to next
        }

        // If we were in quotes, skip the closing quote
        if (in_quotes && *ptr == quote_char) {
            in_quotes = 0;        // Reset in_quotes flag
            ptr++;                // Move past the closing quote
        }

        token[token_index] = '\0'; // Null-terminate the token

        // Add token to args
        args[num_args] = strdup(token); // Duplicate the token string
        num_args++; // Increment the number of arguments

        // Check for argument overflow
        if (num_args >= MAX_NUM_TOKENS - 1) {
            dprintf(STDERR_FD, "Shell error: Too many arguments.\n");
            break;
        }
    }

    args[num_args] = NULL; // Null-terminate the args array

    return num_args; // Return the number of arguments parsed
}

/* Function to execute the parsed commands */
int execute_command(char **commands, int num_commands) {
    int pipe_fds[2 * MAX_NUM_PIPES]; // Array to hold pipe file descriptors
    int status;                      // Variable to hold the status of child processes
    pid_t pid;                       // Process ID for forked processes

    // Create the required number of pipes
    for (int i = 0; i < num_commands - 1; i++) {
        if (pipe(pipe_fds + i * 2) < 0) {
            perror("Pipe creation failed"); // Print error if pipe creation fails
            return -1; // Return -1 to indicate an error
        }
    }

    int command_index = 0; // Index of the current command
    int fd_index = 0;      // Index in the pipe_fds array

    // Add a flag to track if any output was produced
    int output_produced = 0;
    
    // Add a pipe to capture output from the last command
    int output_pipe[2];
    if (pipe(output_pipe) < 0) {
        perror("Output pipe creation failed");
        return -1;
    }

    while (command_index < num_commands) {
        // Array to hold the arguments for the current command
        char *args[MAX_NUM_TOKENS] = {NULL};

        // Parse arguments for the current command
        int num_args = parse_tokens(commands[command_index], args);

        if (num_args == 0) {
            handle_error("Empty command.");
            return -1; // Return -1 to indicate an error
        }

        // Check for built-in commands
        if (is_builtin(args[0])) {
            execute_builtin(args);
            // Free allocated memory for args
            for (int i = 0; args[i] != NULL; i++) {
                free(args[i]);
            }
            command_index++;
            fd_index += 2;
            continue; // Continue to the next command
        }

        // Handle input/output/error redirection
        int in_fd = STDIN_FD;   // File descriptor for input
        int out_fd = STDOUT_FD; // File descriptor for output
        int err_fd = STDERR_FD; // File descriptor for error output

        // Updated to check for errors returned by handle_redirection()
        if (handle_redirection(args, &in_fd, &out_fd, &err_fd) < 0) {
            // Error occurred during redirection handling
            // Free allocated memory for args
            for (int i = 0; args[i] != NULL; i++) {
                free(args[i]);
            }
            // Close any open file descriptors and pipes
            for (int i = 0; i < 2 * (num_commands - 1); i++) {
                close(pipe_fds[i]);
            }
            return -1; // Return -1 to indicate an error
        }

        // Check if args is empty after redirection handling
        if (args[0] == NULL) {
            handle_error("Invalid command.");
            // Free allocated memory for args
            for (int i = 0; args[i] != NULL; i++) {
                free(args[i]);
            }
            // Close any open file descriptors and pipes
            for (int i = 0; i < 2 * (num_commands - 1); i++) {
                close(pipe_fds[i]);
            }
            return -1; // Return -1 to indicate an error
        }

        // Fork a child process
        pid = fork();
        if (pid < 0) {
            perror("Fork failed"); // Print error if fork fails
            // Free allocated memory for args
            for (int i = 0; args[i] != NULL; i++) {
                free(args[i]);
            }
            // Close any open file descriptors and pipes
            for (int i = 0; i < 2 * (num_commands - 1); i++) {
                close(pipe_fds[i]);
            }
            return -1; // Return -1 to indicate an error
        }

        if (pid == 0) {
            // Child process

            // If not the first command, redirect input from the previous pipe
            if (command_index != 0) {
                if (dup2(pipe_fds[(command_index - 1) * 2], STDIN_FD) < 0) {
                    perror("dup2 input failed"); // Print error if dup2 fails
                    exit(EXIT_FAILURE); // Exit child process with failure status
                }
            }

            // If not the last command, redirect output to the next pipe
            if (command_index != num_commands - 1) {
                if (dup2(pipe_fds[command_index * 2 + 1], STDOUT_FD) < 0) {
                    perror("dup2 output failed"); // Print error if dup2 fails
                    exit(EXIT_FAILURE); // Exit child process with failure status
                }
            }

            // Redirect input if necessary
            if (in_fd != STDIN_FD) {
                if (dup2(in_fd, STDIN_FD) < 0) {
                    perror("dup2 input redirection failed");
                    exit(EXIT_FAILURE);
                }
                close(in_fd); // Close the original file descriptor
            }
            // Redirect output if necessary
            if (out_fd != STDOUT_FD) {
                if (dup2(out_fd, STDOUT_FD) < 0) {
                    perror("dup2 output redirection failed");
                    exit(EXIT_FAILURE);
                }
                close(out_fd); // Close the original file descriptor
            }
            // Redirect error output if necessary
            if (err_fd != STDERR_FD) {
                if (dup2(err_fd, STDERR_FD) < 0) {
                    perror("dup2 error redirection failed");
                    exit(EXIT_FAILURE);
                }
                close(err_fd); // Close the original file descriptor
            }

            // Close all pipe file descriptors in child
            for (int i = 0; i < 2 * (num_commands - 1); i++) {
                close(pipe_fds[i]);
            }

            // If this is the last command, redirect output through our capture pipe
            if (command_index == num_commands - 1 && out_fd == STDOUT_FD) {
                if (dup2(output_pipe[1], STDOUT_FD) < 0) {
                    perror("dup2 output capture failed");
                    exit(EXIT_FAILURE);
                }
            }

            // Execute the command
            if (execvp(args[0], args) < 0) {
                char error_msg[MAX_COMMAND_LENGTH];
                snprintf(error_msg, MAX_COMMAND_LENGTH, "Failed to execute '%s': %s", args[0], strerror(errno));
                handle_error(error_msg);
                exit(EXIT_FAILURE); // Exit child process with failure status
            }
        } else {
            // Parent process

            // Close the used ends of the pipe in the parent
            if (command_index != 0) {
                close(pipe_fds[(command_index - 1) * 2]);
                close(pipe_fds[(command_index - 1) * 2 + 1]);
            }
        }

        // Free allocated memory for args
        for (int i = 0; args[i] != NULL; i++) {
            free(args[i]);
        }

        command_index++; // Move to the next command
        fd_index += 2;   // Increment the file descriptor index
    }

    // Close any remaining pipe file descriptors in parent
    for (int i = 0; i < 2 * (num_commands - 1); i++) {
        close(pipe_fds[i]);
    }

    // Close write end of output pipe in parent
    close(output_pipe[1]);

    // Read and check for output from the last command
    char buffer[1024];
    ssize_t bytes_read = read(output_pipe[0], buffer, sizeof(buffer));
    if (bytes_read > 0) {
        // Check if output is just a newline
        if (bytes_read == 1 && buffer[0] == '\n') {
            handle_error("No output from myshell\n");
        } else {
            // Output exists, write it to stdout
            write(STDOUT_FD, buffer, bytes_read);
            while ((bytes_read = read(output_pipe[0], buffer, sizeof(buffer))) > 0) {
                write(STDOUT_FD, buffer, bytes_read);
            }
        }
    } else if (bytes_read == 0) {
        // No output was produced
        printf("No output from myshell\n");
    }

    close(output_pipe[0]);

    // Wait for all child processes to finish
    for (int i = 0; i < num_commands; i++) {
        wait(&status); // Wait for each child process
    }

    return 0; // Return 0 to indicate successful execution
}

/* Function to check if a command is a built-in command */
int is_builtin(char *command) {
    if (strcmp(command, "exit") == 0) {
        return 1; // Return 1 if the command is 'exit'
    }
    // Add more built-in commands here if needed
    return 0; // Return 0 if the command is not built-in
}

/* Function to execute built-in commands */
void execute_builtin(char **args) {
    if (strcmp(args[0], "exit") == 0) {
        exit(EXIT_SUCCESS); // Exit the shell program
    }
    // Implement other built-in commands here if needed
}

/* Function to handle input/output/error redirection */
int handle_redirection(char **args, int *in_fd, int *out_fd, int *err_fd) {
    int i = 0; // Index to iterate over args
    while (args[i] != NULL) {
        if (strcmp(args[i], "<") == 0) {
            // Input redirection
            if (args[i + 1] == NULL) {
                handle_error("Input file not specified.");
                return -1; // Return -1 to indicate an error
            }
            *in_fd = open(args[i + 1], O_RDONLY); // Open the input file
            if (*in_fd < 0) {
                perror("Input file error"); // Print error if file cannot be opened
                return -1; // Return -1 to indicate an error
            }
            // Remove the redirection symbol and filename from args
            int j = i;
            while (args[j + 2] != NULL) {
                args[j] = args[j + 2]; // Shift arguments left
                j++;
            }
            args[j] = NULL;     // Null-terminate the array
            args[j + 1] = NULL; // Ensure the next position is also NULL
            continue; // Re-evaluate at the same index
        } else if (strcmp(args[i], ">") == 0) {
            // Output redirection
            if (args[i + 1] == NULL) {
                handle_error("Output file not specified.");
                return -1; // Return -1 to indicate an error
            }
            *out_fd = open(args[i + 1], O_WRONLY | O_CREAT | O_TRUNC, 0644); // Open/create the output file
            if (*out_fd < 0) {
                perror("Output file error");
                return -1; // Return -1 to indicate an error
            }
            // Remove the redirection symbol and filename from args
            int j = i;
            while (args[j + 2] != NULL) {
                args[j] = args[j + 2]; // Shift arguments left
                j++;
            }
            args[j] = NULL;
            args[j + 1] = NULL;
            continue; // Re-evaluate at the same index
        } else if (strcmp(args[i], "2>") == 0) {
            // Error output redirection
            if (args[i + 1] == NULL) {
                handle_error("Error output file not specified.");
                return -1; // Return -1 to indicate an error
            }
            *err_fd = open(args[i + 1], O_WRONLY | O_CREAT | O_TRUNC, 0644); // Open/create the error output file
            if (*err_fd < 0) {
                perror("Error output file error");
                return -1; // Return -1 to indicate an error
            }
            // Remove the redirection symbol and filename from args
            int j = i;
            while (args[j + 2] != NULL) {
                args[j] = args[j + 2]; // Shift arguments left
                j++;
            }
            args[j] = NULL;
            args[j + 1] = NULL;
            continue; // Re-evaluate at the same index
        }
        i++; // Move to the next argument
    }
    return 0; // Return 0 to indicate success
}

/* Function to handle shell-level error messages */
void handle_error(const char *msg) {
    // Write to STDOUT_FD to ensure error messages follow the pipe chain
    dprintf(STDOUT_FD, "Shell error: %s\n", msg);
}
