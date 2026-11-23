/**
 * @file llsidepanelinventory.cpp
 * @brief LLPanelObjectInventory class implementation
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Linden Research, Inc.
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

//*****************************************************************************
//
// Implementation of the panel inventory - used to view and control a
// task's inventory.
//
//*****************************************************************************

#include "llviewerprecompiledheaders.h"

#include "llpanelobjectinventory.h"

#include "llmenugl.h"
#include "llnotificationsutil.h"
#include "roles_constants.h"

#include "llagent.h"
#include "llavataractions.h"
#include "llcallbacklist.h"
#include "llbuycurrencyhtml.h"
#include "llfloaterreg.h"
#include "llfolderview.h"
#include "llinventorybridge.h"
#include "llinventorydefines.h"
#include "llinventoryicon.h"
#include "llinventoryfilter.h"
#include "llinventoryfunctions.h"
#include "llmaterialeditor.h"
#include "llpreviewanim.h"
#include "llpreviewgesture.h"
#include "llpreviewnotecard.h"
#include "llpreviewscript.h"
#include "llpreviewsound.h"
#include "llpreviewtexture.h"
#include "llscrollcontainer.h"
#include "llselectmgr.h"
#include "llstatusbar.h"
#include "lltooldraganddrop.h"
#include "lltrans.h"
#include "llviewerassettype.h"
#include "llviewerinventory.h"
#include "llviewerregion.h"
#include "llviewerobjectlist.h"
#include "llviewermessage.h"
// [RLVa:KB] - Checked: 2011-05-22 (RLVa-1.3.1a)
#include "rlvactions.h"
#include "rlvhandler.h"
#include "rlvlocks.h"
// [/RLVa:KB]
#include "llfloaterproperties.h" // <FS:Ansariel> Keep legacy properties floater
// <FS:Trish> Whole linkset view
#include "fslinksetinventoryloader.h"
#include "fsobjectnames.h"
#include "llframetimer.h"
#include <unordered_set>
// </FS:Trish>

const LLColor4U DEFAULT_WHITE(255, 255, 255);
#include "tea.h" // <FS:AW opensim currency support>

///----------------------------------------------------------------------------
/// Class LLTaskInvFVBridge
///----------------------------------------------------------------------------

class LLTaskInvFVBridge : public LLFolderViewModelItemInventory
{
protected:
    LLUUID mUUID;
    std::string mName;
    mutable std::string mDisplayName;
    mutable std::string mSearchableName;
    LLPanelObjectInventory* mPanel;
    U32 mFlags;
    LLAssetType::EType mAssetType;
    LLInventoryType::EType mInventoryType;

    LLInventoryObject* findInvObject() const;
    LLInventoryItem* findItem() const;
    // <FS:Trish> Each item acts on its own link. Null only for the panel's root
    // folder, which follows whatever object the panel shows.
    LLUUID mTaskID;
    const LLUUID& getTaskID() const { return mTaskID.notNull() ? mTaskID : mPanel->getTaskUUID(); }
    // </FS:Trish>

public:
    LLTaskInvFVBridge(LLPanelObjectInventory* panel,
                      const LLUUID& uuid,
                      const std::string& name,
                      U32 flags=0);
    virtual ~LLTaskInvFVBridge() {}

    virtual LLFontGL::StyleFlags getLabelStyle() const { return LLFontGL::NORMAL; }
    virtual std::string getLabelSuffix() const { return LLStringUtil::null; }

    // <FS:Trish> Bridges belong to one link
    //static LLTaskInvFVBridge* createObjectBridge(LLPanelObjectInventory* panel,
    //                                             LLInventoryObject* object);
    static LLTaskInvFVBridge* createObjectBridge(LLPanelObjectInventory* panel,
                                                 LLInventoryObject* object,
                                                 const LLUUID& task_id = LLUUID::null);
    const LLUUID& getBridgeTaskID() const { return getTaskID(); }
    // </FS:Trish>
    void showProperties();
    S32 getPrice();

    // LLFolderViewModelItemInventory functionality
    virtual const std::string& getName() const;
    virtual const std::string& getDisplayName() const;
    virtual const std::string& getSearchableName() const;

    virtual std::string getSearchableDescription() const {return LLStringUtil::null;}
    virtual std::string getSearchableCreatorName() const {return LLStringUtil::null;}
    virtual std::string getSearchableUUIDString() const {return LLStringUtil::null;}
    virtual std::string getSearchableAll() const { return LLStringUtil::null; } // <FS:Ansariel> Zi's extended inventory search

    virtual PermissionMask getPermissionMask() const { return PERM_NONE; }
    /*virtual*/ LLFolderType::EType getPreferredType() const { return LLFolderType::FT_NONE; }
    virtual const LLUUID& getUUID() const { return mUUID; }
    virtual const LLUUID& getThumbnailUUID() const { return LLUUID::null;}
    virtual time_t getCreationDate() const;
    virtual void setCreationDate(time_t creation_date_utc);

    virtual LLUIImagePtr getIcon() const;
    virtual void openItem();
    virtual bool canOpenItem() const { return false; }
    virtual void closeItem() {}
    virtual void selectItem() {}
    virtual void navigateToFolder(bool new_window = false, bool change_mode = false) {}
    virtual bool isItemRenameable() const;
    virtual bool renameItem(const std::string& new_name);
    virtual bool isFavorite() const { return false; }
    virtual bool isItemMovable() const;
    virtual bool isItemRemovable(bool check_worn = true) const;
    virtual bool removeItem();
    virtual void removeBatch(std::vector<LLFolderViewModelItem*>& batch);
    virtual void move(LLFolderViewModelItem* parent_listener);
    virtual bool isItemCopyable(bool can_copy_as_link = true) const;
    virtual bool copyToClipboard() const;
    virtual bool cutToClipboard();
    virtual bool isClipboardPasteable() const;
    virtual void pasteFromClipboard();
    virtual void pasteLinkFromClipboard();
    virtual void buildContextMenu(LLMenuGL& menu, U32 flags);
    virtual void performAction(LLInventoryModel* model, std::string action);
    virtual bool isUpToDate() const { return true; }
    virtual bool hasChildren() const { return false; }
    // <FS:Trish> Report the item's real type so the Content tab's type filter can sort it
    //virtual LLInventoryType::EType getInventoryType() const { return LLInventoryType::IT_NONE; }
    virtual LLInventoryType::EType getInventoryType() const { return mInventoryType; }
    // </FS:Trish>
    virtual LLWearableType::EType getWearableType() const { return LLWearableType::WT_NONE; }
    virtual LLSettingsType::type_e getSettingsType() const { return LLSettingsType::ST_NONE; }
    virtual EInventorySortGroup getSortGroup() const { return SG_ITEM; }
    virtual LLInventoryObject* getInventoryObject() const { return findInvObject(); }


    // LLDragAndDropBridge functionality
    virtual LLToolDragAndDrop::ESource getDragSource() const { return LLToolDragAndDrop::SOURCE_WORLD; }
    virtual bool startDrag(EDragAndDropType* type, LLUUID* id) const;
    virtual bool dragOrDrop(MASK mask, bool drop,
                            EDragAndDropType cargo_type,
                            void* cargo_data,
                            std::string& tooltip_msg);
};

// <FS:Trish> The link a bridge is being created for. Passed this way rather than through
// every subclass constructor; bridges are only created on the main thread, in createObjectBridge().
static const LLUUID* sCreatingTaskID = nullptr;
// </FS:Trish>

LLTaskInvFVBridge::LLTaskInvFVBridge(
    LLPanelObjectInventory* panel,
    const LLUUID& uuid,
    const std::string& name,
    U32 flags)
:   LLFolderViewModelItemInventory(panel->getRootViewModel()),
    mUUID(uuid),
    mName(name),
    mPanel(panel),
    mFlags(flags),
    mAssetType(LLAssetType::AT_NONE),
    mInventoryType(LLInventoryType::IT_NONE)
{
    // <FS:Trish> Remember the link before looking the item up in it
    if (sCreatingTaskID)
    {
        mTaskID = *sCreatingTaskID;
    }
    // </FS:Trish>
    if (const LLInventoryItem* item = findItem())
    {
        mAssetType = item->getType();
        mInventoryType = item->getInventoryType();
    }
}

LLInventoryObject* LLTaskInvFVBridge::findInvObject() const
{
    // <FS:Trish> Act on the item's own link
    //const LLUUID& id = mPanel->getTaskUUID();
    const LLUUID& id = getTaskID();
    // </FS:Trish>
    if (LLViewerObject* object = gObjectList.findObject(id))
    {
        return object->getInventoryObject(mUUID);
    }

    return NULL;
}

LLInventoryItem* LLTaskInvFVBridge::findItem() const
{
    return dynamic_cast<LLInventoryItem*>(findInvObject());
}

void LLTaskInvFVBridge::showProperties()
{
    // <FS:Trish> Act on the item's own link
    //show_task_item_profile(mUUID, mPanel->getTaskUUID());
    show_task_item_profile(mUUID, getTaskID());
    // </FS:Trish>
}


S32 LLTaskInvFVBridge::getPrice()
{
    if (LLInventoryItem* item = findItem())
    {
        return item->getSaleInfo().getSalePrice();
    }

    return -1;
}

// virtual
const std::string& LLTaskInvFVBridge::getName() const
{
    return mName;
}

// virtual
const std::string& LLTaskInvFVBridge::getDisplayName() const
{
    if (LLInventoryItem* item = findItem())
    {
        mDisplayName.assign(item->getName());

        // Localize "New Script", "New Script 1", "New Script 2", etc.
        if (item->getType() == LLAssetType::AT_LSL_TEXT &&
            LLStringUtil::startsWith(item->getName(), "New Script"))
        {
            LLStringUtil::replaceString(mDisplayName, "New Script", LLTrans::getString("PanelContentsNewScript"));
        }

        const LLPermissions& perm(item->getPermissions());
        bool copy = gAgent.allowOperation(PERM_COPY, perm, GP_OBJECT_MANIPULATE);
        bool mod  = gAgent.allowOperation(PERM_MODIFY, perm, GP_OBJECT_MANIPULATE);
        bool xfer = gAgent.allowOperation(PERM_TRANSFER, perm, GP_OBJECT_MANIPULATE);

        if (!copy)
        {
            mDisplayName.append(LLTrans::getString("no_copy"));
        }
        if (!mod)
        {
            mDisplayName.append(LLTrans::getString("no_modify"));
        }
        if (!xfer)
        {
            mDisplayName.append(LLTrans::getString("no_transfer"));
        }
    }

    mSearchableName.assign(mDisplayName + getLabelSuffix());
    LLStringUtil::toUpper(mSearchableName);

    return mDisplayName;
}

// virtual
const std::string& LLTaskInvFVBridge::getSearchableName() const
{
    return mSearchableName;
}

// BUG: No creation dates for task inventory
time_t LLTaskInvFVBridge::getCreationDate() const
{
    return 0;
}

void LLTaskInvFVBridge::setCreationDate(time_t creation_date_utc)
{
}

LLUIImagePtr LLTaskInvFVBridge::getIcon() const
{
    const bool item_is_multi = mFlags & LLInventoryItemFlags::II_FLAGS_OBJECT_HAS_MULTIPLE_ITEMS;

    return LLInventoryIcon::getIcon(mAssetType, mInventoryType, 0, item_is_multi);
}

void LLTaskInvFVBridge::openItem()
{
    // no-op.
    LL_DEBUGS() << "LLTaskInvFVBridge::openItem()" << LL_ENDL;
}

bool LLTaskInvFVBridge::isItemRenameable() const
{
// [RLVa:KB] - Checked: 2010-09-28 (RLVa-1.2.1f) | Modified: RLVa-1.0.5a
    // <FS:Trish> Act on the item's own link
    //LLViewerObject* object = gObjectList.findObject(mPanel->getTaskUUID());
    LLViewerObject* object = gObjectList.findObject(getTaskID());
    // </FS:Trish>
    if (rlv_handler_t::isEnabled() && object && gRlvAttachmentLocks.isLockedAttachment(object->getRootEdit()))
    {
        return false;
    }
// [/RLVa:KB]

    if(gAgent.isGodlike())
        return true;
    //if (LLViewerObject* object = gObjectList.findObject(mPanel->getTaskUUID()))
    if(object)
    {
        if (LLInventoryItem* item = (LLInventoryItem*)(object->getInventoryObject(mUUID)))
        {
            if (gAgent.allowOperation(PERM_MODIFY, item->getPermissions(), GP_OBJECT_MANIPULATE, GOD_LIKE))
            {
                return true;
            }
        }
    }

    return false;
}

bool LLTaskInvFVBridge::renameItem(const std::string& new_name)
{
// [RLVa:KB] - Checked: 2010-09-28 (RLVa-1.2.1f) | Modified: RLVa-1.0.5a
    // <FS:Trish> Act on the item's own link
    //LLViewerObject* object = gObjectList.findObject(mPanel->getTaskUUID());
    LLViewerObject* object = gObjectList.findObject(getTaskID());
    // </FS:Trish>
    if (rlv_handler_t::isEnabled() && object && gRlvAttachmentLocks.isLockedAttachment(object->getRootEdit()))
    {
        return false;
    }
// [/RLVa:KB]

    //if (LLViewerObject* object = gObjectList.findObject(mPanel->getTaskUUID()))
    if(object)
    {
        if (LLViewerInventoryItem* item = (LLViewerInventoryItem*)object->getInventoryObject(mUUID))
        {
            if (gAgent.allowOperation(PERM_MODIFY, item->getPermissions(), GP_OBJECT_MANIPULATE, GOD_LIKE))
            {
                LLPointer<LLViewerInventoryItem> new_item = new LLViewerInventoryItem(item);
                new_item->rename(new_name);
                object->updateInventory(new_item, TASK_INVENTORY_ITEM_KEY, false);
            }
        }
    }

    return true;
}

bool LLTaskInvFVBridge::isItemMovable() const
{
    //LLViewerObject* object = gObjectList.findObject(mPanel->getTaskUUID());
    //if(object && (object->permModify() || gAgent.isGodlike()))
    //{
    //  return true;
    //}
    //return false;
// [RLVa:KB] - Checked: 2010-04-01 (RLVa-1.2.0c) | Modified: RLVa-1.0.5a
    if (rlv_handler_t::isEnabled())
    {
        // <FS:Trish> Act on the item's own link
        //const LLViewerObject* pObj = gObjectList.findObject(mPanel->getTaskUUID());
        const LLViewerObject* pObj = gObjectList.findObject(getTaskID());
        // </FS:Trish>
        if (pObj)
        {
            if (gRlvAttachmentLocks.isLockedAttachment(pObj->getRootEdit()))
            {
                return false;
            }
            else if ( (gRlvHandler.hasBehaviour(RLV_BHVR_UNSIT)) || (gRlvHandler.hasBehaviour(RLV_BHVR_SITTP)) )
            {
                if ( (isAgentAvatarValid()) && (gAgentAvatarp->isSitting()) && (gAgentAvatarp->getRoot() == pObj->getRootEdit()) )
                    return false;
            }
        }
    }
// [/RLVa:KB]
    return true;
}

