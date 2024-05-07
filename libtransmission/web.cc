// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#ifdef _WIN32
#include <array>
#endif
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint> // for uint64_t
#include <ctime>
#include <functional> // for std::less()
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <stack>
#include <string>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h> // setsockopt, SOL_SOCKET, SO_RC...
#endif

#include <curl/curl.h>

#include <event2/buffer.h>

#include <fmt/core.h>

#ifdef _WIN32
#include "libtransmission/crypto-utils.h"
#endif
#include "libtransmission/log.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/utils-ev.h"
#include "libtransmission/utils.h"
#include "libtransmission/web.h"
#include "libtransmission/web-utils.h"

using namespace std::literals;

#if LIBCURL_VERSION_NUM >= 0x070F06 // CURLOPT_SOCKOPT* was added in 7.15.6
#define USE_LIBCURL_SOCKOPT
#endif

// ---

namespace
{
namespace curl_helpers
{

struct ShareDeleter
{
    void operator()(CURLSH* shared)
    {
        if (shared == nullptr)
        {
            return;
        }

        [[maybe_unused]] auto const status = curl_share_cleanup(shared);
        TR_ASSERT(status == CURLSHE_OK);
    }
};

using shared_unique_ptr = std::unique_ptr<CURLSH, ShareDeleter>;

struct MultiDeleter
{
    void operator()(CURLM* multi) const
    {
        if (multi == nullptr)
        {
            return;
        }

        [[maybe_unused]] auto const status = curl_multi_cleanup(multi);
        TR_ASSERT(status == CURLM_OK);
    }
};

using multi_unique_ptr = std::unique_ptr<CURLM, MultiDeleter>;

struct EasyDeleter
{
    void operator()(CURL* val) const noexcept
    {
        if (val != nullptr)
        {
            curl_easy_cleanup(val);
        }
    }
};

using easy_unique_ptr = std::unique_ptr<CURL, EasyDeleter>;

} // namespace curl_helpers

#ifdef _WIN32
CURLcode ssl_context_func(CURL* /*curl*/, void* ssl_ctx, void* /*user_data*/)
{
    auto const cert_store = tr_ssl_get_x509_store(ssl_ctx);
    if (cert_store == nullptr)
    {
        return CURLE_OK;
    }

    curl_version_info_data const* const curl_ver = curl_version_info(CURLVERSION_NOW);
    if (curl_ver->age >= 0 && strncmp(curl_ver->ssl_version, "Schannel", 8) == 0)
    {
        return CURLE_OK;
    }

    static auto constexpr SysStoreNames = std::array<LPCWSTR, 2>{
        L"CA",
        L"ROOT",
    };

    for (auto& sys_store_name : SysStoreNames)
    {
        auto* const sys_cert_store = CertOpenSystemStoreW(0, sys_store_name);
        if (sys_cert_store == nullptr)
        {
            continue;
        }

        PCCERT_CONTEXT sys_cert = nullptr;

        while (true)
        {
            sys_cert = CertFindCertificateInStore(sys_cert_store, X509_ASN_ENCODING, 0, CERT_FIND_ANY, nullptr, sys_cert);
            if (sys_cert == nullptr)
            {
                break;
            }

            auto* const cert = tr_x509_cert_new(sys_cert->pbCertEncoded, sys_cert->cbCertEncoded);
            if (cert == nullptr)
            {
                continue;
            }

            tr_x509_store_add(cert_store, cert);
            tr_x509_cert_free(cert);
        }

        CertCloseStore(sys_cert_store, 0);
    }

    return CURLE_OK;
}
#endif
} // namespace

// ---

