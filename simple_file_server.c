#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sendfile.h>  // 用于零拷贝
#include <syslog.h>        // 日志兼容
#include <signal.h>        // 信号处理
#include <ctype.h>         
#include <cjson/cJSON.h> //完整 cJSON 库

#define DEFAULT_PORT 18945
#define DEFAULT_BACKLOG 100
#define DEFAULT_THREAD_POOL_SIZE 64
#define DEFAULT_ROOT_DIR "."
#define MAX_BUFFER 1024
#define QUEUE_SIZE 1024


// 配置结构体
typedef struct {
    int port;
    int backlog;
    int thread_pool_size;
    char *root_dir;
} config_t;

// 任务队列
typedef struct {
    int client_sock;
} task_t;

task_t task_queue[QUEUE_SIZE];
int queue_head = 0, queue_tail = 0;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;

// 客户端数据
typedef struct {
    int client_sock;
    char *root_dir;
} client_data_t;

// MIME类型映射
const char *get_mime_type(const char *filename) {
    char *ext = strrchr(filename, '.');
    if (!ext) return "application/octet-stream";
    ext++; // 跳过 '.'

    struct mime_map {
        const char *ext;
        const char *type;
    } mime_types[] = {
        {"txt", "text/plain"},
        {"html", "text/html"},
        {"htm", "text/html"},
        {"jpg", "image/jpeg"},
        {"jpeg", "image/jpeg"},
        {"png", "image/png"},
        {"gif", "image/gif"},
        {"pdf", "application/pdf"},
        {"js", "application/javascript"},
        {"css", "text/css"},
        {"json", "application/json"},
        {"mp4", "video/mp4"},
        {NULL, NULL}
    };

    for (int i = 0; mime_types[i].ext; i++) {
        if (strcasecmp(ext, mime_types[i].ext) == 0) {
            return mime_types[i].type;
        }
    }
    return "application/octet-stream";
}

