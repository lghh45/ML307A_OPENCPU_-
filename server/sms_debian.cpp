#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>

#define SERVER_PORT 5656
#define SERVER_IP "0.0.0.0"
#define EPOLL_SIZE 1024
#define RECV_BUFFER_SIZE 4096
#define app_Token ""
#define wx_UID ""
#define WX_HOST ""
#define WX_PORT "80"
#define WX_PATH "/api/send/message"
#define WX_TIMEOUT_SEC 5

namespace {
void addfd(int epfd, int fd, bool enable_et)
{
    struct epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN;
    if (enable_et) {
        event.events |= EPOLLET;
    }
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &event) < 0) {
        perror("epoll_ctl add");
    }
}

void removefd(int epfd, int fd)
{
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
}

void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        perror("fcntl get");
        return;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl set");
    }
}

void append_indent(std::string *out, int indent, bool *need_indent)
{
    *out += '\n';
    out->append(static_cast<size_t>(indent) * 2, ' ');
    *need_indent = false;
}

bool is_json_scalar(const std::string &token)
{
    if (token == "true" || token == "false" || token == "null") {
        return true;
    }
    size_t i = 0;
    if (i < token.size() && token[i] == '-') {
        ++i;
    }
    size_t digits = 0;
    while (i < token.size() && std::isdigit(static_cast<unsigned char>(token[i])) != 0) {
        ++i;
        ++digits;
    }
    if (digits == 0) {
        return false;
    }
    if (i < token.size() && token[i] == '.') {
        ++i;
        digits = 0;
        while (i < token.size() && std::isdigit(static_cast<unsigned char>(token[i])) != 0) {
            ++i;
            ++digits;
        }
        if (digits == 0) {
            return false;
        }
    }
    if (i < token.size() && (token[i] == 'e' || token[i] == 'E')) {
        ++i;
        if (i < token.size() && (token[i] == '+' || token[i] == '-')) {
            ++i;
        }
        digits = 0;
        while (i < token.size() && std::isdigit(static_cast<unsigned char>(token[i])) != 0) {
            ++i;
            ++digits;
        }
        if (digits == 0) {
            return false;
        }
    }
    return i == token.size();
}

bool pretty_json(const std::string &text, std::string *out)
{
    std::string result;
    std::string open_stack; 
    std::string token;       
    int indent = 0;
    bool in_string = false;
    bool escaped = false;
    bool need_indent = false;  
    bool after_comma = false; 
    bool has_token = false;

    auto flush_token = [&token]() {
        if (!token.empty() && !is_json_scalar(token)) {
            return false;
        }
        token.clear();
        return true;
    };

    for (char ch : text) {
        if (in_string) {
            result += ch;
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
            continue;
        }
        has_token = true;
        const bool prev_was_comma = after_comma;
        after_comma = false;
        if (ch == '"') {
            if (!flush_token()) {
                return false;
            }
            if (need_indent) {
                append_indent(&result, indent, &need_indent);
            }
            in_string = true;
            result += ch;
        } else if (ch == '{' || ch == '[') {
            if (!flush_token()) {
                return false;
            }
            if (need_indent) {
                append_indent(&result, indent, &need_indent);
            }
            open_stack += ch;
            ++indent;
            result += ch;
            need_indent = true;
        } else if (ch == '}' || ch == ']') {
            if (!flush_token()) {
                return false;
            }
            if (prev_was_comma) {
                return false;  // "{"a": 1,}" 这类尾随逗号，Python 也会报错
            }
            if (open_stack.empty() || open_stack.back() != (ch == '}' ? '{' : '[')) {
                return false;
            }
            open_stack.pop_back();
            --indent;
            if (need_indent) {
                result += ch;  // 空对象/空数组，Python 输出 "{}"、"[]"，不换行
            } else {
                append_indent(&result, indent, &need_indent);
                result += ch;
            }
            need_indent = false;
        } else if (ch == ',') {
            if (!flush_token()) {
                return false;
            }
            if (open_stack.empty() || need_indent) {
                return false;
            }
            result += ',';
            need_indent = true;
            after_comma = true;
        } else if (ch == ':') {
            if (!flush_token()) {
                return false;
            }
            if (open_stack.empty()) {
                return false;
            }
            result += ": ";
            need_indent = false;
        } else {
            if (need_indent) {
                append_indent(&result, indent, &need_indent);
            }
            result += ch;
            token += ch;
        }
    }

    if (in_string || !open_stack.empty() || !has_token || !flush_token()) {
        return false;
    }
    *out = result;
    return true;
}

struct HttpRequest {
    std::string path;  
    std::string body;  
};

