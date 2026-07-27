#pragma once

// ============================================================
// tls_socket.hpp — TLS 加密套接字 (包装 tcp_socket + OpenSSL SSL*)
// ============================================================
// 在 tcp_socket 之上添加 TLS 加密层，实现透明的加密通信。
// 满足 transport concept，可与 tcp_socket 互换使用。
//
// 核心架构 — BIO 桥接模式：
//
//   ┌──────────────────────────────────────────────────┐
//   │                  tls_socket                       │
//   │                                                   │
//   │   用户明文          OpenSSL 加密         网络密文   │
//   │   ┌──────┐    ┌─────────────────┐    ┌────────┐  │
//   │   │ recv │ ←─ │ SSL_read (rbio) │ ←─ │tcp.recv│  │
//   │   │ send │ ─→ │ SSL_write(wbio) │ ─→ │tcp.send│  │
//   │   └──────┘    └─────────────────┘    └────────┘  │
//   │                                                   │
//   │   rbio (read BIO):  网络 → SSL (写入加密数据)      │
//   │   wbio (write BIO): SSL → 网络 (读取加密数据)      │
//   └──────────────────────────────────────────────────┘
//
// 异步 I/O 桥接：
//   SSL_read/SSL_write 是同步调用，但可能返回 WANT_READ/WANT_WRITE。
//   - WANT_READ: SSL 需要更多加密数据 → co_await tcp.recv() → BIO_write(rbio)
//   - WANT_WRITE: SSL 产生了加密数据 → drain wbio → co_await tcp.send()
//
// 使用示例：
//   // 服务端（通过 tls_acceptor）
//   tls_acceptor ac{addr, ctx};
//   auto conn = co_await ac.accept_socket();
//   int n = co_await conn.recv(buf);
//   co_await conn.send(data);
//
//   // 客户端
//   auto sock = co_await tls_socket::connect(addr, ctx);
//   co_await sock.send(data);
//   int n = co_await sock.recv(buf);
//
// 前置条件：CORONET_HAS_TLS 定义时编译，需链接 OpenSSL。

#include "coronet/net/tcp_socket.hpp"
#include "coronet/net/tls/tls_context.hpp"
#include "coronet/task.hpp"

#ifdef CORONET_HAS_TLS

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <span>
#include <cstdint>

namespace coronet {

/// TLS 加密套接字 — 在 TCP 之上提供 TLS 加密。
///
/// 满足 transport concept。
///
/// 生命周期：
///   1. 构造：从已连接的 tcp_socket + tls_context 创建
///   2. 握手：handshake()（服务端）或 connect()（客户端，含 TCP 连接 + 握手）
///   3. 数据传输：recv() / send()
///   4. 关闭：close()（发送 close_notify + 关闭 TCP）
///
/// 内存模型：
///   - SSL 会话和 BIO 由 OpenSSL 管理，SSL_free 时自动释放
///   - tls_socket 可移动，移动后源对象为空壳
///   - 析构时自动释放 SSL 会话和 TCP 套接字
class tls_socket {
public:
    // ---- 构造/析构 ----

    /// 从已连接的 TCP 套接字构造 TLS 套接字。
    /// 调用者负责确保 TCP 已连接。
    /// 构造后需调用 handshake()（服务端）或使用 connect() 工厂（客户端）。
    ///
    /// @param sock 已连接的 TCP 套接字（所有权转移）
    /// @param ctx TLS 上下文（共享，不获取所有权）
    /// @throws std::runtime_error SSL 会话创建失败
    tls_socket(tcp_socket&& sock, const tls_context& ctx)
        : tcp_(std::move(sock)) {
        ssl_ = SSL_new(ctx.native_handle());
        if (!ssl_) {
            unsigned long err = ERR_get_error();
            char buf[256];
            ERR_error_string_n(err, buf, sizeof(buf));
            throw std::runtime_error(
                std::string("tls_socket: SSL_new failed: ") + buf);
        }

        // 创建内存 BIO 对（SSL 接管所有权）
        BIO* rbio = BIO_new(BIO_s_mem());
        BIO* wbio = BIO_new(BIO_s_mem());
        if (!rbio || !wbio) {
            BIO_free(rbio);
            BIO_free(wbio);
            SSL_free(ssl_);
            ssl_ = nullptr;
            throw std::runtime_error("tls_socket: BIO_new failed");
        }

        // 设置非阻塞模式：BIO 不缓冲，SSL 操作立即返回
        BIO_set_mem_eof_return(rbio, -1);
        BIO_set_mem_eof_return(wbio, -1);

        SSL_set_bio(ssl_, rbio, wbio);
        // rbio 和 wbio 的所有权已转移给 ssl_，不要单独释放
    }

