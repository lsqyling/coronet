#pragma once

// ============================================================
// tls_context.hpp — TLS 上下文 (SSL_CTX RAII 包装)
// ============================================================
// 管理 OpenSSL SSL_CTX 对象的生命周期，配置证书、密钥、ALPN、
// TLS 版本等。一个 tls_context 可被多个 tls_socket 共享。
//
// 设计要点：
//   - RAII：构造时创建 SSL_CTX，析构时自动释放
//   - 不可拷贝（SSL_CTX 是共享资源），可移动
//   - 线程安全：SSL_CTX 在握手前配置，握手后只读共享
//   - 证书/密钥支持文件和内存字符串两种加载方式
//   - ALPN 协商：配置应用层协议列表（如 h2, http/1.1）
//
// 使用示例：
//   // 服务端
//   tls_context ctx{tls_context::mode::server};
//   ctx.load_cert_file("server.crt", "server.key");
//   ctx.set_alpn({"h2", "http/1.1"});
//
//   // 客户端
//   tls_context ctx{tls_context::mode::client};
//   ctx.set_verify_peer(true);
//   ctx.set_ca_file("ca.pem");
//   ctx.set_alpn({"h2", "http/1.1"});
//
// 前置条件：CORONET_HAS_TLS 定义时编译，需链接 OpenSSL。

#include "coronet/platform/platform.hpp"