bool LLTaskInvFVBridge::isItemRemovable(bool check_worn) const
{
    // <FS:Trish> Act on the item's own link
    //const LLViewerObject* object = gObjectList.findObject(mPanel->getTaskUUID());
    const LLViewerObject* object = gObjectList.findObject(getTaskID());
    // </FS:Trish>
// [RLVa:KB] - Checked: 2010-04-01 (RLVa-1.2.0c) | Modified: RLVa-1.0.5a
    if (object && rlv_handler_t::isEnabled())
    {
        if (gRlvAttachmentLocks.isLockedAttachment(object->getRootEdit()))
        {
            return false;
        }
        else if ( (gRlvHandler.hasBehaviour(RLV_BHVR_UNSIT)) || (gRlvHandler.hasBehaviour(RLV_BHVR_SITTP)) )
        {
            if ( (isAgentAvatarValid()) && (gAgentAvatarp->isSitting()) && (gAgentAvatarp->getRoot() == object->getRootEdit()) )
                return false;
        }
    }
// [/RLVa:KB]

    //if (const LLViewerObject* object = gObjectList.findObject(mPanel->getTaskUUID()))
    if (object)
    {
        return object->permModify() || object->permYouOwner();
    }

    return false;
}

bool remove_task_inventory_callback(const LLSD& notification, const LLSD& response, LLPanelObjectInventory* panel)
{
    S32 option = LLNotificationsUtil::getSelectedOption(notification, response);
    if (option == 0)
    {
        if (LLViewerObject* object = gObjectList.findObject(notification["payload"]["task_id"].asUUID()))
        {
            // yes
            const LLSD& inventory_ids = notification["payload"]["inventory_ids"];
            LLSD::array_const_iterator list_it = inventory_ids.beginArray();
            LLSD::array_const_iterator list_end = inventory_ids.endArray();
            for (; list_it != list_end; ++list_it)
            {
                object->removeInventory(list_it->asUUID());
            }

            // refresh the UI.
            panel->refresh();
        }
    }

    return false;
}

// helper for remove
// ! REFACTOR ! two_uuids_list_t is also defined in llinventorybridge.h, but differently.
typedef std::pair<LLUUID, std::list<LLUUID> > panel_two_uuids_list_t;
typedef std::pair<LLPanelObjectInventory*, panel_two_uuids_list_t> remove_data_t;
bool LLTaskInvFVBridge::removeItem()
{
    if (isItemRemovable() && mPanel)
    {
        // <FS:Trish> Act on the item's own link
        //if (LLViewerObject* object = gObjectList.findObject(mPanel->getTaskUUID()))
        if (LLViewerObject* object = gObjectList.findObject(getTaskID()))
        // </FS:Trish>
        {
            if (object->permModify())
            {
                // just do it.
                object->removeInventory(mUUID);
                return true;
            }

            LLNotificationsUtil::add("CantModifyContentInNoModTask");
            return false;
        }
    }

    return false;
}

void LLTaskInvFVBridge::removeBatch(std::vector<LLFolderViewModelItem*>& batch)
{
    if (!mPanel)
    {
        return;
    }

    // <FS:Trish> A selection can span several links: remove each item from its own link
    if (mPanel->isLinksetMode())
    {
        bool refused = false;
        for (LLFolderViewModelItem* item : batch)
        {
            LLTaskInvFVBridge* bridge = dynamic_cast<LLTaskInvFVBridge*>(item);
            if (!bridge || !bridge->isItemRemovable())
            {
                continue;
            }
            LLViewerObject* link = gObjectList.findObject(bridge->getTaskID());
            if (!link)
            {
                continue;
            }
            if (!link->permModify())
            {
                refused = true;
                continue;
            }
            link->removeInventory(bridge->getUUID());
        }
        if (refused)
        {
            LLNotificationsUtil::add("CantModifyContentInNoModTask");
        }
        return;
    }
    // </FS:Trish>

    // <FS:Trish> Act on the item's own link
    //LLViewerObject* object = gObjectList.findObject(mPanel->getTaskUUID());
    LLViewerObject* object = gObjectList.findObject(getTaskID());
    // </FS:Trish>
    if (!object)
    {
        return;
    }

    if (!object->permModify())
    {
        LLNotificationsUtil::add("CantModifyContentInNoModTask");
    }
    else
    {
        for (LLFolderViewModelItem* item : batch)
        {
            LLTaskInvFVBridge* bridge = (LLTaskInvFVBridge*)item;
            if (bridge->isItemRemovable())
            {
                // Just do it.
                object->removeInventory(bridge->getUUID());
            }
        }
    }
}

void LLTaskInvFVBridge::move(LLFolderViewModelItem* parent_listener)
{
}

bool LLTaskInvFVBridge::isItemCopyable(bool can_link) const
{
    if (LLInventoryItem* item = findItem())
    {
        return gAgent.allowOperation(PERM_COPY, item->getPermissions(), GP_OBJECT_MANIPULATE);
    }

    return false;
}

bool LLTaskInvFVBridge::copyToClipboard() const
{
    return false;
}

bool LLTaskInvFVBridge::cutToClipboard()
{
    return false;
}

bool LLTaskInvFVBridge::isClipboardPasteable() const
{
    return false;
}

void LLTaskInvFVBridge::pasteFromClipboard()
{
}

void LLTaskInvFVBridge::pasteLinkFromClipboard()
{
}

bool LLTaskInvFVBridge::startDrag(EDragAndDropType* type, LLUUID* id) const
{
    //LL_INFOS() << "LLTaskInvFVBridge::startDrag()" << LL_ENDL;
    if (!mPanel)
        return false;

    // <FS:Trish> A drag carries one source object, so items from several links
    // can't be dragged out together. The drag's source is this item's link.
    if (mPanel->isLinksetMode())
    {
        if (mPanel->getSelectionTaskID() != getTaskID())
        {
            return false;
        }
        mPanel->getRootViewModel().setTaskID(getTaskID());
    }
    // </FS:Trish>

    // <FS:Trish> Act on the item's own link
    //if (LLViewerObject* object = gObjectList.findObject(mPanel->getTaskUUID()))
    if (LLViewerObject* object = gObjectList.findObject(getTaskID()))
    // </FS:Trish>
    {
        if (LLInventoryItem* inv = (LLInventoryItem*)object->getInventoryObject(mUUID))
        {
            const LLPermissions& perm = inv->getPermissions();
            bool can_copy = gAgent.allowOperation(PERM_COPY, perm, GP_OBJECT_MANIPULATE);
// [RLVa:KB] - Checked: 2009-10-10 (RLVa-1.2.1f) | Modified: RLVa-1.0.5a
            // Kind of redundant due to the note below, but in case that ever gets fixed
            if ( (rlv_handler_t::isEnabled()) && (gRlvAttachmentLocks.isLockedAttachment(object->getRootEdit())) )
            {
                return false;
            }
// [/RLVa:KB]
            if (!can_copy && object->isAttachment())
            {
                //RN: no copy contents of attachments cannot be dragged out
                // due to a race condition and possible exploit where
                // attached objects do not update their inventory items
                // when their contents are manipulated
                return false;
            }

            if ((can_copy && perm.allowTransferTo(gAgent.getID()))
                || object->permYouOwner())
//                 || gAgent.isGodlike())
            {
                *type = LLViewerAssetType::lookupDragAndDropType(inv->getType());
                *id = inv->getUUID();
                return true;
            }
        }
    }

    return false;
}

bool LLTaskInvFVBridge::dragOrDrop(MASK mask, bool drop,
                                   EDragAndDropType cargo_type,
                                   void* cargo_data,
                                   std::string& tooltip_msg)
{
    //LL_INFOS() << "LLTaskInvFVBridge::dragOrDrop()" << LL_ENDL;
    return false;
}

// virtual
void LLTaskInvFVBridge::performAction(LLInventoryModel* model, std::string action)
{
    if (action == "task_open")
    {
        openItem();
    }
    else if (action == "task_properties")
    {
        showProperties();
    }
}

void LLTaskInvFVBridge::buildContextMenu(LLMenuGL& menu, U32 flags)
{
    LLInventoryItem* item = findItem();
    std::vector<std::string> items;
    std::vector<std::string> disabled_items;

    if (!item)
    {
        hide_context_entries(menu, items, disabled_items);
        return;
    }

    if (canOpenItem())
    {
        items.push_back(std::string("Task Open"));
// [RLVa:KB] - Checked: 2010-03-01 (RLVa-1.2.0b) | Modified: RLVa-1.1.0a
        if (rlv_handler_t::isEnabled())
        {
            // <FS:Trish> Act on the item's own link
            //LLViewerObject* pAttachObj = gObjectList.findObject(mPanel->getTaskUUID());
            LLViewerObject* pAttachObj = gObjectList.findObject(getTaskID());
            // </FS:Trish>
            bool fLocked = (pAttachObj) ? gRlvAttachmentLocks.isLockedAttachment(pAttachObj->getRootEdit()) : false;
            if ( ((LLAssetType::AT_NOTECARD == item->getType()) && ((gRlvHandler.hasBehaviour(RLV_BHVR_VIEWNOTE)) || (fLocked))) ||
                 ((LLAssetType::AT_LSL_TEXT == item->getType()) && ((gRlvHandler.hasBehaviour(RLV_BHVR_VIEWSCRIPT)) || (fLocked))) ||
                 ((LLAssetType::AT_TEXTURE == item->getType()) && (!RlvActions::canPreviewTextures())))
            {
                disabled_items.push_back(std::string("Task Open"));
            }
        }
// [/RLVa:KB]
    }
    items.push_back(std::string("Task Properties"));
    if (item->getType() == LLAssetType::AT_LSL_TEXT)
    {
        items.push_back(std::string("Task Stop Script"));
        bool is_running = true;
        // <FS:Trish> Act on the item's own link
        //if (!mPanel->getScriptRunningState(item->getUUID(), is_running))
        if (!mPanel->getScriptRunningState(getTaskID(), item->getUUID(), is_running))
        // </FS:Trish>
        {
            // <FS:Trish> Act on the item's own link
            //mPanel->requestScriptRunningInfo(item->getUUID());
            mPanel->requestScriptRunningInfo(getTaskID(), item->getUUID());
            // </FS:Trish>
        }
        if (LLMenuItemGL* menu_item = menu.getChild<LLMenuItemGL>("Task Stop Script", true))
        {
            menu_item->setLabel(LLStringExplicit(is_running ? "Stop Script" : "Start Script"));
        }
        // <FS:Trish> Act on the item's own link
        //if (!mPanel->canStopScripts())
        if (!mPanel->canStopScripts(getTaskID()))
        // </FS:Trish>
        {
            disabled_items.push_back(std::string("Task Stop Script"));
        }
        // <FS:Trish> Whole linkset view: run, stop or reset every selected script, each in its own link
        if (mPanel->isLinksetMode())
        {
            items.push_back(std::string("Task Scripts Running"));
            items.push_back(std::string("Task Scripts Not Running"));
            items.push_back(std::string("Task Reset Scripts"));
            LLViewerObject* link = gObjectList.findObject(getTaskID());
            const bool locked = link && rlv_handler_t::isEnabled() && gRlvAttachmentLocks.isLockedAttachment(link->getRootEdit());
            if (locked || !mPanel->canStopScripts(getTaskID()))
            {
                disabled_items.push_back(std::string("Task Scripts Running"));
                disabled_items.push_back(std::string("Task Scripts Not Running"));
                disabled_items.push_back(std::string("Task Reset Scripts"));
            }
        }
        // </FS:Trish>
    }
    // <FS:Ansariel> Improved object properties
    //if ((flags & FIRST_SELECTED_ITEM) == 0)
    //{
    //    disabled_items.push_back(std::string("Task Properties"));
    //}
    // </FS:Ansariel>
// [RLVa:KB] - Checked: 2010-09-28 (RLVa-1.2.1f) | Added: RLVa-1.2.1f
    items.push_back(std::string("Task Rename"));
    if ( (!isItemRenameable()) || ((flags & FIRST_SELECTED_ITEM) == 0) )
    {
        disabled_items.push_back(std::string("Task Rename"));
    }
    items.push_back(std::string("Task Remove"));
    if (!isItemRemovable())
    {
        disabled_items.push_back(std::string("Task Remove"));
    }
// [/RLVa:KB]
//  if (isItemRenameable())
//  {
//      items.push_back(std::string("Task Rename"));
//      if ((flags & FIRST_SELECTED_ITEM) == 0)
//      {
//          disabled_items.push_back(std::string("Task Rename"));
//      }
//  }
//  if (isItemRemovable())
//  {
//      items.push_back(std::string("Task Remove"));
//  }

    hide_context_entries(menu, items, disabled_items);
}


///----------------------------------------------------------------------------
/// Class LLTaskFolderBridge
///----------------------------------------------------------------------------

class LLTaskCategoryBridge : public LLTaskInvFVBridge
{
public:
    LLTaskCategoryBridge(
        LLPanelObjectInventory* panel,
        const LLUUID& uuid,
        const std::string& name);

    virtual LLUIImagePtr getIcon() const;
    virtual const std::string& getDisplayName() const;
    virtual bool isItemRenameable() const { return false; }
    // virtual bool isItemCopyable() const { return false; }
    virtual bool renameItem(const std::string& new_name) { return false; }
    virtual bool isItemRemovable(bool check_worn = true) const { return false; }
    virtual void buildContextMenu(LLMenuGL& menu, U32 flags);
    virtual bool hasChildren() const;
    virtual bool startDrag(EDragAndDropType* type, LLUUID* id) const;
    virtual bool dragOrDrop(MASK mask, bool drop,
                            EDragAndDropType cargo_type,
                            void* cargo_data,
                            std::string& tooltip_msg);
    virtual bool canOpenItem() const { return true; }
    virtual void openItem();
    virtual EInventorySortGroup getSortGroup() const { return SG_NORMAL_FOLDER; }
    // <FS:Trish> Not IT_CATEGORY: the inventory filter would look the folder up in the
    // agent's inventory and try to fetch it. As "unknown" a folder passes only when every
    // type is shown, so a narrowed type filter keeps just the folders holding a match.
    LLInventoryType::EType getInventoryType() const override { return LLInventoryType::IT_UNKNOWN; }
    // </FS:Trish>
};

LLTaskCategoryBridge::LLTaskCategoryBridge(
    LLPanelObjectInventory* panel,
    const LLUUID& uuid,
    const std::string& name) :
    LLTaskInvFVBridge(panel, uuid, name)
{
}

// virtual
LLUIImagePtr LLTaskCategoryBridge::getIcon() const
{
    return LLUI::getUIImage("Inv_FolderClosed");
}

