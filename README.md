# simple_file_server
c语言实现的并发千级并发，最小实现，通过共享目录实现所有文件HTTP简单服务。
仅支持 HTTP/1.1 的 GET 请求，解析 HTTP 请求头（方法、路径、版本），返回标准 HTTP 响应（200 OK, 400, 404, 405 等）。
代码使用 Content-Type（通过 MIME 类型推断）和 Content-Length 构建响应头，HTTP/1.1 规范。
config.json 提供端口等配置。
