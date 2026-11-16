# code-insight

一个用 C++17 写的高性能代码分析服务，作为 **AI Code Review Agent 的本地工具后端**：Agent 负责编排和自然语言汇报，重计算的部分（静态扫描、重复代码检测）交给它。

不依赖任何第三方库（只用标准库 + 系统 socket API），单文件可执行。

## 这个项目解决什么

代码评审 Agent 的一个现实问题是：把整段 diff 丢给大模型，既贵又慢，而且很多问题（硬编码密钥、空 catch、SQL 拼接）用规则就能查出来，根本不需要模型。

所以拆成两层：

```
Agent（Java/Spring Boot）  --HTTP-->  code-insight（C++）  -->  规则命中 + 统计指标
        |                                    |
    自然语言报告                     正则/结构分析/复杂度
```

Agent 把工具调用当做一个 function，拿到结构化结果后再组织语言。这样既省 token，也让"哪些问题是规则查的、哪些是模型判断的"边界清晰。

## 设计要点

**跨平台事件轮询。** Linux 走 `epoll`，Windows 走 `WSAPoll`，通过 `Poller` 接口抽象，上层 Reactor 与平台无关。

```cpp
class Poller {
    virtual bool add(socket_t, Event) = 0;
    virtual bool modify(socket_t, Event) = 0;
    virtual bool remove(socket_t) = 0;
    virtual int wait(std::vector<Ready>&, int timeout_ms) = 0;
};
```

**单线程 Reactor + 非阻塞 IO。** 所有连接在一个事件循环里处理，读写都是非阻塞的；写缓冲没发完就挂 `Writable` 事件等下次可写，不阻塞循环。

**请求流水线支持。** 一次 `read` 可能收到多个完整请求，处理时循环解析直到数据不足。

**零拷贝解析。** HTTP 解析直接在接收缓冲区上做，只对 body 做一次拷贝，避免多次拼接。

## 接口

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/health` | 健康检查 |
| POST | `/scan?filename=Demo.java` | 扫描一段代码，返回行数、注释比、嵌套深度、圈复杂度、问题列表 |

请求体就是代码原文，`filename` 只用于判断语言和回显。

响应示例：

```json
{
  "file": "Demo.java",
  "language": "java",
  "total_lines": 7,
  "code_lines": 6,
  "max_nesting": 2,
  "cyclomatic": 2,
  "issue_count": 2,
  "issues": [
    {"rule": "SECRET_HARDCODED", "severity": "critical", "line": 3,
     "message": "疑似硬编码凭据，应改为环境变量或配置中心",
     "snippet": "String password = \"hardcoded123\";"},
    {"rule": "EMPTY_CATCH", "severity": "warning", "line": 4,
     "message": "空 catch 块会吞掉异常，至少应记录日志"}
  ]
}
```

## 内置规则

| 规则 | 级别 | 检测内容 |
|---|---|---|
| `SECRET_HARDCODED` | critical | 硬编码的密码 / token / api key |
| `SQL_CONCAT` | critical | SQL 由字符串拼接构造（注入风险） |
| `C_DANGEROUS_FUNC` | warning | `strcpy` / `sprintf` / `gets` 等不安全函数 |
| `EMPTY_CATCH` | warning | 空 catch 块 |
| `DEBUG_RESIDUE` | warning | 调试输出残留（`console.log` / `System.out.println`） |

## 构建

```bash
# Linux
cmake -B build && cmake --build build -j
./build/code-insight --port 18080

# Windows (MinGW)
g++ -std=c++17 -O2 -static -Iinclude src/*.cpp -o build/code-insight.exe -lws2_32
```

## 压测

```bash
./scripts/bench.sh 127.0.0.1 18080 2000
```

脚本用一个长连接连发 2000 个 `/scan` 请求，输出 QPS 与 P50/P99。换成 `wrk -t4 -c200 -d10s --latency` 可以测并发连接下的表现。

## 性能

单连接 keep-alive 连续请求，本机回环（WSL2 Ubuntu 22.04，g++ 11.2 -O2），2000 次 `/scan` 请求：

```
requests : 2000
elapsed  : 0.248s
qps      : 8070
latency  : p50=0.12ms  p99=0.17ms  mean=0.12ms
```

说明几个前提，避免误读这个数字：

- 这是**单连接串行**的成绩，测的是"单个请求处理有多快"，不是并发吞吐
- 请求体是 6 行的短文件，正则逐行匹配的开销小；代码越长、规则越多，耗时会线性增长
- 瓶颈主要在正则匹配，不在网络层——把连接换成并发后 QPS 会明显下降

并发场景用 wrk 另测：

```bash
wrk -t4 -c200 -d10s --latency http://127.0.0.1:18080/health
```

## 已知限制

- 圈复杂度是简化的关键字计数，不是完整的 AST 分析
- 正则逐行匹配，跨行问题（比如多行 SQL 拼接）查不出来
- 没有 TLS，生产环境需要前置反向代理
