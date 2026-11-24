/**
 * @file fslinksetinventoryloader.h
 * @brief Loads the contents of every link of a linkset, a few links at a time
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

#ifndef FS_LINKSETINVENTORYLOADER_H
#define FS_LINKSETINVENTORYLOADER_H

#include "lltimer.h"
#include "lluuid.h"

#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

class LLViewerObject;

// Keeps one inventory listener per link and asks the simulator for the
// contents of only a few links at once, so selecting a 255 prim linkset
// doesn't fire 255 inventory requests in the same frame.
//
// Inventory callbacks only record which link changed. The owner collects
// those in update(), from its idle callback, so folder views are never
// rebuilt from inside an inventory callback.
class FSLinksetInventoryLoader
{
public:
    static constexpr S32 MAX_IN_FLIGHT = 4;

    FSLinksetInventoryLoader();
    ~FSLinksetInventoryLoader();

    // Starts listening to a link and queues a request for its contents.
    void addLink(LLViewerObject* object);
    // Stops listening to a link (unlinked or gone) and forgets it.
    void removeLink(const LLUUID& link_id);
    // Asks again for a link's contents, ahead of the links not loaded yet.
    void requestAgain(const LLUUID& link_id);
    void clear();

    // Starts queued requests while fewer than MAX_IN_FLIGHT are running and
    // hands back the links whose contents arrived or changed since last call.
    void update(std::vector<LLUUID>& changed);

    bool hasLink(const LLUUID& link_id) const { return mLinks.count(link_id) != 0; }
    bool isLoaded(const LLUUID& link_id) const;
    // Links queued or waiting for the simulator
    S32 getPendingCount() const { return mPending; }

private:
    class Listener;
    friend class Listener;

    enum EState
    {
        QUEUED,
        IN_FLIGHT,
        DONE
    };

    struct Link
    {
        std::unique_ptr<Listener> mListener;
        EState mState = QUEUED;
        bool mEverLoaded = false;
        bool mChangedQueued = false;
        LLTimer mRequestTimer;
    };

    void onInventoryChanged(const LLUUID& link_id);
    void setState(Link& link, EState state);

    std::unordered_map<LLUUID, Link> mLinks;
    std::deque<LLUUID> mQueue;
    std::vector<LLUUID> mInFlight;
    std::vector<LLUUID> mChanged;
    S32 mPending = 0;
};

#endif // FS_LINKSETINVENTORYLOADER_H
