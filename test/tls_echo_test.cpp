// ============================================================
// tls_echo_test.cpp — TLS echo 集成测试
// ============================================================
// 测试流程：
//   1. 运行时生成自签名 RSA 证书
//   2. 启动 TLS echo 服务器
//   3. TLS 客户端连接并发送数据
//   4. 验证 echo 回环正确
//
// 前置条件：CORONET_WITH_TLS=ON + OpenSSL

#include <coronet/coronet.hpp>
#include <coronet/net/tls.hpp>
#include <coronet/io_context.hpp>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rsa.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/bio.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>

// ---- 测试参数 ----
constexpr uint16_t DefaultPort = 9443;
constexpr const char* TestMsg = "Hello, coronet TLS echo!";
constexpr int BufSize = 4096;

// ============================================================
// 动态选端口（Windows 兼容）
// ============================================================
// WSL2/Hyper-V 会在启动时动态保留若干 TCP 端口段（excluded port range），
// 绑定到保留段内的端口会返回 WSAEACCES（与端口是否被占用无关）。
// 探测候选端口，取第一个可绑定的端口，避免固定端口命中保留段。
// Linux 上第一个候选端口通常直接成功，行为不变。
//
// 注意：必须在 io_context 构造之后调用 —— Windows 上 WSAStartup
// 在 io_context 构造函数中执行。
static uint16_t pick_usable_port(uint16_t start) {
    for (uint16_t p = start; p < start + 64; ++p) {
        try {
            coronet::tcp_socket s = coronet::tcp_socket::create_tcp(AF_INET);
            s.set_reuse_addr(true);
            s.bind(coronet::inet_address{p});  // 与服务器相同的绑定方式
            return p;                          // bind 成功 → 端口可用
        } catch (...) { /* WSAEACCES / EADDRINUSE → 试下一个 */ }
    }
    return start;  // 兜底，保持原行为
}

// ============================================================
// 运行时生成自签名证书（仅用于测试，勿用于生产）
// ============================================================
struct pem_cert_key {
    std::string cert;
    std::string key;
};