// virtual
const std::string& LLTaskCategoryBridge::getDisplayName() const
{
    if (LLInventoryObject* cat = findInvObject())
    {
        std::string name = cat->getName();
        // <FS:Zi> FIRE-24142 - Show number of elements in object inventory
        //if (mChildren.size() > 0)
        //{
        //    // Add item count
        //    // Normally we would be using getLabelSuffix for this
        //    // but object's inventory just uses displaynames
        //    LLStringUtil::format_map_t args;
        //    args["[ITEMS_COUNT]"] = llformat("%d", mChildren.size());

        //    name.append(" " + LLTrans::getString("InventoryItemsCount", args));
        //}
        //mDisplayName.assign(name);
        //LLStringUtil::toUpper(name);
        //mSearchableName.assign(name);
        if (cat->getParentUUID().isNull() && cat->getName() == "Contents")
        {
            // <FS:Trish> Act on the item's own link
            //if (LLViewerObject* object = gObjectList.findObject(mPanel->getTaskUUID()))
            if (LLViewerObject* object = gObjectList.findObject(getTaskID()))
            // </FS:Trish>
            {
                LLInventoryObject::object_list_t contents;

                object->getInventoryContents(contents);
                S32 numElements = static_cast<S32>(contents.size());

                std::string elementsString = "FSObjectInventoryNoElements";
                if (numElements == 1)
                {
                    elementsString = "FSObjectInventoryOneElement";
                }
                else if (numElements > 1)
                {
                    elementsString = "FSObjectInventoryElements";
                }

                LLSD args;
                args["NUM_ELEMENTS"] = numElements;
                name = llformat("%s (%s)",
                    LLTrans::getString("Contents").c_str(),
                    LLTrans::getString(elementsString, args).c_str());
            }
            // fallback in case something goes wrong with finding the inventory object
            else
            {
                name = LLTrans::getString("Contents");
            }
        }
        // </FS:Zi>

        mDisplayName.assign(name);
        LLStringUtil::toUpper(name);
        mSearchableName.assign(name);
    }

    return mDisplayName;
}

void LLTaskCategoryBridge::buildContextMenu(LLMenuGL& menu, U32 flags)
{
    std::vector<std::string> items;
    std::vector<std::string> disabled_items;

    LLInventoryObject* category = findInvObject();
    if (category && category->getParentUUID().isNull() && category->getName() == "Contents")
    {
        items.push_back(std::string("Task Stop Scripts"));
        // <FS:Trish> Act on the item's own link
        //if (!mPanel->canStopScripts())
        if (!mPanel->canStopScripts(getTaskID()))
        // </FS:Trish>
        {
            disabled_items.push_back(std::string("Task Stop Scripts"));
        }
    }

    hide_context_entries(menu, items, disabled_items);
}

// virtual
bool LLTaskCategoryBridge::hasChildren() const
{
    // return true if we have or do know know if we have children.
    // *FIX: For now, return false - we will know for sure soon enough.
    return false;
}

// virtual
void LLTaskCategoryBridge::openItem()
{
}

// virtual
bool LLTaskCategoryBridge::startDrag(EDragAndDropType* type, LLUUID* id) const
{
    //LL_INFOS() << "LLTaskInvFVBridge::startDrag()" << LL_ENDL;
    if (mPanel && mUUID.notNull())
    {
        // <FS:Trish> Act on the item's own link
        //if (LLViewerObject* object = gObjectList.findObject(mPanel->getTaskUUID()))
        if (LLViewerObject* object = gObjectList.findObject(getTaskID()))
        // </FS:Trish>
        {
            const LLInventoryObject* cat = object->getInventoryObject(mUUID);
            if (cat && move_inv_category_world_to_agent(mUUID, LLUUID::null, false))
            {
                *type = LLViewerAssetType::lookupDragAndDropType(cat->getType());
                *id = mUUID;
                return true;
            }
        }
    }
    return false;
}

// virtual
bool LLTaskCategoryBridge::dragOrDrop(MASK mask, bool drop,
                                      EDragAndDropType cargo_type,
                                      void* cargo_data,
                                      std::string& tooltip_msg)
{
    //LL_INFOS() << "LLTaskCategoryBridge::dragOrDrop()" << LL_ENDL;
    bool accept = false;
    // <FS:Trish> Act on the item's own link
    //LLViewerObject* object = gObjectList.findObject(mPanel->getTaskUUID());
    LLViewerObject* object = gObjectList.findObject(getTaskID());
    // </FS:Trish>
    if(object)
    {
        switch(cargo_type)
        {
        case DAD_CATEGORY:
            accept = LLToolDragAndDrop::getInstance()->dadUpdateInventoryCategory(object,drop);
            break;
        case DAD_TEXTURE:
        case DAD_SOUND:
        case DAD_LANDMARK:
        case DAD_OBJECT:
        case DAD_NOTECARD:
        case DAD_CLOTHING:
        case DAD_BODYPART:
        case DAD_ANIMATION:
        case DAD_GESTURE:
        case DAD_CALLINGCARD:
        case DAD_MESH:
        case DAD_SETTINGS:
        case DAD_MATERIAL:
            accept = LLToolDragAndDrop::isInventoryDropAcceptable(object, (LLViewerInventoryItem*)cargo_data);
            if(accept && drop)
            {
                LLToolDragAndDrop::dropInventory(object,
                                                 (LLViewerInventoryItem*)cargo_data,
                                                 LLToolDragAndDrop::getInstance()->getSource(),
                                                 LLToolDragAndDrop::getInstance()->getSourceID());
            }
            break;
        case DAD_SCRIPT:
            // *HACK: In order to resolve SL-22177, we need to block
            // drags from notecards and objects onto other
            // objects. uncomment the simpler version when we have
            // that right.
            //accept = LLToolDragAndDrop::isInventoryDropAcceptable(object, (LLViewerInventoryItem*)cargo_data);
            if(LLToolDragAndDrop::isInventoryDropAcceptable(
                   object, (LLViewerInventoryItem*)cargo_data)
               && (LLToolDragAndDrop::SOURCE_WORLD != LLToolDragAndDrop::getInstance()->getSource())
               && (LLToolDragAndDrop::SOURCE_NOTECARD != LLToolDragAndDrop::getInstance()->getSource()))
            {
                accept = true;
            }
            if(accept && drop)
            {
                LLViewerInventoryItem* item = (LLViewerInventoryItem*)cargo_data;
                // rez in the script active by default, rez in
                // inactive if the control key is being held down.
                bool active = ((mask & MASK_CONTROL) == 0);
                LLToolDragAndDrop::dropScript(object, item, active,
                                              LLToolDragAndDrop::getInstance()->getSource(),
                                              LLToolDragAndDrop::getInstance()->getSourceID());
            }
            break;
        default:
            break;
        }
    }
    return accept;
}

// <FS:Trish> Whole linkset view: the folder holding one link's contents.
// Dropping on it puts the item into that link (LLTaskCategoryBridge::dragOrDrop
// uses the bridge's own link).
class LLTaskLinkBridge : public LLTaskCategoryBridge
{
public:
    LLTaskLinkBridge(LLPanelObjectInventory* panel,
                     const LLUUID& link_id,
                     const std::string& label) :
        LLTaskCategoryBridge(panel, link_id, label)
    {
        mTaskID = link_id;
        setLabel(label);
    }

    void setLabel(const std::string& label)
    {
        mLabel = label;
        mDisplayName = label;
        mSearchableName = label;
        LLStringUtil::toUpper(mSearchableName);
    }

    const std::string& getName() const override { return mLabel; }
    const std::string& getDisplayName() const override { return mDisplayName; }
    LLUIImagePtr getIcon() const override { return LLUI::getUIImage("Inv_Object"); }
    void buildContextMenu(LLMenuGL& menu, U32 flags) override
    {
        std::vector<std::string> items;
        std::vector<std::string> disabled_items;
        hide_context_entries(menu, items, disabled_items);
    }
    bool startDrag(EDragAndDropType* type, LLUUID* id) const override { return false; }

private:
    std::string mLabel;
};
// </FS:Trish>

///----------------------------------------------------------------------------
/// Class LLTaskTextureBridge
///----------------------------------------------------------------------------

class LLTaskTextureBridge : public LLTaskInvFVBridge
{
public:
    LLTaskTextureBridge(LLPanelObjectInventory* panel,
                        const LLUUID& uuid,
                        const std::string& name) :
        LLTaskInvFVBridge(panel, uuid, name) {}

    virtual bool canOpenItem() const { return true; }
    virtual void openItem();
};

// virtual
void LLTaskTextureBridge::openItem()
{
    LL_INFOS() << "LLTaskTextureBridge::openItem()" << LL_ENDL;
    // <FS:Ansariel> FIRE-17096 / BUG-10486 / MAINT-5753: Viewer crashes when opening a texture from object contents
    // <FS:Trish> Act on the item's own link
    //LLViewerObject* object = gObjectList.findObject(mPanel->getTaskUUID());
    LLViewerObject* object = gObjectList.findObject(getTaskID());
    // </FS:Trish>
    if(!object || object->isInventoryPending())
    {
        return;
    }
    // </FS:Ansariel>

    if (LLPreviewTexture* preview = LLFloaterReg::showTypedInstance<LLPreviewTexture>("preview_texture", LLSD(mUUID), TAKE_FOCUS_YES))
    {
        if (LLInventoryItem* item = findItem())
        {
            preview->setAuxItem(item);
        }
        // <FS:Trish> Act on the item's own link
        //preview->setObjectID(mPanel->getTaskUUID());
        preview->setObjectID(getTaskID());
        // </FS:Trish>
    }
}


///----------------------------------------------------------------------------
/// Class LLTaskSoundBridge
///----------------------------------------------------------------------------

class LLTaskSoundBridge : public LLTaskInvFVBridge
{
public:
    LLTaskSoundBridge(LLPanelObjectInventory* panel,
                      const LLUUID& uuid,
                      const std::string& name) :
        LLTaskInvFVBridge(panel, uuid, name) {}

    virtual bool canOpenItem() const { return true; }
    virtual void openItem();
    virtual void performAction(LLInventoryModel* model, std::string action);
    virtual void buildContextMenu(LLMenuGL& menu, U32 flags);
    static void openSoundPreview(void* data);
};

// virtual
void LLTaskSoundBridge::openItem()
{
    // <FS:Ansariel> FIRE-17096 / BUG-10486 / MAINT-5753: Viewer crashes when opening a texture from object contents
    // <FS:Trish> Act on the item's own link
    //LLViewerObject* object = gObjectList.findObject(mPanel->getTaskUUID());
    LLViewerObject* object = gObjectList.findObject(getTaskID());
    // </FS:Trish>
    if(!object || object->isInventoryPending())
    {
        return;
    }
    // </FS:Ansariel>
    openSoundPreview((void*)this);
}

// static
void LLTaskSoundBridge::openSoundPreview(void* data)
{
    LLTaskSoundBridge* self = (LLTaskSoundBridge*)data;
    if(!self)
        return;

    LLPreviewSound* preview = LLFloaterReg::showTypedInstance<LLPreviewSound>("preview_sound", LLSD(self->mUUID), TAKE_FOCUS_YES);
    if (preview)
    {
        // <FS:Trish> Act on the item's own link
        //preview->setObjectID(self->mPanel->getTaskUUID());
        preview->setObjectID(self->getTaskID());
        // </FS:Trish>
    }
}

// virtual
void LLTaskSoundBridge::performAction(LLInventoryModel* model, std::string action)
{
    if (action == "task_play")
    {
        LLInventoryItem* item = findItem();
        if(item)
        {
            send_sound_trigger(item->getAssetUUID(), 1.0);
        }
    }
    LLTaskInvFVBridge::performAction(model, action);
}

// virtual
void LLTaskSoundBridge::buildContextMenu(LLMenuGL& menu, U32 flags)
{
    LLInventoryItem* item = findItem();
    std::vector<std::string> items;
    std::vector<std::string> disabled_items;
    if (!item)
    {
        hide_context_entries(menu, items, disabled_items);
        return;
    }

    if (canOpenItem())
    {
        if (!isItemCopyable())
        {
            disabled_items.push_back(std::string("Task Open"));
        }
    }
    items.push_back(std::string("Task Properties"));
    // <FS:Ansariel> Improved object properties
    //if ((flags & FIRST_SELECTED_ITEM) == 0)
    //{
    //    disabled_items.push_back(std::string("Task Properties"));
    //}
    // </FS:Ansariel>
    if (isItemRenameable())
    {
        items.push_back(std::string("Task Rename"));
    }
    if (isItemRemovable())
    {
        items.push_back(std::string("Task Remove"));
    }

    items.push_back(std::string("Task Play"));

    hide_context_entries(menu, items, disabled_items);
}

///----------------------------------------------------------------------------
/// Class LLTaskLandmarkBridge
///----------------------------------------------------------------------------

class LLTaskLandmarkBridge : public LLTaskInvFVBridge
{
public:
    LLTaskLandmarkBridge(LLPanelObjectInventory* panel,
                         const LLUUID& uuid,
                         const std::string& name) :
        LLTaskInvFVBridge(panel, uuid, name) {}
};

///----------------------------------------------------------------------------
/// Class LLTaskCallingCardBridge
///----------------------------------------------------------------------------

class LLTaskCallingCardBridge : public LLTaskInvFVBridge
{
public:
    LLTaskCallingCardBridge(LLPanelObjectInventory* panel,
                            const LLUUID& uuid,
                            const std::string& name) :
        LLTaskInvFVBridge(panel, uuid, name) {}

    virtual bool isItemRenameable() const { return false; }
    virtual bool renameItem(const std::string& new_name) { return false; }
};


///----------------------------------------------------------------------------
/// Class LLTaskScriptBridge
///----------------------------------------------------------------------------

class LLTaskScriptBridge : public LLTaskInvFVBridge
{
public:
    LLTaskScriptBridge(LLPanelObjectInventory* panel,
                       const LLUUID& uuid,
                       const std::string& name) :
        LLTaskInvFVBridge(panel, uuid, name) {}

    LLUIImagePtr getIcon() const override;
    LLFontGL::StyleFlags getLabelStyle() const override;
    std::string getLabelSuffix() const override;
    //static bool enableIfCopyable( void* userdata );
};

// virtual
LLUIImagePtr LLTaskScriptBridge::getIcon() const
{
    // Pass the item's flags so the script subtype (e.g. SST_LUA) is honored
    // and the correct icon (Inv_Script vs Inv_Script_Luau) is selected.
    LLInventoryItem* item = findItem();
    U32 misc_flag = item ? item->getFlags() : 0;
    return LLInventoryIcon::getIcon(mAssetType, mInventoryType, misc_flag, false);
}

LLFontGL::StyleFlags LLTaskScriptBridge::getLabelStyle() const
{
    LLFontGL::StyleFlags style = LLTaskInvFVBridge::getLabelStyle();
    const LLInventoryItem* item = findItem();
    if (!item || item->getType() != LLAssetType::AT_LSL_TEXT)
    {
        return style;
    }

    bool is_running = true;
    if (mPanel)
    {
        // <FS:Trish> Act on the item's own link
        //if (!mPanel->getScriptRunningState(mUUID, is_running))
        if (!mPanel->getScriptRunningState(getTaskID(), mUUID, is_running))
        // </FS:Trish>
        {
            // <FS:Trish> Act on the item's own link
            //mPanel->requestScriptRunningInfo(mUUID);
            mPanel->requestScriptRunningInfo(getTaskID(), mUUID);
            // </FS:Trish>
        }
    }

    if (!is_running)
    {
        style = static_cast<LLFontGL::StyleFlags>(style | LLFontGL::ITALIC);
    }

    return style;
}

