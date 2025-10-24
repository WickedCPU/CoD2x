#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include "mongoose/mongoose.h"
#include <functional>
#include <string>
#include <map>
#include <cstring>
#include <cstdint>
#include <cstdio>

#undef poll

/**
 * A simple HTTP client using the Mongoose library.
 * Supports GET and POST requests with custom headers and timeouts.
 * Connection is closed after each request.
 * Call poll() periodically to process events.
 */
class HttpClient {
public:
    // Response object
    struct Response {
        int status = 0;
        std::map<std::string, std::string> headers;
        std::string body;
    };
    using Callback = std::function<void(const Response&)>;
    using ErrorCallback = std::function<void(const std::string& error)>;
    using DownloadCallback = std::function<void(const char* data, size_t length, size_t downloaded, size_t total)>;
    
    // Headers used in every request
    std::vector<std::string> headers = {};


    HttpClient() {
        mg_log_set(MG_LL_NONE);
        mg_mgr_init(&mgr);
    }

    ~HttpClient() {
        mg_mgr_free(&mgr);
    }

    void poll(int wait_time_ms = 0) {
        mg_mgr_poll(&mgr, wait_time_ms);
    }

    // Basic GET
    void get(const char* url,
             Callback onDone,
             ErrorCallback onError = nullptr,
             int timeout_ms = 5000)
    {
        request("GET", url, "", "", onDone, onError, timeout_ms);
    }

    // GET with headers
    void get(const char* url,
             const char* headers,
             Callback onDone,
             ErrorCallback onError = nullptr,
             int timeout_ms = 5000)
    {
        request("GET", url, "", headers, onDone, onError, timeout_ms);
    }

    // Basic POST
    void post(const char* url,
              const char* body,
              Callback onDone,
              ErrorCallback onError = nullptr,
              int timeout_ms = 5000)
    {
        request("POST", url, body, "", onDone, onError, timeout_ms);
    }

    // POST with headers
    void post(const char* url,
              const char* body,
              const char* headers,
              Callback onDone,
              ErrorCallback onError = nullptr,
              int timeout_ms = 5000)
    {
        request("POST", url, body, headers, onDone, onError, timeout_ms);
    }

    // JSON POST convenience
    void postJson(const char* url,
                  const char* json,
                  Callback onDone,
                  ErrorCallback onError = nullptr,
                  int timeout_ms = 5000)
    {
        post(url, json, "Content-Type: application/json", onDone, onError, timeout_ms);
    }