class tr_web::Impl
{
public:
    explicit Impl(Mediator& mediator_in)
        : mediator{ mediator_in }
    {
        if (auto bundle = tr_env_get_string("CURL_CA_BUNDLE"); !std::empty(bundle))
        {
            curl_ca_bundle = std::move(bundle);
        }

        share_setopt();

        if (curl_ssl_verify)
        {
            auto const* bundle = std::empty(curl_ca_bundle) ? "none" : curl_ca_bundle.c_str();
            tr_logAddInfo(fmt::format(
                fmt::runtime(_("Will verify tracker certs using envvar CURL_CA_BUNDLE: {bundle}")),
                fmt::arg("bundle", bundle)));
            tr_logAddInfo(_("NB: this only works if you built against libcurl with openssl or gnutls, NOT nss"));
            tr_logAddInfo(_("NB: Invalid certs will appear as 'Could not connect to tracker' like many other errors"));
        }

        if (auto const& file = mediator.cookieFile(); file)
        {
            this->cookie_file = *file;
        }

        if (auto const& ua = mediator.userAgent(); ua)
        {
            this->user_agent = *ua;
        }

        auto const lock = std::unique_lock{ tasks_mutex_ };
        curl_thread = std::make_unique<std::thread>(&Impl::curlThreadFunc, this);
    }

    Impl(Impl&&) = delete;
    Impl(Impl const&) = delete;
    Impl& operator=(Impl&&) = delete;
    Impl& operator=(Impl const&) = delete;

    ~Impl()
    {
        deadline_ = mediator.now();
        queued_tasks_cv_.notify_one();
        curl_thread->join();
    }

    void startShutdown(std::chrono::milliseconds deadline)
    {
        deadline_ = mediator.now() + std::chrono::duration_cast<std::chrono::seconds>(deadline).count();
        queued_tasks_cv_.notify_one();
    }

    void fetch(FetchOptions&& options)
    {
        if (deadline_exists())
        {
            return;
        }

        auto const lock = std::unique_lock{ tasks_mutex_ };
        queued_tasks_.emplace_back(*this, std::move(options));
        queued_tasks_cv_.notify_one();
    }

    [[nodiscard]] bool is_idle() const noexcept
    {
        return std::empty(queued_tasks_) && std::empty(running_tasks_);
    }

    class Task
    {
    public:
        Task(tr_web::Impl& impl_in, tr_web::FetchOptions&& options_in)
            : impl{ impl_in }
            , options{ std::move(options_in) }
        {
            auto const parsed = tr_urlParse(options.url);
            easy_ = parsed ? impl.get_easy(parsed->host) : nullptr;

            response.user_data = options.done_func_user_data;
        }

        // Some of the curl_easy_setopt() args took a pointer to this task.
        // Disable moving so that we don't accidentally invalidate those pointers.
        Task(Task&&) = delete;
        Task(Task const&) = delete;
        Task& operator=(Task&&) = delete;
        Task& operator=(Task const&) = delete;

        ~Task()
        {
            easy_dispose(easy_);
        }

        [[nodiscard]] constexpr auto* easy() const
        {
            return easy_;
        }

        [[nodiscard]] auto* body() const
        {
            return options.buffer != nullptr ? options.buffer : privbuf.get();
        }

        [[nodiscard]] constexpr auto const& speedLimitTag() const
        {
            return options.speed_limit_tag;
        }

        [[nodiscard]] constexpr auto const& url() const
        {
            return options.url;
        }

        [[nodiscard]] constexpr auto const& range() const
        {
            return options.range;
        }

        [[nodiscard]] constexpr auto const& cookies() const
        {
            return options.cookies;
        }

        [[nodiscard]] constexpr auto const& sndbuf() const
        {
            return options.sndbuf;
        }

        [[nodiscard]] constexpr auto const& rcvbuf() const
        {
            return options.rcvbuf;
        }

        [[nodiscard]] constexpr auto const& timeoutSecs() const
        {
            return options.timeout_secs;
        }

        [[nodiscard]] constexpr auto ipProtocol() const
        {
            switch (options.ip_proto)
            {
            case FetchOptions::IPProtocol::V4:
                return CURL_IPRESOLVE_V4;
            case FetchOptions::IPProtocol::V6:
                return CURL_IPRESOLVE_V6;
            default:
                return CURL_IPRESOLVE_WHATEVER;
            }
        }

        [[nodiscard]] auto bind_address() const
        {
            switch (options.ip_proto)
            {
            case FetchOptions::IPProtocol::V4:
                return impl.mediator.bind_address_V4();
            case FetchOptions::IPProtocol::V6:
                return impl.mediator.bind_address_V6();
            default:
                auto ip = impl.mediator.bind_address_V4();
                if (ip == std::nullopt)
                {
                    ip = impl.mediator.bind_address_V6();
                }

                return ip;
            }
        }