std::string LLTaskScriptBridge::getLabelSuffix() const
{
    std::string suffix = LLTaskInvFVBridge::getLabelSuffix();
    const LLInventoryItem* item = findItem();
    if (!item || item->getType() != LLAssetType::AT_LSL_TEXT)
    {
        return suffix;
    }

    bool is_running = true;
    if (mPanel)
    {
        // <FS:Trish> Act on the item's own link
        //if (!mPanel->getScriptRunningState(mUUID, is_running))
        if (!mPanel->getScriptRunningState(getTaskID(), mUUID, is_running))
        // </FS:Trish>
        {
            // <FS:Trish> Act on the item's own link
            //mPanel->requestScriptRunningInfo(mUUID);
            mPanel->requestScriptRunningInfo(getTaskID(), mUUID);
            // </FS:Trish>
            return suffix;
        }
    }

    if (!is_running)
    {
        static const std::string stopped_suffix = " [stopped]";
        suffix += stopped_suffix;
    }

    return suffix;
}

class LLTaskLSLBridge : public LLTaskScriptBridge
{
public:
    LLTaskLSLBridge(LLPanelObjectInventory* panel,
                    const LLUUID& uuid,
                    const std::string& name) :
        LLTaskScriptBridge(panel, uuid, name) {}

    virtual bool canOpenItem() const { return true; }
    virtual void openItem();
    virtual bool removeItem();
    //virtual void buildContextMenu(LLMenuGL& menu);

    //static void copyToInventory(void* userdata);
};

// virtual
void LLTaskLSLBridge::openItem()
{
    LL_INFOS() << "LLTaskLSLBridge::openItem() " << mUUID << LL_ENDL;
    // <FS:Trish> Act on the item's own link
    //LLViewerObject* object = gObjectList.findObject(mPanel->getTaskUUID());
    LLViewerObject* object = gObjectList.findObject(getTaskID());
    // </FS:Trish>
    if(!object || object->isInventoryPending())
    {
        return;
    }
// [RLVa:KB] - Checked: 2010-03-27 (RLVa-1.2.0b) | Modified: RLVa-1.1.0a
    if ( (rlv_handler_t::isEnabled()) && (gRlvAttachmentLocks.isLockedAttachment(object->getRootEdit())) )
    {
        RlvUtil::notifyBlockedViewXXX(LLAssetType::AT_SCRIPT);
        return;
    }
// [/RLVa:KB]
    if (object->permModify() || gAgent.isGodlike())
    {
        LLSD floater_key;
        // <FS:Trish> Act on the item's own link
        //floater_key["taskid"] = mPanel->getTaskUUID();
        floater_key["taskid"] = getTaskID();
        // </FS:Trish>
        floater_key["itemid"] = mUUID;

        LLLiveLSLEditor* preview = LLFloaterReg::showTypedInstance<LLLiveLSLEditor>("preview_scriptedit", floater_key, TAKE_FOCUS_YES);
        if (preview)
        {
            // <FS:Trish> Name the script's own link when the whole linkset is shown
            //LLSelectNode *node = LLSelectMgr::getInstance()->getSelection()->getFirstRootNode(NULL, true);
            LLSelectNode *node = mPanel->isLinksetMode() ? LLSelectMgr::getInstance()->getSelection()->findNode(object) : nullptr;
            if (!node)
            {
                node = LLSelectMgr::getInstance()->getSelection()->getFirstRootNode(NULL, true);
            }
            // </FS:Trish>
            if (node && node->mValid)
            {
                preview->setObjectName(node->mName);
            }
            // <FS:Trish> Act on the item's own link
            //preview->setObjectID(mPanel->getTaskUUID());
            preview->setObjectID(getTaskID());
            // </FS:Trish>
        }
    }
    else
    {
        LLNotificationsUtil::add("CannotOpenScriptObjectNoMod");
    }
}

// virtual
bool LLTaskLSLBridge::removeItem()
{
    LLFloaterReg::hideInstance("preview_scriptedit", LLSD(mUUID));
    return LLTaskInvFVBridge::removeItem();
}

///----------------------------------------------------------------------------
/// Class LLTaskObjectBridge
///----------------------------------------------------------------------------

class LLTaskObjectBridge : public LLTaskInvFVBridge
{
public:
    LLTaskObjectBridge(LLPanelObjectInventory* panel,
                       const LLUUID& uuid,
                       const std::string& name,
                       U32 flags = 0) :
        LLTaskInvFVBridge(panel, uuid, name, flags) {}
};

///----------------------------------------------------------------------------
/// Class LLTaskNotecardBridge
///----------------------------------------------------------------------------

class LLTaskNotecardBridge : public LLTaskInvFVBridge
{
public:
    LLTaskNotecardBridge(LLPanelObjectInventory* panel,
                         const LLUUID& uuid,
                         const std::string& name) :
        LLTaskInvFVBridge(panel, uuid, name) {}

    virtual bool canOpenItem() const { return true; }
    virtual void openItem();
    virtual bool removeItem();
};

// virtual
void LLTaskNotecardBridge::openItem()
{
    // <FS:Trish> Act on the item's own link
    //LLViewerObject* object = gObjectList.findObject(mPanel->getTaskUUID());
    LLViewerObject* object = gObjectList.findObject(getTaskID());
    // </FS:Trish>
    if(!object || object->isInventoryPending())
    {
        return;
    }

// [RLVa:KB] - Checked: 2010-03-27 (RLVa-1.2.0b) | Modified: RLVa-1.2.0b
    if ( (rlv_handler_t::isEnabled()) && (gRlvAttachmentLocks.isLockedAttachment(object->getRootEdit())) )
    {
        RlvUtil::notifyBlockedViewXXX(LLAssetType::AT_NOTECARD);
        return;
    }
// [/RLVa:KB]

    // Note: even if we are not allowed to modify copyable notecard, we should be able to view it
    LLInventoryItem *item = dynamic_cast<LLInventoryItem*>(object->getInventoryObject(mUUID));
    bool item_copy = item && gAgent.allowOperation(PERM_COPY, item->getPermissions(), GP_OBJECT_MANIPULATE);
    if( item_copy
        || object->permModify()
        || gAgent.isGodlike())
    {
        LLSD floater_key;
        // <FS:Trish> Act on the item's own link
        //floater_key["taskid"] = mPanel->getTaskUUID();
        floater_key["taskid"] = getTaskID();
        // </FS:Trish>
        floater_key["itemid"] = mUUID;
        LLPreviewNotecard* preview = LLFloaterReg::showTypedInstance<LLPreviewNotecard>("preview_notecard", floater_key, TAKE_FOCUS_YES);
        if (preview)
        {
            // <FS:Trish> Act on the item's own link
            //preview->setObjectID(mPanel->getTaskUUID());
            preview->setObjectID(getTaskID());
            // </FS:Trish>
        }
    }
}

// virtual
bool LLTaskNotecardBridge::removeItem()
{
    LLFloaterReg::hideInstance("preview_notecard", LLSD(mUUID));
    return LLTaskInvFVBridge::removeItem();
}

///----------------------------------------------------------------------------
/// Class LLTaskGestureBridge
///----------------------------------------------------------------------------

class LLTaskGestureBridge : public LLTaskInvFVBridge
{
public:
    LLTaskGestureBridge(LLPanelObjectInventory* panel,
                        const LLUUID& uuid,
                        const std::string& name) :
    LLTaskInvFVBridge(panel, uuid, name) {}

    virtual bool canOpenItem() const { return true; }
    virtual void openItem();
    virtual bool removeItem();
};

// virtual
void LLTaskGestureBridge::openItem()
{
    // <FS:Trish> Act on the item's own link
    //LLViewerObject* object = gObjectList.findObject(mPanel->getTaskUUID());
    LLViewerObject* object = gObjectList.findObject(getTaskID());
    // </FS:Trish>
    if(!object || object->isInventoryPending())
    {
        return;
    }
    // <FS:Trish> Act on the item's own link
    //LLPreviewGesture::show(mUUID, mPanel->getTaskUUID());
    LLPreviewGesture::show(mUUID, getTaskID());
    // </FS:Trish>
}

// virtual
bool LLTaskGestureBridge::removeItem()
{
    // Don't need to deactivate gesture because gestures inside objects can never be active.
    LLFloaterReg::hideInstance("preview_gesture", LLSD(mUUID));
    return LLTaskInvFVBridge::removeItem();
}

///----------------------------------------------------------------------------
/// Class LLTaskAnimationBridge
///----------------------------------------------------------------------------

class LLTaskAnimationBridge : public LLTaskInvFVBridge
{
public:
    LLTaskAnimationBridge(LLPanelObjectInventory* panel,
                          const LLUUID& uuid,
                          const std::string& name) :
        LLTaskInvFVBridge(panel, uuid, name) {}

    virtual bool canOpenItem() const { return true; }
    virtual void openItem();
    virtual bool removeItem();
};

// virtual
void LLTaskAnimationBridge::openItem()
{
    // <FS:Trish> Act on the item's own link
    //const LLUUID& task_id = mPanel->getTaskUUID();
    const LLUUID& task_id = getTaskID();
    // </FS:Trish>
    LLViewerObject* object = gObjectList.findObject(task_id);
    if (!object || object->isInventoryPending())
    {
        return;
    }

    LLPreviewAnim* preview = LLFloaterReg::showTypedInstance<LLPreviewAnim>("preview_anim", LLSD(mUUID), TAKE_FOCUS_YES);
    if (preview && (object->permModify() || gAgent.isGodlike()))
    {
        // <FS:Trish> Act on the item's own link
        //preview->setObjectID(mPanel->getTaskUUID());
        preview->setObjectID(getTaskID());
        // </FS:Trish>
    }
}

// virtual
bool LLTaskAnimationBridge::removeItem()
{
    LLFloaterReg::hideInstance("preview_anim", LLSD(mUUID));
    return LLTaskInvFVBridge::removeItem();
}

///----------------------------------------------------------------------------
/// Class LLTaskWearableBridge
///----------------------------------------------------------------------------

class LLTaskWearableBridge : public LLTaskInvFVBridge
{
public:
    LLTaskWearableBridge(LLPanelObjectInventory* panel,
                         const LLUUID& uuid,
                         const std::string& name,
                         U32 flags) :
        LLTaskInvFVBridge(panel, uuid, name, flags) {}

    virtual LLUIImagePtr getIcon() const;
};

// virtual
LLUIImagePtr LLTaskWearableBridge::getIcon() const
{
    return LLInventoryIcon::getIcon(mAssetType, mInventoryType, mFlags, false );
}

///----------------------------------------------------------------------------
/// Class LLTaskSettingsBridge
///----------------------------------------------------------------------------

class LLTaskSettingsBridge : public LLTaskInvFVBridge
{
public:
    LLTaskSettingsBridge(LLPanelObjectInventory* panel,
        const LLUUID& uuid,
        const std::string& name,
        U32 flags) :
        LLTaskInvFVBridge(panel, uuid, name, flags) {}

    virtual LLUIImagePtr getIcon() const;
    virtual LLSettingsType::type_e  getSettingsType() const;
};

// virtual
LLUIImagePtr LLTaskSettingsBridge::getIcon() const
{
    return LLInventoryIcon::getIcon(mAssetType, mInventoryType, mFlags, false);
}

// virtual
LLSettingsType::type_e LLTaskSettingsBridge::getSettingsType() const
{
    return LLSettingsType::ST_NONE;
}

///----------------------------------------------------------------------------
/// Class LLTaskMaterialBridge
///----------------------------------------------------------------------------

class LLTaskMaterialBridge : public LLTaskInvFVBridge
{
public:
    LLTaskMaterialBridge(LLPanelObjectInventory* panel,
                         const LLUUID& uuid,
                         const std::string& name) :
        LLTaskInvFVBridge(panel, uuid, name) {}

    bool canOpenItem() const override { return true; }
    void openItem() override;
    bool removeItem() override;
};

// virtual
void LLTaskMaterialBridge::openItem()
{
    // <FS:Trish> Act on the item's own link
    //LLViewerObject* object = gObjectList.findObject(mPanel->getTaskUUID());
    LLViewerObject* object = gObjectList.findObject(getTaskID());
    // </FS:Trish>
    if(!object || object->isInventoryPending())
    {
        return;
    }

    // Note: even if we are not allowed to modify copyable notecard, we should be able to view it
    LLInventoryItem *item = dynamic_cast<LLInventoryItem*>(object->getInventoryObject(mUUID));
    bool item_copy = item && gAgent.allowOperation(PERM_COPY, item->getPermissions(), GP_OBJECT_MANIPULATE);
    if( item_copy
        || object->permModify()
        || gAgent.isGodlike())
    {
        LLSD floater_key;
        // <FS:Trish> Act on the item's own link
        //floater_key["taskid"] = mPanel->getTaskUUID();
        floater_key["taskid"] = getTaskID();
        // </FS:Trish>
        floater_key["itemid"] = mUUID;
        LLMaterialEditor* mat = LLFloaterReg::getTypedInstance<LLMaterialEditor>("material_editor", floater_key);
        if (mat)
        {
            // <FS:Trish> Act on the item's own link
            //mat->setObjectID(mPanel->getTaskUUID());
            mat->setObjectID(getTaskID());
            // </FS:Trish>
            mat->openFloater(floater_key);
            mat->setFocus(true);
        }
    }
}

// virtual
bool LLTaskMaterialBridge::removeItem()
{
    LLFloaterReg::hideInstance("material_editor", LLSD(mUUID));
    return LLTaskInvFVBridge::removeItem();
}

///----------------------------------------------------------------------------
/// LLTaskInvFVBridge impl
//----------------------------------------------------------------------------

// <FS:Trish> Bridges belong to one link
//LLTaskInvFVBridge* LLTaskInvFVBridge::createObjectBridge(LLPanelObjectInventory* panel,
//                                                         LLInventoryObject* object)
LLTaskInvFVBridge* LLTaskInvFVBridge::createObjectBridge(LLPanelObjectInventory* panel,
                                                         LLInventoryObject* object,
                                                         const LLUUID& task_id)
