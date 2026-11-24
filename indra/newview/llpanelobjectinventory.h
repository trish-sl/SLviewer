/**
 * @file llpanelobjectinventory.h
 * @brief LLPanelObjectInventory class definition
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef LL_LLPANELOBJECTINVENTORY_H
#define LL_LLPANELOBJECTINVENTORY_H

#include "llvoinventorylistener.h"
#include "llpanel.h"
#include "llinventorypanel.h" // for LLFolderViewModelInventory

#include "llinventory.h"

#include <map>
#include <memory> // <FS:Trish/>
#include <set>
#include <unordered_map>

class LLScrollContainer;
class LLFolderView;
class LLFolderViewFolder;
class LLViewerObject;
class FSLinksetInventoryLoader; // <FS:Trish/> whole linkset view

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// Class LLPanelObjectInventory
//
// This class represents the panel used to view and control a
// particular task's inventory.
//
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
class LLPanelObjectInventory : public LLPanel, public LLVOInventoryListener
{
public:
    struct Params : public LLInitParam::Block<Params, LLPanel::Params>
    {
        Optional<bool> show_root_folder;

        Params()
            : show_root_folder("show_root_folder", true)
        {}
    };

    LLPanelObjectInventory(const Params&);
    virtual ~LLPanelObjectInventory();

    virtual bool postBuild();

    LLFolderViewModelInventory& getRootViewModel() { return mInventoryViewModel; }

    void doToSelected(const LLSD& userdata);
    void toggleScript();
    void stopScripts();
    bool canStopScripts() const;
    // <FS:Trish> Whole linkset view: items act on their own link
    bool canStopScripts(const LLUUID& task_id) const;
    // "run", "stop" or "reset" for every selected script, each in its own link
    void doScriptsAction(const LLSD& userdata);
    // Shows one folder per link of the selected linkset instead of one object's contents
    void setLinksetMode(bool enabled);
    bool isLinksetMode() const { return mLinksetActive; }
    // The link every selected item belongs to, or null when they span several links
    const LLUUID& getSelectionTaskID();
    bool isShowingTask(const LLUUID& task_id) const;
    // Key for per-item maps: an item id is only unique within its own link
    static LLUUID makeItemKey(const LLUUID& task_id, const LLUUID& item_id);
    S32 getLinksetPendingCount() const;
    // </FS:Trish>

    void refresh();
    const LLUUID& getTaskUUID() { return mTaskUUID;}
    void clearInventoryTask();
    void removeSelectedItem();
    void startRenamingSelectedItem();

    LLFolderView* getRootFolder() const { return mFolders; }
    LLInventoryFilter& getFilter() { return mInventoryViewModel.getFilter(); }
    const LLInventoryFilter& getFilter() const { return mInventoryViewModel.getFilter(); }

    // <FS:Trish> Script state is per link
    bool getScriptRunningState(const LLUUID& task_id, const LLUUID& item_id, bool& running) const;
    void requestScriptRunningInfo(const LLUUID& task_id, const LLUUID& item_id);
    // </FS:Trish>
    // <FS:Trish> vm: -1 unknown, 0 LSL2, 1 Mono, 2 Lua (from the running state reply)
    static void handleScriptRunningReply(const LLUUID& object_id, const LLUUID& item_id, bool running, S32 vm = -1);
    // Counts for the scripts in this object's contents; state and VM are filled in as replies arrive
    struct ScriptSummary
    {
        S32 mTotal = 0;
        S32 mRunning = 0;
        S32 mStopped = 0;
        S32 mMono = 0;
        S32 mLSL2 = 0;
        S32 mLua = 0;
    };
    ScriptSummary getScriptSummary() const;
    const LLUUID& getTaskUUID() const { return mTaskUUID; }
    // </FS:Trish>

    virtual void draw();
    virtual void deleteAllChildren();
    virtual bool handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop, EDragAndDropType cargo_type, void *cargo_data, EAcceptance *accept, std::string& tooltip_msg);

    // <FS:Ansariel> Fix broken return and delete key in task inventory
    //virtual bool handleKeyHere(KEY key, MASK mask);

    /*virtual*/ void onFocusLost();
    /*virtual*/ void onFocusReceived();

    static void idle(void* user_data);

    bool hasInventory(){ return mHaveInventory; };

