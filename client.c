// client.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h> // For signal handling
#include <errno.h>  // For errno
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// Define constants for the server port, buffer size, and server IP address
#define PORT 12345
#define BUFFER_SIZE 4096
#define SERVER_IP "127.0.0.1"  // Replace with your server's IP address

int main(int argc, char *argv[])
{
    int sock_fd;
    struct sockaddr_in server_addr;
    char buf[BUFFER_SIZE];

    // Create a TCP socket
    if ((sock_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
        perror("socket");
        exit(1);
    }

    // Initialize server address structure
    server_addr.sin_family = AF_INET;                    // Use IPv4
    server_addr.sin_port = htons(PORT);                  // Convert port to network byte order
    inet_aton(SERVER_IP, &(server_addr.sin_addr));       // Convert IP address string to binary
    memset(&(server_addr.sin_zero), '\0', 8);            // Zero out the rest of the struct

    // Attempt to connect to the server
    if (connect(sock_fd, (struct sockaddr *)&server_addr, sizeof(struct sockaddr)) == -1)
    {
        perror("connect");
        exit(1);
    }

    printf("Connected to server %s on port %d\n", SERVER_IP, PORT);

    // Main loop for sending commands and receiving responses
    while (1)
    {
        // Prompt user for input
        printf("myshell$ ");
        fflush(stdout);  // Ensure the prompt is displayed immediately

        // Read user input
        if (fgets(buf, BUFFER_SIZE, stdin) == NULL)
        {
            // Handle EOF (Ctrl+D) gracefully
            printf("\n");
            break;
        }
        
        // Remove trailing newline from user input
        buf[strcspn(buf, "\n")] = '\0';

        // Check if user wants to exit
        if (strcmp(buf, "exit") == 0)
        {
            break;
        }

        if (strlen(buf) == 0)
        {
            continue;
        }

        // Send the command to the server
        if (send(sock_fd, buf, strlen(buf), 0) == -1)
        {
            perror("send");
            exit(1);
        }

        // Prepare to receive output from server
        char output_buf[BUFFER_SIZE];
        int numbytes;

        // Loop to receive data until server stops sending
        while ((numbytes = recv(sock_fd, output_buf, BUFFER_SIZE - 1, 0)) > 0)
        {
            // Null-terminate the received data
            output_buf[numbytes] = '\0';
            
            // Print the received data
            printf("%s", output_buf);
            
            // If we received less than a full buffer, assume the server is done sending
            if (numbytes < BUFFER_SIZE - 1)
            {
                break;
            }
        }
        
        // Check for receive errors
        if (numbytes == -1)
        {
            perror("recv");
            exit(1);
        }

        printf("\n"); // Print newline after output for readability
    }

    // Close the socket before exiting
    close(sock_fd);
    return 0;
}