// </FS:Trish>
{
    LLTaskInvFVBridge* new_bridge = NULL;
    // <FS:Trish> Hand the link to the bridge constructors, see sCreatingTaskID
    struct CreatingTask
    {
        CreatingTask(const LLUUID& id) { sCreatingTaskID = id.notNull() ? &id : nullptr; }
        ~CreatingTask() { sCreatingTaskID = nullptr; }
    } creating_task(task_id);
    // </FS:Trish>
    const LLInventoryItem* item = dynamic_cast<LLInventoryItem*>(object);
    const U32 itemflags = ( NULL == item ? 0 : item->getFlags() );
    LLAssetType::EType type = object ? object->getType() : LLAssetType::AT_CATEGORY;
    LLUUID object_id = object ? object->getUUID() : LLUUID::null;
    std::string object_name = object ? object->getName() : std::string();

    switch(type)
    {
    case LLAssetType::AT_TEXTURE:
        new_bridge = new LLTaskTextureBridge(panel,
                             object_id,
                             object_name);
        break;
    case LLAssetType::AT_SOUND:
        new_bridge = new LLTaskSoundBridge(panel,
                           object_id,
                           object_name);
        break;
    case LLAssetType::AT_LANDMARK:
        new_bridge = new LLTaskLandmarkBridge(panel,
                              object_id,
                              object_name);
        break;
    case LLAssetType::AT_CALLINGCARD:
        new_bridge = new LLTaskCallingCardBridge(panel,
                             object_id,
                             object_name);
        break;
    case LLAssetType::AT_SCRIPT:
        // OLD SCRIPTS DEPRECATED - JC
        LL_WARNS() << "Old script" << LL_ENDL;
        //new_bridge = new LLTaskOldScriptBridge(panel,
        //                                     object_id,
        //                                     object_name);
        break;
    case LLAssetType::AT_OBJECT:
        new_bridge = new LLTaskObjectBridge(panel,
                            object_id,
                            object_name,
                            itemflags);
        break;
    case LLAssetType::AT_NOTECARD:
        new_bridge = new LLTaskNotecardBridge(panel,
                              object_id,
                              object_name);
        break;
    case LLAssetType::AT_ANIMATION:
        new_bridge = new LLTaskAnimationBridge(panel,
                               object_id,
                               object_name);
        break;
    case LLAssetType::AT_GESTURE:
        new_bridge = new LLTaskGestureBridge(panel,
                             object_id,
                             object_name);
        break;
    case LLAssetType::AT_CLOTHING:
    case LLAssetType::AT_BODYPART:
        new_bridge = new LLTaskWearableBridge(panel,
                              object_id,
                              object_name,
                              itemflags);
        break;
    case LLAssetType::AT_CATEGORY:
        new_bridge = new LLTaskCategoryBridge(panel,
                              object_id,
                              object_name);
        break;
    case LLAssetType::AT_LSL_TEXT:
        new_bridge = new LLTaskLSLBridge(panel,
                         object_id,
                         object_name);
        break;
    case LLAssetType::AT_SETTINGS:
        new_bridge = new LLTaskSettingsBridge(panel,
                                          object_id,
                                          object_name,
                                          itemflags);
        break;
    case LLAssetType::AT_MATERIAL:
        new_bridge = new LLTaskMaterialBridge(panel,
                              object_id,
                              object_name);
        break;
    default:
        LL_INFOS() << "Unhandled inventory type (llassetstorage.h): "
                << (S32)type << LL_ENDL;
        break;
    }
    return new_bridge;
}


///----------------------------------------------------------------------------
/// Class LLPanelObjectInventory
///----------------------------------------------------------------------------

static LLDefaultChildRegistry::Register<LLPanelObjectInventory> r("panel_inventory_object");
std::set<LLPanelObjectInventory*> LLPanelObjectInventory::sInstances;

void do_nothing()
{
}

// Default constructor
LLPanelObjectInventory::LLPanelObjectInventory(const LLPanelObjectInventory::Params& p) :
    LLPanel(p),
    mScroller(NULL),
    mFolders(NULL),
    mHaveInventory(false),
    mIsInventoryEmpty(true),
    mInventoryNeedsUpdate(false),
    mInventoryViewModel(p.name),
    mShowRootFolder(p.show_root_folder)
{
    sInstances.insert(this);

    // Setup context menu callbacks
    mCommitCallbackRegistrar.add("Inventory.DoToSelected", boost::bind(&LLPanelObjectInventory::doToSelected, this, _2));
    mCommitCallbackRegistrar.add("Inventory.StopScript", boost::bind(&LLPanelObjectInventory::toggleScript, this));
    mCommitCallbackRegistrar.add("Inventory.StopScripts", boost::bind(&LLPanelObjectInventory::stopScripts, this));
    mCommitCallbackRegistrar.add("Inventory.FSTaskScripts", boost::bind(&LLPanelObjectInventory::doScriptsAction, this, _2)); // <FS:Trish/>
    mCommitCallbackRegistrar.add("Inventory.EmptyTrash", boost::bind(&LLInventoryModel::emptyFolderType, &gInventory, "ConfirmEmptyTrash", LLFolderType::FT_TRASH));
    mCommitCallbackRegistrar.add("Inventory.EmptyLostAndFound", boost::bind(&LLInventoryModel::emptyFolderType, &gInventory, "ConfirmEmptyLostAndFound", LLFolderType::FT_LOST_AND_FOUND));
    mCommitCallbackRegistrar.add("Inventory.DoCreate", boost::bind(&do_nothing));
    mCommitCallbackRegistrar.add("Inventory.AttachObject", boost::bind(&do_nothing));
    mCommitCallbackRegistrar.add("Inventory.BeginIMSession", boost::bind(&do_nothing));
    mCommitCallbackRegistrar.add("Inventory.Share",  boost::bind(&LLAvatarActions::shareWithAvatars, this));
    mCommitCallbackRegistrar.add("Inventory.FileUploadLocation", boost::bind(&do_nothing));
    mCommitCallbackRegistrar.add("Inventory.SetFavoritesFolder", boost::bind(&do_nothing)); // <FS:Ansariel> Prevent warning "No callback found for: 'Inventory.SetFavoritesFolder' in control: Set Favorites folder"
    mCommitCallbackRegistrar.add("Inventory.ResetFavoritesFolder", boost::bind(&do_nothing)); // <FS:Ansariel> Prevent warning "No callback found for: 'Inventory.ResetFavoritesFolder' in control: Reset Favorites folder"
    mCommitCallbackRegistrar.add("Inventory.CustomAction", boost::bind(&do_nothing)); // <FS:Ansariel> Prevent warning "No callback found for: 'Inventory.CustomAction' in control: Find Links"
}

// Destroys the object
LLPanelObjectInventory::~LLPanelObjectInventory()
{
    sInstances.erase(this);

    if (mFolders)
    {
        mFolders->cancelRenaming();
        if (LLEditMenuHandler::gEditMenuHandler == mFolders)
        {
            LLEditMenuHandler::gEditMenuHandler = NULL;
        }
    }
    if (!gIdleCallbacks.deleteFunction(idle, this))
    {
        LL_WARNS() << "LLPanelObjectInventory::~LLPanelObjectInventory() failed to delete callback" << LL_ENDL;
    }
}

bool LLPanelObjectInventory::postBuild()
{
    // clear contents and initialize menus, sets up mFolders
    reset();

    // Register an idle update callback
    gIdleCallbacks.addFunction(idle, this);

    return true;
}

// <FS:Trish> Script state is per link
//bool LLPanelObjectInventory::getScriptRunningState(const LLUUID& item_id, bool& running) const
//{
//auto it = mScriptRunningState.find(item_id);
bool LLPanelObjectInventory::getScriptRunningState(const LLUUID& task_id, const LLUUID& item_id, bool& running) const
{
    auto it = mScriptRunningState.find(makeItemKey(task_id, item_id));
// </FS:Trish>
    if (it == mScriptRunningState.end())
    {
        return false;
    }

    running = it->second;
    return true;
}

// <FS:Trish> Script state is per link
//void LLPanelObjectInventory::requestScriptRunningInfo(const LLUUID& item_id)
//{
void LLPanelObjectInventory::requestScriptRunningInfo(const LLUUID& task_id, const LLUUID& item_id)
{
    const LLUUID key = makeItemKey(task_id, item_id);
// </FS:Trish>
    if (item_id.isNull())
    {
        return;
    }

    // <FS:Trish> Script state is per link
    //if (mScriptRunningRequested.find(item_id) != mScriptRunningRequested.end() ||
    //mScriptRunningState.find(item_id) != mScriptRunningState.end())
    if (mScriptRunningRequested.find(key) != mScriptRunningRequested.end() ||
        mScriptRunningState.find(key) != mScriptRunningState.end())
    // </FS:Trish>
    {
        return;
    }

    // <FS:Trish> Script state is per link
    //LLViewerObject* object = gObjectList.findObject(mTaskUUID);
    LLViewerObject* object = gObjectList.findObject(task_id);
    // </FS:Trish>
    if (!object || object->isInventoryPending())
    {
        return;
    }

    LLViewerRegion* region = object->getRegion();
    if (!region)
    {
        return;
    }

    LLInventoryObject* inv_obj = object->getInventoryObject(item_id);
    if (!inv_obj || inv_obj->getType() != LLAssetType::AT_LSL_TEXT)
    {
        return;
    }

    LLMessageSystem* msg = gMessageSystem;
    msg->newMessageFast(_PREHASH_GetScriptRunning);
    msg->nextBlockFast(_PREHASH_Script);
    // <FS:Trish> Script state is per link
    //msg->addUUIDFast(_PREHASH_ObjectID, mTaskUUID);
    //msg->addUUIDFast(_PREHASH_ItemID, item_id);
    //msg->sendReliable(region->getHost());
    //
    //mScriptRunningRequested.insert(item_id);
    msg->addUUIDFast(_PREHASH_ObjectID, task_id);
    msg->addUUIDFast(_PREHASH_ItemID, item_id);
    msg->sendReliable(region->getHost());

    mScriptRunningRequested.insert(key);
    // </FS:Trish>
}

// <FS:Trish> Script state is per link
//void LLPanelObjectInventory::setScriptRunningState(const LLUUID& item_id, bool running)
//{
//auto existing = mScriptRunningState.find(item_id);
//const bool state_changed = existing == mScriptRunningState.end() || existing->second != running;
//mScriptRunningState[item_id] = running;
//mScriptRunningRequested.insert(item_id);
void LLPanelObjectInventory::setScriptRunningState(const LLUUID& task_id, const LLUUID& item_id, bool running)
{
    const LLUUID key = makeItemKey(task_id, item_id);
    auto existing = mScriptRunningState.find(key);
    const bool state_changed = existing == mScriptRunningState.end() || existing->second != running;
    mScriptRunningState[key] = running;
    mScriptRunningRequested.insert(key);
// </FS:Trish>

    // GetScriptRunning replies can be duplicated while task inventory is being
    // saved or rebuilt. Do not repeatedly invalidate layout for the same state.
    if (!state_changed)
    {
        return;
    }

    // <FS:Trish> Script state is per link
    //if (LLFolderViewItem* item = getItemByID(item_id))
    mLinksetSummaryDirty = true;
    if (LLFolderViewItem* item = getItemByID(key))
    // </FS:Trish>
    {
        // Only the style and "[stopped]" suffix changed. A full refresh also
        // dirties the inventory filter and can create a refresh/arrange loop
        // during task inventory mutation.
        item->refreshSuffix();
        if (mFolders)
        {
            mFolders->requestArrange();
        }
    }
}

// <FS:Trish> Also remembers which VM the script runs on, for the Content tab summary
//void LLPanelObjectInventory::handleScriptRunningReply(const LLUUID& object_id, const LLUUID& item_id, bool running)
void LLPanelObjectInventory::handleScriptRunningReply(const LLUUID& object_id, const LLUUID& item_id, bool running, S32 vm)
// </FS:Trish>
{
    for (LLPanelObjectInventory* panel : sInstances)
    {
        // <FS:Trish> Script state is per link
        //if (!panel || panel->mTaskUUID != object_id)
        if (!panel || !panel->isShowingTask(object_id))
        // </FS:Trish>
        {
            continue;
        }

        // <FS:Trish> Script state is per link
        //panel->setScriptRunningState(item_id, running);
        panel->setScriptRunningState(object_id, item_id, running);
        // </FS:Trish>
        // <FS:Trish>
        if (vm >= 0)
        {
            panel->mScriptVM[makeItemKey(object_id, item_id)] = vm; // per link
        }
        // </FS:Trish>
    }
}

// <FS:Trish> Script counts for the Content tab
// Adds the scripts of one object to the counts
static void add_script_summary(LLViewerObject* object,
                               const std::map<LLUUID, bool>& running_state,
                               const std::map<LLUUID, S32>& script_vm,
                               LLPanelObjectInventory::ScriptSummary& summary)
{
    if (!object || object->isInventoryPending())
    {
        return;
    }

    const LLUUID& task_id = object->getID();
    LLInventoryObject::object_list_t contents;
    object->getInventoryContents(contents);
    for (const LLPointer<LLInventoryObject>& obj : contents)
    {
        if (!obj || obj->getType() != LLAssetType::AT_LSL_TEXT)
        {
            continue;
        }
        const LLUUID key = LLPanelObjectInventory::makeItemKey(task_id, obj->getUUID());
        ++summary.mTotal;

        auto state = running_state.find(key);
        if (state != running_state.end())
        {
            ++(state->second ? summary.mRunning : summary.mStopped);
        }

        auto vm = script_vm.find(key);
        if (vm != script_vm.end())
        {
            switch (vm->second)
            {
                case 0: ++summary.mLSL2; break;
                case 1: ++summary.mMono; break;
                case 2: ++summary.mLua; break;
                default: break;
            }
        }
    }
}

LLPanelObjectInventory::ScriptSummary LLPanelObjectInventory::getScriptSummary() const
{
    if (!mLinksetActive)
    {
        ScriptSummary summary;
        add_script_summary(gObjectList.findObject(mTaskUUID), mScriptRunningState, mScriptVM, summary);
        return summary;
    }

    // Whole linkset: counted again only after a link's contents or a script's state changed
    if (mLinksetSummaryDirty)
    {
        mLinksetSummary = ScriptSummary();
        for (const LLUUID& link_id : mLinkOrder)
        {
            auto link = mLinkFolders.find(link_id);
            if (link != mLinkFolders.end() && link->second.mCount > 0)
            {
                add_script_summary(gObjectList.findObject(link_id), mScriptRunningState, mScriptVM, mLinksetSummary);
            }
        }
        mLinksetSummaryDirty = false;
    }
    return mLinksetSummary;
}
// </FS:Trish>

void LLPanelObjectInventory::doToSelected(const LLSD& userdata)
{
    std::string action = userdata.asString();
    // <FS:Trish> Whole linkset view
    //if ("rename" == action || "delete" == action)
    // In the whole linkset view each item checks its own link (removeBatch, isItemRenameable)
    if (!mLinksetActive && ("rename" == action || "delete" == action))
    // </FS:Trish>
    {
        LLViewerObject* objectp = gObjectList.findObject(mTaskUUID);
        if (objectp && !objectp->permModify())
        {
            LLNotificationsUtil::add("CantModifyContentInNoModTask");
        }
        else
        {
            LLInventoryAction::doToSelected(&gInventory, mFolders, action);
        }
    }
    else
    {
        LLInventoryAction::doToSelected(&gInventory, mFolders, action);
    }
}

// <FS:Trish> Any link of the linkset
//bool LLPanelObjectInventory::canStopScripts() const
//{
//LLViewerObject* object = gObjectList.findObject(mTaskUUID);
bool LLPanelObjectInventory::canStopScripts() const
{
    return canStopScripts(mTaskUUID);
}