    ~tls_socket() {
        if (ssl_) {
            SSL_free(ssl_);
            ssl_ = nullptr;
        }
        // tcp_ 析构器自动关闭套接字
    }

    // 可移动，不可拷贝
    tls_socket(tls_socket&& other) noexcept
        : tcp_(std::move(other.tcp_))
        , ssl_(other.ssl_)
        , handshake_done_(other.handshake_done_)
        , closed_(other.closed_) {
        other.ssl_ = nullptr;
        other.handshake_done_ = false;
        other.closed_ = true;
    }

    tls_socket& operator=(tls_socket&& other) noexcept {
        if (this != &other) {
            if (ssl_) SSL_free(ssl_);
            tcp_ = std::move(other.tcp_);
            ssl_ = other.ssl_;
            handshake_done_ = other.handshake_done_;
            closed_ = other.closed_;
            other.ssl_ = nullptr;
            other.handshake_done_ = false;
            other.closed_ = true;
        }
        return *this;
    }

    tls_socket(const tls_socket&) = delete;
    tls_socket& operator=(const tls_socket&) = delete;

    // ---- 客户端连接 ----

    /// 客户端连接工厂：创建 TCP 套接字 → 连接 → TLS 握手。
    ///
    /// @param addr 目标地址
    /// @param ctx TLS 上下文（客户端模式）
    /// @returns 已完成 TLS 握手的 tls_socket
    /// @throws std::runtime_error TCP 连接或 TLS 握手失败
    [[nodiscard("Did you forget to co_await?")]]
    static task<tls_socket> connect(const inet_address& addr,
                                    const tls_context& ctx) {
        // 1. TCP 连接
        tcp_socket tcp = tcp_socket::create_tcp(addr.family());
        int ret = co_await tcp.connect(addr);
        if (ret < 0) {
            throw std::runtime_error(
                "tls_socket::connect: TCP connect failed (ret=" +
                std::to_string(ret) + ")");
        }

        // 2. 构造 TLS 套接字
        tls_socket sock{std::move(tcp), ctx};

        // 3. 客户端模式 TLS 握手
        SSL_set_connect_state(sock.ssl_);
        co_await sock.do_handshake();

        co_return std::move(sock);
    }

    // ---- 服务端握手 ----

    /// 服务端 TLS 握手（在 TCP accept 之后调用）。
    ///
    /// @throws std::runtime_error TLS 握手失败
    [[nodiscard("Did you forget to co_await?")]]
    task<void> handshake() {
        SSL_set_accept_state(ssl_);
        co_await do_handshake();
    }

    // ---- 数据传输（实现 transport concept）----

    /// 异步接收解密数据。
    ///
    /// @returns >0: 接收字节数; 0: EOF（对端关闭）; <0: 错误
    [[nodiscard("Did you forget to co_await?")]]
    task<int> recv(std::span<char> buf) {
        if (!handshake_done_ || closed_ || !ssl_) {
            co_return -1;
        }

        while (true) {
            int n = SSL_read(ssl_, buf.data(),
                             static_cast<int>(buf.size()));
            if (n > 0) {
                // 成功 — 刷新可能产生的 wbio 数据（alert, key update 等）
                co_await flush_wbio();
                co_return n;
            }

            int err = SSL_get_error(ssl_, n);
            if (err == SSL_ERROR_WANT_READ) {
                // SSL 需要更多加密数据
                co_await flush_wbio();
                int raw_n = co_await tcp_.recv(raw_buf_);
                if (raw_n <= 0) {
                    co_return raw_n;  // 网络错误或 EOF
                }
                BIO_write(SSL_get_rbio(ssl_), raw_buf_, raw_n);
                // 继续循环重试 SSL_read
            } else if (err == SSL_ERROR_WANT_WRITE) {
                // wbio 满，需要刷新
                co_await flush_wbio();
            } else if (err == SSL_ERROR_ZERO_RETURN) {
                // 对端发送了 close_notify
                co_return 0;  // EOF
            } else {
                // SSL_ERROR_SSL 或其他致命错误
                co_return -1;
            }
        }
    }