    // Download file with chunked processing and progress
    void downloadFile(const char* url,
                      DownloadCallback onDownload,
                      Callback onDone,
                      ErrorCallback onError = nullptr,
                      int timeout_ms = 60000,
                      int connect_timeout_ms = 10000)
    {
        auto* ctx = new RequestContext{};
        ctx->url = url ? url : "";
        ctx->method = "GET";
        ctx->isDownload = true;
        // Combine global headers
        ctx->headers.clear();
        for (const auto& h : this->headers) {
            ctx->headers += h;
            ctx->headers += "\r\n";
        }
        ctx->onDone = std::move(onDone);
        ctx->onDownload = std::move(onDownload);
        ctx->onError = std::move(onError);

        if (timeout_ms <= 0) connect_timeout_ms = 0;
        ctx->timeout_ms = timeout_ms;
        ctx->timeout_connect_ms = connect_timeout_ms;

        // For streaming downloads, still use HTTP connection but intercept body data
        struct mg_connection* c = mg_http_connect(&mgr, ctx->url.c_str(), ev_handler, ctx);
        if (!c) {
            if (ctx->onError) ctx->onError("Failed to connect");
            delete ctx;
        }
    }

    
    /**
     * Sends an HTTP request with the specified parameters.
     *
     * @param method             The HTTP method to use (e.g., "GET", "POST").
     * @param url                The URL to which the request is sent.
     * @param data               The data to send with the request (for POST/PUT methods), or nullptr if not applicable.
     * @param headers            Additional HTTP headers to include in the request separated by \r\n (e.g., "Content-Type: application/json\r\nAccept: application/json"), or nullptr if none.
     * @param onDone             Callback function to be called upon successful completion of the request.
     * @param onError            Callback function to be called if an error occurs during the request.
     * @param timeout_ms         Timeout for the entire request in milliseconds.
     * @param connect_timeout_ms Timeout for the initial connection in milliseconds.
     *
     * If the connection cannot be established, the onError callback is invoked with an error message.
     */
    void request(const char* method, const char* url, const char* data, const char* headers, Callback onDone, ErrorCallback onError, int timeout_ms = 60000, int connect_timeout_ms = 5000)
    {
        // Own all strings inside the context to avoid dangling pointers
        auto* ctx = new RequestContext{};
        ctx->url     = url     ? url     : "";
        ctx->method  = method  ? method  : "GET";
        // Combine global headers and per-request headers
        ctx->headers.clear();
        for (const auto& h : this->headers) {
            ctx->headers += h;
            ctx->headers += "\r\n";
        }
        if (headers && *headers) {
            ctx->headers += headers;
            ctx->headers += "\r\n";
        }
        ctx->data    = data    ? data    : "";
        ctx->onDone  = std::move(onDone);
        ctx->onError = std::move(onError);

        if (timeout_ms <= 0) connect_timeout_ms = 0;
        ctx->timeout_ms = timeout_ms;
        ctx->timeout_connect_ms = connect_timeout_ms;

        struct mg_connection* c = mg_http_connect(&mgr, ctx->url.c_str(), ev_handler, ctx);
        if (!c) {
            if (ctx->onError) ctx->onError("Failed to connect");
            delete ctx;
        }
    }

private:
    struct RequestContext {
        std::string url;
        std::string method;
        std::string headers;
        std::string data;
        Callback onDone;
        ErrorCallback onError;
        
        // Timeout tracking
        int timeout_ms = 0;                     // Overall timeout
        int timeout_connect_ms = 0;             // Timeout first for connection only
        uint64_t timeout_endtime = 0;           // Deadline for entire request
        uint64_t timeout_connect_endtime = 0;   // Deadline for first connection
        bool connected = false;                 // Track if connection is established
        bool error_occurred = false;            // Track if an error has occurred
        
        // Download-specific fields
        DownloadCallback onDownload;
        bool isDownload = false;
        size_t downloaded = 0;
        size_t total_size = 0;
        bool headers_received = false;
        int http_status = 0;
        size_t header_offset = 0;  // Track where headers end in the first buffer
        size_t last_buffer_size = 0;  // Track last processed buffer size
        

    };

    mg_mgr mgr;

