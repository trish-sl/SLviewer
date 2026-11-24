/**
 * @file fsscriptreposfetch.cpp
 * @brief Script repositories: requests to GitHub, GitLab, Gitea/Forgejo, Bitbucket and zip links, the reading of
 *        local folders, and the tokens.
 */

#include "llviewerprecompiledheaders.h"

#include "fsscriptreposfetch.h"

#include "llbase64.h"
#include "llcorehttputil.h"
#include "lleventcoro.h"
#include "llevents.h"
#include "llhttpconstants.h"
#include "llsdjson.h"
#include "llsecapi.h"
#include "lluri.h"
#include "workqueue.h"

#include <curl/curl.h>

namespace FSScriptRepos
{
namespace
{
    // github.com keeps the place the first version stored its token in
    const std::string GITHUB_TOKEN_TYPE("github_token");
    const std::string GITHUB_TOKEN_ID("default");
    const std::string TOKEN_TYPE("git_token");
    const std::string GITHUB_API("https://api.github.com/");
    constexpr unsigned int TRANSFER_TIMEOUT = 300;
    constexpr F32 FOLDER_TIMEOUT = 600.f;   // a folder read is bounded by the file limits; this only guards a lost reply
    constexpr int MAX_REDIRECTS = 3;
    constexpr size_t MAX_API_BYTES = 2 * 1024 * 1024;

    // Who may see the token, and how it is sent
    struct Auth
    {
        std::vector<std::string> prefixes;  // a URL starting with one of these gets the header
        std::string header;
        std::string value;

        bool allows(const std::string& url) const
        {
            for (const std::string& prefix : prefixes)
            {
                if (url.compare(0, prefix.size(), prefix) == 0)
                {
                    return true;
                }
            }
            return false;
        }
    };

    std::string apiRoot(const RepoRef& ref)
    {
        switch (ref.provider)
        {
        case Provider::GITHUB: return GITHUB_API;
        case Provider::GITLAB: return "https://" + ref.host + "/api/v4/";
        case Provider::GITEA: return "https://" + ref.host + "/api/v1/";
        case Provider::BITBUCKET: return "https://api.bitbucket.org/2.0/";
        default: return std::string();
        }
    }

    // The repository's own API address
    std::string repoBase(const RepoRef& ref)
    {
        switch (ref.provider)
        {
        case Provider::GITHUB:
        case Provider::GITEA:
            return apiRoot(ref) + "repos/" + LLURI::escape(ref.owner) + "/" + LLURI::escape(ref.repo);
        case Provider::GITLAB:
            // The project ID is its whole path, '/' encoded
            return apiRoot(ref) + "projects/" + LLURI::escape(ref.owner + "/" + ref.repo);
        case Provider::BITBUCKET:
            return apiRoot(ref) + "repositories/" + LLURI::escape(ref.owner) + "/" + LLURI::escape(ref.repo);
        default:
            return std::string();
        }
    }

    std::string bitbucketArchiveRoot(const RepoRef& ref)
    {
        return "https://bitbucket.org/" + LLURI::escape(ref.owner) + "/" + LLURI::escape(ref.repo) + "/get/";
    }

    Auth authFor(const RepoRef& ref)
    {
        Auth auth;
        const std::string token = loadToken(tokenHost(ref));
        if (token.empty())
        {
            return auth;
        }
        switch (ref.provider)
        {
        case Provider::GITHUB:
            auth.prefixes = { apiRoot(ref) };
            auth.header = "Authorization";
            auth.value = "Bearer " + token;
            break;
        case Provider::GITLAB:
            auth.prefixes = { apiRoot(ref) };
            auth.header = "PRIVATE-TOKEN";
            auth.value = token;
            break;
        case Provider::GITEA:
            auth.prefixes = { apiRoot(ref) };
            auth.header = "Authorization";
            auth.value = "token " + token;
            break;
        case Provider::BITBUCKET:
            // "username:app password" (or "email:API token"); the archive is on bitbucket.org itself
            auth.prefixes = { apiRoot(ref), bitbucketArchiveRoot(ref) };
            auth.header = "Authorization";
            auth.value = "Basic " + LLBase64::encode(reinterpret_cast<const U8*>(token.data()), token.size());
            break;
        default:
            break;
        }
        return auth;
    }

    std::string header(const LLSD& results, const std::string& name)
    {
        const LLSD& headers = results[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_HEADERS];
        return headers.has(name) ? headers[name].asString() : std::string();
    }

    struct Response
    {
        S32 code{ 0 };          // HTTP status; 0 no answer; -1 bad redirect; -2 over the size limit
        LLSD results;
        std::string body;
    };

