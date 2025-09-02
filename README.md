# 简单文件服务器

一个用 C 语言实现的轻量级、高性能 HTTP/1.1 文件服务器，设计用于从共享目录提供文件，支持千级并发。支持 GET 请求、标准 HTTP 响应，并通过 `config.json` 进行配置。

## 功能特性

- **HTTP/1.1 支持**：处理 GET 请求，返回标准 HTTP 响应（200 OK、400 Bad Request、404 Not Found、405 Method Not Allowed）。
- **高并发**：使用线程池（默认 64 个线程）高效处理千级并发连接。
- **零拷贝传输**：在 Linux 上使用 `sendfile` 优化文件传输，非 Linux 系统提供回退机制。
- **MIME 类型推断**：根据文件扩展名自动检测文件类型（如 `text/plain`、`text/html`、`image/jpeg`）。
- **配置支持**：从 `config.json` 加载配置（端口、连接队列、线程池大小、根目录），或通过命令行参数指定。
- **健壮的错误处理**：包含路径遍历防护、syslog 日志记录和信号处理以实现优雅关闭。
- **跨平台兼容性**：针对 Linux 优化，包含非 Linux 系统的回退支持。

## 安装

### 前置条件

- **操作系统**：Debian（或其他 POSIX 兼容系统）。
- **编译器**：GCC。
- **库**：`pthread`、`rt`（Debian 默认包含）。
- **cJSON**：嵌入简化版 cJSON；需完整功能可从 cJSON GitHub 下载。

### 编译

1. 将代码保存为 `simple_file_server.c`。

2. 使用以下命令编译：

   ```bash
   gcc -o simple_file_server simple_file_server.c -lpthread -lrt
   ```

   **注意**：若使用完整 cJSON 库，需先安装（Debian 上运行 `sudo apt install libcjson-dev`），并链接 `-lcjson`。

## 使用方法

通过指定目录运行服务器以提供文件：

```bash
./simple_file_server /path/to/directory
```

或使用当前目录下的 `config.json` 进行配置：

```bash
./simple_file_server
```

通过 HTTP 访问文件，例如：`http://localhost:18945/file.txt`。

### 配置（config.json）

在工作目录中放置 `config.json` 以覆盖默认设置。示例：

```json
{
    "port": 18945,
    "backlog": 100,
    "thread_pool_size": 64,
    "root_dir": "."
}
```

- **port**：服务器监听的 TCP 端口（默认：18945）。范围：1-65535，低端口（&lt;1024）需 root 权限。
- **backlog**：最大待处理连接数（默认：100）。受系统参数 `/proc/sys/net/core/somaxconn` 限制。
- **thread_pool_size**：工作线程数（默认：64）。根据 CPU 和内存调整。
- **root_dir**：提供文件的目录（默认：`.`）。生产环境建议使用绝对路径。

## 局限性

- 仅支持 HTTP/1.1 GET 请求，不支持 POST、PUT 等方法。
- 无 HTTPS 支持，建议使用反向代理（如 Nginx）实现安全连接。
- 提供基本路径遍历防护，生产环境需额外安全加固。
- `cJSON` 解析假设简单 JSON 结构，复杂结构可能需更新库。

## 性能

- **并发能力**：通过线程池支持千级连接（使用 `ab` 或 `wrk` 测试验证）。
- **优化**：`sendfile` 零拷贝降低大文件传输的 CPU 开销。
- **调优建议**：高负载下可增加 `thread_pool_size` 或 `backlog`，但需监控内存和 CPU 使用。

## 贡献

通过 GitHub 提交问题或拉取请求（若项目托管）。若需自定义功能（如 POST 支持、HTTPS），请提供详细需求。