bool try_parse_request(const std::string &buffer, HttpRequest *req, bool *bad_request)
{
    size_t header_end = buffer.find("\r\n\r\n");
    size_t sep_len = 4;
    if (header_end == std::string::npos) {
        header_end = buffer.find("\n\n");
        sep_len = 2;
    }
    if (header_end == std::string::npos) {
        return false;
    }

    std::string request_line = buffer.substr(0, buffer.find('\n'));
    while (!request_line.empty() && request_line.back() == '\r') {
        request_line.pop_back();
    }
    size_t first_space = request_line.find(' ');
    size_t second_space = first_space == std::string::npos
                              ? std::string::npos
                              : request_line.find(' ', first_space + 1);
    if (first_space == std::string::npos || second_space == std::string::npos) {
        *bad_request = true;
        return true;
    }
    req->path = request_line.substr(first_space + 1, second_space - first_space - 1);

    size_t body_start = header_end + sep_len;
    size_t content_length = 0;
    bool has_content_length = false;
    size_t line_begin = buffer.find('\n') + 1;
    while (line_begin < header_end) {
        size_t line_end = buffer.find('\n', line_begin);
        if (line_end == std::string::npos || line_end > header_end) {
            line_end = header_end;
        }
        std::string line = buffer.substr(line_begin, line_end - line_begin);
        line_begin = line_end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        size_t value_begin = value.find_first_not_of(' ');
        value = value_begin == std::string::npos ? std::string() : value.substr(value_begin);
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (key == "content-length") {
            content_length = static_cast<size_t>(strtoull(value.c_str(), nullptr, 10));
            has_content_length = true;
        }
    }

    if (has_content_length) {
        if (buffer.size() - body_start < content_length) {
            return false;  
        }
        req->body = buffer.substr(body_start, content_length);
    }
    return true;
}

void send_all(int fd, const std::string &data)
{
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t count = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (count > 0) {
            sent += static_cast<size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            break;  // 对端已经断开
        }
    }
}

std::string json_escape(const std::string &text)
{
    std::string out;
    out.reserve(text.size() + text.size() / 8 + 16);
    for (char ch : text) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (c == '"') {
            out += "\\\"";
        } else if (c == '\\') {
            out += "\\\\";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c == '\b') {
            out += "\\b";
        } else if (c == '\f') {
            out += "\\f";
        } else if (c < 0x20 || c == 0x7f) {
            char escape[8];
            snprintf(escape, sizeof(escape), "\\u%04x", c);
            out += escape;
        } else {
            out += ch;  
        }
    }
    return out;
}

// 从响应里取出 wxpusher 的 code 字段，取不到返回 -1。
int parse_wxpusher_code(const std::string &response)
{
    size_t pos = response.find("\"code\"");
    if (pos == std::string::npos) {
        return -1;
    }
    pos += sizeof("\"code\"") - 1;
    while (pos < response.size() &&
           (response[pos] == ':' || response[pos] == ' ' || response[pos] == '\t')) {
        ++pos;
    }
    size_t begin = pos;
    while (pos < response.size() && std::isdigit(static_cast<unsigned char>(response[pos])) != 0) {
        ++pos;
    }
    if (pos == begin) {
        return -1;
    }
    return atoi(response.substr(begin, pos - begin).c_str());
}

// 把 content 作为 JSON 文本 POST 到 wxpusher，返回 wxpusher 是否接收成功。
bool send_wxpusher(const std::string &content)
{
    std::string body;
    body = "{\"appToken\":\"" app_Token "\",\"content\":\"";
    body += json_escape(content);
    body += "\",\"summary\":\"短信转发\",\"contentType\":1,\"uids\":[\"" wx_UID "\"]}";

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *addrs = nullptr;
    int gai = getaddrinfo(WX_HOST, WX_PORT, &hints, &addrs);
    if (gai != 0) {
        printf("wxpusher: resolve %s failed: %s\n", WX_HOST, gai_strerror(gai));
        fflush(stdout);
        return false;
    }

    bool ok = false;
    for (struct addrinfo *ai = addrs; ai != nullptr; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        struct timeval timeout;
        timeout.tv_sec = WX_TIMEOUT_SEC;
        timeout.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) < 0) {
            printf("wxpusher: connect failed: %s\n", strerror(errno));
            fflush(stdout);
            close(fd);
            continue;
        }

        // wxpusher 是纯 HTTP 接口，body 是 UTF-8 JSON，长度按字节数算。
        std::string request;
        request += "POST " WX_PATH " HTTP/1.1\r\n";
        request += "Host: " WX_HOST "\r\n";
        request += "Content-Type: application/json\r\n";
        request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        request += "Connection: close\r\n\r\n";
        request += body;
        send_all(fd, request);

        std::string response;
        char chunk[1024];
        while (true) {
            ssize_t count = recv(fd, chunk, sizeof(chunk), 0);
            if (count > 0) {
                response.append(chunk, static_cast<size_t>(count));
            } else if (count < 0 && errno == EINTR) {
                continue;
            } else {
                break;  // 0 表示对端读完就关；<0 表示出错或超时
            }
        }
        close(fd);

        int code = parse_wxpusher_code(response);
        if (code == 1000) {
            printf("wxpusher: 推送成功\n");
            ok = true;
        } else {
            std::string brief = response.substr(0, 200);
            for (char &ch : brief) {
                if (static_cast<unsigned char>(ch) < 0x20) {
                    ch = ' ';
                }
            }
            printf("wxpusher: 推送失败, code=%d | %s\n", code, brief.c_str());
        }
        fflush(stdout);
        break;  
    }
    freeaddrinfo(addrs);
    return ok;
}

