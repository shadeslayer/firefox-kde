/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* ***** BEGIN LICENSE BLOCK *****
 *	 Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 * 
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is globalmenu-extension.
 *
 * The Initial Developer of the Original Code is
 * Canonical Ltd.
 * Portions created by the Initial Developer are Copyright (C) 2010
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 * Chris Coulson <chris.coulson@canonical.com>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 * 
 * ***** END LICENSE BLOCK ***** */

#include <nsDebug.h>
#include <nsIAtom.h>
#include <nsIDOMEvent.h>
#include <nsIDOMMouseEvent.h>
#include <nsIDOMWindow.h>
#include <nsIDOMDocument.h>
#include <nsStringAPI.h>
#include <nsIDOMEventTarget.h>
#if MOZILLA_BRANCH_MAJOR_VERSION < 16
# include <nsIPrivateDOMEvent.h>
#endif
#include <nsPIDOMWindow.h>
#include <nsIDOMXULCommandEvent.h>
#include <nsIXPConnect.h>
#include <nsIScriptGlobalObject.h>
#include <nsIScriptContext.h>
#include <jsapi.h>
#include <mozilla/dom/Element.h>
#if MOZILLA_BRANCH_MAJOR_VERSION < 15
# include <nsIXBLService.h>
#endif
#include <nsIRunnable.h>

#include <glib-object.h>

#include "uGlobalMenuService.h"
#include "uGlobalMenu.h"
#include "uGlobalMenuBar.h"
#include "uGlobalMenuUtils.h"

#include "uDebug.h"
#include "compat.h"

uGlobalMenu::RecycleList::RecycleList(uGlobalMenu *aMenu,
                                      PRUint32 aMarker):
  mMarker(aMarker), mMenu(aMenu)
{
  mFreeEvent = NS_NewNonOwningRunnableMethod(mMenu,
                                             &uGlobalMenu::FreeRecycleList);
  NS_DispatchToCurrentThread(mFreeEvent);
}

uGlobalMenu::RecycleList::~RecycleList()
{
  Empty();
  mFreeEvent->Revoke();
}

void
uGlobalMenu::RecycleList::Empty()
{
  TRACEM(mMenu);

  while(mList.Length() > 0) {
    dbusmenu_menuitem_child_delete(mMenu->GetDbusMenuItem(), Shift());
  }

  dbusmenu_menuitem_property_set(mMenu->GetDbusMenuItem(),
                                 DBUSMENU_MENUITEM_PROP_CHILD_DISPLAY,
                                 DBUSMENU_MENUITEM_CHILD_DISPLAY_SUBMENU);
}

DbusmenuMenuitem*
uGlobalMenu::RecycleList::Shift()
{
  if (mList.Length() == 0) {
    LOGM(mMenu, "No items to shift from the front of the recycle list");
    return nullptr;
  }

  ++mMarker;
  DbusmenuMenuitem *recycled = mList[0];
  mList.RemoveElementAt(0);

  LOGM(mMenu, "Shifting %p from the front of the recycle list. New marker=%d",
       (void *)recycled, mMarker);
  return recycled;
}

void
uGlobalMenu::RecycleList::Unshift(DbusmenuMenuitem *aItem)
{
  if (mList.Length() != 0) {
    --mMarker;
  }

  LOGM(mMenu, "Prepending %p to recycle list. New marker=%d", (void *)aItem, mMarker);
  mList.InsertElementAt(0, aItem);
}

void
uGlobalMenu::RecycleList::Push(DbusmenuMenuitem *aItem)
{
  LOGM(mMenu, "Appending %p to recycle list", (void *)aItem);
  mList.AppendElement(aItem);
}

/*static*/ bool
uGlobalMenu::MenuEventCallback(DbusmenuMenuitem *menu,
                               const gchar *name,
                               GVariant *value,
                               guint timestamp,
                               void *data)
{
  uGlobalMenu *self = static_cast<uGlobalMenu *>(data);
  if (!g_strcmp0("closed", name)) {
    self->OnClose();
    return true;
  }

  if (!g_strcmp0("opened", name)) {
    self->AboutToOpen();
    return true;
  }

  return false;
}