protected:
    void reset();
    /*virtual*/ void inventoryChanged(LLViewerObject* object,
                                 LLInventoryObject::object_list_t* inventory,
                                 S32 serial_num,
                                 void* user_data);
    void updateInventory();
    void createFolderViews(LLInventoryObject* inventory_root, LLInventoryObject::object_list_t& contents);
    // <FS:Trish> Views are built for one link at a time
    //void createViewsForCategory(LLInventoryObject::object_list_t* inventory,
    //                            LLInventoryObject* parent,
    //                            LLFolderViewFolder* folder);
    void createViewsForCategory(LLInventoryObject::object_list_t* inventory,
                                LLInventoryObject* parent,
                                LLFolderViewFolder* folder,
                                const LLUUID& task_id,
                                std::vector<LLUUID>* keys = nullptr);
    void refreshLinkset(LLViewerObject* root);
    void syncLinks(const std::vector<LLViewerObject*>& links);
    void updateLinkset();
    void buildLinkFolder(const LLUUID& link_id);
    void removeLinkFolder(const LLUUID& link_id);
    void leaveLinkset();
    std::string getLinkLabel(const LLUUID& link_id) const;
    // </FS:Trish>
    void clearContents();
    LLFolderViewItem* getItemByID(const LLUUID& id);

    void addItemID( const LLUUID& id, LLFolderViewItem*   itemp );
    void removeItemID(const LLUUID& id);
    void clearItemIDs();

    bool            handleKeyHere( KEY key, MASK mask );
    bool            isSelectionRemovable();

private:
    std::unordered_map<LLUUID, LLFolderViewItem*> mItemMap;
    // <FS:Trish> Script state is per link
    void setScriptRunningState(const LLUUID& task_id, const LLUUID& item_id, bool running);
    // </FS:Trish>
    std::map<LLUUID, S32> mScriptVM; // <FS:Trish/> -1 unknown, 0 LSL2, 1 Mono, 2 Lua

    static std::set<LLPanelObjectInventory*> sInstances;

    std::map<LLUUID, bool> mScriptRunningState;
    std::set<LLUUID> mScriptRunningRequested;

    LLScrollContainer* mScroller;
    LLFolderView* mFolders;

    LLUUID mTaskUUID;
    LLUUID mAttachmentUUID;
    bool mHaveInventory; // 'Loading' label and used for initial request
    bool mIsInventoryEmpty; // 'Empty' label
    bool mInventoryNeedsUpdate; // for idle, set on changed callback
    LLFolderViewModelInventory  mInventoryViewModel;
    bool mShowRootFolder;

    // <FS:Trish> Whole linkset view
    struct LinkFolder
    {
        LLFolderViewFolder* mFolder = nullptr;
        LLViewerObject* mObject = nullptr;  // only compared, to notice a link that was killed and recreated
        S32 mLinkNum = 0;
        S32 mCount = 0;
        S16 mRequestedSerial = -1;
        bool mOpen = false;
        std::string mName;
        std::vector<LLUUID> mKeys;          // item keys in mItemMap, for removal without a tree walk
    };
    bool mLinksetWanted = false;
    bool mLinksetActive = false;
    mutable bool mLinksetSummaryDirty = true;
    LLUUID mLinksetRootID;
    std::vector<LLUUID> mLinkOrder;
    std::unordered_map<LLUUID, LinkFolder> mLinkFolders;
    std::unique_ptr<FSLinksetInventoryLoader> mLinksetLoader;
    mutable ScriptSummary mLinksetSummary;
    U32 mSelectionTaskFrame = 0;
    LLUUID mSelectionTaskID;
    // </FS:Trish>
};

#endif // LL_LLPANELOBJECTINVENTORY_H
