// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <chrono>
#include <cstddef> // size_t
#include <ctime> // time_t
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

struct evbuffer;

class tr_web
{
public:
    // The response struct passed to the user's FetchDoneFunc callback
    // when a fetch() finishes.
    struct FetchResponse
    {
        long status = 0; // http server response, e.g. 200
        std::string body;
        std::string primary_ip;
        bool did_connect = false;
        bool did_timeout = false;
        void* user_data = nullptr;
    };

    // Callback to invoke when fetch() is done
    using FetchDoneFunc = std::function<void(FetchResponse const&)>;

    class FetchOptions
    {
    public:
        enum class IPProtocol
        {
            ANY,
            V4,
            V6,
        };

        FetchOptions(
            std::string_view url_in,
            FetchDoneFunc&& done_func_in,
            void* done_func_user_data_in,
            std::chrono::seconds timeout_secs_in = DefaultTimeoutSecs)
            : url{ url_in }
            , done_func{ std::move(done_func_in) }
            , done_func_user_data{ done_func_user_data_in }
            , timeout_secs{ timeout_secs_in }
        {
        }

        // the URL to fetch
        std::string url;

        // Callback to invoke with a FetchResponse when done
        FetchDoneFunc done_func = nullptr;
        void* done_func_user_data = nullptr;

        // If you need to set multiple cookies, set them all using a single
        // option concatenated like this: "name1=content1; name2=content2;"
        std::optional<std::string> cookies;

        // https://developer.mozilla.org/en-US/docs/Web/HTTP/Range_requests
        std::optional<std::string> range;

        // Tag used by tr_web::Mediator to limit some transfers' bandwidth
        std::optional<int> speed_limit_tag;

        // Optionally set the underlying sockets' send/receive buffers' size.
        // Can be used to conserve resources for scrapes and announces, where
        // the payload is known to be small.
        std::optional<int> sndbuf;
        std::optional<int> rcvbuf;

        // Maximum time to wait before timeout
        std::chrono::seconds timeout_secs = DefaultTimeoutSecs;

        // If provided, this buffer will be used to hold the response body.
        // Provided for webseeds, which need to set low-level callbacks on
        // the buffer itself.
        evbuffer* buffer = nullptr;

        // IP protocol to use when making the request
        IPProtocol ip_proto = IPProtocol::ANY;

        static auto constexpr DefaultTimeoutSecs = std::chrono::seconds{ 120 };
    };

    void fetch(FetchOptions&& options);

    // Notify tr_web that it's going to be destroyed soon.
    // New fetch() tasks will be rejected, but already-running tasks
    // are left alone so that they can finish.
    void startShutdown(std::chrono::milliseconds /*deadline*/);

    [[nodiscard]] bool is_idle() const noexcept;

    // If you want to give running tasks a chance to finish, call closeSoon()
    // before destroying the tr_web object. Deleting the object will cancel
    // all of its tasks.
    ~tr_web();

    /**
     * Mediates between `tr_web` and its clients.
     *
     * NB: Note that `tr_web` calls all these methods from its own thread.
     * Overridden methods should take care to be threadsafe.
     */
    class Mediator
    {
    public:
        virtual ~Mediator() = default;

        // Return the location of the cookie file, or nullopt to not use one
        [[nodiscard]] virtual std::optional<std::string> cookieFile() const
        {
            return std::nullopt;
        }

        // Return IPv4 user public address string, or nullopt to not use one
        [[nodiscard]] virtual std::optional<std::string> bind_address_V4() const
        {
            return std::nullopt;
        }

        // Return IPv6 user public address string, or nullopt to not use one
        [[nodiscard]] virtual std::optional<std::string> bind_address_V6() const
        {
            return std::nullopt;
        }

        // Return the preferred user aagent, or nullopt to not use one
        [[nodiscard]] virtual std::optional<std::string_view> userAgent() const
        {
            return std::nullopt;
        }

        // Return the number of bytes that should be allowed. See tr_bandwidth::clamp()
        [[nodiscard]] virtual size_t clamp([[maybe_unused]] int bandwidth_tag, size_t byte_count) const
        {
            return byte_count;
        }

        // Return the preferred proxy url
        [[nodiscard]] virtual std::optional<std::string_view> proxyUrl() const
        {
            return std::nullopt;
        }

        // Invoke the user-provided fetch callback
        virtual void run(FetchDoneFunc&& func, FetchResponse&& response) const
        {
            func(response);
        }

        [[nodiscard]] virtual time_t now() const
        {
            return time(nullptr);
        }
    };

    // Note that tr_web does no management of the `mediator` reference.
    // The caller must ensure `mediator` is valid for tr_web's lifespan.
    [[nodiscard]] static std::unique_ptr<tr_web> create(Mediator& mediator);

private:
    class Impl;
    std::unique_ptr<Impl> const impl_;
    explicit tr_web(Mediator& mediator);
};

void tr_sessionFetch(struct tr_session* session, tr_web::FetchOptions&& options);