/*static*/ bool
uGlobalMenu::MenuAboutToOpenCallback(DbusmenuMenuitem *menu,
                                     void *data)
{
  uGlobalMenu *self = static_cast<uGlobalMenu *>(data);

  // XXX: We ignore the first AboutToOpen on top-level menus, because Unity
  //      sends this signal on all top-levels when the window opens.
  //      This isn't useful for us and it doesn't finish the job by sending
  //      open/close events, so we end up in a state where we resend the
  //      entire menu structure over dbus on every page navigation
  if (!(self->mFlags & UNITY_MENU_READY)) {
    LOGM(self, "Ignoring first AboutToOpen");
    self->SetFlags(UNITY_MENU_READY);
    return false;
  }

  self->AboutToOpen();

  // We return false here for "needsUpdate", as we have no way of
  // knowing in advance if the menu structure is going to be updated.
  // The menu layout will still update on the client, but we won't block
  // opening the menu until it's happened
  return false;
}

bool
uGlobalMenu::CanOpen()
{
  if (IsDestroyed()) {
    return false;
  }
    
  bool isVisible = dbusmenu_menuitem_property_get_bool(mDbusMenuItem,
                                                       DBUSMENU_MENUITEM_PROP_VISIBLE);
  bool isDisabled = mContent->AttrValueIs(kNameSpaceID_None,
                                          uWidgetAtoms::disabled,
                                          uWidgetAtoms::_true,
                                          eCaseMatters);

  return (isVisible && !isDisabled);
}

void
uGlobalMenu::SetPopupState(uMenuPopupState aState)
{
  ClearFlags(UNITY_MENU_POPUP_STATE_MASK);
  SetFlags(aState << 8);

  if (!mPopupContent) {
    return;
  }

  nsAutoString state;
  switch (aState) {
    case ePopupClosed:
      state.Assign(NS_LITERAL_STRING("closed"));
      break;
    case ePopupShowing:
      state.Assign(NS_LITERAL_STRING("showing"));
      break;
    case ePopupOpen:
      state.Assign(NS_LITERAL_STRING("open"));
      break;
    case ePopupHiding:
      state.Assign(NS_LITERAL_STRING("hiding"));
      break;
    default:
      NS_NOTREACHED("Invalid popup state");
  }

  mPopupContent->SetAttr(kNameSpaceID_None, uWidgetAtoms::_ubuntu_state,
                         state, false);
}

static void
DispatchMouseEvent(nsIContent *aPopup, const nsAString& aType)
{
  if (!aPopup) {
    return;
  }

  nsCOMPtr<nsIDOMEventTarget> target = do_QueryInterface(aPopup);
  NS_ASSERTION(target, "Content failed QI to nsIDOMEventTarget");
  if (!target) {
    return;
  }

  nsCOMPtr<nsIDOMDocument> domDoc = do_QueryInterface(aPopup->OwnerDoc());
  NS_ASSERTION(domDoc, "Document failed QI to nsIDOMDocument");
  if (!domDoc) {
    return;
  }

  nsCOMPtr<nsIDOMEvent> event;
  domDoc->CreateEvent(NS_LITERAL_STRING("mouseevent"),
                      getter_AddRefs(event));
  NS_ASSERTION(event, "Failed to create mouseevent");
  if (!event) {
    return;
  }

  nsCOMPtr<nsIDOMMouseEvent> mouseEvent = do_QueryInterface(event);
  NS_ASSERTION(mouseEvent, "Event failed QI to nsIDOMMouseEvent");
  if (!mouseEvent) {
    return;
  }

  nsCOMPtr<nsIDOMWindow> window;
  domDoc->GetDefaultView(getter_AddRefs(window));
  if (!window) {
    return;
  }

  mouseEvent->InitMouseEvent(aType, true, true, window, 0,
                             0, 0, 0, 0, false, false, false, false,
                             0, nullptr);

  nsCOMPtr<nsIPrivateDOMEvent> priv = do_QueryInterface(event);
  NS_ASSERTION(priv, "Event failed QI to nsIPrivateDOMEvent");
  if (!priv) {
    return;
  }

  priv->SetTrusted(true);

  bool dummy;
  // XXX: dummy == false means that we should prevent the
  //      the menu from opening, but there's no way to do this
  target->DispatchEvent(event, &dummy);
}