    std::string errorFor(const Response& response)
    {
        switch (response.code)
        {
        case -1:
            return "The server sent an unexpected answer.";
        case -2:
            return "The download is over 50 MB.";
        case 0:
            return "Could not reach the server.";
        default:
            break;
        }
        if (response.code == 429 ||
            (response.code == 403 && header(response.results, "x-ratelimit-remaining") == "0"))
        {
            return "Request limit reached. Try again later (a token raises the limit).";
        }
        switch (response.code)
        {
        case 401:
            return "The token was not accepted.";
        case 403:
        case 404:
            return "Not found, or the token cannot read it.";
        case 409:
            return "The repository is empty.";
        case 422:
            return "Branch or tag not found.";
        default:
            return llformat("The request failed (HTTP %d).", response.code);
        }
    }

    // Redirects are never followed automatically, so no header can reach another host behind the caller's back
    Response get(const std::string& url, const std::string& accept, const Auth& auth, size_t max_bytes,
        bool head = false)
    {
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter(new LLCoreHttpUtil::HttpCoroutineAdapter(
            "ScriptReposFetch", LLCore::HttpRequest::DEFAULT_POLICY_ID));
        LLCore::HttpRequest::ptr_t request(new LLCore::HttpRequest);
        LLCore::HttpOptions::ptr_t options(new LLCore::HttpOptions);
        LLCore::HttpHeaders::ptr_t headers(new LLCore::HttpHeaders);
        options->setWantHeaders(true);
        options->setFollowRedirects(false);
        options->setTransferTimeout(TRANSFER_TIMEOUT);
        options->setMaxBodySize(max_bytes);
        options->setHeadersOnly(head);
        headers->append(HTTP_OUT_HEADER_ACCEPT, accept);

        if (url.compare(0, GITHUB_API.size(), GITHUB_API) == 0)
        {
            headers->append("X-GitHub-Api-Version", "2022-11-28");
        }
        if (!auth.value.empty() && auth.allows(url))
        {
            headers->append(auth.header, auth.value);
        }

        LLSD result = adapter->getRawAndSuspend(request, url, options, headers);

        Response response;
        response.results = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
        LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(response.results);
        if (status)
        {
            response.code = HTTP_OK;
        }
        else if (status.isHttpStatus())
        {
            response.code = status.getType();
        }
        else if (status == LLCore::HttpStatus(LLCore::HttpStatus::EXT_CURL_EASY, CURLE_WRITE_ERROR))
        {
            response.code = -2; // setMaxBodySize cut it off
        }
        const LLSD::Binary& raw = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
        response.body.assign(raw.begin(), raw.end());

        // The query of a link may be a credential (signed download links): never logged. Nor is any header.
        LL_INFOS("ScriptRepos") << (head ? "HEAD " : "GET ") << url.substr(0, url.find('?')) << " -> "
                                << response.code << LL_ENDL;
        return response;
    }

    // Follows redirects by hand (a renamed repository, an archive on a download host). Each hop is checked again
    // against auth, so the token never goes to codeload.github.com, a storage bucket or any other host.
    Response getFollowing(const std::string& url, const std::string& accept, const std::string& redirect_accept,
        const Auth& auth, size_t max_bytes, bool head = false)
    {
        Response response = get(url, accept, auth, max_bytes, head);
        std::string current = url;
        for (int hops = 0; hops < MAX_REDIRECTS &&
             (response.code == 301 || response.code == 302 || response.code == 303 || response.code == 307 ||
              response.code == 308); ++hops)
        {
            std::string location = header(response.results, "location");
            if (location.size() > 1 && location[0] == '/' && location[1] != '/')
            {
                // Relative to the same host
                location = current.substr(0, current.find('/', 8)) + location;
            }
            if (location.compare(0, 8, "https://") != 0)
            {
                response.code = -1; // never to http, another scheme or nowhere
                return response;
            }
            current = location;
            response = get(location, auth.allows(location) ? accept : redirect_accept, auth, max_bytes, head);
        }
        return response;
    }

    bool isCommitHash(const std::string& commit)
    {
        return (commit.size() == 40 || commit.size() == 64) &&
            commit.find_first_not_of("0123456789abcdef") == std::string::npos;
    }

    // A JSON document from the repository's API
    bool getJson(const std::string& url, const Auth& auth, LLSD& out, std::string& error)
    {
        Response response = getFollowing(url, "application/json", "application/json", auth, MAX_API_BYTES);
        if (response.code != HTTP_OK)
        {
            error = errorFor(response);
            return false;
        }
        boost::system::error_code ec;
        const boost::json::value json = boost::json::parse(response.body, ec);
        if (ec)
        {
            error = "The server sent an unexpected answer.";
            return false;
        }
        out = LlsdFromJson(json);
        return true;
    }