    /// 异步发送加密数据。
    ///
    /// @returns >0: 发送字节数; <0: 错误
    [[nodiscard("Did you forget to co_await?")]]
    task<int> send(std::span<const char> buf) {
        if (!handshake_done_ || closed_ || !ssl_) {
            co_return -1;
        }

        while (true) {
            int n = SSL_write(ssl_, buf.data(),
                              static_cast<int>(buf.size()));
            if (n > 0) {
                // 成功 — 刷新加密数据到网络
                co_await flush_wbio();
                co_return n;
            }

            int err = SSL_get_error(ssl_, n);
            if (err == SSL_ERROR_WANT_READ) {
                // 罕见：SSL 需要读取（renegotiation / key update）
                co_await flush_wbio();
                int raw_n = co_await tcp_.recv(raw_buf_);
                if (raw_n <= 0) {
                    co_return raw_n;
                }
                BIO_write(SSL_get_rbio(ssl_), raw_buf_, raw_n);
            } else if (err == SSL_ERROR_WANT_WRITE) {
                // wbio 满，需要刷新
                co_await flush_wbio();
            } else if (err == SSL_ERROR_ZERO_RETURN) {
                // 对端关闭了连接
                co_return -1;
            } else {
                co_return -1;
            }
        }
    }

    // ---- 关闭 ----

    /// 异步关闭：发送 TLS close_notify + 关闭 TCP（快速关闭，不等待对端响应）。
    [[nodiscard("Did you forget to co_await?")]]
    task<void> close() {
        if (closed_) {
            co_return;
        }
        closed_ = true;

        if (ssl_ && handshake_done_) {
            // 发送 close_notify（单向关闭，不等待对端响应）
            (void)SSL_shutdown(ssl_);
            co_await flush_wbio();
        }

        if (ssl_) {
            SSL_free(ssl_);
            ssl_ = nullptr;
        }

        // MSVC 对 co_await + [[nodiscard]] 有误报（C4834），抑制此行
#pragma warning(suppress: 4834)
        co_await tcp_.close();
    }

    /// 优雅关闭：发送 close_notify → 等待对端 close_notify → 关闭 TCP。
    /// P2-1 fix: 完整双向 SSL_shutdown，确保对端收到干净关闭而非 RST。
    /// 比 close() 慢（需等待一个 RTT），但避免对端收到 RST 导致数据丢失。
    [[nodiscard("Did you forget to co_await?")]]
    task<void> close_graceful() {
        if (closed_) {
            co_return;
        }
        closed_ = true;

        if (ssl_ && handshake_done_) {
            // 双向 shutdown：第一次发送 close_notify，第二次等待对端 close_notify
            for (int i = 0; i < 2; ++i) {
                while (true) {
                    int ret = SSL_shutdown(ssl_);
                    if (ret >= 0) {
                        co_await flush_wbio();
                        break;  // 0 = 等待对端; 1 = 双向完成
                    }
                    int err = SSL_get_error(ssl_, ret);
                    if (err == SSL_ERROR_WANT_READ) {
                        co_await flush_wbio();
                        int raw_n = co_await tcp_.recv(raw_buf_);
                        if (raw_n <= 0) break;  // 对端已关闭
                        BIO_write(SSL_get_rbio(ssl_), raw_buf_, raw_n);
                    } else if (err == SSL_ERROR_WANT_WRITE) {
                        co_await flush_wbio();
                    } else {
                        co_await flush_wbio();
                        break;  // 致命错误，放弃
                    }
                }
                if (i == 0) {
                    // 第一次 shutdown 后，给对端一个短时间窗口响应
                    co_await flush_wbio();
                }
            }
        }

        if (ssl_) {
            SSL_free(ssl_);
            ssl_ = nullptr;
        }

#pragma warning(suppress: 4834)
        co_await tcp_.close();
    }

    /// 半关闭写端：发送 TLS close_notify，通知对端数据发送完毕。
    /// P1-2 fix: 改为循环重试，处理 WANT_READ/WANT_WRITE（与 do_handshake 一致）。
    [[nodiscard("Did you forget to co_await?")]]
    task<void> shutdown_write() {
        if (!handshake_done_ || closed_ || !ssl_) {
            co_return;
        }

        // 循环重试 SSL_shutdown 直到成功或不可恢复
        while (true) {
            int ret = SSL_shutdown(ssl_);
            if (ret >= 0) {
                // 0 = 已发送 close_notify，等待对端；1 = 双向关闭完成
                co_await flush_wbio();
                co_return;
            }

            int err = SSL_get_error(ssl_, ret);
            if (err == SSL_ERROR_WANT_READ) {
                // SSL 需要读取对端数据（可能是对端的 close_notify）
                co_await flush_wbio();
                int raw_n = co_await tcp_.recv(raw_buf_);
                if (raw_n <= 0) {
                    // 网络错误，放弃
                    co_return;
                }
                BIO_write(SSL_get_rbio(ssl_), raw_buf_, raw_n);
                // 继续循环重试
            } else if (err == SSL_ERROR_WANT_WRITE) {
                // wbio 满，刷新后重试
                co_await flush_wbio();
                // 继续循环重试
            } else {
                // 致命错误，放弃
                co_await flush_wbio();
                co_return;
            }
        }
    }