void
uGlobalMenu::AboutToOpen()
{
  TRACETM();

  uMenuAutoSuspendMutationEvents as;

  if (DoesNeedRebuild()) {
    Build();
  }

  if (GetPopupState() == ePopupShowing || GetPopupState() == ePopupOpen) {
    LOGTM("Ignoring AboutToOpen for already open menu");
    return;
  }

  SetPopupState(ePopupShowing);
  DispatchMouseEvent(mPopupContent, NS_LITERAL_STRING("popupshowing"));

  for (PRUint32 i = 0; i < mMenuObjects.Length(); i++) {
    mMenuObjects[i]->ContainerIsOpening();
  }

  SetPopupState(ePopupOpen);
  mContent->SetAttr(kNameSpaceID_None, uWidgetAtoms::open,
                    NS_LITERAL_STRING("true"), true);

  nsCOMPtr<nsIRunnable> event =
    NS_NewRunnableMethod(this, &uGlobalMenu::FirePopupShownEvent);
  NS_DispatchToCurrentThread(event);
}

void
uGlobalMenu::FirePopupShownEvent()
{
  TRACETM();

  if (IsDestroyed()) {
    return;
  }

  if (GetPopupState() == ePopupOpen) {
    DispatchMouseEvent(mPopupContent, NS_LITERAL_STRING("popupshown"));
  }
}

void
uGlobalMenu::OnClose()
{
  TRACETM();

  if (GetPopupState() != ePopupShowing && GetPopupState() != ePopupOpen) {
    LOGTM("Ignoring OnClose for menu which isn't open");
    return;
  }

  SetPopupState(ePopupHiding);

  nsCOMPtr<nsIRunnable> event =
    NS_NewRunnableMethod(this, &uGlobalMenu::FirePopupHidingEvent);
  NS_DispatchToCurrentThread(event);
}

void
uGlobalMenu::FirePopupHidingEvent()
{
  TRACETM();

  uMenuAutoSuspendMutationEvents as;

  if (IsDestroyed()) {
    return;
  }

  if (GetPopupState() != ePopupHiding) {
    return;
  }

  DispatchMouseEvent(mPopupContent, NS_LITERAL_STRING("popuphiding"));

  SetPopupState(ePopupClosed);
  DispatchMouseEvent(mPopupContent, NS_LITERAL_STRING("popuphidden"));

  mContent->UnsetAttr(kNameSpaceID_None, uWidgetAtoms::open, true);
}

void
uGlobalMenu::Refresh(uMenuObjectRefreshMode aMode)
{
  TRACETM();

  if (aMode == eRefreshFull) {
    SyncLabelFromContent();
    SyncSensitivityFromContent();
  }

  SyncVisibilityFromContent();
  SyncIconFromContent();
}

void
uGlobalMenu::InitializeDbusMenuItem()
{
  // This happens automatically when we add children, but we have to
  // do this manually for menus which don't initially have children,
  // so we can receive about-to-show which triggers a build of the menu
  dbusmenu_menuitem_property_set(mDbusMenuItem,
                                 DBUSMENU_MENUITEM_PROP_CHILD_DISPLAY,
                                 DBUSMENU_MENUITEM_CHILD_DISPLAY_SUBMENU);

  g_signal_connect(G_OBJECT(mDbusMenuItem), "about-to-show",
                   G_CALLBACK(MenuAboutToOpenCallback), this);
  g_signal_connect(G_OBJECT(mDbusMenuItem), "event",
                   G_CALLBACK(MenuEventCallback), this);
}

static bool
IsRecycledItemCompatible(DbusmenuMenuitem *aRecycled,
                         uGlobalMenuObject *aNewItem)
{
  // If the recycled item was a separator, it can only be reused as a separator
  if ((g_strcmp0(dbusmenu_menuitem_property_get(aRecycled,
                                                DBUSMENU_MENUITEM_PROP_TYPE),
                 "separator") == 0) !=
      (aNewItem->GetType() == eMenuSeparator)) {
    return false;
  }

  // Everything else is fine
  return true;
}