bool LLPanelObjectInventory::canStopScripts(const LLUUID& task_id) const
{
    LLViewerObject* object = gObjectList.findObject(task_id);
// </FS:Trish>
    if (!object || object->isInventoryPending() || !object->permModify())
    {
        return false;
    }

    LLInventoryObject::object_list_t contents;
    object->getInventoryContents(contents);
    for (const LLPointer<LLInventoryObject>& inventory : contents)
    {
        if (inventory && inventory->getType() == LLAssetType::AT_LSL_TEXT)
        {
            return true;
        }
    }

    return false;
}

void LLPanelObjectInventory::stopScripts()
{
    if (!canStopScripts())
    {
        return;
    }

    LLViewerObject* object = gObjectList.findObject(mTaskUUID);
    LLViewerRegion* region = object ? object->getRegion() : nullptr;
    if (!object || !region)
    {
        return;
    }

    LLInventoryObject::object_list_t contents;
    object->getInventoryContents(contents);
    for (const LLPointer<LLInventoryObject>& inventory : contents)
    {
        if (!inventory || inventory->getType() != LLAssetType::AT_LSL_TEXT)
        {
            continue;
        }

        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_SetScriptRunning);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->nextBlockFast(_PREHASH_Script);
        msg->addUUIDFast(_PREHASH_ObjectID, mTaskUUID);
        msg->addUUIDFast(_PREHASH_ItemID, inventory->getUUID());
        msg->addBOOLFast(_PREHASH_Running, false);
        msg->sendReliable(region->getHost());

        // <FS:Trish> Script state is per link
        //setScriptRunningState(inventory->getUUID(), false);
        setScriptRunningState(mTaskUUID, inventory->getUUID(), false);
        // </FS:Trish>
    }
}

void LLPanelObjectInventory::toggleScript()
{
    // <FS:Trish> Whole linkset view: the clicked script decides, every selected script follows in its own link
    if (mLinksetActive)
    {
        LLFolderViewItem* current = mFolders ? mFolders->getCurSelectedItem() : nullptr;
        LLTaskInvFVBridge* bridge = current ? dynamic_cast<LLTaskInvFVBridge*>(current->getViewModelItem()) : nullptr;
        if (!bridge)
        {
            return;
        }
        bool is_running = true;
        if (!getScriptRunningState(bridge->getBridgeTaskID(), bridge->getUUID(), is_running))
        {
            requestScriptRunningInfo(bridge->getBridgeTaskID(), bridge->getUUID());
        }
        doScriptsAction(LLSD(is_running ? "stop" : "run"));
        return;
    }
    // </FS:Trish>
    if (!canStopScripts() || !mFolders)
    {
        return;
    }

    LLFolderViewItem* selected_item = mFolders->getCurSelectedItem();
    if (!selected_item)
    {
        return;
    }

    const LLFolderViewModelItemInventory* selected_inventory =
        dynamic_cast<const LLFolderViewModelItemInventory*>(selected_item->getViewModelItem());
    if (!selected_inventory)
    {
        return;
    }

    LLViewerObject* object = gObjectList.findObject(mTaskUUID);
    LLViewerRegion* region = object ? object->getRegion() : nullptr;
    LLInventoryObject* inventory = object ? object->getInventoryObject(selected_inventory->getUUID()) : nullptr;
    if (!object || !region || !inventory || inventory->getType() != LLAssetType::AT_LSL_TEXT)
    {
        return;
    }

    bool is_running = true;
    // <FS:Trish> Script state is per link
    //if (!getScriptRunningState(inventory->getUUID(), is_running))
    //{
    //requestScriptRunningInfo(inventory->getUUID());
    //}
    if (!getScriptRunningState(mTaskUUID, inventory->getUUID(), is_running))
    {
        requestScriptRunningInfo(mTaskUUID, inventory->getUUID());
    }
    // </FS:Trish>
    const bool new_running_state = !is_running;

    // The context-menu label and action are based on the current item, but
    // users expect that action to apply to every selected script in the task.
    for (LLFolderViewItem* item : mFolders->getSelectionList())
    {
        const LLFolderViewModelItemInventory* selected_inventory = item ?
            dynamic_cast<const LLFolderViewModelItemInventory*>(item->getViewModelItem()) : nullptr;
        LLInventoryObject* selected_script = selected_inventory ?
            object->getInventoryObject(selected_inventory->getUUID()) : nullptr;
        if (!selected_script || selected_script->getType() != LLAssetType::AT_LSL_TEXT)
        {
            continue;
        }

        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_SetScriptRunning);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->nextBlockFast(_PREHASH_Script);
        msg->addUUIDFast(_PREHASH_ObjectID, mTaskUUID);
        msg->addUUIDFast(_PREHASH_ItemID, selected_script->getUUID());
        msg->addBOOLFast(_PREHASH_Running, new_running_state);
        msg->sendReliable(region->getHost());

        // <FS:Trish> Script state is per link
        //setScriptRunningState(selected_script->getUUID(), new_running_state);
        setScriptRunningState(mTaskUUID, selected_script->getUUID(), new_running_state);
        // </FS:Trish>
    }
}

void LLPanelObjectInventory::clearContents()
{
    mHaveInventory = false;
    mIsInventoryEmpty = true;
    mScriptRunningState.clear();
    mScriptRunningRequested.clear();
    if (LLToolDragAndDrop::getInstance() && LLToolDragAndDrop::getInstance()->getSource() == LLToolDragAndDrop::SOURCE_WORLD)
    {
        LLToolDragAndDrop::getInstance()->endDrag();
    }

    clearItemIDs();
    // <FS:Trish> Link folders go with the tree
    mLinksetSummaryDirty = true;
    for (auto& link : mLinkFolders)
    {
        link.second.mFolder = nullptr;
        link.second.mKeys.clear();
    }
    // </FS:Trish>

    if (mFolders)
    {
        mFolders->cancelRenaming();
        if (LLEditMenuHandler::gEditMenuHandler == mFolders)
        {
            LLEditMenuHandler::gEditMenuHandler = NULL;
        }
    }

    if (mFolders)
    {
        mFolders->cancelRenaming();
        if (LLEditMenuHandler::gEditMenuHandler == mFolders)
        {
            LLEditMenuHandler::gEditMenuHandler = NULL;
        }
    }

    if( mScroller )
    {
        // removes mFolders
        removeChild( mScroller ); //*TODO: Really shouldn't do this during draw()/refresh()
        mScroller->die();
        mScroller = NULL;
        mFolders = NULL;
    }
}

void LLPanelObjectInventory::reset()
{
    clearContents();

    mCommitCallbackRegistrar.pushScope(); // push local callbacks

    // Reset the inventory model to show all folders by default
    mInventoryViewModel.getFilter().setShowFolderState(LLInventoryFilter::SHOW_ALL_FOLDERS);

    // Create a new folder view root
    LLRect dummy_rect(0, 1, 1, 0);
    LLFolderView::Params p;
    p.name = "task inventory";
    p.title = "task inventory";
    p.parent_panel = this;
    p.tool_tip= LLTrans::getString("PanelContentsTooltip");
    p.listener = LLTaskInvFVBridge::createObjectBridge(this, NULL);
    p.folder_indentation = -14; // subtract space normally reserved for folder expanders
    p.view_model = &mInventoryViewModel;
    p.root = NULL;
    p.options_menu = "menu_inventory.xml";
    // Enable label styling and suffixes (used for stopped scripts).
    p.use_label_suffix = true;

    // <FS:Ansariel> Inventory specials
    p.for_inventory = true;

    static LLCachedControl<S32> fsFolderViewItemHeight(gSavedSettings, "FSFolderViewItemHeight");
    const LLFolderViewItem::Params& default_params = LLUICtrlFactory::getDefaultParams<LLFolderViewItem>();
    p.item_height = fsFolderViewItemHeight;
    p.item_top_pad = default_params.item_top_pad - (default_params.item_height - fsFolderViewItemHeight) / 2 - 1;
    // </FS:Ansariel>

    mFolders = LLUICtrlFactory::create<LLFolderView>(p);

    mFolders->setCallbackRegistrar(&mCommitCallbackRegistrar);
    mFolders->setEnableRegistrar(&mEnableCallbackRegistrar);

    if (hasFocus())
    {
        LLEditMenuHandler::gEditMenuHandler = mFolders;
    }

    int offset = hasBorder() ? getBorder()->getBorderWidth() << 1 : 0;
    LLRect scroller_rect(0, getRect().getHeight() - offset, getRect().getWidth() - offset, 0);
    LLScrollContainer::Params scroll_p;
    scroll_p.name("task inventory scroller");
    scroll_p.rect(scroller_rect);
    scroll_p.tab_stop(true);
    scroll_p.follows.flags(FOLLOWS_ALL);
    mScroller = LLUICtrlFactory::create<LLFolderViewScrollContainer>(scroll_p);
    addChild(mScroller);
    mScroller->addChild(mFolders);

    mFolders->setScrollContainer( mScroller );

    mCommitCallbackRegistrar.popScope();
}

void LLPanelObjectInventory::inventoryChanged(LLViewerObject* object,
                                        LLInventoryObject::object_list_t* inventory,
                                        S32 serial_num,
                                        void* data)
{
    if (!object)
        return;

    //LL_INFOS() << "invetnory arrived: \n"
    //      << " panel UUID: " << panel->mTaskUUID << "\n"
    //      << " task  UUID: " << object->mID << LL_ENDL;
    if (mTaskUUID == object->mID)
    {
        mInventoryNeedsUpdate = true;
    }

    // <FS:Ansariel> Keep legacy properties floater
    // refresh any properties floaters that are hanging around.
    if(inventory)
    {
        for (LLInventoryObject::object_list_t::const_iterator iter = inventory->begin();
             iter != inventory->end(); )
        {
            LLInventoryObject* item = *iter++;
            LLFloaterProperties* floater = LLFloaterReg::findTypedInstance<LLFloaterProperties>("properties", item->getUUID());
            if(floater)
            {
                floater->refresh();
            }
        }
    }
    // </FS:Ansariel>
}

void LLPanelObjectInventory::updateInventory()
{
    //LL_INFOS() << "inventory arrived: \n"
    //      << " panel UUID: " << panel->mTaskUUID << "\n"
    //      << " task  UUID: " << object->mID << LL_ENDL;
    // We're still interested in this task's inventory.
    bool inventory_has_focus = mHaveInventory && mFolders && gFocusMgr.childHasKeyboardFocus(mFolders);

    std::vector<LLUUID> selected_item_ids;
    if (mHaveInventory && mFolders)
    {
        std::set<LLFolderViewItem*> selected_items = mFolders->getSelectionList();
        for (LLFolderViewItem* item : selected_items)
        {
            // <FS:Trish> Item keys are per link
            //selected_item_ids.push_back(static_cast<LLFolderViewModelItemInventory*>(item->getViewModelItem())->getUUID());
            selected_item_ids.push_back(makeItemKey(mTaskUUID, static_cast<LLFolderViewModelItemInventory*>(item->getViewModelItem())->getUUID()));
            // </FS:Trish>
        }
    }

    if (LLViewerObject* objectp = gObjectList.findObject(mTaskUUID))
    {
        LLInventoryObject* inventory_root = objectp->getInventoryRoot();
        LLInventoryObject::object_list_t contents;
        objectp->getInventoryContents(contents);

        if (inventory_root)
        {
            reset();
            mIsInventoryEmpty = false;
            createFolderViews(inventory_root, contents);
            mFolders->setEnabled(true);
        }
        else
        {
            // TODO: create an empty inventory
            mIsInventoryEmpty = true;
        }

        mHaveInventory = !mIsInventoryEmpty || !objectp->isInventoryDirty();
        if (objectp->isInventoryDirty())
        {
            // Inventory is dirty, yet we received inventoryChanged() callback.
            // User changed something during ongoing request.
            // Rerequest. It will clear dirty flag and won't create dupplicate requests.
            objectp->requestInventory();
        }
    }
    else
    {
        // TODO: create an empty inventory
        mIsInventoryEmpty = true;
        mHaveInventory = true;
    }

    // restore previous selection
    bool first_item = true;
    for (const LLUUID& item_id : selected_item_ids)
    {
        if (LLFolderViewItem* selected_item = getItemByID(item_id))
        {
            // HACK: "set" first item then "change" each other one to get keyboard focus right
            if (first_item)
            {
                mFolders->setSelection(selected_item, true, inventory_has_focus);
                first_item = false;
            }
            else
            {
                mFolders->changeSelection(selected_item, true);
            }
        }
    }

    if (mFolders)
    {
        mFolders->requestArrange();
    }

    mInventoryNeedsUpdate = false;
    // Edit menu handler is set in onFocusReceived
}

// *FIX: This is currently a very expensive operation, because we have
// to iterate through the inventory one time for each category. This
// leads to an N^2 based on the category count. This could be greatly
// speeded with an efficient multimap implementation, but we don't
// have that in our current arsenal.
void LLPanelObjectInventory::createFolderViews(LLInventoryObject* inventory_root, LLInventoryObject::object_list_t& contents)
{
    if (!inventory_root)
    {
        return;
    }

    // Create a visible root category.
    if (LLTaskInvFVBridge* bridge = LLTaskInvFVBridge::createObjectBridge(this, inventory_root))
    {
        LLUIColor item_color = LLUIColorTable::instance().getColor("MenuItemEnabledColor", DEFAULT_WHITE);

        LLFolderViewFolder::Params p;
        p.name = inventory_root->getName();
        p.tool_tip = p.name;
        p.root = mFolders;
        p.listener = bridge;
        p.font_color = item_color;
        p.font_highlight_color = item_color;

        // <FS:Ansariel> Inventory specials
        p.for_inventory = true;

        static LLCachedControl<S32> fsFolderViewItemHeight(gSavedSettings, "FSFolderViewItemHeight");
        const LLFolderViewItem::Params& default_params = LLUICtrlFactory::getDefaultParams<LLFolderViewItem>();
        p.item_height = fsFolderViewItemHeight;
        p.item_top_pad = default_params.item_top_pad - (default_params.item_height - fsFolderViewItemHeight) / 2 - 1;
        // </FS:Ansariel>

        LLFolderViewFolder* new_folder = LLUICtrlFactory::create<LLFolderViewFolder>(p);

        if (mShowRootFolder)
        {
            new_folder->addToFolder(mFolders);
            new_folder->toggleOpen();
        }

        if (!contents.empty())
        {
            // <FS:Trish> Views are built for one link at a time
            //createViewsForCategory(&contents, inventory_root, mShowRootFolder ? new_folder : mFolders);
            createViewsForCategory(&contents, inventory_root, mShowRootFolder ? new_folder : mFolders, mTaskUUID);
            // </FS:Trish>
        }

        if (mShowRootFolder)
        {
            // Refresh for label to add item count
            new_folder->refresh();
        }
    }
}

typedef std::pair<LLInventoryObject*, LLFolderViewFolder*> obj_folder_pair;

