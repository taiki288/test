#pragma once
#include "hexa_udon/protocol/result.hpp"
#include <chrono>
#include <cstddef>
#include <string>
#include <vector>
namespace hexa_udon::protocol {
enum class HttpMethod { Get, Post };
struct HttpRequest { HttpMethod method; std::string url; std::vector<std::string> headers; std::string body; std::chrono::milliseconds connect_timeout{2000}; std::chrono::milliseconds total_timeout{5000}; std::size_t max_response_bytes=1024*1024; std::string user_agent="hexa-udon/0.1"; };
struct HttpResponse { long status; std::string body; std::chrono::milliseconds elapsed; };
class HttpTransport { public: virtual ~HttpTransport()=default; [[nodiscard]] virtual Result<HttpResponse> execute(const HttpRequest& request)=0; };
class CurlHttpTransport final : public HttpTransport {
public:
    CurlHttpTransport();
    ~CurlHttpTransport() override;
    CurlHttpTransport(const CurlHttpTransport&) = delete;
    CurlHttpTransport& operator=(const CurlHttpTransport&) = delete;
    [[nodiscard]] Result<HttpResponse> execute(const HttpRequest& request) override;

private:
    bool initialized_ = false;
};
}