uint32_t
uGlobalMenu::IndexOf(nsIContent *aContent)
{
  if (!aContent) {
    return -1;
  }

  for (uint32_t i = 0; i < mMenuObjects.Length(); i++) {
    if (mMenuObjects[i]->GetContent() == aContent) {
      return i;
    }
  }

  return -1;
}

bool
uGlobalMenu::InsertMenuObjectAfterContent(uGlobalMenuObject *menuObj,
                                          nsIContent *aPrevSibling)
{
  int32_t index = IndexOf(aPrevSibling);
  NS_ASSERTION(index >= 0 || !aPrevSibling, "Previous sibling not found");
  if (index < 0 && aPrevSibling) {
    return false;
  }

  uint32_t correctedIndex = ++index;

  DbusmenuMenuitem *recycled = nullptr;
  if (mRecycleList && mRecycleList->mList.Length() > 0) {
    if (index < mRecycleList->mMarker) {
      ++mRecycleList->mMarker;
    } else if (index > mRecycleList->mMarker) {
      correctedIndex += mRecycleList->mList.Length();
    } else {
      // If this node is being inserted in to a gap left by previously
      // removed nodes, then recycle one that we just removed
      recycled = mRecycleList->Shift();
      if (!IsRecycledItemCompatible(recycled, menuObj)) {
        recycled = nullptr;
        mRecycleList = nullptr;
      }
    }
  }

  gboolean res = TRUE;
  if (recycled) {
    LOGTM("Inserting item at index %d using recycled DbusmenuMenuitem %p",
          index, (void *)recycled);
    menuObj->SetDbusMenuItem(recycled);
  } else {
    LOGTM("Inserting item at index %d", index);
    res = dbusmenu_menuitem_child_add_position(mDbusMenuItem,
                                               menuObj->GetDbusMenuItem(),
                                               correctedIndex);
  }

  return res && mMenuObjects.InsertElementAt(index, menuObj);
}

bool
uGlobalMenu::AppendMenuObject(uGlobalMenuObject *menuObj)
{
  DbusmenuMenuitem *recycled = nullptr;
  if (mRecycleList && mRecycleList->mList.Length() > 0 &&
      mRecycleList->mMarker == mMenuObjects.Length()) {
    // If any nodes were just removed from the end of the menu, then recycle
    // one now
    recycled = mRecycleList->Shift();
    if (!IsRecycledItemCompatible(recycled, menuObj)) {
      recycled = nullptr;
      mRecycleList = nullptr;
    }
  }

  gboolean res = TRUE;
  if (recycled) {
    LOGTM("Appending item using recycled DbusmenuMenuitem %p", (void *)recycled);
    menuObj->SetDbusMenuItem(recycled);
  } else {
    LOGTM("Appending item");
    res = dbusmenu_menuitem_child_append(mDbusMenuItem,
                                         menuObj->GetDbusMenuItem());
  }

  return res && mMenuObjects.AppendElement(menuObj);
}

bool
uGlobalMenu::RemoveMenuObjectAt(uint32_t index, bool recycle)
{
  NS_ASSERTION(index < mMenuObjects.Length(), "Invalid index");
  if (index >= mMenuObjects.Length()) {
    return false;
  }

  // We add contiguous blocks of removed nodes to a recycle list, so that
  // we can reuse them again if they can be reinserted in to the menu without
  // changing its structure. The list is cleaned in an idle event, so nodes
  // must be removed and inserted without running the event loop if they are
  // to benefit from this recycling feature.
  // This feature allows menu contents to be refreshed by removing all children
  // and inserting new ones, without altering the overall structure. It is used
  // by the history menu in Firefox
  if (recycle) {
    if (!mRecycleList) {
      mRecycleList = new RecycleList(this, index);
    } else if (mRecycleList->mList.Length() > 0 &&
               ((mRecycleList->mMarker != 0 &&
                 index < mRecycleList->mMarker - 1) ||
                index > mRecycleList->mMarker)) {
      // If this node is not adjacent to any previously removed nodes, then
      // free the existing nodes already and restart the process
      mRecycleList->Empty();
      mRecycleList->mMarker = index;
    }

    if (index == mRecycleList->mMarker) {
      mRecycleList->Push(mMenuObjects[index]->GetDbusMenuItem());
    } else {
      mRecycleList->Unshift(mMenuObjects[index]->GetDbusMenuItem());
    }
  } else {
    dbusmenu_menuitem_child_delete(mDbusMenuItem,
                                   mMenuObjects[index]->GetDbusMenuItem());
  }

  LOGTM("Removing item at index %d", index);
  mMenuObjects[index]->Destroy();
  mMenuObjects.RemoveElementAt(index);

  return true;
}