// <FS:Trish> Views are built for one link at a time
//void LLPanelObjectInventory::createViewsForCategory(LLInventoryObject::object_list_t* inventory,
//LLInventoryObject* parent,
//LLFolderViewFolder* folder)
void LLPanelObjectInventory::createViewsForCategory(LLInventoryObject::object_list_t* inventory,
                                              LLInventoryObject* parent,
                                              LLFolderViewFolder* folder,
                                              const LLUUID& task_id,
                                              std::vector<LLUUID>* keys)
// </FS:Trish>
{
    LLUIColor item_color = LLUIColorTable::instance().getColor("MenuItemEnabledColor", DEFAULT_WHITE);

    // Find all in the first pass
    std::vector<obj_folder_pair*> child_categories;
    for (const LLPointer<LLInventoryObject>& obj : *inventory)
    {
        if (parent->getUUID() == obj->getParentUUID())
        {
            // <FS:Trish> Bridges belong to one link
            //if (LLTaskInvFVBridge* bridge = LLTaskInvFVBridge::createObjectBridge(this, obj))
            if (LLTaskInvFVBridge* bridge = LLTaskInvFVBridge::createObjectBridge(this, obj, task_id))
            // </FS:Trish>
            {
                LLFolderViewItem* view;
                if (LLAssetType::AT_CATEGORY == obj->getType())
                {
                    LLFolderViewFolder::Params params;
                    params.name = obj->getName();
                    params.root = mFolders;
                    params.listener = bridge;
                    params.font_color = item_color;
                    params.font_highlight_color = item_color;

                    // <FS:Ansariel> Inventory specials
                    params.for_inventory = true;

                    static LLCachedControl<S32> fsFolderViewItemHeight(gSavedSettings, "FSFolderViewItemHeight");
                    const LLFolderViewItem::Params& default_params = LLUICtrlFactory::getDefaultParams<LLFolderViewItem>();
                    params.item_height = fsFolderViewItemHeight;
                    params.item_top_pad = default_params.item_top_pad - (default_params.item_height - fsFolderViewItemHeight) / 2 - 1;
                    // </FS:Ansariel>

                    view = LLUICtrlFactory::create<LLFolderViewFolder>(params);
                    child_categories.push_back(new obj_folder_pair(obj, (LLFolderViewFolder*)view));
                }
                else
                {
                    LLFolderViewItem::Params params;
                    params.name = obj->getName();
                    params.root = mFolders;
                    params.listener = bridge;
                    params.creation_date = bridge->getCreationDate();
                    params.rect = LLRect();
                    params.font_color = item_color;
                    params.font_highlight_color = item_color;

                    // <FS:Ansariel> Inventory specials
                    params.for_inventory = true;

                    static LLCachedControl<S32> fsFolderViewItemHeight(gSavedSettings, "FSFolderViewItemHeight");
                    const LLFolderViewItem::Params& default_params = LLUICtrlFactory::getDefaultParams<LLFolderViewItem>();
                    params.item_height = fsFolderViewItemHeight;
                    params.item_top_pad = default_params.item_top_pad - (default_params.item_height - fsFolderViewItemHeight) / 2 - 1;
                    // </FS:Ansariel>

                    view = LLUICtrlFactory::create<LLFolderViewItem>(params);
                }

                view->addToFolder(folder);
                // <FS:Trish> Item keys are per link
                //addItemID(obj->getUUID(), view);
                //
                //if (obj->getType() == LLAssetType::AT_LSL_TEXT)
                //{
                //requestScriptRunningInfo(obj->getUUID());
                //}
                const LLUUID key = makeItemKey(task_id, obj->getUUID());
                addItemID(key, view);
                if (keys)
                {
                    keys->push_back(key);
                }

                if (obj->getType() == LLAssetType::AT_LSL_TEXT)
                {
                    requestScriptRunningInfo(task_id, obj->getUUID());
                }
                // </FS:Trish>
            }
        }
    }

    // now, for each category, do the second pass
    for (obj_folder_pair* pair : child_categories)
    {
        // <FS:Trish> Views are built for one link at a time
        //createViewsForCategory(inventory, pair->first, pair->second);
        createViewsForCategory(inventory, pair->first, pair->second, task_id, keys);
        // </FS:Trish>
        delete pair;
    }
    folder->setChildrenInited(true);
}

void LLPanelObjectInventory::refresh()
{
    //LL_INFOS() << "LLPanelObjectInventory::refresh()" << LL_ENDL;
    // <FS:Trish> Whole linkset view, for a single selected linkset of more than one prim
    if (mLinksetWanted)
    {
        LLObjectSelectionHandle selection = LLSelectMgr::getInstance()->getSelection();
        LLSelectNode* node = selection->getFirstRootNode(NULL, true);
        LLViewerObject* object = (node && node->mValid) ? node->getObject() : nullptr;
        LLViewerObject* root = object ? object->getRootEdit() : nullptr;
        bool has_links = false;
        if (root && !root->isDead() && !root->isAvatar()
            && ((selection->getRootObjectCount() == 1) || (selection->getObjectCount() == 1)))
        {
            for (const LLPointer<LLViewerObject>& child : root->getChildren())
            {
                if (child && !child->isAvatar())
                {
                    has_links = true;
                    break;
                }
            }
        }
        if (has_links)
        {
            refreshLinkset(root);
            mInventoryViewModel.setTaskID(mTaskUUID);
            return;
        }
        if (mLinksetActive)
        {
            leaveLinkset();
        }
    }
    // </FS:Trish>
    bool has_inventory = false;
    const bool non_root_ok = true;
    LLObjectSelectionHandle selection = LLSelectMgr::getInstance()->getSelection();
    LLSelectNode* node = selection->getFirstRootNode(NULL, non_root_ok);
    if (node && node->mValid)
    {
        LLViewerObject* object = node->getObject();
        if (object && ((selection->getRootObjectCount() == 1) || (selection->getObjectCount() == 1)))
        {
            // determine if we need to make a request. Start with a
            // default based on if we have inventory at all.
            bool make_request = !mHaveInventory;

            // If the task id is different than what we've stored,
            // then make the request.
            if (mTaskUUID != object->mID)
            {
                mTaskUUID = object->mID;
                mAttachmentUUID = object->getAttachmentItemID();
                make_request = true;

                // This is a new object so pre-emptively clear the contents
                // Otherwise we show the old stuff until the update comes in
                clearContents();

                // Register for updates from this object,
                registerVOInventoryListener(object,NULL);
            }
            else if (mAttachmentUUID != object->getAttachmentItemID())
            {
                mAttachmentUUID = object->getAttachmentItemID();
                if (mAttachmentUUID.notNull())
                {
                    // Server unsubsribes viewer (deselects object) from property
                    // updates after "ObjectAttach" so we need to resubscribe
                    LLSelectMgr::getInstance()->sendSelect();
                }
            }

            // Based on the node information, we may need to dirty the
            // object inventory and get it again.
            if (node->mValid)
            {
                if (node->mInventorySerial != object->getInventorySerial() || object->isInventoryDirty())
                {
                    make_request = true;
                }
            }

            // do the request if necessary.
            if (make_request)
            {
                requestVOInventory();
            }
            has_inventory = true;
        }
    }

    if (!has_inventory)
    {
        clearInventoryTask();
    }

    mInventoryViewModel.setTaskID(mTaskUUID);
    //LL_INFOS() << "LLPanelObjectInventory::refresh() " << mTaskUUID << LL_ENDL;
}

void LLPanelObjectInventory::clearInventoryTask()
{
    // <FS:Trish> Whole linkset view
    mLinksetActive = false;
    mLinksetLoader.reset();
    mLinkFolders.clear();
    mLinkOrder.clear();
    mLinksetRootID.setNull();
    // </FS:Trish>
    mTaskUUID = LLUUID::null;
    mAttachmentUUID = LLUUID::null;
    removeVOInventoryListener();
    clearContents();
}

void LLPanelObjectInventory::removeSelectedItem()
{
    if (mFolders)
    {
        mFolders->removeSelectedItems();
    }
}

void LLPanelObjectInventory::startRenamingSelectedItem()
{
    if (mFolders)
    {
        mFolders->startRenamingSelectedItem();
    }
}

void LLPanelObjectInventory::draw()
{
    LLPanel::draw();

    if (mIsInventoryEmpty)
    {
        std::string text;
        if (!mHaveInventory && mTaskUUID.notNull())
        {
            text = LLTrans::getString("LoadingContents");
        }
        else if (mHaveInventory)
        {
            text = LLTrans::getString("NoContents");
        }

        if (!text.empty())
        {
            LLFontGL::getFontSansSerif()->renderUTF8(text, 0,
                (S32)(getRect().getWidth() * 0.5f),
                10,
                LLColor4(1, 1, 1, 1),
                LLFontGL::HCENTER,
                LLFontGL::BOTTOM);
        }
    }
}

void LLPanelObjectInventory::deleteAllChildren()
{
    if (mFolders)
    {
        mFolders->cancelRenaming();
        if (LLEditMenuHandler::gEditMenuHandler == mFolders)
        {
            LLEditMenuHandler::gEditMenuHandler = NULL;
        }
    }
    mScroller = NULL;
    mFolders = NULL;
    // <FS:Trish> Link folders go with the tree
    for (auto& link : mLinkFolders)
    {
        link.second.mFolder = nullptr;
        link.second.mKeys.clear();
    }
    clearItemIDs();
    // </FS:Trish>
    LLView::deleteAllChildren();
}

bool LLPanelObjectInventory::handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop, EDragAndDropType cargo_type, void *cargo_data, EAcceptance *accept, std::string& tooltip_msg)
{
    LLFolderViewItem* folderp = mFolders ? mFolders->getNextFromChild(NULL) : NULL;
    if (!folderp)
        return false;

    // Try to pass on unmodified mouse coordinates
    S32 local_x = x - mFolders->getRect().mLeft;
    S32 local_y = y - mFolders->getRect().mBottom;

    if (mFolders->pointInView(local_x, local_y))
    {
        return mFolders->handleDragAndDrop(local_x, local_y, mask, drop, cargo_type, cargo_data, accept, tooltip_msg);
    }

    //force mouse coordinates to be inside folder rectangle
    return mFolders->handleDragAndDrop(5, 1, mask, drop, cargo_type, cargo_data, accept, tooltip_msg);
}

//static
void LLPanelObjectInventory::idle(void* user_data)
{
    LLPanelObjectInventory* self = (LLPanelObjectInventory*)user_data;

    if (self->mFolders)
    {
        self->mFolders->update();
    }
    if (self->mInventoryNeedsUpdate)
    {
        self->updateInventory();
    }
    // <FS:Trish> Whole linkset view: start queued requests, fill links that arrived.
    // Only while the Content tab is shown: a hidden tab doesn't need 255 requests.
    if (self->mLinksetActive && self->isInVisibleChain())
    {
        self->updateLinkset();
    }
    // </FS:Trish>
}

void LLPanelObjectInventory::onFocusLost()
{
    // inventory no longer handles cut/copy/paste/delete
    if (LLEditMenuHandler::gEditMenuHandler == mFolders)
    {
        LLEditMenuHandler::gEditMenuHandler = NULL;
    }

    LLPanel::onFocusLost();
}

void LLPanelObjectInventory::onFocusReceived()
{
    // inventory now handles cut/copy/paste/delete
    LLEditMenuHandler::gEditMenuHandler = mFolders;

    LLPanel::onFocusReceived();
}

LLFolderViewItem* LLPanelObjectInventory::getItemByID( const LLUUID& id )
{
    auto map_it = mItemMap.find(id);
    if (map_it != mItemMap.end())
    {
        return map_it->second;
    }

    return NULL;
}

void LLPanelObjectInventory::removeItemID( const LLUUID& id )
{
    mItemMap.erase(id);
}

void LLPanelObjectInventory::addItemID( const LLUUID& id, LLFolderViewItem* itemp )
{
    mItemMap[id] = itemp;
}

void LLPanelObjectInventory::clearItemIDs()
{
    mItemMap.clear();
}

bool LLPanelObjectInventory::handleKeyHere( KEY key, MASK mask )
{
    bool handled = false;
    switch (key)
    {
// <FS:Ansariel> Fix broken return key in task inventory
    case KEY_RETURN:
        if (mask == MASK_NONE)
        {
            LLPanelObjectInventory::doToSelected(LLSD("task_open"));
            handled = true;
        }
        break;
// </FS:Ansariel> Fix broken return key in task inventory
    case KEY_DELETE:
#if LL_DARWIN
    case KEY_BACKSPACE:
#endif
        // Delete selected items if delete or backspace key hit on the inventory panel
        // Note: on Mac laptop keyboards, backspace and delete are one and the same
        if (isSelectionRemovable() && mask == MASK_NONE)
        {
            LLInventoryAction::doToSelected(&gInventory, mFolders, "delete");
            handled = true;
        }
        break;
    }
    return handled;
}

bool LLPanelObjectInventory::isSelectionRemovable()
{
    if (!mFolders || !mFolders->getRoot())
    {
        return false;
    }

    std::set<LLFolderViewItem*> selection_set = mFolders->getRoot()->getSelectionList();
    if (selection_set.empty())
    {
        return false;
    }

    for (LLFolderViewItem* item : selection_set)
    {
        const LLFolderViewModelItemInventory *listener = dynamic_cast<const LLFolderViewModelItemInventory*>(item->getViewModelItem());
        if (!listener || !listener->isItemRemovable() || listener->isItemInTrash())
        {
            return false;
        }
    }

    return true;
}

// <FS:Trish> Whole linkset view: one folder per link, each item acting on its own link

// static
LLUUID LLPanelObjectInventory::makeItemKey(const LLUUID& task_id, const LLUUID& item_id)
{
    // Item ids are only unique within one object; mixing in the link keeps
    // the per-item maps apart when two links hold an item with the same id.
    LLUUID key = item_id;
    for (S32 i = 0; i < UUID_BYTES; ++i)
    {
        key.mData[i] ^= task_id.mData[i];
    }
    return key;
}

bool LLPanelObjectInventory::isShowingTask(const LLUUID& task_id) const
{
    if (task_id.isNull())
    {
        return false;
    }
    if (task_id == mTaskUUID)
    {
        return true;
    }
    return mLinksetActive && mLinkFolders.count(task_id) != 0;
}

S32 LLPanelObjectInventory::getLinksetPendingCount() const
{
    return (mLinksetActive && mLinksetLoader) ? mLinksetLoader->getPendingCount() : 0;
}

const LLUUID& LLPanelObjectInventory::getSelectionTaskID()
{
    // Asked once per dragged item; work it out once per frame
    const U32 frame = LLFrameTimer::getFrameCount();
    if (frame != mSelectionTaskFrame)
    {
        mSelectionTaskFrame = frame;
        mSelectionTaskID.setNull();
        if (mFolders)
        {
            bool first = true;
            for (LLFolderViewItem* item : mFolders->getSelectionList())
            {
                LLTaskInvFVBridge* bridge = item ? dynamic_cast<LLTaskInvFVBridge*>(item->getViewModelItem()) : nullptr;
                if (!bridge)
                {
                    continue;
                }
                if (first)
                {
                    mSelectionTaskID = bridge->getBridgeTaskID();
                    first = false;
                }
                else if (bridge->getBridgeTaskID() != mSelectionTaskID)
                {
                    mSelectionTaskID.setNull();
                    break;
                }
            }
        }
    }
    return mSelectionTaskID;
}

