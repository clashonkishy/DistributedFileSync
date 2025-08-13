#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/inotify.h>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>


#define MAX_WATCHES 1024  // Max number of directories to watch

char *wd_to_path[MAX_WATCHES];  // Map watch descriptor (wd) to directory path
pthread_mutex_t wd_map_lock = PTHREAD_MUTEX_INITIALIZER;  // Mutex for thread safety


#define BUFFER_SIZE 1024
#define EVENT_SIZE (sizeof(struct inotify_event))
#define EVENT_BUF_LEN (1024 * (EVENT_SIZE + 16))
#define MAX_IGNORE_ENTRIES 100

// Struct to store client data
typedef struct {
    int socket;
    char *ignore_list[MAX_IGNORE_ENTRIES]; // Stores ignore paths
    int ignore_count;
} Client;

Client *clients;
int client_count = 0;
int max_clients;
int server_port;
char sync_dir[PATH_MAX];

pthread_mutex_t lock;

// Function to print currently connected clients
void print_clients() {
    printf("\nCurrent Connected Clients (%d/%d):\n", client_count, max_clients);
    for (int i = 0; i < client_count; i++) {
        printf("Client %d | Socket: %d\n", i + 1, clients[i].socket);
    }
    printf("---------------------------------\n");
}

void remove_client(int client_sock) {
    pthread_mutex_lock(&lock);
    for (int i = 0; i < client_count; i++) {
        if (clients[i].socket == client_sock) {
            printf("Client (Socket: %d) disconnected\n", client_sock);
            close(client_sock);

            // Free allocated ignore list entries
            for (int j = 0; j < clients[i].ignore_count; j++) {
                free(clients[i].ignore_list[j]);  // Free each ignore entry
            }

            // Shift remaining clients down
            for (int j = i; j < client_count - 1; j++) {
                clients[j] = clients[j + 1];
            }

            client_count--;  // Reduce client count
            break;
        }
    }
    pthread_mutex_unlock(&lock);
}

void *handle_client(void *arg) {
    int client_sock = *(int *)arg;
    free(arg);  // Free dynamically allocated memory for client socket

    char buffer[BUFFER_SIZE];
    while (1) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes_received = recv(client_sock, buffer, BUFFER_SIZE, 0);

        if (bytes_received <= 0) {
            // Client disconnected
            printf("Client (Socket: %d) disconnected.\n", client_sock);
            remove_client(client_sock);  // Call remove_client when client disconnects
            pthread_exit(NULL);
        }

        printf("Received from client %d: %s\n", client_sock, buffer);
    }
    return NULL;
}


void findext(char* message, char* ext) {
    char *dot = strrchr(message, '.');  // Find the last occurrence of '.'
    
    if (!dot || dot == message) {  // No valid dot found or it's at the beginning
        ext[0] = '\0';  // No extension found
        return;
    }

    char *dollar = strchr(dot, '$');  // Find the next '$' after the dot
    if (!dollar) {  
        ext[0] = '\0';  // No '$' found, treat as no extension
        return;
    }

    size_t len = dollar - (dot + 1);  // Length of the extension
    if (len > 0 && len < BUFFER_SIZE) {  // Ensure valid length
        strncpy(ext, dot + 1, len);
        ext[len] = '\0';  // Null-terminate the extracted extension
    } else {
        ext[0] = '\0';  // Invalid case
    }

    printf("findext result: %s\n", ext);
}

bool checkignore(char* message, char* ignore_list) {
    char temp[BUFFER_SIZE];
    findext(message, temp);

    printf("Ignore list: \"%s\"\n", ignore_list);
    printf("Checking extension: \"%s\"\n", temp);

    // If no extension is found, allow it (return true)
    if (temp[0] == '\0') {
        printf("No extension found. Allowing file.\n");
        return true;
    }

    // Check if the extracted extension exists in the ignore list
    if (strstr(ignore_list, temp) != NULL) {
        printf("Extension \"%s\" found in ignore list. Ignoring file.\n", temp);
        return false; // Ignore the file
    }

    printf("Extension \"%s\" not in ignore list. Allowing file.\n", temp);
    return true; // Allow the file
}

// Broadcast a message to all connected clients
void broadcast_message(const char *message) {
    pthread_mutex_lock(&lock);
    for (int i = 0; i < client_count; i++) {
        if(checkignore(message, clients[i].ignore_list[0])){
            if (send(clients[i].socket, message, strlen(message), 0) < 0) {
                perror("Failed to send message to client");
                remove_client(clients[i].socket);
            }
        }
    }
    pthread_mutex_unlock(&lock);
}