bool
uGlobalMenu::RemoveMenuObjectForContent(nsIContent *aContent, bool recycle)
{
  int32_t index = IndexOf(aContent);
  NS_ASSERTION(index >= 0, "Previous sibling not found");
  if (index < 0 ) {
    return false;
  }

  return RemoveMenuObjectAt(index, recycle);
}

void
uGlobalMenu::InitializePopup()
{
  nsCOMPtr<nsIContent> oldPopupContent;
  oldPopupContent.swap(mPopupContent);

#if MOZILLA_BRANCH_MAJOR_VERSION < 15
  // Taken from widget/src/cocoa/nsMenuX.mm
  nsIXBLService *xblService = uGlobalMenuService::GetXBLService();
  if (xblService) {
    PRInt32 dummy;
    nsCOMPtr<nsIAtom> tag;
    xblService->ResolveTag(mContent, &dummy, getter_AddRefs(tag));

    if (tag == uWidgetAtoms::menupopup) {
      mPopupContent = mContent;
    } else {
#else
  {
    {
#endif
      PRUint32 count = mContent->GetChildCount();

      for (PRUint32 i = 0; i < count; i++) {
        PRInt32 dummy;
        nsIContent *child = mContent->GetChildAt(i);
#if MOZILLA_BRANCH_MAJOR_VERSION < 15
        nsCOMPtr<nsIAtom> tag;
        xblService->ResolveTag(child, &dummy, getter_AddRefs(tag));
        if (tag == uWidgetAtoms::menupopup) {
#else
        // FIXME: What are we meant to do here? Are there any scenario's where this
        //        is actually broken? (I guess a menu could have a binding that
        //        extends a menupopup, but that doesn't seem to happen anywhere
        //        by default)
        if (child->Tag() == uWidgetAtoms::menupopup) {
#endif
          mPopupContent = child;
          break;
        }
      }
    }
  }

  if (!mPopupContent) {
    LOGTM("Menu now has no menupopup");
  }

  if (oldPopupContent == mPopupContent) {
    return;
  }

  SetPopupState(ePopupClosed);

  // If the popup has changed, disconnect the old one from the doc observer,
  // attach the new one and attach its bindings
  if (oldPopupContent && oldPopupContent != mContent) {
    mListener->UnregisterForContentChanges(oldPopupContent, this);
  }

  if (!mPopupContent) {
    return;
  }

  LOGTM("Menu has new menupopup");

  // Wrap the native menupopup node, as this results in style resolution
  // and attachment of XBL bindings, which normally doesn't happen because
  // we are a child of an element with "display: none"
  // Borrowed from widget/src/cocoa/nsMenuX.mm, we need this to make
  // some menus in Thunderbird work
  nsIXPConnect *xpconnect = uGlobalMenuService::GetXPConnect();
  NS_ASSERTION(xpconnect, "Could not get xpconnect");
  if (xpconnect) {
    nsIScriptGlobalObject *sgo = mPopupContent->OwnerDoc()->GetScriptGlobalObject();
    nsCOMPtr<nsIScriptContext> scriptContext = sgo->GetContext();
    JSObject *global = sgo->GetGlobalJSObject();
    if (scriptContext && global) {
      JSContext *cx = (JSContext *)scriptContext->GetNativeContext();
      if (cx) {
        LOGTM("Wrapping menupopup");
        nsCOMPtr<nsIXPConnectJSObjectHolder> wrapper;
        nsresult rv = xpconnect->WrapNative(cx, global,
                                            mPopupContent,
                                            NS_GET_IID(nsISupports),
                                            getter_AddRefs(wrapper));
        if (NS_FAILED(rv)) {
          NS_WARNING("Failed to wrap menupopup");
        }
      }
    }
  }

  if (mContent != mPopupContent) {
    mListener->RegisterForContentChanges(mPopupContent, this);
  }
}

nsresult
uGlobalMenu::Build()
{
  TRACETM();

  PRUint32 count = mMenuObjects.Length();
  for (PRUint32 i = 0; i < count; i++) {
    RemoveMenuObjectAt(0, true);
  }

  InitializePopup();

  if (!mPopupContent) {
    // The menu has no popup, so there are no menuitems here
    return NS_OK;
  }

  ClearNeedsRebuild();

  count = mPopupContent->GetChildCount();

  for (PRUint32 i = 0; i < count; i++) {
    nsIContent *child = mPopupContent->GetChildAt(i);
    uGlobalMenuObject *menuObject = NewGlobalMenuItem(this, mListener, child);
    bool res = false;
    if (menuObject) {
      res = AppendMenuObject(menuObject);
    }
    NS_WARN_IF_FALSE(res, "Failed to append menuitem. Marking menu invalid");
    if (!res) {
      SetNeedsRebuild();
      return NS_ERROR_FAILURE;
    }
  }

  return NS_OK;
}

nsresult
uGlobalMenu::Init(uGlobalMenuObject *aParent,
                  uGlobalMenuDocListener *aListener,
                  nsIContent *aContent)
{
  NS_ENSURE_ARG(aParent);
  NS_ENSURE_ARG(aListener);
  NS_ENSURE_ARG(aContent);

  mParent = aParent;
  mListener = aListener;
  mContent = aContent;

  SetNeedsRebuild();

  // See the hack comment above for why this workaround is here
  if (mParent->GetType() != eMenuBar || GetMenuBar()->IsRegistered()) {
    SetFlags(UNITY_MENU_READY);
  }

  mListener->RegisterForContentChanges(mContent, this);

  return NS_OK;
}

uGlobalMenu::uGlobalMenu(): uGlobalMenuObject()
{
  MOZ_COUNT_CTOR(uGlobalMenu);
}

uGlobalMenu::~uGlobalMenu()
{
  TRACETM();

  if (!IsDestroyed()) {
    Destroy();
  }

  MOZ_COUNT_DTOR(uGlobalMenu);
}

/*static*/ uGlobalMenuObject*
uGlobalMenu::Create(uGlobalMenuObject *aParent,
                    uGlobalMenuDocListener *aListener,
                    nsIContent *aContent)
{
  TRACEC(aContent);

  uGlobalMenu *menu = new uGlobalMenu();
  if (!menu) {
    return nullptr;
  }

  if (NS_FAILED(menu->Init(aParent, aListener, aContent))) {
    delete menu;
    return nullptr;
  }

  return static_cast<uGlobalMenuObject *>(menu);
}

void
uGlobalMenu::Destroy()
{
  NS_ASSERTION(!IsDestroyed(), "Menu is already destroyed");
  if (IsDestroyed()) {
    return;
  }

  // The recycle list doesn't hold a strong ref to our dbusmenuitem or
  // any of it's children, so we need to drop them now to avoid crashing later
  mRecycleList = nullptr;

  // Although nsTArray will take care of this in its destructor,
  // we have to manually ensure children are removed from our dbusmenu
  // item, just in case our parent recycles it
  while (mMenuObjects.Length() > 0) {
    RemoveMenuObjectAt(0, false);
  }

  if (mListener && mPopupContent && mContent != mPopupContent) {
    mListener->UnregisterForContentChanges(mPopupContent, this);
  }

  if (mDbusMenuItem) {
    guint found = g_signal_handlers_disconnect_by_func(mDbusMenuItem,
                                                       FuncToVoidPtr(MenuAboutToOpenCallback),
                                                       this);
    NS_ASSERTION(found == 1, "Failed to disconnect \"about-to-open\" handler");

    found = g_signal_handlers_disconnect_by_func(mDbusMenuItem,
                                                 FuncToVoidPtr(MenuEventCallback),
                                                 this);
    NS_ASSERTION(found == 1, "Failed to disconnect \"event\" handler");
  }

  uGlobalMenuObject::Destroy();
}

/*static*/ gboolean
uGlobalMenu::DoOpen(gpointer user_data)
{
  DbusmenuMenuitem *menuitem = static_cast<DbusmenuMenuitem *>(user_data);
  dbusmenu_menuitem_show_to_user(menuitem, 0);
  g_object_unref(menuitem);
  return FALSE;
}

void
uGlobalMenu::OpenMenuDelayed()
{
  NS_ASSERTION(!IsDestroyed(), "Menu has been destroyed");
  if (IsDestroyed()) {
    return;
  }

  if (!CanOpen()) {
    return;
  }

  // Here, we manually call AboutToOpen and then open the menu after a short
  // delay. This avoids an issue where opening the History menu in Firefox
  // with the keyboard causes extra items to appear at the top of the menu,
  // but keyboard focus is not on the first item
  AboutToOpen();
  g_timeout_add(100, DoOpen, g_object_ref(mDbusMenuItem));
}

void
uGlobalMenu::ObserveAttributeChanged(nsIContent *aContent,
                                     nsIAtom *aAttribute)
{
  TRACETM();
  NS_ASSERTION(aContent == mContent || aContent == mPopupContent,
               "Received an event that wasn't meant for us!");

  if (aAttribute == uWidgetAtoms::open ||
      aAttribute == uWidgetAtoms::_ubuntu_state) {
    return;
  }

  if (aContent == mContent) {
    if (aAttribute == uWidgetAtoms::disabled) {
      SyncSensitivityFromContent();
    } else if (aAttribute == uWidgetAtoms::label || 
               aAttribute == uWidgetAtoms::accesskey) {
      SyncLabelFromContent();
    } else if (aAttribute == uWidgetAtoms::image) {
      SyncIconFromContent();
    } else if (aAttribute == uWidgetAtoms::hidden ||
               aAttribute == uWidgetAtoms::collapsed) {
      SyncVisibilityFromContent();
    }
  }
}

void
uGlobalMenu::ObserveContentRemoved(nsIContent *aContainer,
                                   nsIContent *aChild)
{
  TRACETM();
  NS_ASSERTION(aContainer == mContent || aContainer == mPopupContent,
               "Received an event that wasn't meant for us!");

  if (DoesNeedRebuild()) {
    LOGTM("Previously marked as needing a rebuild");
    return;
  }

  if (aContainer == mPopupContent) {
    bool res = RemoveMenuObjectForContent(aChild, true);
    NS_WARN_IF_FALSE(res, "Failed to remove menuitem - marking menu as needing a rebuild");
    if (!res) {
      SetNeedsRebuild();
    }
  } else {
    Build();
  }
}

void
uGlobalMenu::ObserveContentInserted(nsIContent *aContainer,
                                    nsIContent *aChild,
                                    nsIContent *aPrevSibling)
{
  TRACETM();
  NS_ASSERTION(aContainer == mContent || aContainer == mPopupContent,
               "Received an event that wasn't meant for us!");

  if (DoesNeedRebuild()) {
    LOGTM("Previously marked as needing a rebuild");
    return;
  }

  if (aContainer == mPopupContent) {
    uGlobalMenuObject *newItem = NewGlobalMenuItem(this, mListener, aChild);
    bool res = false;
    if (newItem) {
      res = InsertMenuObjectAfterContent(newItem, aPrevSibling);
    }
    NS_WARN_IF_FALSE(res, "Failed to insert menuitem - marking menu as needing a rebuild");
    if (!res) {
      SetNeedsRebuild();
    } 
  } else {
    Build();
  }
}
