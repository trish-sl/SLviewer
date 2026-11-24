/**
 * @file fsfloaterfinduses.cpp
 * @brief Lists the faces of objects in the region that use an inventory texture or material
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

#include "llviewerprecompiledheaders.h"

#include "fsfloaterfinduses.h"

#include "fsareasearch.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "llfloaterreg.h"
#include "llgltfmaterial.h"
#include "llinventorymodel.h"
#include "llmaterial.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llselectmgr.h"
#include "lltextbox.h"
#include "lltextureentry.h"
#include "llviewerinventory.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"

#include <algorithm>

FSFloaterFindUses::FSFloaterFindUses(const LLSD& key)
:   LLFloater(key)
{
}

bool FSFloaterFindUses::postBuild()
{
    mList = getChild<LLScrollListCtrl>("uses");
    mStatus = getChild<LLTextBox>("status");
    mList->setDoubleClickCallback([this]() { onDoubleClick(); });
    getChild<LLUICtrl>("refresh")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickRefresh(); });
    return true;
}

// static
void FSFloaterFindUses::findUses(const LLUUID& item_id)
{
    const LLViewerInventoryItem* item = gInventory.getItem(item_id);
    if (!item)
    {
        return;
    }

    const bool is_material = item->getType() == LLAssetType::AT_MATERIAL;
    if (!is_material && item->getType() != LLAssetType::AT_TEXTURE)
    {
        return;
    }

    LLUUID asset_id = item->getAssetUUID();
    if (is_material && asset_id.isNull())
    {
        // an inventory material without an asset is the blank material, as when it is applied
        asset_id = BLANK_MATERIAL_ASSET_ID;
    }
    if (asset_id.isNull())
    {
        return;
    }

    // one window, its results replaced by each search
    if (FSFloaterFindUses* floater = LLFloaterReg::showTypedInstance<FSFloaterFindUses>("fs_find_uses"))
    {
        floater->search(asset_id, item->getName(), is_material);
    }
}

void FSFloaterFindUses::search(const LLUUID& asset_id, const std::string& name, bool is_material)
{
    mAssetID = asset_id;
    mAssetName = name;
    mIsMaterial = is_material;
    mUses.clear();

    LLStringUtil::format_map_t title_args;
    title_args["NAME"] = name;
    setTitle(getString("title", title_args));

    const LLVector3d agent_pos = gAgent.getPositionGlobal();
    const S32 object_count = gObjectList.getNumObjects();
    for (S32 i = 0; i < object_count; ++i)
    {
        searchObject(gObjectList.getObject(i), agent_pos);
    }

    std::stable_sort(mUses.begin(), mUses.end(), [](const Use& a, const Use& b) { return a.mDistance < b.mDistance; });

    // Rows are added in distance order and the list doesn't sort, as its columns would sort as text
    mList->deleteAllItems();
    for (size_t i = 0; i < mUses.size(); ++i)
    {
        const Use& use = mUses[i];
        LLSD row;
        row["value"] = (S32)i;
        LLSD& columns = row["columns"];
        columns[0]["column"] = "name";
        columns[0]["value"] = use.mObjectName;
        columns[1]["column"] = "face";
        columns[1]["value"] = llformat("%d", use.mFace);
        columns[2]["column"] = "channel";
        columns[2]["value"] = channelName(use.mChannel);
        columns[3]["column"] = "distance";
        columns[3]["value"] = llformat("%.0f", use.mDistance);
        mList->addElement(row);
    }

    LLStringUtil::format_map_t count_args;
    count_args["COUNT"] = llformat("%d", (S32)mUses.size());
    mStatus->setText(getString(mUses.empty() ? "status_none" : "status", count_args));
}

void FSFloaterFindUses::searchObject(LLViewerObject* object, const LLVector3d& agent_pos)
{
    LLViewerRegion* region = gAgent.getRegion();
    if (!object || object->isDead() || object->isAvatar() || object->isHUDAttachment()
        || !region || object->getRegion() != region || object->getPCode() != LL_PCODE_VOLUME)
    {
        return;
    }

    // The texture entries, not the drawn faces: an object not drawn yet (beyond the draw distance,
    // still loading) has no faces, but its entries say what it uses all the same
    const S32 face_count = (S32)object->getNumTEs();
    if (face_count <= 0)
    {
        return;
    }

    std::string object_name;    // looked up once, for the first face that matches
    const F32 distance = (F32)(object->getPositionGlobal() - agent_pos).magVec();

    auto add_use = [&](S32 face, EChannel channel)
    {
        if (object_name.empty())
        {
            object_name = objectName(object);
        }
        Use use;
        use.mObjectID = object->getID();
        use.mFace = face;
        use.mChannel = channel;
        use.mDistance = distance;
        use.mObjectName = object_name;
        mUses.push_back(use);
    };

    for (S32 face = 0; face < face_count; ++face)
    {
        const LLTextureEntry* te = object->getTE((U8)face);
        if (!te)
        {
            continue;
        }

        if (mIsMaterial)
        {
            if (object->getRenderMaterialID((U8)face) == mAssetID)
            {
                add_use(face, CHANNEL_MATERIAL);
            }
            continue;
        }

        if (te->getID() == mAssetID)
        {
            add_use(face, CHANNEL_DIFFUSE);
        }

        const LLMaterialPtr material = te->getMaterialParams();
        if (material.notNull())
        {
            if (material->getNormalID() == mAssetID)
            {
                add_use(face, CHANNEL_NORMAL);
            }
            if (material->getSpecularID() == mAssetID)
            {
                add_use(face, CHANNEL_SPECULAR);
            }
        }

        // the maps of the face's GLTF material, then the textures its overrides put in their place
        if (const LLGLTFMaterial* gltf = te->getGLTFMaterial())
        {
            for (const LLUUID& id : gltf->mTextureId)
            {
                if (id == mAssetID)
                {
                    add_use(face, CHANNEL_IN_MATERIAL);
                    break;
                }
            }
        }
        if (const LLGLTFMaterial* overrides = te->getGLTFMaterialOverride())
        {
            for (const LLUUID& id : overrides->mTextureId)
            {
                if (id == mAssetID)
                {
                    add_use(face, CHANNEL_OVERRIDE);
                    break;
                }
            }
        }
    }
}

std::string FSFloaterFindUses::objectName(LLViewerObject* object) const
{
    // The viewer only knows an object's name once it has asked for its properties: when it is selected
    // or area search has looked at it. Asking for every object here would be one request per object.
    LLViewerObject* root = object->getRootEdit();
    LLObjectSelectionHandle selection = LLSelectMgr::getInstance()->getSelection();
    for (LLViewerObject* candidate : { object, root })
    {
        if (!candidate)
        {
            continue;
        }
        LLSelectNode* node = selection->findNode(candidate);
        if (node && node->mValid && !node->mName.empty())
        {
            return node->mName;
        }
    }

    if (FSAreaSearch* area_search = LLFloaterReg::findTypedInstance<FSAreaSearch>("area_search"))
    {
        for (LLViewerObject* candidate : { object, root })
        {
            if (!candidate)
            {
                continue;
            }
            auto found = area_search->mObjectDetails.find(candidate->getID());
            if (found != area_search->mObjectDetails.end()
                && found->second.request == FSObjectProperties::FINISHED
                && !found->second.name.empty())
            {
                return found->second.name;
            }
        }
    }

    LLStringUtil::format_map_t args;
    args["ID"] = object->getID().asString().substr(0, 8);
    return getString("unnamed_object", args);
}

std::string FSFloaterFindUses::channelName(EChannel channel) const
{
    switch (channel)
    {
        case CHANNEL_NORMAL:        return getString("channel_normal");
        case CHANNEL_SPECULAR:      return getString("channel_specular");
        case CHANNEL_MATERIAL:      return getString("channel_material");
        case CHANNEL_IN_MATERIAL:   return getString("channel_in_material");
        case CHANNEL_OVERRIDE:      return getString("channel_override");
        case CHANNEL_DIFFUSE:
        default:                    return getString("channel_diffuse");
    }
}

void FSFloaterFindUses::onClickRefresh()
{
    if (mAssetID.notNull())
    {
        search(mAssetID, mAssetName, mIsMaterial);
    }
}

void FSFloaterFindUses::onDoubleClick()
{
    LLScrollListItem* item = mList->getFirstSelected();
    if (!item)
    {
        return;
    }
    const S32 index = item->getValue().asInteger();
    if (index < 0 || index >= (S32)mUses.size())
    {
        return;
    }
    const Use& use = mUses[index];

    LLViewerObject* object = gObjectList.findObject(use.mObjectID);
    if (!object || object->isDead())
    {
        mStatus->setText(getString("object_gone"));
        return;
    }

    // select just that face, and look at it
    LLSelectMgr::getInstance()->deselectAll();
    LLSelectMgr::getInstance()->selectObjectOnly(object, use.mFace);
    gAgentCamera.setFocusOnAvatar(false, ANIMATE);
    gAgentCamera.setFocusGlobal(object->getPositionGlobal(), use.mObjectID);
}
