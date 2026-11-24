/**
 * @file fsscriptreposfetch.h
 * @brief Script repositories: requests to GitHub, GitLab, Gitea/Forgejo, Bitbucket and zip links, the reading of
 *        local folders, and the tokens.
 */

#ifndef FS_SCRIPTREPOSFETCH_H
#define FS_SCRIPTREPOSFETCH_H

#include "fsscriptrepos.h"

#include <string>

namespace FSScriptRepos
{
    // Tokens live in the viewer's protected data store, one per host, never in settings or logs. A token is only
    // sent to its own host's API (and Bitbucket's own archive address), never to a host a redirect points to.
    // The host a repository's token belongs to; empty for zip links and local folders (they never get one).
    std::string tokenHost(const RepoRef& ref);
    std::string loadToken(const std::string& host);
    void saveToken(const std::string& host, const std::string& token);
    void forgetToken(const std::string& host);

    struct FetchResult
    {
        bool ok{ false };
        std::string commit;
        std::string body;
        std::string error;
    };

    // All three must be called from inside a coroutine.
    // The commit the ref (or the default branch) points to. For a zip link: its ETag or Last-Modified if the server
    // gives one, else empty (only the download tells whether it changed). For a local folder: empty (only the read
    // tells).
    FetchResult fetchCommit(const RepoRef& ref);
    // The archive of that commit; body holds the zip bytes. For a zip link, commit is the one fetchCommit gave, or
    // a hash of the download when that was empty. Not for a local folder.
    FetchResult fetchArchive(const RepoRef& ref, const std::string& commit);
    // A local folder's files, read on a worker thread (the coroutine suspends meanwhile); the same limits as an
    // archive. commit is a hash of the files read ("md5:...", as a zip link without a server version).
    FetchResult fetchFolder(const RepoRef& ref, std::vector<std::pair<std::string, std::string>>& files,
        std::vector<std::string>& unsafe_paths);
}

#endif // FS_SCRIPTREPOSFETCH_H