#ifdef CORONET_HAS_TLS

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace coronet {

/// TLS 上下文 — 管理 SSL_CTX 生命周期和配置。
///
/// 一个 tls_context 对象代表一组 TLS 配置（证书、密钥、ALPN、验证策略），
/// 可被多个 tls_socket 共享。在服务端，通常创建一个 tls_context 用于
/// 所有入站连接；在客户端，创建一个用于所有出站连接。
///
/// 线程安全保证：
///   - SSL_CTX 配置（load_cert, set_alpn 等）应在创建 tls_socket 之前完成
///   - 配置完成后，SSL_CTX 在内部被多个 SSL 会话只读共享，OpenSSL 保证安全
///   - tls_context 本身不是线程安全的（移动/析构需外部同步）
class tls_context {
public:
    /// 工作模式 — 客户端或服务端
    enum class mode : uint8_t {
        client,  ///< 客户端模式（验证服务器证书）
        server   ///< 服务端模式（出示服务器证书，可选验证客户端证书）
    };

    /// TLS 版本
    enum class version : uint8_t {
        tls12,          ///< 仅 TLS 1.2
        tls13,          ///< 仅 TLS 1.3
        tls12_and_13    ///< TLS 1.2 + 1.3（推荐）
    };

    // ---- 构造/析构 ----

    /// 创建 TLS 上下文。
    /// @param m 客户端或服务端模式
    /// @param v 允许的 TLS 版本（默认 1.2 + 1.3）
    explicit tls_context(mode m, version v = version::tls12_and_13)
        : mode_(m) {
        init_ctx(v);
    }

    ~tls_context() {
        if (ctx_) {
            SSL_CTX_free(ctx_);
            ctx_ = nullptr;
        }
    }

    // 可移动，不可拷贝
    // P1-1 fix: 转移 alpn_protocols_ — 旧代码遗漏导致 use-after-free
    // （SSL_CTX app_data 指向已释放的 alpn_protocols_）
    tls_context(tls_context&& other) noexcept
        : ctx_(other.ctx_)
        , mode_(other.mode_)
        , alpn_protocols_(std::move(other.alpn_protocols_)) {
        other.ctx_ = nullptr;
    }

    tls_context& operator=(tls_context&& other) noexcept {
        if (this != &other) {
            if (ctx_) SSL_CTX_free(ctx_);
            ctx_ = other.ctx_;
            mode_ = other.mode_;
            alpn_protocols_ = std::move(other.alpn_protocols_);
            other.ctx_ = nullptr;
        }
        return *this;
    }

    tls_context(const tls_context&) = delete;
    tls_context& operator=(const tls_context&) = delete;

    // ---- 证书/密钥 ----

    /// 从文件加载证书和私钥。
    /// @throws std::runtime_error 加载失败
    void load_cert_file(const std::string& cert_path,
                        const std::string& key_path) {
        if (SSL_CTX_use_certificate_chain_file(ctx_, cert_path.c_str()) <= 0) {
            throw_ssl_error("tls_context::load_cert_file (certificate)");
        }
        if (SSL_CTX_use_PrivateKey_file(ctx_, key_path.c_str(),
                                        SSL_FILETYPE_PEM) <= 0) {
            throw_ssl_error("tls_context::load_cert_file (private key)");
        }
        if (SSL_CTX_check_private_key(ctx_) <= 0) {
            throw_ssl_error("tls_context::load_cert_file (key mismatch)");
        }
    }

    /// 从内存字符串加载证书和私钥（PEM 格式）。
    /// @throws std::runtime_error 加载失败
    void load_cert_string(std::string_view cert, std::string_view key) {
        BIO* cert_bio = BIO_new_mem_buf(cert.data(), static_cast<int>(cert.size()));
        if (!cert_bio) throw_ssl_error("tls_context::load_cert_string (cert BIO)");
        X509* x509 = PEM_read_bio_X509(cert_bio, nullptr, nullptr, nullptr);
        BIO_free(cert_bio);
        if (!x509) throw_ssl_error("tls_context::load_cert_string (parse cert)");
        if (SSL_CTX_use_certificate(ctx_, x509) <= 0) {
            X509_free(x509);
            throw_ssl_error("tls_context::load_cert_string (use cert)");
        }
        X509_free(x509);  // SSL_CTX_use_certificate increments refcount

        BIO* key_bio = BIO_new_mem_buf(key.data(), static_cast<int>(key.size()));
        if (!key_bio) throw_ssl_error("tls_context::load_cert_string (key BIO)");
        EVP_PKEY* pkey = PEM_read_bio_PrivateKey(key_bio, nullptr, nullptr, nullptr);
        BIO_free(key_bio);
        if (!pkey) throw_ssl_error("tls_context::load_cert_string (parse key)");
        if (SSL_CTX_use_PrivateKey(ctx_, pkey) <= 0) {
            EVP_PKEY_free(pkey);
            throw_ssl_error("tls_context::load_cert_string (use key)");
        }
        EVP_PKEY_free(pkey);  // SSL_CTX_use_PrivateKey increments refcount

        if (SSL_CTX_check_private_key(ctx_) <= 0) {
            throw_ssl_error("tls_context::load_cert_string (key mismatch)");
        }
    }

    // ---- ALPN (Application-Layer Protocol Negotiation) ----

    /// 设置 ALPN 协议列表（如 "h2", "http/1.1"）。
    /// 客户端在握手时发送偏好列表，服务端从中选择一个。
    /// @throws std::runtime_error 设置失败
    void set_alpn(std::span<const std::string_view> protocols) {
        // ALPN 协议列表格式：长度前缀 + 协议字符串，连续拼接
        auto buf = std::make_unique<std::vector<unsigned char>>();
        buf->reserve(protocols.size() * 8);
        for (auto& proto : protocols) {
            if (proto.size() > 255) {
                throw std::runtime_error("tls_context::set_alpn: protocol name too long");
            }
            buf->push_back(static_cast<unsigned char>(proto.size()));
            buf->insert(buf->end(), proto.begin(), proto.end());
        }

        if (mode_ == mode::server) {
            // 使用 app_data 存储协议列表指针（堆分配，移动安全）
            SSL_CTX_set_app_data(ctx_, buf.get());
            SSL_CTX_set_alpn_select_cb(ctx_,
                [](SSL* ssl, const unsigned char** out, unsigned char* outlen,
                   const unsigned char* in, unsigned int inlen,
                   void* /*arg*/) -> int {
                    // 从 SSL_CTX 的 app_data 获取协议列表
                    // 这样 tls_context 移动后指针仍有效（堆地址不变）
                    auto* protos = static_cast<std::vector<unsigned char>*>(
                        SSL_CTX_get_app_data(SSL_get_SSL_CTX(ssl)));
                    if (!protos || protos->empty()) {
                        return SSL_TLSEXT_ERR_NOACK;
                    }
                    // 服务端选择客户端列表中第一个自己支持的协议
                    if (SSL_select_next_proto(
                            const_cast<unsigned char**>(out), outlen,
                            protos->data(), protos->size(), in, inlen)
                        != OPENSSL_NPN_NEGOTIATED) {
                        return SSL_TLSEXT_ERR_NOACK;
                    }
                    return SSL_TLSEXT_ERR_OK;
                }, nullptr);
        } else {
            // 客户端：SSL_CTX 内部拷贝协议列表，无需保留 buf
            SSL_CTX_set_alpn_protos(ctx_, buf->data(), buf->size());
        }

        alpn_protocols_ = std::move(buf);
    }

    // ---- 证书验证 ----

    /// 启用/禁用对端证书验证。
    /// 客户端默认应启用，服务端默认禁用（除非需要双向 TLS）。
    void set_verify_peer(bool on) {
        int mode = on ? SSL_VERIFY_PEER : SSL_VERIFY_NONE;
        if (mode_ == mode::server && on) {
            mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
        }
        SSL_CTX_set_verify(ctx_, mode, nullptr);
    }

    /// 加载 CA 证书文件用于验证对端证书。
    /// @throws std::runtime_error 加载失败
    void set_ca_file(const std::string& path) {
        if (SSL_CTX_load_verify_locations(ctx_, path.c_str(), nullptr) <= 0) {
            throw_ssl_error("tls_context::set_ca_file");
        }
    }

    /// 使用系统默认 CA 路径。
    /// @throws std::runtime_error 加载失败
    void set_default_verify_paths() {
        if (SSL_CTX_set_default_verify_paths(ctx_) <= 0) {
            throw_ssl_error("tls_context::set_default_verify_paths");
        }
    }

    // ---- 访问器 ----

    /// 获取底层 SSL_CTX 指针（供 tls_socket 内部使用）
    [[nodiscard]] SSL_CTX* native_handle() const noexcept { return ctx_; }

    /// 是否为服务端模式
    [[nodiscard]] bool is_server() const noexcept { return mode_ == mode::server; }

    /// 是否为客户端模式
    [[nodiscard]] bool is_client() const noexcept { return mode_ == mode::client; }

private:
    SSL_CTX* ctx_{nullptr};
    mode mode_;
    ///< ALPN 协议列表（堆分配，移动后地址不变，SSL_CTX app_data 安全）
    std::unique_ptr<std::vector<unsigned char>> alpn_protocols_;

    /// 初始化 SSL_CTX
    void init_ctx(version v) {
        // TLS 方法（支持客户端和服务端，由 mode_ 决定行为）
        ctx_ = SSL_CTX_new(TLS_method());
        if (!ctx_) {
            throw_ssl_error("tls_context::init_ctx (SSL_CTX_new)");
        }

        // 设置允许的 TLS 版本
        switch (v) {
            case version::tls12:
                SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
                SSL_CTX_set_max_proto_version(ctx_, TLS1_2_VERSION);
                break;
            case version::tls13:
                SSL_CTX_set_min_proto_version(ctx_, TLS1_3_VERSION);
                SSL_CTX_set_max_proto_version(ctx_, TLS1_3_VERSION);
                break;
            case version::tls12_and_13:
                SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
                SSL_CTX_set_max_proto_version(ctx_, TLS1_3_VERSION);
                break;
        }

        // 通用选项（客户端和服务端共用）
        // 注意：不使用 SSL_MODE_AUTO_RETRY — 它与内存 BIO 不兼容，
        //       会导致 SSL_read 在 BIO 空时无限循环。
        //       SSL_MODE_RELEASE_BUFFERS 在空闲时释放内部缓冲区，节省内存。
        SSL_CTX_set_mode(ctx_, SSL_MODE_RELEASE_BUFFERS);

        // P2-2 fix: 安全加固选项
        // SSL_OP_NO_COMPRESSION — 防止 CRIME/BREACH 攻击
        // SSL_OP_NO_RENEGOTIATION — 禁止 TLS 重协商（防止 DoS）
        SSL_CTX_set_options(ctx_, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
    }

    /// 将 OpenSSL 错误转换为异常
    [[noreturn]] static void throw_ssl_error(const char* context) {
        unsigned long err = ERR_get_error();
        char buf[256];
        ERR_error_string_n(err, buf, sizeof(buf));
        throw std::runtime_error(
            std::string(context) + ": " + buf);
    }
};

} // namespace coronet

#endif // CORONET_HAS_TLS