void add_watch_recursive(int inotify_fd, const char *dir_path) {
    DIR *dir = opendir(dir_path);
    if (!dir) {
        perror("opendir failed");
        return;
    }

    int wd = inotify_add_watch(inotify_fd, dir_path, IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);
    if (wd < 0) {
        perror("inotify_add_watch failed");
    } else {
        printf("Watching directory: %s\n", dir_path);
        
        // Store the mapping between wd and dir_path
        pthread_mutex_lock(&wd_map_lock);
        wd_to_path[wd] = strdup(dir_path);  // Duplicate string to store safely
        pthread_mutex_unlock(&wd_map_lock);
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR && strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
            char subdir_path[PATH_MAX];
            snprintf(subdir_path, PATH_MAX, "%s/%s", dir_path, entry->d_name);
            add_watch_recursive(inotify_fd, subdir_path);
        }
    }
    closedir(dir);
}


bool is_directory(const char *path) {
    struct stat path_stat;
    if (stat(path, &path_stat) != 0) {
        perror("stat failed");
        return false;  // Error case
    }
    return S_ISDIR(path_stat.st_mode);
}

bool is_file(const char *path) {
    struct stat path_stat;
    if (stat(path, &path_stat) != 0) {
        perror("stat failed");
        return false;  // Error case
    }
    return S_ISREG(path_stat.st_mode);
}

void strip_server_path(const char *server_path, const char *event_path, char *relative_path) {
    size_t server_len = strlen(server_path);

    // Check if event_path starts with server_path
    if (strncmp(event_path, server_path, server_len) == 0) {
        // Skip the server path and remove the leading slash if present
        if (event_path[server_len] == '/') {
            strcpy(relative_path, event_path + server_len + 1);
        } else {
            strcpy(relative_path, event_path + server_len);
        }
    } else {
        // If not matching, return full event path
        strcpy(relative_path, event_path);
    }
}

// Thread function to monitor the sync directory
void *monitor_directory(void *arg) {
    int inotify_fd = inotify_init();
    if (inotify_fd < 0) {
        perror("inotify_init failed");
        return NULL;
    }

    add_watch_recursive(inotify_fd, sync_dir);
    char buffer[EVENT_BUF_LEN];

    while (1) {
        int length = read(inotify_fd, buffer, EVENT_BUF_LEN);
        if (length < 0) {
            perror("read failed");
            continue;
        }

        int i = 0;
        while (i < length) {
            struct inotify_event *event = (struct inotify_event *)&buffer[i];
            if (event->len) {
                char event_path[PATH_MAX];
               // Get directory path associated with watch descriptor
                pthread_mutex_lock(&wd_map_lock);
                const char *dir_path = (event->wd >= 0 && event->wd < MAX_WATCHES && wd_to_path[event->wd]) ? wd_to_path[event->wd] : sync_dir;
                pthread_mutex_unlock(&wd_map_lock);

                // Construct full path of the event
                snprintf(event_path, PATH_MAX, "%s/%s", dir_path, event->name);

                char message[BUFFER_SIZE];

                if (event->mask & IN_CREATE) {
                    struct stat path_stat;
                    if (stat(event_path, &path_stat) == 0 && S_ISDIR(path_stat.st_mode)) {
                        char rel_path[PATH_MAX];
                        strip_server_path(sync_dir, event_path, rel_path);

                        if(is_directory(event_path)){
                            snprintf(message, BUFFER_SIZE, "Z$%s$", rel_path);
                        }
                        else{
                            // Read file data
                            FILE *file = fopen(event_path, "rb");
                            if (file) {
                                size_t file_size = path_stat.st_size;
                                char *file_data = malloc(file_size);
                                if (file_data) {
                                    fread(file_data, 1, file_size, file);
                                    fclose(file);

                                    // Create message with file data
                                    int header_len = snprintf(message, BUFFER_SIZE, "C$%s$%zu$", rel_path, file_size);
                                    memcpy(message + header_len, file_data, file_size);

                                    free(file_data);
                                } else {
                                    fclose(file);
                                    snprintf(message, BUFFER_SIZE, "C$%s$0$", rel_path);
                                }
                            } else {
                                snprintf(message, BUFFER_SIZE, "C$%s$0$", rel_path);
                            }
                        }
                        printf("[SERVER LOG] Directory Created: %s\n", event_path);
                        broadcast_message(message);
                        add_watch_recursive(inotify_fd, event_path);
                    } else {
                        char rel_path[PATH_MAX];
                        strip_server_path(sync_dir, event_path, rel_path);

                        if(is_directory(event_path)){
                            snprintf(message, BUFFER_SIZE, "Z$%s$", rel_path);
                        }
                        else{
                            // Read file data
                            FILE *file = fopen(event_path, "rb");
                            if (file) {
                                size_t file_size = path_stat.st_size;
                                char *file_data = malloc(file_size);
                                if (file_data) {
                                    fread(file_data, 1, file_size, file);
                                    fclose(file);

                                    // Create message with file data
                                    int header_len = snprintf(message, BUFFER_SIZE, "C$%s$%zu$", rel_path, file_size);
                                    memcpy(message + header_len, file_data, file_size);

                                    free(file_data);
                                } else {
                                    fclose(file);
                                    snprintf(message, BUFFER_SIZE, "C$%s$0$", rel_path);
                                }
                            } else {
                                snprintf(message, BUFFER_SIZE, "C$%s$0$", rel_path);
                            }
                        }
                        printf("[SERVER LOG] File Created: %s\n", event_path);
                        broadcast_message(message);
                    }
                }
                if (event->mask & IN_DELETE) {
                    pthread_mutex_lock(&wd_map_lock);
                    if (wd_to_path[event->wd]) {
                        free(wd_to_path[event->wd]);  // Free memory
                        wd_to_path[event->wd] = NULL;
                    }
                    pthread_mutex_unlock(&wd_map_lock);

                    char rel_path[PATH_MAX];
                    strip_server_path(sync_dir, event_path, rel_path);
                    
                    if(is_directory(event_path)){
                        snprintf(message, BUFFER_SIZE, "E$%s", rel_path);
                    }
                    else{
                        snprintf(message, BUFFER_SIZE, "D$%s", rel_path);
                    }

                    printf("[SERVER LOG] File/Directory Deleted: %s\n", event_path);
                    broadcast_message(message);
                }

                int offset;

                if (event->mask & IN_MOVED_FROM) {
                    char rel_path[PATH_MAX];
                    strip_server_path(sync_dir, event_path, rel_path);

                    offset = snprintf(message, BUFFER_SIZE, "F$%s$", rel_path);
                    printf("[SERVER LOG] File/Directory Moved From: %s\n", event_path);
                    //broadcast_message(message);
                }
                if (event->mask & IN_MOVED_TO) {
                    char rel_path[PATH_MAX];
                    strip_server_path(sync_dir, event_path, rel_path);

                    snprintf(message+offset, BUFFER_SIZE, "T$%s", rel_path);
                    printf("[SERVER LOG] File/Directory Moved To: %s\n", event_path);
                    broadcast_message(message);
                    struct stat path_stat;
                    if (stat(event_path, &path_stat) == 0 && S_ISDIR(path_stat.st_mode)) {
                        add_watch_recursive(inotify_fd, event_path);
                    }
                }
            }
            i += EVENT_SIZE + event->len;
        }
    }
    close(inotify_fd);
    return NULL;
}