        void done()
        {
            if (!options.done_func)
            {
                return;
            }

            response.body.assign(reinterpret_cast<char const*>(evbuffer_pullup(body(), -1)), evbuffer_get_length(body()));
            impl.mediator.run(std::move(options.done_func), std::move(this->response));
            options.done_func = {};
        }

        [[nodiscard]] bool operator==(Task const& that) const noexcept
        {
            return easy() == that.easy();
        }

        tr_web::Impl& impl;
        tr_web::FetchResponse response;

    private:
        void easy_dispose(CURL* easy)
        {
            if (easy == nullptr)
            {
                return;
            }

            impl.paused_easy_handles.erase(easy_);

            if (auto const url = tr_urlParse(options.url); url)
            {
                curl_easy_reset(easy);
                impl.easy_pool_[std::string{ url->host }].emplace(easy);
            }
            else
            {
                curl_easy_cleanup(easy);
            }
        }

        libtransmission::evhelpers::evbuffer_unique_ptr privbuf{ evbuffer_new() };

        tr_web::FetchOptions options;

        CURL* easy_;
    };

    static auto constexpr BandwidthPauseMsec = long{ 500 };
    static auto constexpr DnsCacheTimeoutSecs = long{ 60 * 60 };
    static auto constexpr MaxRedirects = long{ 10 };

    bool const curl_verbose = tr_env_key_exists("TR_CURL_VERBOSE");
    bool const curl_ssl_verify = !tr_env_key_exists("TR_CURL_SSL_NO_VERIFY");
    bool const curl_proxy_ssl_verify = !tr_env_key_exists("TR_CURL_PROXY_SSL_NO_VERIFY");

    Mediator& mediator;

    std::string curl_ca_bundle;

    std::string cookie_file;
    std::string user_agent;

    std::unique_ptr<std::thread> curl_thread;

    // if unset: steady-state, all is good
    // if set: do not accept new tasks
    // if set and deadline reached: kill all remaining tasks
    std::atomic<time_t> deadline_ = {}; // NOLINT(readability-redundant-member-init)

    [[nodiscard]] auto deadline() const
    {
        return deadline_.load();
    }

    [[nodiscard]] bool deadline_exists() const
    {
        return deadline() != time_t{};
    }

    [[nodiscard]] bool deadline_reached() const
    {
        return deadline_exists() && deadline() <= mediator.now();
    }

    [[nodiscard]] CURL* get_easy(std::string_view host)
    {
        CURL* easy = nullptr;

        if (auto iter = easy_pool_.find(host); iter != std::end(easy_pool_) && !std::empty(iter->second))
        {
            easy = iter->second.top().release();
            iter->second.pop();
        }

        if (easy == nullptr)
        {
            easy = curl_easy_init();
        }

        return easy;
    }