// 处理客户端（优化版）
// URL 解码函数
char *url_decode(const char *src) {
    char *decoded = malloc(strlen(src) + 1);
    char *dst = decoded;
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = {src[1], src[2], 0};
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
    return decoded;
}
//回应
void *handle_client(void *arg) {
    client_data_t *data = (client_data_t *)arg;
    int client_sock = data->client_sock;
    char *root_dir = data->root_dir;
    char buffer[MAX_BUFFER];
    char *file_path = NULL;
    int bytes_read, fd = -1;

    bytes_read = read(client_sock, buffer, MAX_BUFFER - 1);
    if (bytes_read <= 0) goto cleanup;

    buffer[bytes_read] = '\0';

    char method[16], path[256], version[16];
    if (sscanf(buffer, "%s %s %s", method, path, version) != 3) {
        const char *err = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
        write(client_sock, err, strlen(err));
        syslog(LOG_WARNING, "Bad request received");
        goto cleanup;
    }

    if (strcmp(method, "GET") != 0) {
        const char *err = "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n\r\n";
        write(client_sock, err, strlen(err));
        syslog(LOG_WARNING, "Method not allowed: %s", method);
        goto cleanup;
    }

    char *query = strchr(path, '?');
    if (query) *query = '\0';

    char *filename = url_decode(path[0] == '/' ? path + 1 : path);
    if (strlen(filename) == 0) {
        const char *err = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
        write(client_sock, err, strlen(err));
        syslog(LOG_WARNING, "Empty filename requested");
        free(filename);
        goto cleanup;
    }

    // 动态分配 file_path
    size_t path_len = strlen(root_dir) + strlen(filename) + 2;
    file_path = malloc(path_len);
    if (!file_path) {
        const char *err = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";
        write(client_sock, err, strlen(err));
        syslog(LOG_ERR, "Memory allocation failed for file_path");
        free(filename);
        goto cleanup;
    }
    snprintf(file_path, path_len, "%s/%s", root_dir, filename);

    // 使用 realpath 规范化路径
    char *real_path = realpath(file_path, NULL);
    if (!real_path || strncmp(real_path, root_dir, strlen(root_dir)) != 0) {
        const char *err = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n";
        write(client_sock, err, strlen(err));
        syslog(LOG_WARNING, "Path traversal attempt: %s", file_path);
        free(filename);
        free(real_path);
        goto cleanup;
    }

    fd = open(real_path, O_RDONLY);
    if (fd < 0) {
        const char *err = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        write(client_sock, err, strlen(err));
        syslog(LOG_INFO, "File not found: %s", real_path);
        free(filename);
        free(real_path);
        goto cleanup;
    }

    struct stat file_stat;
    if (fstat(fd, &file_stat) < 0 || !S_ISREG(file_stat.st_mode)) {
        const char *err = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n";
        write(client_sock, err, strlen(err));
        syslog(LOG_WARNING, "Invalid file type: %s", real_path);
        free(filename);
        free(real_path);
        close(fd);
        goto cleanup;
    }

    char header[1024]; // 增大缓冲区
    size_t header_len = snprintf(header, sizeof(header),
                            "HTTP/1.1 200 OK\r\n"
                            "Content-Length: %lld\r\n"
                            "Content-Type: %s\r\n"
                            "\r\n",
                            (long long)file_stat.st_size, get_mime_type(filename));
    if (header_len >= sizeof(header)) {
        const char *err = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";
        write(client_sock, err, strlen(err));
        syslog(LOG_ERR, "Header buffer overflow for file %s", filename);
        free(filename);
        free(real_path);
        close(fd);
        goto cleanup;
    }

    if (write(client_sock, header, strlen(header)) < 0) {
        syslog(LOG_ERR, "Failed to send header for %s", real_path);
        free(filename);
        free(real_path);
        close(fd);
        goto cleanup;
    }

#ifdef __linux__
    off_t offset = 0;
    sendfile(client_sock, fd, &offset, file_stat.st_size);
#else
    char file_buffer[4096];
    while ((bytes_read = read(fd, file_buffer, sizeof(file_buffer))) > 0) {
        write(client_sock, file_buffer, bytes_read);
    }
#endif

    free(filename);
    free(real_path);
    close(fd);

cleanup:
    if (file_path) free(file_path);
    close(client_sock);
    free(data);
    return NULL;
}

// 线程池工作者
void *worker_thread(void *arg) {
    config_t *config = (config_t *)arg;
    while (1) {
        pthread_mutex_lock(&queue_mutex);
        while (queue_head == queue_tail) {
            pthread_cond_wait(&queue_cond, &queue_mutex);
        }
        int client_sock = task_queue[queue_head].client_sock;
        queue_head = (queue_head + 1) % QUEUE_SIZE;
        pthread_mutex_unlock(&queue_mutex);

        client_data_t *data = malloc(sizeof(client_data_t));
        data->client_sock = client_sock;
        data->root_dir = config->root_dir;
        handle_client(data);  // 直接调用，无新线程
    }
    return NULL;
}