    // GitLab and Bitbucket have no name for "the default branch's head"
    bool defaultBranch(const RepoRef& ref, const Auth& auth, std::string& branch, std::string& error)
    {
        LLSD repo;
        if (!getJson(repoBase(ref), auth, repo, error))
        {
            return false;
        }
        branch = ref.provider == Provider::BITBUCKET ? repo["mainbranch"]["name"].asString() :
                                                       repo["default_branch"].asString();
        if (branch.empty())
        {
            error = "The repository is empty.";
            return false;
        }
        return true;
    }

    FetchResult zipLinkVersion(const RepoRef& ref)
    {
        FetchResult out;
        Response response = getFollowing(ref.url, "application/zip, */*", "application/zip, */*", Auth(), 0, true);
        if (response.code == 401 || response.code == 403 || response.code == 404 || response.code == 410 ||
            response.code < 0)
        {
            out.error = errorFor(response);
            return out;
        }
        // Anything else (a server without HEAD, no answer) leaves it to the download
        out.ok = true;
        if (response.code == HTTP_OK)
        {
            const std::string etag = header(response.results, "etag");
            const std::string modified = header(response.results, "last-modified");
            out.commit = !etag.empty() ? "etag:" + etag : !modified.empty() ? "modified:" + modified : std::string();
        }
        return out;
    }
}

std::string tokenHost(const RepoRef& ref)
{
    return ref.provider == Provider::ZIP || ref.provider == Provider::LOCAL ? std::string() : ref.host;
}

std::string loadToken(const std::string& host)
{
    if (!gSecAPIHandler || host.empty())
    {
        return std::string();
    }
    LLSD data = host == "github.com" ? gSecAPIHandler->getProtectedData(GITHUB_TOKEN_TYPE, GITHUB_TOKEN_ID) :
                                       gSecAPIHandler->getProtectedData(TOKEN_TYPE, host);
    return data.isString() ? data.asString() : std::string();
}

void saveToken(const std::string& host, const std::string& token)
{
    if (!gSecAPIHandler || host.empty())
    {
        return;
    }
    if (token.empty())
    {
        forgetToken(host);
        return;
    }
    if (host == "github.com")
    {
        gSecAPIHandler->setProtectedData(GITHUB_TOKEN_TYPE, GITHUB_TOKEN_ID, LLSD(token));
    }
    else
    {
        gSecAPIHandler->setProtectedData(TOKEN_TYPE, host, LLSD(token));
    }
    gSecAPIHandler->syncProtectedMap();
}

void forgetToken(const std::string& host)
{
    if (!gSecAPIHandler || host.empty())
    {
        return;
    }
    if (host == "github.com")
    {
        gSecAPIHandler->deleteProtectedData(GITHUB_TOKEN_TYPE, GITHUB_TOKEN_ID);
    }
    else
    {
        gSecAPIHandler->deleteProtectedData(TOKEN_TYPE, host);
    }
    gSecAPIHandler->syncProtectedMap();
}

FetchResult fetchCommit(const RepoRef& ref)
{
    if (ref.provider == Provider::ZIP)
    {
        return zipLinkVersion(ref);
    }
    if (ref.provider == Provider::LOCAL)
    {
        // Nothing cheaper than reading it tells whether a folder changed
        FetchResult out;
        out.ok = true;
        return out;
    }

    FetchResult out;
    const Auth auth = authFor(ref);
    std::string commit;
    if (ref.provider == Provider::GITHUB)
    {
        Response response = getFollowing(repoBase(ref) + "/commits/" +
            LLURI::escapePathAndData(ref.ref.empty() ? std::string("HEAD") : ref.ref),
            "application/vnd.github.sha", "application/vnd.github.sha", auth, MAX_API_BYTES);
        if (response.code != HTTP_OK)
        {
            out.error = errorFor(response);
            return out;
        }
        commit = response.body;
    }
    else
    {
        std::string branch = ref.ref;
        if (branch.empty() && ref.provider != Provider::GITEA && !defaultBranch(ref, auth, branch, out.error))
        {
            return out;
        }
        LLSD answer;
        switch (ref.provider)
        {
        case Provider::GITLAB:
            if (!getJson(repoBase(ref) + "/repository/commits/" + LLURI::escape(branch), auth, answer, out.error))
            {
                return out;
            }
            commit = answer["id"].asString();
            break;
        case Provider::GITEA:
            // Without a ref, the default branch
            if (!getJson(repoBase(ref) + "/commits?limit=1&stat=false&verification=false&files=false" +
                    (branch.empty() ? std::string() : "&sha=" + LLURI::escape(branch)), auth, answer, out.error))
            {
                return out;
            }
            if (!answer.isArray() || answer.size() == 0)
            {
                out.error = "The repository is empty.";
                return out;
            }
            commit = answer[0]["sha"].asString();
            break;
        case Provider::BITBUCKET:
            if (!getJson(repoBase(ref) + "/commit/" + LLURI::escape(branch), auth, answer, out.error))
            {
                return out;
            }
            commit = answer["hash"].asString();
            break;
        default:
            break;
        }
    }

    LLStringUtil::trim(commit);
    LLStringUtil::toLower(commit);
    if (!isCommitHash(commit))
    {
        out.error = "The server sent an unexpected answer.";
        return out;
    }
    out.ok = true;
    out.commit = commit;
    return out;
}

FetchResult fetchArchive(const RepoRef& ref, const std::string& commit)
{
    FetchResult out;
    out.commit = commit;

    std::string url;
    std::string accept = "application/zip";
    switch (ref.provider)
    {
    case Provider::GITHUB:
        url = repoBase(ref) + "/zipball/" + commit;
        accept = "application/vnd.github+json";
        break;
    case Provider::GITLAB:
        url = repoBase(ref) + "/repository/archive.zip?sha=" + commit;
        break;
    case Provider::GITEA:
        url = repoBase(ref) + "/archive/" + commit + ".zip";
        break;
    case Provider::BITBUCKET:
        url = bitbucketArchiveRoot(ref) + commit + ".zip";
        break;
    case Provider::ZIP:
        url = ref.url;
        accept = "application/zip, */*";
        break;
    case Provider::LOCAL:
        out.error = "A folder is read, not downloaded.";
        return out;
    }
    if (ref.provider != Provider::ZIP && !isCommitHash(commit))
    {
        out.error = "The server sent an unexpected answer.";
        return out;
    }

    // One byte over the limit is enough to know it is too big; the transfer stops there
    Response response = getFollowing(url, accept, "application/zip, */*", authFor(ref), MAX_ZIP_BYTES + 1);
    if (response.code != HTTP_OK)
    {
        out.error = errorFor(response);
        return out;
    }
    if (response.body.size() > MAX_ZIP_BYTES)
    {
        out.error = "The download is over 50 MB.";
        return out;
    }
    if (ref.provider == Provider::ZIP && out.commit.empty())
    {
        // No version from the server: the content tells
        out.commit = "md5:" + md5Hex(response.body);
    }
    out.ok = true;
    out.body = std::move(response.body);
    return out;
}

FetchResult fetchFolder(const RepoRef& ref, std::vector<std::pair<std::string, std::string>>& files,
    std::vector<std::string>& unsafe_paths)
{
    struct Read
    {
        bool ok{ false };
        std::string error;
        std::vector<std::pair<std::string, std::string>> files;
        std::vector<std::string> unsafe_paths;
    };
    FetchResult out;
    if (ref.provider != Provider::LOCAL)
    {
        out.error = "Not a folder.";
        return out;
    }

    // The read holds its own result: the coroutine may be gone (window closed, logout) when the worker finishes
    auto read = std::make_shared<Read>();
    const std::string dir = ref.path;
    // An own mail drop: a late reply must not wake a later wait
    LLEventMailDrop pump("ScriptReposFolder-" + LLUUID::generateNewID().asString(), true);
    const std::string pump_name = pump.getName();
    auto work = [read, dir, pump_name]()
    {
        read->ok = readFolder(dir, read->files, read->unsafe_paths, read->error);
        LL::WorkQueue::postToMainLoop([pump_name]()
        {
            LLEventPumps::instance().post(pump_name, LLSDMap("done", LLSD::Boolean(true)));
        });
    };

    LL::WorkQueue::ptr_t queue = LL::WorkQueue::getInstance("General");
    if (queue && queue->post(work))
    {
        LLSD reply = llcoro::suspendUntilEventOnWithTimeout(pump, FOLDER_TIMEOUT, LLSDMap("timeout", LLSD::Boolean(true)));
        if (reply.has("timeout"))
        {
            out.error = "The folder took too long to read.";
            return out;
        }
    }
    else
    {
        // No worker pool (shutting down): read here
        read->ok = readFolder(dir, read->files, read->unsafe_paths, read->error);
    }

    LL_INFOS("ScriptRepos") << "READ " << dir << " -> " << (read->ok ? "ok" : read->error) << LL_ENDL;
    if (!read->ok)
    {
        out.error = read->error;
        return out;
    }
    files = std::move(read->files);
    unsafe_paths = std::move(read->unsafe_paths);
    out.ok = true;
    out.commit = filesVersion(files);
    return out;
}
}