    static size_t onDataReceived(void* data, size_t size, size_t nmemb, void* vtask)
    {
        size_t const bytes_used = size * nmemb;
        auto* task = static_cast<Task*>(vtask);
        TR_ASSERT(std::this_thread::get_id() == task->impl.curl_thread->get_id());

        if (auto const range = task->range(); range)
        {
            // https://curl.se/libcurl/c/CURLINFO_RESPONSE_CODE.html
            // "The stored value will be zero if no server response code has been received"
            static auto constexpr NoResponseCode = 0L;
            static auto constexpr PartialContentResponseCode = 206L;

            // Test for webservers that don't support partial-content, see GH #4595
            auto code = long{};
            (void)curl_easy_getinfo(task->easy(), CURLINFO_RESPONSE_CODE, &code);
            if (code != NoResponseCode && code != PartialContentResponseCode)
            {
                tr_logAddWarn(fmt::format(
                    fmt::runtime(_("Couldn't fetch '{url}': expected HTTP response code {expected_code}, got {actual_code}")),
                    fmt::arg("url", task->url()),
                    fmt::arg("expected_code", PartialContentResponseCode),
                    fmt::arg("actual_code", code)));

                // Tell curl to error out. Returning anything that's not
                // `bytes_used` signals an error and causes the transfer
                // to be aborted w/CURLE_WRITE_ERROR.
                return bytes_used + 1;
            }
        }

        if (auto const& tag = task->speedLimitTag(); tag)
        {
            // If this is more bandwidth than is allocated for this tag,
            // then pause the torrent for a tick. curl will deliver `data`
            // again when the transfer is unpaused.
            if (task->impl.mediator.clamp(*tag, bytes_used) < bytes_used)
            {
                task->impl.paused_easy_handles.emplace(task->easy(), tr_time_msec());
                return CURL_WRITEFUNC_PAUSE;
            }
        }

        evbuffer_add(task->body(), data, bytes_used);
        tr_logAddTrace(fmt::format("wrote {} bytes to task {}'s buffer", bytes_used, fmt::ptr(task)));
        return bytes_used;
    }

#ifdef USE_LIBCURL_SOCKOPT
    static int onSocketCreated(void* vtask, curl_socket_t fd, curlsocktype /*purpose*/)
    {
        auto const* const task = static_cast<Task const*>(vtask);
        TR_ASSERT(std::this_thread::get_id() == task->impl.curl_thread->get_id());

        // Ignore the sockopt() return values -- these are suggestions
        // rather than hard requirements & it's OK for them to fail

        if (auto const& buf = task->sndbuf(); buf)
        {
            (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char const*>(&*buf), sizeof(*buf));
        }
        if (auto const& buf = task->rcvbuf(); buf)
        {
            (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char const*>(&*buf), sizeof(*buf));
        }

        // return nonzero if this function encountered an error
        return 0;
    }
#endif

    void initEasy(Task& task)
    {
        TR_ASSERT(std::this_thread::get_id() == curl_thread->get_id());
        auto* const e = task.easy();

        (void)curl_easy_setopt(e, CURLOPT_SHARE, shared());
        (void)curl_easy_setopt(e, CURLOPT_DNS_CACHE_TIMEOUT, DnsCacheTimeoutSecs);
        (void)curl_easy_setopt(e, CURLOPT_AUTOREFERER, 1L);
        (void)curl_easy_setopt(e, CURLOPT_ACCEPT_ENCODING, "");
        (void)curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 1L);
        (void)curl_easy_setopt(e, CURLOPT_MAXREDIRS, MaxRedirects);
        (void)curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
        (void)curl_easy_setopt(e, CURLOPT_PRIVATE, &task);
        (void)curl_easy_setopt(e, CURLOPT_IPRESOLVE, task.ipProtocol());

#ifdef USE_LIBCURL_SOCKOPT
        (void)curl_easy_setopt(e, CURLOPT_SOCKOPTFUNCTION, onSocketCreated);
        (void)curl_easy_setopt(e, CURLOPT_SOCKOPTDATA, &task);
#endif

        if (!curl_ssl_verify)
        {
#if LIBCURL_VERSION_NUM >= 0x073400 /* 7.52.0 */
            (void)curl_easy_setopt(e, CURLOPT_SSL_VERIFYHOST, 0L);
            (void)curl_easy_setopt(e, CURLOPT_SSL_VERIFYPEER, 0L);
#endif
        }
        else if (!std::empty(curl_ca_bundle))
        {
            (void)curl_easy_setopt(e, CURLOPT_CAINFO, curl_ca_bundle.c_str());
        }
        else
        {
#ifdef _WIN32
            (void)curl_easy_setopt(e, CURLOPT_SSL_CTX_FUNCTION, ssl_context_func);
#endif
        }

        if (!curl_proxy_ssl_verify)
        {
            (void)curl_easy_setopt(e, CURLOPT_CAINFO, NULL);
            (void)curl_easy_setopt(e, CURLOPT_CAPATH, NULL);
#if LIBCURL_VERSION_NUM >= 0x073400 /* 7.52.0 */
            (void)curl_easy_setopt(e, CURLOPT_PROXY_SSL_VERIFYHOST, 0L);
            (void)curl_easy_setopt(e, CURLOPT_PROXY_SSL_VERIFYPEER, 0L);
#endif
        }
        else if (!std::empty(curl_ca_bundle))
        {
#if LIBCURL_VERSION_NUM >= 0x073400 /* 7.52.0 */
            (void)curl_easy_setopt(e, CURLOPT_PROXY_CAINFO, curl_ca_bundle.c_str());
#endif
        }