static pem_cert_key generate_self_signed_cert() {
    // 1. 生成 RSA 2048 密钥对
    // RSA_new/RSA_generate_key_ex 在 OpenSSL 3.0 中已废弃，
    // 但 EVP_PKEY 高层 API 的行为在旧版本间不兼容，此处保留兼容性。
    EVP_PKEY* pkey = EVP_PKEY_new();
    BIGNUM* bn = BN_new();
    BN_set_word(bn, RSA_F4);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    RSA* rsa = RSA_new();
    RSA_generate_key_ex(rsa, 2048, bn, nullptr);
#pragma GCC diagnostic pop
    EVP_PKEY_assign_RSA(pkey, rsa);
    BN_free(bn);

    // 2. 创建 X509 证书
    X509* x509 = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 31536000L);  // 1 year
    X509_set_pubkey(x509, pkey);

    // 3. 设置 subject name（OpenSSL 4.0 中 X509_get_subject_name 返回 const）
    X509_NAME* name = X509_NAME_new();
    X509_NAME_add_entry_by_txt(name, "C", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>("CN"), -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>("coronet-test"), -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
    X509_set_subject_name(x509, name);
    X509_set_issuer_name(x509, name);
    X509_NAME_free(name);

    // 4. 自签名
    X509_sign(x509, pkey, EVP_sha256());

    // 5. 导出为 PEM 字符串
    pem_cert_key result;

    BIO* cert_bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(cert_bio, x509);
    char* cert_data;
    long cert_len = BIO_get_mem_data(cert_bio, &cert_data);
    result.cert.assign(cert_data, cert_len);
    BIO_free(cert_bio);

    BIO* key_bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(key_bio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    char* key_data;
    long key_len = BIO_get_mem_data(key_bio, &key_data);
    result.key.assign(key_data, key_len);
    BIO_free(key_bio);

    X509_free(x509);
    EVP_PKEY_free(pkey);

    return result;
}

// ============================================================
// TLS Echo 服务器会话
// ============================================================
// 注意：conn 用引用传递而非按值 —— tls_socket 含 16KB raw_buf_，
// MSVC 2022 (14.41) 对协程按值传 ~16KB 大对象时的参数复制代码生成有 bug
// （目标地址被 32 位截断 → 访问冲突）。调用方保证 conn 存活期间会话完成。
coronet::task<> tls_echo_session(coronet::tls_socket& conn) {
    char buf[BufSize];
    while (true) {
        int nr = co_await conn.recv(buf);
        if (nr <= 0) break;
        int ns = co_await conn.send({buf, static_cast<size_t>(nr)});
        if (ns <= 0) break;
    }
}

// ============================================================
// TLS Echo 服务器（接受一个连接后退出）
// ============================================================
coronet::task<> tls_echo_server(
    coronet::tls_context& ctx, uint16_t port, bool& server_ready) {
    coronet::tls_acceptor ac{coronet::inet_address{port}, ctx};
    std::printf("[server] TLS listening on port %d\n", port);
    server_ready = true;

    auto conn = co_await ac.accept_socket();
    std::printf("[server] TLS connection accepted\n");
    co_await tls_echo_session(conn);
    std::printf("[server] TLS session ended\n");
}

// ============================================================
// TLS Echo 客户端
// ============================================================
coronet::task<> tls_echo_client(
    coronet::tls_context& ctx, uint16_t port, bool& success,
    coronet::io_context& client_ctx) {
    coronet::inet_address addr;
    if (!coronet::inet_address::resolve("127.0.0.1", port, addr)) {
        std::fprintf(stderr, "[client] resolve failed\n");
        success = false;
        client_ctx.can_stop();
        co_return;
    }

    std::printf("[client] connecting to 127.0.0.1:%d (TLS)\n", port);
    auto sock = co_await coronet::tls_socket::connect(addr, ctx);
    std::printf("[client] TLS connected, handshake done\n");

    // 验证 ALPN（如果设置了）
    auto alpn = sock.negotiated_alpn();
    if (alpn) {
        std::printf("[client] negotiated ALPN: %s\n", alpn->c_str());
    }

    // 发送测试消息
    int ns = co_await sock.send({TestMsg, std::strlen(TestMsg)});
    std::printf("[client] sent %d bytes\n", ns);
    assert(ns > 0);

    // 接收 echo 回复
    char buf[BufSize] = {};
    int nr = co_await sock.recv(buf);
    std::printf("[client] received %d bytes: '%.*s'\n", nr, nr, buf);
    assert(nr == ns);
    assert(std::strncmp(buf, TestMsg, nr) == 0);

    // 半关闭
    co_await sock.shutdown_write();

    success = true;
    std::printf("[client] TLS echo test passed!\n");
    client_ctx.can_stop();
}

// ============================================================
// main
// ============================================================
int main() {
    setvbuf(stdout, NULL, _IONBF, 0);  // 诊断: 立即刷新 (调试后保留, 与其它测试一致)
    std::printf("=== coronet TLS Echo Integration Test ===\n");

    // 初始化 OpenSSL
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    // 生成自签名证书
    auto [cert_pem, key_pem] = generate_self_signed_cert();
    std::printf("[setup] self-signed certificate generated (%zu bytes cert, %zu bytes key)\n",
                cert_pem.size(), key_pem.size());

    // 创建服务端 TLS 上下文
    coronet::tls_context server_ctx{coronet::tls_context::mode::server};
    server_ctx.load_cert_string(cert_pem, key_pem);
    std::string_view alpn = "http/1.1";
    server_ctx.set_alpn({&alpn, 1});

    // 创建客户端 TLS 上下文（不验证证书，因为是自签名）
    coronet::tls_context client_ctx{coronet::tls_context::mode::client};
    client_ctx.set_verify_peer(false);
    std::string_view client_alpn = "http/1.1";
    client_ctx.set_alpn({&client_alpn, 1});

    bool success = false;
    bool server_ready = false;

    // 启动服务器
    coronet::io_context server_io;
    // 动态选端口：避免 WSL2/Hyper-V 排除端口范围导致 bind 失败 (WSAEACCES)
    const uint16_t test_port = pick_usable_port(DefaultPort);
    std::printf("[setup] using test port %u\n", test_port);
    server_io.co_spawn(tls_echo_server(server_ctx, test_port, server_ready));
    server_io.start();

    // 等待服务器就绪
    for (int i = 0; i < 50 && !server_ready; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!server_ready) {
        std::fprintf(stderr, "[error] server failed to start\n");
        server_io.can_stop();
        server_io.join();
        return 1;
    }

    // 运行客户端
    coronet::io_context client_io;
    client_io.co_spawn(tls_echo_client(client_ctx, test_port, success, client_io));
    client_io.start();
    client_io.join();

    // 停止服务器
    server_io.can_stop();
    server_io.join();

    // 清理 OpenSSL
    EVP_cleanup();
    ERR_free_strings();

    if (success) {
        std::printf("=== TLS Echo Integration Test PASSED ===\n");
        return 0;
    } else {
        std::fprintf(stderr, "=== TLS Echo Integration Test FAILED ===\n");
        return 1;
    }
}