// 加载config.json
config_t load_config() {
    config_t config;
    config.port = DEFAULT_PORT;
    config.backlog = DEFAULT_BACKLOG;
    config.thread_pool_size = DEFAULT_THREAD_POOL_SIZE;
    config.root_dir = strdup(DEFAULT_ROOT_DIR);

    FILE *fp = fopen("config.json", "r");
    if (!fp) {
        syslog(LOG_WARNING, "Config file not found, using defaults");
        return config;
    }

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *json_str = malloc(len + 1);
    if (!json_str) {
        syslog(LOG_ERR, "Memory allocation failed for config");
        fclose(fp);
        return config;
    }
    fread(json_str, 1, len, fp);
    json_str[len] = '\0';
    fclose(fp);

    cJSON *json = cJSON_Parse(json_str);
    free(json_str);

    if (!json) {
        syslog(LOG_ERR, "Failed to parse config.json: %s", cJSON_GetErrorPtr());
        return config;
    }

    // 验证配置项
    int port = cJSON_GetObjectItemInt(json, "port", DEFAULT_PORT);
    if (port < 1 || port > 65535) {
        syslog(LOG_ERR, "Invalid port %d, using default %d", port, DEFAULT_PORT);
        port = DEFAULT_PORT;
    }

    int backlog = cJSON_GetObjectItemInt(json, "backlog", DEFAULT_BACKLOG);
    if (backlog < 1) {
        syslog(LOG_ERR, "Invalid backlog %d, using default %d", backlog, DEFAULT_BACKLOG);
        backlog = DEFAULT_BACKLOG;
    }

    int thread_pool_size = cJSON_GetObjectItemInt(json, "thread_pool_size", DEFAULT_THREAD_POOL_SIZE);
    if (thread_pool_size < 1) {
        syslog(LOG_ERR, "Invalid thread_pool_size %d, using default %d", thread_pool_size, DEFAULT_THREAD_POOL_SIZE);
        thread_pool_size = DEFAULT_THREAD_POOL_SIZE;
    }

    char *dir = cJSON_GetObjectItemString(json, "root_dir", DEFAULT_ROOT_DIR);
    struct stat dir_stat;
    if (stat(dir, &dir_stat) != 0 || !S_ISDIR(dir_stat.st_mode)) {
        syslog(LOG_ERR, "Invalid root_dir %s, using default %s", dir, DEFAULT_ROOT_DIR);
        free(dir);
        dir = strdup(DEFAULT_ROOT_DIR);
    }

    config.port = port;
    config.backlog = backlog;
    config.thread_pool_size = thread_pool_size;
    free(config.root_dir);
    config.root_dir = dir;
    cJSON_Delete(json);

    return config;
}

// 信号处理（兼容性）
void sig_handler(int sig) {
    syslog(LOG_INFO, "Server shutting down");
    exit(0);
}

int main(int argc, char *argv[]) {
    openlog("file_server", LOG_PID, LOG_DAEMON);  // 日志

    config_t config;
    if (argc == 2) {
        config.root_dir = argv[1];
        config.port = DEFAULT_PORT;
        config.backlog = DEFAULT_BACKLOG;
        config.thread_pool_size = DEFAULT_THREAD_POOL_SIZE;
    } else {
        config = load_config();
    }

    struct stat dir_stat;
    if (stat(config.root_dir, &dir_stat) != 0 || !S_ISDIR(dir_stat.st_mode)) {
        syslog(LOG_ERR, "Invalid directory: %s", config.root_dir);
        exit(1);
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        syslog(LOG_ERR, "Socket creation failed");
        exit(1);
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(config.port);

    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        syslog(LOG_ERR, "Bind failed");
        close(server_sock);
        exit(1);
    }

    if (listen(server_sock, config.backlog) < 0) {
        syslog(LOG_ERR, "Listen failed");
        close(server_sock);
        exit(1);
    }

    syslog(LOG_INFO, "Server running on port %d, serving %s", config.port, config.root_dir);

    // 创建线程池
    pthread_t threads[config.thread_pool_size];
    for (int i = 0; i < config.thread_pool_size; i++) {
        pthread_create(&threads[i], NULL, worker_thread, &config);
    }

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_len);
        if (client_sock < 0) continue;

        pthread_mutex_lock(&queue_mutex);
        if ((queue_tail + 1) % QUEUE_SIZE == queue_head) {
            // 队列满，丢弃（高并发处理）
            close(client_sock);
        } else {
            task_queue[queue_tail].client_sock = client_sock;
            queue_tail = (queue_tail + 1) % QUEUE_SIZE;
            pthread_cond_signal(&queue_cond);
        }
        pthread_mutex_unlock(&queue_mutex);
    }

    close(server_sock);
    free(config.root_dir);
    closelog();
    return 0;
}