        if (auto const& ua = user_agent; !std::empty(ua))
        {
            (void)curl_easy_setopt(e, CURLOPT_USERAGENT, ua.c_str());
        }

        (void)curl_easy_setopt(e, CURLOPT_TIMEOUT, static_cast<long>(task.timeoutSecs().count()));
        (void)curl_easy_setopt(e, CURLOPT_URL, task.url().c_str());
        (void)curl_easy_setopt(e, CURLOPT_VERBOSE, curl_verbose ? 1L : 0L);
        (void)curl_easy_setopt(e, CURLOPT_WRITEDATA, &task);
        (void)curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, &tr_web::Impl::onDataReceived);

        if (auto const addrstr = task.bind_address(); addrstr)
        {
            (void)curl_easy_setopt(e, CURLOPT_INTERFACE, addrstr->c_str());
        }

        if (auto const& cookies = task.cookies(); cookies)
        {
            (void)curl_easy_setopt(e, CURLOPT_COOKIE, cookies->c_str());
        }

        if (auto const& file = cookie_file; !std::empty(file))
        {
            (void)curl_easy_setopt(e, CURLOPT_COOKIEFILE, file.c_str());
        }

        if (auto const& range = task.range(); range)
        {
            /* don't bother asking the server to compress webseed fragments */
            (void)curl_easy_setopt(e, CURLOPT_ACCEPT_ENCODING, "identity");
            (void)curl_easy_setopt(e, CURLOPT_HTTP_CONTENT_DECODING, 0L);
            (void)curl_easy_setopt(e, CURLOPT_RANGE, range->c_str());
        }
    }

    void resumePausedTasks()
    {
        TR_ASSERT(std::this_thread::get_id() == curl_thread->get_id());

        auto& paused = paused_easy_handles;
        if (std::empty(paused))
        {
            return;
        }

        auto const now = tr_time_msec();

        for (auto it = std::begin(paused); it != std::end(paused);)
        {
            if (it->second + BandwidthPauseMsec < now)
            {
                curl_easy_pause(it->first, CURLPAUSE_CONT);
                it = paused.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void remove_task(Task const& task)
    {
        auto const lock = std::unique_lock{ tasks_mutex_ };

        auto const iter = std::find(std::begin(running_tasks_), std::end(running_tasks_), task);
        TR_ASSERT(iter != std::end(running_tasks_));
        if (iter == std::end(running_tasks_))
        {
            return;
        }

        iter->done();
        running_tasks_.erase(iter);
    }

    void timeout_task(Task& task)
    {
        task.response.status = 408; // request timed out
        task.response.did_timeout = true;
        remove_task(task);
    }

    // the thread started by Impl.curl_thread runs this function
    void curlThreadFunc()
    {
        auto const multi = curl_helpers::multi_unique_ptr{ curl_multi_init() };

        auto repeats = unsigned{};
        for (;;)
        {
            if (deadline_reached())
            {
                while (!std::empty(running_tasks_))
                {
                    auto& task = running_tasks_.front();
                    curl_multi_remove_handle(multi.get(), task.easy());
                    timeout_task(task);
                }
            }

            if (deadline_exists() && is_idle())
            {
                break;
            }

            if (auto lock = std::unique_lock{ tasks_mutex_ }; lock.owns_lock())
            {
                // sleep until there's something to do
                auto const stop_waiting = [this]()
                {
                    return !is_idle() || !deadline_exists();
                };
                if (!stop_waiting())
                {
                    queued_tasks_cv_.wait(lock, stop_waiting);
                }

                // add queued tasks
                if (!std::empty(queued_tasks_))
                {
                    for (auto& task : queued_tasks_)
                    {
                        initEasy(task);
                        curl_multi_add_handle(multi.get(), task.easy());
                    }

                    running_tasks_.splice(std::end(running_tasks_), queued_tasks_);
                }
            }

            resumePausedTasks();

            // Adapted from https://curl.se/libcurl/c/curl_multi_wait.html docs.
            // 'numfds' being zero means either a timeout or no file descriptors to
            // wait for. Try timeout on first occurrence, then assume no file
            // descriptors and no file descriptors to wait for means wait for 100
            // milliseconds.
            auto numfds = int{};
            curl_multi_wait(multi.get(), nullptr, 0, 1000, &numfds);
            if (numfds == 0)
            {
                ++repeats;
                if (repeats > 1U)
                {
                    std::this_thread::sleep_for(100ms);
                }
            }
            else
            {
                repeats = 0;
            }

            // nonblocking update of the tasks
            auto n_running = int{};
            curl_multi_perform(multi.get(), &n_running);

            // process any tasks that just finished
            CURLMsg* msg = nullptr;
            auto unused = int{};
            while ((msg = curl_multi_info_read(multi.get(), &unused)) != nullptr)
            {
                if (msg->msg == CURLMSG_DONE && msg->easy_handle != nullptr)
                {
                    auto* const e = msg->easy_handle;

                    Task* task = nullptr;
                    curl_easy_getinfo(e, CURLINFO_PRIVATE, &task);

                    auto req_bytes_sent = long{};
                    auto total_time = double{};
                    char* primary_ip = nullptr;
                    curl_easy_getinfo(e, CURLINFO_REQUEST_SIZE, &req_bytes_sent);
                    curl_easy_getinfo(e, CURLINFO_TOTAL_TIME, &total_time);
                    curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &task->response.status);
                    curl_easy_getinfo(e, CURLINFO_PRIMARY_IP, &primary_ip);
                    task->response.did_connect = task->response.status > 0 || req_bytes_sent > 0;
                    task->response.did_timeout = task->response.status == 0 &&
                        std::chrono::duration<double>(total_time) >= task->timeoutSecs();
                    task->response.primary_ip = primary_ip;
                    curl_multi_remove_handle(multi.get(), e);
                    remove_task(*task);
                }
            }
        }
    }

    curl_helpers::shared_unique_ptr const curlsh_{ curl_share_init() };

    std::map<std::string /*host*/, std::stack<curl_helpers::easy_unique_ptr>, std::less<>> easy_pool_;

    std::mutex tasks_mutex_;
    std::condition_variable queued_tasks_cv_;
    std::list<Task> queued_tasks_;
    std::list<Task> running_tasks_;

    CURLSH* shared()
    {
        return curlsh_.get();
    }

    void share_setopt()
    {
        // Do *not* share HSTS cache
        // https://github.com/transmission/transmission/issues/5199

        auto* const sh = shared();
        curl_share_setopt(sh, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
        curl_share_setopt(sh, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
#if LIBCURL_VERSION_NUM >= 0x071700 /* 7.23.0 */
        curl_share_setopt(sh, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
#endif
#if LIBCURL_VERSION_NUM >= 0x073900 /* 7.57.0 */
        curl_share_setopt(sh, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
#endif
#if LIBCURL_VERSION_NUM >= 0x073D00 /* 7.61.0 */
        curl_share_setopt(sh, CURLSHOPT_SHARE, CURL_LOCK_DATA_PSL);
#endif
    }

    std::map<CURL*, uint64_t /*tr_time_msec()*/> paused_easy_handles;
};

tr_web::tr_web(Mediator& mediator)
    : impl_{ std::make_unique<Impl>(mediator) }
{
}

tr_web::~tr_web()
{
    impl_->startShutdown(0ms);
}

std::unique_ptr<tr_web> tr_web::create(Mediator& mediator)
{
    return std::unique_ptr<tr_web>(new tr_web(mediator));
}

void tr_web::fetch(FetchOptions&& options)
{
    impl_->fetch(std::move(options));
}

void tr_web::startShutdown(std::chrono::milliseconds deadline)
{
    impl_->startShutdown(deadline);
}

bool tr_web::is_idle() const noexcept
{
    return impl_->is_idle();
}
