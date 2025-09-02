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
#include <ctype.h>         // MIME辅助

#define DEFAULT_PORT 18945
#define DEFAULT_BACKLOG 100
#define DEFAULT_THREAD_POOL_SIZE 64
#define DEFAULT_ROOT_DIR "."
#define MAX_BUFFER 1024
#define QUEUE_SIZE 1024

// 嵌入cJSON库（简化版，完整版可从github下载，此为最小实现）
typedef struct cJSON {
    struct cJSON *next, *prev, *child;
    int type;
    char *valuestring;
    int valueint;
    double valuedouble;
    char *string;
} cJSON;

enum { cJSON_Invalid = (0), cJSON_False = (1 << 0), cJSON_True = (1 << 1), cJSON_NULL = (1 << 2),
       cJSON_Number = (1 << 3), cJSON_String = (1 << 4), cJSON_Array = (1 << 5), cJSON_Object = (1 << 6),
       cJSON_Raw = (1 << 7) };

cJSON *cJSON_Parse(const char *value);
void cJSON_Delete(cJSON *c);
char *cJSON_Print(const cJSON *item);
cJSON *cJSON_GetObjectItem(const cJSON *object, const char *string);
int cJSON_GetObjectItemInt(const cJSON *object, const char *string, int default_val);
char *cJSON_GetObjectItemString(const cJSON *object, const char *string, char *default_val);

// cJSON最小实现（实际使用时替换为完整库，此为占位）
cJSON *cJSON_Parse(const char *value) { /* 实现解析 */ return NULL; }
void cJSON_Delete(cJSON *c) { /* 释放 */ }
cJSON *cJSON_GetObjectItem(const cJSON *object, const char *string) { return NULL; }
int cJSON_GetObjectItemInt(const cJSON *object, const char *string, int default_val) { return default_val; }
char *cJSON_GetObjectItemString(const cJSON *object, const char *string, char *default_val) { return strdup(default_val); }

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
    if (strcasecmp(ext, ".txt") == 0) return "text/plain";
    if (strcasecmp(ext, ".html") == 0) return "text/html";
    if (strcasecmp(ext, ".jpg") == 0) return "image/jpeg";
    // 添加更多...
    return "application/octet-stream";
}

// 处理客户端（优化版）
void *handle_client(void *arg) {
    client_data_t *data = (client_data_t *)arg;
    int client_sock = data->client_sock;
    char *root_dir = data->root_dir;
    char buffer[MAX_BUFFER];
    char file_path[256];
    int bytes_read;

    bytes_read = read(client_sock, buffer, MAX_BUFFER - 1);
    if (bytes_read <= 0) goto cleanup;

    buffer[bytes_read] = '\0';

    char method[16], path[256], version[16];
    if (sscanf(buffer, "%s %s %s", method, path, version) != 3) {
        const char *err = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
        write(client_sock, err, strlen(err));
        goto cleanup;
    }

    if (strcmp(method, "GET") != 0) {
        const char *err = "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n\r\n";
        write(client_sock, err, strlen(err));
        goto cleanup;
    }

    // 忽略query string
    char *query = strchr(path, '?');
    if (query) *query = '\0';

    char *filename = path[0] == '/' ? path + 1 : path;
    if (strlen(filename) == 0) {
        const char *err = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
        write(client_sock, err, strlen(err));
        goto cleanup;
    }

    // 防止路径遍历（虽模式无视安全，但添加以兼容）
    if (strstr(filename, "..")) {
        const char *err = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n";
        write(client_sock, err, strlen(err));
        goto cleanup;
    }

    snprintf(file_path, sizeof(file_path), "%s/%s", root_dir, filename);

    int fd = open(file_path, O_RDONLY);
    if (fd < 0) {
        const char *err = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        write(client_sock, err, strlen(err));
        goto cleanup;
    }

    struct stat file_stat;
    if (fstat(fd, &file_stat) < 0) {
        const char *err = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";
        write(client_sock, err, strlen(err));
        close(fd);
        goto cleanup;
    }

    char header[512];
    snprintf(header, sizeof(header),
             "HTTP/1.1 200 OK\r\n"
             "Content-Length: %lld\r\n"
             "Content-Type: %s\r\n"
             "\r\n",
             (long long)file_stat.st_size, get_mime_type(filename));

    if (write(client_sock, header, strlen(header)) < 0) {
        close(fd);
        goto cleanup;
    }

    // 零拷贝发送
#ifdef __linux__
    off_t offset = 0;
    sendfile(client_sock, fd, &offset, file_stat.st_size);
#else
    char file_buffer[4096];
    while ((bytes_read = read(fd, file_buffer, sizeof(file_buffer))) > 0) {
        write(client_sock, file_buffer, bytes_read);
    }
#endif

    close(fd);

cleanup:
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
    if (!fp) return config;

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *json_str = malloc(len + 1);
    fread(json_str, 1, len, fp);
    json_str[len] = '\0';
    fclose(fp);

    cJSON *json = cJSON_Parse(json_str);
    free(json_str);

    if (json) {
        config.port = cJSON_GetObjectItemInt(json, "port", DEFAULT_PORT);
        config.backlog = cJSON_GetObjectItemInt(json, "backlog", DEFAULT_BACKLOG);
        config.thread_pool_size = cJSON_GetObjectItemInt(json, "thread_pool_size", DEFAULT_THREAD_POOL_SIZE);
        char *dir = cJSON_GetObjectItemString(json, "root_dir", DEFAULT_ROOT_DIR);
        free(config.root_dir);
        config.root_dir = dir;
        cJSON_Delete(json);
    }

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