void receive_ignore_list(int client_sock, Client *client) {
    char buffer[BUFFER_SIZE];
    memset(buffer, 0, BUFFER_SIZE);

    if (recv(client_sock, buffer, BUFFER_SIZE, 0) <= 0) {
        perror("Failed to receive ignore list");
        return;
    }

    client->ignore_count = 0;
    char *token = strtok(buffer, ";");
    while (token && client->ignore_count < MAX_IGNORE_ENTRIES) {
        client->ignore_list[client->ignore_count] = strdup(token);
        client->ignore_count++;
        token = strtok(NULL, ";");
    }

    printf("[SERVER LOG] Received Ignore List for Client %d:\n", client_sock);
    for (int i = 0; i < client->ignore_count; i++) {
        printf("  - %s\n", client->ignore_list[i]);
    }
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        printf("Usage: %s <server_dir_path> <port> <max_clients>\n", argv[0]);
        return 1;
    }

    strncpy(sync_dir, argv[1], PATH_MAX);
    server_port = atoi(argv[2]);
    max_clients = atoi(argv[3]); // Taking max_clients from command-line argument

    int server_sock, client_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_size = sizeof(client_addr);
    pthread_t monitor_thread;

    pthread_mutex_init(&lock, NULL);
    pthread_create(&monitor_thread, NULL, monitor_directory, NULL);
    pthread_detach(monitor_thread);

    clients = (Client *)malloc(max_clients * sizeof(Client)); // Allocate memory for clients
    if (!clients) {
        perror("Memory allocation failed");
        return 1;
    }

    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(server_port);

    bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
    listen(server_sock, max_clients); // Use max_clients here
    printf("Server listening on port %d...\n", server_port);

    while (1) {
        client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &addr_size);
        if (client_sock < 0) continue;
    
        pthread_mutex_lock(&lock);
        if (client_count < max_clients) {
            clients[client_count].socket = client_sock;
            receive_ignore_list(client_sock, &clients[client_count]); // Receive ignore list
            client_count++;
    
            pthread_t client_thread;
            int *new_sock = malloc(sizeof(int));
            *new_sock = client_sock;
            pthread_create(&client_thread, NULL, handle_client, new_sock);
            pthread_detach(client_thread);  // Automatically clean up thread
    
            print_clients();
        } else {
            close(client_sock);
        }
        pthread_mutex_unlock(&lock);
    }
    

    close(server_sock);
    free(clients); // Free allocated memory
    return 0;
}

