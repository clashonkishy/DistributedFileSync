#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/stat.h>

#define BUFFER_SIZE 1024
#define PATH_MAX 1024

void combine_paths(const char *base_path, const char *relative_path, char *result) {
    result[0] = '\0';
    strcpy(result, base_path);

    if (result[strlen(result) - 1] != '/' && relative_path[0] != '/') {
        strcat(result, "/");
    }

    strcat(result, relative_path);
}

// Function to send the entire ignore list file contents as a single message
void send_ignore_list(int sock, const char *ignore_list_path) {
    FILE *file = fopen(ignore_list_path, "r");
    if (!file) {
        perror("[CLIENT ERROR] Failed to open ignore list file");
        return;
    }

    char ignore_data[BUFFER_SIZE] = "IGNORE$";  // Start message with "IGNORE$"
    size_t offset = strlen(ignore_data);
    
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\n")] = ',';  // Replace newline with comma for a list format
        size_t len = strlen(line);

        // Ensure we don't overflow the buffer
        if (offset + len >= BUFFER_SIZE - 1) {
            break;
        }

        strcpy(ignore_data + offset, line);
        offset += len;
    }

    fclose(file);

    // Remove the last comma if there are entries
    if (offset > 7) {
        ignore_data[offset - 1] = '\0';
    }

    // Send the entire ignore list as one string
    send(sock, ignore_data, strlen(ignore_data), 0);
    printf("[CLIENT LOG] Ignore list sent to server: %s\n", ignore_data);
}

int main(int argc, char *argv[]) {
    if (argc < 5) {
        printf("Usage: %s <server_ip> <server_port> <client_sync_dir> <ignore_list_file>\n", argv[0]);
        return 1;
    }

    char *server_ip = argv[1];
    int server_port = atoi(argv[2]);
    char *client_sync_dir = argv[3];
    char *ignore_list_path = argv[4];

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        return 1;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    inet_pton(AF_INET, server_ip, &server_addr.sin_addr);

    int attempts = 5;
    while (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0 && attempts > 0) {
        perror("Connection failed, retrying...");
        sleep(1);
        attempts--;
    }

    if (attempts == 0) {
        printf("Failed to connect after multiple attempts.\n");
        close(sock);
        return 1;
    }

    printf("Connected to server at %s:%d\n", server_ip, server_port);

    // **Send the ignore list as a single string**
    send_ignore_list(sock, ignore_list_path);

    // **Keep listening for messages from the server**
    char buffer[BUFFER_SIZE];
    int bytes_received;

    while (1) {
        bytes_received = recv(sock, buffer, BUFFER_SIZE - 1, 0);
        if (bytes_received <= 0) {
            printf("Server disconnected.\n");
            break;
        }
        buffer[bytes_received] = '\0';
        printf("[SERVER MESSAGE] %s\n", buffer);

        if (buffer[0] == 'C') {
            char filePath[PATH_MAX];
            int fileSize;
            
            // Extract file path and size
            if (sscanf(buffer, "C$%[^$]$%d$", filePath, &fileSize) < 2) {
                printf("[CLIENT ERROR] Invalid file format: %s\n", buffer);
                return;
            }
        
            char fullPath[PATH_MAX];
            combine_paths(client_sync_dir, filePath, fullPath);
        
            printf("[CLIENT LOG] Creating file: %s\n", fullPath);
        
            FILE *fp = fopen(fullPath, "wb");
            if (!fp) {
                perror("[CLIENT ERROR] Failed to create file");
                return;
            }
        
            if (fileSize == 0) {
                // Empty file case
                fclose(fp);
                printf("[CLIENT LOG] Empty file created: %s\n", fullPath);
            } else {
                // Locate the start of file data in buffer
                char *fileData = strchr(buffer, '$');
                for (int i = 0; i < 2; i++) {  // Skip past "C$path$size$"
                    fileData = strchr(fileData + 1, '$');
                    if (!fileData) {
                        printf("[CLIENT ERROR] Malformed data format.\n");
                        fclose(fp);
                        return;
                    }
                }
                fileData++;  // Move past last '$'
        
                // Write file data
                size_t written = fwrite(fileData, 1, fileSize, fp);
                fclose(fp);
        
                if (written != fileSize) {
                    printf("[CLIENT ERROR] File write incomplete: Expected %d bytes, wrote %zu\n", fileSize, written);
                } else {
                    printf("[CLIENT LOG] File written successfully: %s (%d bytes)\n", fullPath, fileSize);
                }
            }
        }
        else if (buffer[0] == 'Z') {
            char dirPath[PATH_MAX];
            sscanf(buffer, "Z$%[^$]", dirPath);

            char finPath[PATH_MAX];
            combine_paths(client_sync_dir, dirPath, finPath);

            printf("[CLIENT LOG] Creating directory: %s\n", finPath);

            if (mkdir(finPath, 0777) == 0) {
                printf("[CLIENT LOG] Directory created: %s\n", finPath);
            } else {
                perror("[CLIENT ERROR] Directory creation failed");
            }
        }
        else if (buffer[0] == 'F') {
            char fromPath[PATH_MAX], toPath[PATH_MAX];
            sscanf(buffer, "F$%[^$]$T$%[^$]", fromPath, toPath);

            char fullFromPath[PATH_MAX], fullToPath[PATH_MAX];
            combine_paths(client_sync_dir, fromPath, fullFromPath);
            combine_paths(client_sync_dir, toPath, fullToPath);

            printf("[CLIENT LOG] Moving: %s -> %s\n", fullFromPath, fullToPath);

            if (rename(fullFromPath, fullToPath) == 0) {
                printf("[CLIENT LOG] Move successful: %s -> %s\n", fullFromPath, fullToPath);
            } else {
                perror("[CLIENT ERROR] Move failed");
            }
        }
        else if (buffer[0] == 'D') {
            char filePath[PATH_MAX];
            sscanf(buffer, "D$%[^$]", filePath);

            char finPath[PATH_MAX];
            combine_paths(client_sync_dir, filePath, finPath);

            printf("[CLIENT LOG] Deleting file/directory: %s\n", finPath);

            struct stat path_stat;
            if (stat(finPath, &path_stat) == 0) {
                if (S_ISDIR(path_stat.st_mode)) {
                    if (rmdir(finPath) == 0) {
                        printf("[CLIENT LOG] Directory deleted: %s\n", finPath);
                    } else {
                        perror("[CLIENT ERROR] Directory deletion failed");
                    }
                } else {
                    if (remove(finPath) == 0) {
                        printf("[CLIENT LOG] File deleted: %s\n", finPath);
                    } else {
                        perror("[CLIENT ERROR] File deletion failed");
                    }
                }
            } else {
                perror("[CLIENT ERROR] File/Directory does not exist");
            }
        }
    }

    close(sock);
    return 0;
}