    // ---- 访问器 ----

    /// 获取底层 SSL 会话句柄（满足 transport concept）
    [[nodiscard]] std::uintptr_t native_handle() const noexcept {
        return reinterpret_cast<std::uintptr_t>(ssl_);
    }

    /// 获取协商的 ALPN 协议（握手后可用）
    [[nodiscard]] std::optional<std::string> negotiated_alpn() const {
        if (!ssl_ || !handshake_done_) {
            return std::nullopt;
        }
        const unsigned char* data = nullptr;
        unsigned int len = 0;
        SSL_get0_alpn_selected(ssl_, &data, &len);
        if (len == 0 || !data) {
            return std::nullopt;
        }
        return std::string(reinterpret_cast<const char*>(data), len);
    }

    /// 是否已完成 TLS 握手
    [[nodiscard]] bool is_handshake_done() const noexcept {
        return handshake_done_;
    }

private:
    // ---- 成员 ----

    tcp_socket tcp_;              ///< 底层 TCP 套接字
    SSL* ssl_{nullptr};           ///< OpenSSL SSL 会话
    bool handshake_done_{false};  ///< TLS 握手是否完成
    bool closed_{false};          ///< 是否已关闭

    /// BIO 桥接用缓冲区（TLS 最大记录大小 = 16KB）
    static constexpr int kRawBufSize = 16384;
    char raw_buf_[kRawBufSize];   ///< 网络 → rbio 的原始数据缓冲区

    // ---- 内部协程 ----

    /// 刷新写 BIO：将 SSL 产生的加密数据发送到网络。
    /// 在每次 SSL 操作后调用，确保加密数据及时发送。
    task<void> flush_wbio() {
        BIO* wbio = SSL_get_wbio(ssl_);
        if (!wbio) co_return;

        while (BIO_pending(wbio) > 0) {
            int n = BIO_read(wbio, raw_buf_, kRawBufSize);
            if (n <= 0) break;

            // 发送所有字节（可能需要多次 send）
            // 注意：raw_buf_ 在此处复用，因为 flush_wbio 调用时
            // 不在 recv/send 的网络读取阶段
            int total = 0;
            while (total < n) {
                int sent = co_await tcp_.send(
                    {raw_buf_ + total, static_cast<size_t>(n - total)});
                if (sent <= 0) {
                    // 网络错误，放弃
                    co_return;
                }
                total += sent;
            }
        }
    }

    /// TLS 握手循环（共享：客户端和服务端都用此方法）。
    ///
    /// SSL_do_handshake 可能需要多次网络 I/O 才能完成。
    /// 每次返回 WANT_READ 时从网络读取数据喂给 rbio，
    /// 每次返回 WANT_WRITE 时刷新 wbio 到网络。
    ///
    /// @throws std::runtime_error 握手失败
    task<void> do_handshake() {
        while (true) {
            int ret = SSL_do_handshake(ssl_);
            if (ret == 1) {
                // 握手成功
                co_await flush_wbio();
                handshake_done_ = true;
                co_return;
            }

            int err = SSL_get_error(ssl_, ret);

            if (err == SSL_ERROR_WANT_READ) {
                // SSL 需要更多数据
                // 先刷新 SSL 已产生的数据（如 ServerHello）
                co_await flush_wbio();
                // 从网络读取
                int raw_n = co_await tcp_.recv(raw_buf_);
                if (raw_n <= 0) {
                    throw std::runtime_error(
                        "tls_socket: handshake failed (network read returned " +
                        std::to_string(raw_n) + ")");
                }
                BIO_write(SSL_get_rbio(ssl_), raw_buf_, raw_n);
                // 继续循环重试握手
            } else if (err == SSL_ERROR_WANT_WRITE) {
                // wbio 满，刷新
                co_await flush_wbio();
                // 继续循环重试握手
            } else {
                // 致命错误
                unsigned long ssl_err = ERR_get_error();
                char buf[256];
                ERR_error_string_n(ssl_err, buf, sizeof(buf));
                throw std::runtime_error(
                    std::string("tls_socket: handshake failed: ") + buf);
            }
        }
    }
};

} // namespace coronet

#endif // CORONET_HAS_TLS