void LLPanelObjectInventory::setLinksetMode(bool enabled)
{
    if (mLinksetWanted == enabled)
    {
        return;
    }
    mLinksetWanted = enabled;
    if (!enabled && mLinksetActive)
    {
        // Back to one object; the next refresh() requests its contents again
        leaveLinkset();
    }
}

void LLPanelObjectInventory::leaveLinkset()
{
    mLinksetActive = false;
    mLinksetLoader.reset();
    mLinkFolders.clear();
    mLinkOrder.clear();
    mLinksetRootID.setNull();
    mTaskUUID.setNull();
    mAttachmentUUID.setNull();
    clearContents();
}

std::string LLPanelObjectInventory::getLinkLabel(const LLUUID& link_id) const
{
    auto it = mLinkFolders.find(link_id);
    if (it == mLinkFolders.end())
    {
        return std::string();
    }
    const LinkFolder& link = it->second;
    LLStringUtil::format_map_t args;
    args["[LINK]"] = llformat("%d", link.mLinkNum);
    args["[NAME]"] = link.mName.empty() ? LLTrans::getString("FSContentsLinkUnnamed") : link.mName;
    args["[COUNT]"] = llformat("%d", link.mCount);
    return LLTrans::getString("FSContentsLinkFolder", args);
}

// Puts a new label on a link folder and lets the tree sort it again
static void relabel_link_folder(LLFolderView* root, LLFolderViewFolder* folder, const std::string& label)
{
    if (!root || !folder)
    {
        return;
    }
    if (LLTaskLinkBridge* bridge = dynamic_cast<LLTaskLinkBridge*>(folder->getViewModelItem()))
    {
        bridge->setLabel(label);
        bridge->dirtyFilter();
        folder->refresh();
        root->getViewModelItem()->requestSort();
        root->requestArrange();
    }
}

void LLPanelObjectInventory::refreshLinkset(LLViewerObject* root)
{
    if (!mLinksetActive || mLinksetRootID != root->getID() || !mFolders)
    {
        // A new linkset: start over with an empty tree
        removeVOInventoryListener();
        mLinksetLoader.reset();
        mLinkFolders.clear();
        mLinkOrder.clear();
        reset();
        mLinksetLoader = std::make_unique<FSLinksetInventoryLoader>();
        mLinksetActive = true;
        mLinksetRootID = root->getID();
        mTaskUUID = root->getID();
        mAttachmentUUID = root->getAttachmentItemID();
        mHaveInventory = false;
        mIsInventoryEmpty = true;
        // A single-object update still waiting would rebuild the tree as one object
        mInventoryNeedsUpdate = false;
    }
    else if (mAttachmentUUID != root->getAttachmentItemID())
    {
        mAttachmentUUID = root->getAttachmentItemID();
        if (mAttachmentUUID.notNull())
        {
            // Server unsubscribes viewer (deselects object) from property
            // updates after "ObjectAttach" so we need to resubscribe
            LLSelectMgr::getInstance()->sendSelect();
        }
    }

    // Root first, then the child prims in link order. Seated avatars are children too, but not links.
    const LLViewerObject::child_list_t& children = root->getChildren();
    std::vector<LLViewerObject*> links;
    links.reserve(children.size() + 1);
    links.push_back(root);
    for (const LLPointer<LLViewerObject>& child : children)
    {
        if (child && !child->isAvatar() && !child->isDead())
        {
            links.push_back(child.get());
        }
    }
    syncLinks(links);

    // Names come with the object properties, a contents change with a new inventory serial.
    // One pass over the selection, one over the links.
    std::unordered_map<LLUUID, LLSelectNode*> nodes;
    LLObjectSelectionHandle selection = LLSelectMgr::getInstance()->getSelection();
    for (LLObjectSelection::iterator it = selection->begin(); it != selection->end(); ++it)
    {
        LLSelectNode* node = *it;
        if (node && node->mValid && node->getObject())
        {
            nodes[node->getObject()->getID()] = node;
        }
    }

    // A few name requests per refresh; requestName() won't ask for the same prim again for 30 s
    const S32 MAX_NAME_REQUESTS_PER_REFRESH = 16;
    S32 name_requests = 0;
    for (LLViewerObject* link : links)
    {
        const LLUUID& link_id = link->getID();
        auto folder_it = mLinkFolders.find(link_id);
        if (folder_it == mLinkFolders.end())
        {
            continue;
        }
        LinkFolder& folder = folder_it->second;

        auto node_it = nodes.find(link_id);
        LLSelectNode* node = (node_it != nodes.end()) ? node_it->second : nullptr;
        if (node && !node->mName.empty() && node->mName != folder.mName)
        {
            folder.mName = node->mName;
            relabel_link_folder(mFolders, folder.mFolder, getLinkLabel(link_id));
        }
        else if (!node)
        {
            // A link that isn't selected (Edit linked with one part selected): its name
            // isn't known yet, so ask the region rather than label it "(unnamed)".
            // The answer refreshes the floater, which comes back here.
            std::string name;
            if (FSObjectNames::getName(link_id, name))
            {
                if (!name.empty() && name != folder.mName)
                {
                    folder.mName = name;
                    relabel_link_folder(mFolders, folder.mFolder, getLinkLabel(link_id));
                }
                // <FS:Trish> A link renamed since (a script, another viewer): asked again once the answer is old
                if (FSObjectNames::nameIsOld(link_id) && name_requests < MAX_NAME_REQUESTS_PER_REFRESH)
                {
                    ++name_requests;
                    FSObjectNames::requestName(link);
                }
                // </FS:Trish>
            }
            else if (folder.mName.empty() && name_requests < MAX_NAME_REQUESTS_PER_REFRESH)
            {
                ++name_requests;
                FSObjectNames::requestName(link);
            }
        }

        if (mLinksetLoader->isLoaded(link_id))
        {
            bool stale = link->isInventoryDirty();
            if (node && node->mInventorySerial != link->getInventorySerial() && node->mInventorySerial != folder.mRequestedSerial)
            {
                stale = true;
            }
            if (stale)
            {
                // Remembered so a serial that never matches can't ask again on every refresh
                folder.mRequestedSerial = node ? node->mInventorySerial : -1;
                mLinksetLoader->requestAgain(link_id);
            }
        }
    }
}

void LLPanelObjectInventory::syncLinks(const std::vector<LLViewerObject*>& links)
{
    // Usual case: nothing linked, unlinked or recreated
    bool same = links.size() == mLinkOrder.size();
    for (size_t i = 0; same && i < links.size(); ++i)
    {
        auto it = mLinkFolders.find(links[i]->getID());
        same = mLinkOrder[i] == links[i]->getID() && it != mLinkFolders.end() && it->second.mObject == links[i];
    }
    if (same)
    {
        return;
    }

    std::unordered_set<LLUUID> present;
    present.reserve(links.size());
    std::vector<LLUUID> order;
    order.reserve(links.size());
    for (size_t i = 0; i < links.size(); ++i)
    {
        LLViewerObject* object = links[i];
        const LLUUID& link_id = object->getID();
        present.insert(link_id);
        order.push_back(link_id);

        LinkFolder& folder = mLinkFolders[link_id];
        if (folder.mObject != object)
        {
            // New link, or the prim was killed and sent again: listen to the object we have now.
            // Its folder stays until the new contents arrive.
            folder.mObject = object;
            folder.mRequestedSerial = -1;
            mLinksetLoader->addLink(object);
        }

        const S32 link_num = (S32)i + 1;
        if (folder.mLinkNum != link_num)
        {
            folder.mLinkNum = link_num;
            relabel_link_folder(mFolders, folder.mFolder, getLinkLabel(link_id));
        }
    }

    for (const LLUUID& link_id : mLinkOrder)
    {
        if (!present.count(link_id))
        {
            removeLinkFolder(link_id);
        }
    }
    mLinkOrder.swap(order);
    mLinksetSummaryDirty = true;

    bool empty = true;
    for (const auto& folder : mLinkFolders)
    {
        if (folder.second.mFolder)
        {
            empty = false;
            break;
        }
    }
    mIsInventoryEmpty = empty;
}

void LLPanelObjectInventory::removeLinkFolder(const LLUUID& link_id)
{
    auto it = mLinkFolders.find(link_id);
    if (it == mLinkFolders.end())
    {
        return;
    }

    LinkFolder& folder = it->second;
    for (const LLUUID& key : folder.mKeys)
    {
        removeItemID(key);
    }
    if (folder.mFolder)
    {
        folder.mFolder->destroyView();
        if (mFolders)
        {
            mFolders->requestArrange();
        }
    }
    if (mLinksetLoader)
    {
        mLinksetLoader->removeLink(link_id);
    }
    mLinkFolders.erase(it);
    mLinksetSummaryDirty = true;
}

void LLPanelObjectInventory::buildLinkFolder(const LLUUID& link_id)
{
    auto it = mLinkFolders.find(link_id);
    if (it == mLinkFolders.end() || !mFolders)
    {
        return;
    }

    LinkFolder& link = it->second;
    mLinksetSummaryDirty = true;

    // Only this link's views are rebuilt; the rest of the tree is left alone
    if (link.mFolder)
    {
        link.mOpen = link.mFolder->isOpen();
        for (const LLUUID& key : link.mKeys)
        {
            removeItemID(key);
        }
        link.mKeys.clear();
        link.mFolder->destroyView();
        link.mFolder = nullptr;
    }
    link.mCount = 0;

    LLViewerObject* object = gObjectList.findObject(link_id);
    if (!object || object->isDead())
    {
        return;
    }

    LLInventoryObject* inventory_root = object->getInventoryRoot();
    LLInventoryObject::object_list_t contents;
    object->getInventoryContents(contents);
    link.mCount = (S32)contents.size();
    if (!inventory_root || contents.empty())
    {
        // Links without contents are left out
        return;
    }

    const std::string label = getLinkLabel(link_id);
    LLUIColor item_color = LLUIColorTable::instance().getColor("MenuItemEnabledColor", DEFAULT_WHITE);

    LLFolderViewFolder::Params p;
    p.name = label;
    p.tool_tip = label;
    p.root = mFolders;
    p.listener = new LLTaskLinkBridge(this, link_id, label);
    p.font_color = item_color;
    p.font_highlight_color = item_color;
    p.for_inventory = true;

    static LLCachedControl<S32> fsFolderViewItemHeight(gSavedSettings, "FSFolderViewItemHeight");
    const LLFolderViewItem::Params& default_params = LLUICtrlFactory::getDefaultParams<LLFolderViewItem>();
    p.item_height = fsFolderViewItemHeight;
    p.item_top_pad = default_params.item_top_pad - (default_params.item_height - fsFolderViewItemHeight) / 2 - 1;

    LLFolderViewFolder* folder = LLUICtrlFactory::create<LLFolderViewFolder>(p);
    folder->addToFolder(mFolders);
    createViewsForCategory(&contents, inventory_root, folder, link_id, &link.mKeys);
    if (link.mOpen)
    {
        folder->setOpen(true);
    }
    folder->refresh();
    link.mFolder = folder;

    // Keep legacy properties floaters showing this link's items up to date
    for (const LLPointer<LLInventoryObject>& item : contents)
    {
        if (LLFloaterProperties* floater = LLFloaterReg::findTypedInstance<LLFloaterProperties>("properties", item->getUUID()))
        {
            floater->refresh();
        }
    }
}

void LLPanelObjectInventory::updateLinkset()
{
    if (!mLinksetLoader)
    {
        return;
    }

    std::vector<LLUUID> changed;
    mLinksetLoader->update(changed);

    if (!changed.empty() && mFolders)
    {
        // Rebuilding a folder drops the whole selection; put it back afterwards
        const bool has_focus = gFocusMgr.childHasKeyboardFocus(mFolders);
        std::vector<LLUUID> selected;
        for (LLFolderViewItem* item : mFolders->getSelectionList())
        {
            LLTaskInvFVBridge* bridge = item ? dynamic_cast<LLTaskInvFVBridge*>(item->getViewModelItem()) : nullptr;
            if (bridge)
            {
                selected.push_back(makeItemKey(bridge->getBridgeTaskID(), bridge->getUUID()));
            }
        }

        for (const LLUUID& link_id : changed)
        {
            buildLinkFolder(link_id);
        }

        bool first_item = true;
        for (const LLUUID& key : selected)
        {
            if (LLFolderViewItem* item = getItemByID(key))
            {
                if (first_item)
                {
                    mFolders->setSelection(item, true, has_focus);
                    first_item = false;
                }
                else
                {
                    mFolders->changeSelection(item, true);
                }
            }
        }

        bool empty = true;
        for (const auto& folder : mLinkFolders)
        {
            if (folder.second.mFolder)
            {
                empty = false;
                break;
            }
        }
        mIsInventoryEmpty = empty;
        mFolders->setEnabled(true);
        mFolders->requestArrange();
    }

    mHaveInventory = mLinksetLoader->getPendingCount() == 0;
}

void LLPanelObjectInventory::doScriptsAction(const LLSD& userdata)
{
    if (!mFolders)
    {
        return;
    }

    const std::string action = userdata.asString();
    const bool reset = action == "reset";
    const bool running = action == "run";
    if (!reset && !running && action != "stop")
    {
        return;
    }

    bool blocked = false;
    for (LLFolderViewItem* item : mFolders->getSelectionList())
    {
        LLTaskInvFVBridge* bridge = item ? dynamic_cast<LLTaskInvFVBridge*>(item->getViewModelItem()) : nullptr;
        if (!bridge)
        {
            continue;
        }

        const LLUUID& task_id = bridge->getBridgeTaskID();
        LLViewerObject* object = gObjectList.findObject(task_id);
        LLViewerRegion* region = object ? object->getRegion() : nullptr;
        if (!object || !region || object->isDead() || !object->permModify())
        {
            continue;
        }
        LLInventoryObject* script = object->getInventoryObject(bridge->getUUID());
        if (!script || script->getType() != LLAssetType::AT_LSL_TEXT)
        {
            continue;
        }
        // Same restriction as the script editor's Reset and Running controls
        if (rlv_handler_t::isEnabled() && gRlvAttachmentLocks.isLockedAttachment(object->getRootEdit()))
        {
            blocked = true;
            continue;
        }

        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(reset ? _PREHASH_ScriptReset : _PREHASH_SetScriptRunning);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->nextBlockFast(_PREHASH_Script);
        msg->addUUIDFast(_PREHASH_ObjectID, task_id);
        msg->addUUIDFast(_PREHASH_ItemID, script->getUUID());
        if (!reset)
        {
            msg->addBOOLFast(_PREHASH_Running, running);
        }
        msg->sendReliable(region->getHost());

        if (!reset)
        {
            setScriptRunningState(task_id, script->getUUID(), running);
        }
    }

    if (blocked)
    {
        RlvUtil::notifyBlockedGeneric();
    }
}
// </FS:Trish>