void log_request(const HttpRequest &req, bool bad_request)
{
    if (bad_request) {
        printf("bad request line\n");
        fflush(stdout);
        return;
    }
    printf("--- %s\n", req.path.c_str());
    std::string pretty;
    if (pretty_json(req.body, &pretty)) {
        printf("%s\n", pretty.c_str());
        send_wxpusher(pretty);
    } else {
        printf("not json: parse error | %s\n", req.body.c_str());
    }
    fflush(stdout);
}

void send_response(int fd, const char *status, const std::string &body)
{
    char header[256];
    snprintf(header, sizeof(header),
             "HTTP/1.1 %s\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "\r\n",
             status, body.size());
    send_all(fd, header);
    send_all(fd, body);
}

void close_client(int epfd, int fd, std::unordered_map<int, std::string> *buffers)
{
    removefd(epfd, fd);
    close(fd);
    buffers->erase(fd);
}

void handle_client(int epfd, int fd, std::unordered_map<int, std::string> *buffers)
{
    std::string &buffer = (*buffers)[fd];
    while (true) {
        char chunk[RECV_BUFFER_SIZE];
        ssize_t count = recv(fd, chunk, sizeof(chunk), 0);
        bool peer_closed = false;
        if (count > 0) {
            buffer.append(chunk, static_cast<size_t>(count));
        } else if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            perror("recv error");
            peer_closed = true;
        } else {
            peer_closed = true;
        }

        HttpRequest req;
        bool bad_request = false;
        if (try_parse_request(buffer, &req, &bad_request)) {
            log_request(req, bad_request);
            if (bad_request) {
                send_response(fd, "400 Bad Request", std::string());
            } else {
                send_response(fd, "200 OK", "{\"code\":0}");
            }
            close_client(epfd, fd, buffers);
            return;
        }
        if (peer_closed) {
            break;
        }
    }
    close_client(epfd, fd, buffers);
}

}  // namespace

int main(int argc, char *argv[])
{
    struct sockaddr_in serverAddr;
    serverAddr.sin_family = PF_INET;
    serverAddr.sin_port = htons(SERVER_PORT);
    serverAddr.sin_addr.s_addr = inet_addr(SERVER_IP);

    int listener = socket(PF_INET, SOCK_STREAM, 0);
    if (listener < 0) { perror("listener"); exit(-1); }
    printf("listen socket created \n");

    int reuse_addr = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr));

    if (bind(listener, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0) {
        perror("bind error");
        exit(-1);
    }

    int ret = listen(listener, 5);
    if(ret<0){perror("listen error"); exit(-1);}

    int epfd = epoll_create(EPOLL_SIZE);
    static struct epoll_event events[EPOLL_SIZE];
    std::unordered_map<int, std::string> buffers;
    //往内核事件表里添加事件
    set_nonblocking(listener);
    addfd(epfd, listener, true);

    printf("sms server listen on %s:%d\n", SERVER_IP, SERVER_PORT);
    fflush(stdout);

    while(1)
    {
        //epoll_events_count表示就绪事件的数目
        int epoll_events_count = epoll_wait(epfd, events, EPOLL_SIZE, -1);
        if(epoll_events_count < 0) {
            perror("epoll failure");
            break;
        }
        printf("epoll_events_count = %d\n", epoll_events_count);
        fflush(stdout);

        for(int i = 0; i < epoll_events_count; ++i)
        {
            int fd = events[i].data.fd;
            if (fd == listener) {
                while (true) {
                    struct sockaddr_in clientAddr;
                    socklen_t clientAddrLen = sizeof(clientAddr);
                    int conn = accept(listener, (struct sockaddr *)&clientAddr, &clientAddrLen);
                    if (conn < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        perror("accept error");
                        break;
                    }
                    set_nonblocking(conn);
                    addfd(epfd, conn, true);
                    buffers.emplace(conn, std::string());
                }
            } else {
                handle_client(epfd, fd, &buffers);
            }
        }
    }
    close(listener);
    close(epfd);
    return 0;
}