    static void ev_handler(struct mg_connection* c, int ev, void* ev_data) {
        RequestContext* ctx = (RequestContext*)c->fn_data;

        // Connection created
        if (ev == MG_EV_OPEN) {
            uint64_t now = mg_millis();
            ctx->timeout_endtime = now + ctx->timeout_ms;
            ctx->timeout_connect_endtime = now + ctx->timeout_connect_ms;

        // Every frame
        } else if (ev == MG_EV_POLL) {
            // Check for timeout
            uint64_t now = mg_millis();
            if (!c->is_closing && !ctx->error_occurred) {
                if (ctx->timeout_ms > 0 && !ctx->connected && now > ctx->timeout_connect_endtime) {
                    ctx->error_occurred = true;
                    mg_error(c, "Connection timeout");
                } else if (ctx->timeout_connect_ms && now > ctx->timeout_endtime) {
                    ctx->error_occurred = true;
                    mg_error(c, "Timeout");
                }
            }

        // TCP connection established
        } else if (ev == MG_EV_CONNECT) {
            // Mark connection as established
            ctx->connected = true;
            
            // Connected to server. Extract host name from URL
            struct mg_str host = mg_url_host(ctx->url.c_str());
            const char* uri = mg_url_uri(ctx->url.c_str());
            const size_t body_len = ctx->data.size();

            if (c->is_tls) {
                struct mg_tls_opts opts = {};
                opts.name = host;
                mg_tls_init(c, &opts);
            }

            std::string requestStr;
            requestStr.reserve(256 + ctx->headers.size() + body_len);

            requestStr += ctx->method;
            requestStr += " ";
            requestStr += uri;
            requestStr += " HTTP/1.1\r\n";

            requestStr += "Host: ";
            requestStr.append(host.buf, host.len);
            requestStr += "\r\n";

            if (!ctx->headers.empty()) {
                requestStr += ctx->headers;
            }

            if (ctx->isDownload) {
                requestStr += "Connection: close\r\n\r\n";
            } else {
                requestStr += "Content-Length: ";
                requestStr += std::to_string(body_len);
                requestStr += "\r\n\r\n";

                if (body_len > 0) {
                    requestStr.append(ctx->data.data(), body_len);
                }
            }

            mg_send(c, requestStr.data(), requestStr.size());
        }

        // TLS handshake complete – no-op
        else if (ev == MG_EV_TLS_HS) 
        {}
        
        // HTTP headers received
        else if (ev == MG_EV_HTTP_HDRS) {
            if (ctx->isDownload) {
                auto* hm = (struct mg_http_message*)ev_data;
                
                // Extract HTTP status
                ctx->http_status = mg_http_status(hm);
                
                // Check for HTTP errors
                if (ctx->http_status != 200) {
                    if (ctx->onError) {
                        ctx->onError("HTTP error " + std::to_string(ctx->http_status));
                    }
                    c->is_closing = 1;
                    return;
                }
                
                // Get Content-Length header to show progress
                for (int i = 0; i < MG_MAX_HTTP_HEADERS && hm->headers[i].name.len > 0; i++) {
                    std::string header_name(hm->headers[i].name.buf, hm->headers[i].name.len);
                    if (strcasecmp(header_name.c_str(), "Content-Length") == 0) {
                        std::string content_length(hm->headers[i].value.buf, hm->headers[i].value.len);
                        ctx->total_size = (size_t)std::stoull(content_length);
                        break;
                    }
                }
                
                // Calculate header offset: headers + double CRLF
                ctx->header_offset = hm->head.len;
                ctx->headers_received = true;
            }
        }

        // Data received (raw socket level) - for streaming downloads
        else if (ev == MG_EV_READ) {
            if (ctx->isDownload && ctx->headers_received) {
                struct mg_iobuf* io = &c->recv;
                if (io->len > ctx->header_offset) {
                    // Calculate body data (everything after headers)
                    const char* body_start = (const char*)io->buf + ctx->header_offset;
                    size_t body_size = io->len - ctx->header_offset;
                    
                    if (body_size > 0) {
                        // Update total downloaded with this chunk
                        ctx->downloaded += body_size;
                        
                        // Call streaming callback with body data
                        if (ctx->onDownload) {
                            ctx->onDownload(body_start, body_size, ctx->downloaded, ctx->total_size);
                        }
                        
                        // Clear only the body portion, keep headers intact
                        mg_iobuf_del(io, ctx->header_offset, body_size);
                    }
                }
            }
        }
        
        // HTTP response received
        else if (ev == MG_EV_HTTP_MSG) {
            // Don't process HTTP message if an error has already occurred
            if (ctx->error_occurred) {
                return;
            }

            auto* hm = (struct mg_http_message*)ev_data;

            if (ctx->isDownload) {
                Response res;
                res.status = mg_http_status(hm);
                res.body = "";              
                if (ctx->onDone) ctx->onDone(res);

            } else {
                // For regular requests: create response with body and headers
                Response res;
                res.status = mg_http_status(hm);
                res.body.assign(hm->body.buf, hm->body.len);

                // parse headers
                for (int i = 0; i < MG_MAX_HTTP_HEADERS && hm->headers[i].name.len > 0; i++) {
                    res.headers[std::string(hm->headers[i].name.buf, hm->headers[i].name.len)] =
                        std::string(hm->headers[i].value.buf, hm->headers[i].value.len);
                }

                if (ctx->onDone) ctx->onDone(res);
            }
            c->is_closing = 1;
        }

        else if (ev == MG_EV_ERROR) {
            ctx->error_occurred = true;
            if (ctx && ctx->onError) ctx->onError((char*)ev_data);
            c->is_closing = 1;
        }

        else if (ev == MG_EV_CLOSE) {
            delete ctx;
        }
    }
};


#endif