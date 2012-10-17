/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
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
 * The Original Code is mozilla.org code.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1998
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Josh Aas <josh@mozilla.com>
 *   Thomas K. Dyas <tom.dyas@gmail.com>
 *   Chris Coulson <chris.coulson@canonical.com>
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

#include <prtypes.h>
#include <nsIDocument.h>
#include <nsIAtom.h>
#include <nsINode.h>
#include <nsIContent.h>
#include <nsIDocument.h>
#include <nsIDOMWindow.h>
#include <nsIXPConnect.h>
#include <nsIScriptGlobalObject.h>
#include <nsIScriptContext.h>
#include <nsComponentManagerUtils.h>
#include <nsIJSNativeInitializer.h>
#include <nsIDOMNodeList.h>
#include <nsIAtomService.h>
#include <jsapi.h>

#include "uGlobalMenuDocListener.h"
#include "uGlobalMenuObject.h"
#include "uGlobalMenuService.h"

#include "uDebug.h"

NS_IMPL_ISUPPORTS1(uGlobalMenuDocListener, nsIMutationObserverCallback)

uint32_t uGlobalMenuDocListener::sInhibitDepth = 0;
nsTArray<nsCOMPtr<uGlobalMenuDocListener> >* uGlobalMenuDocListener::sPendingListeners;

nsresult
uGlobalMenuDocListener::Init(nsIContent *rootNode)
{
  NS_ENSURE_ARG(rootNode);

  nsIXPConnect *xpconnect = uGlobalMenuService::GetXPConnect();
  NS_ASSERTION(xpconnect, "Failed to get xpconnect");
  if (!xpconnect) {
    return NS_ERROR_FAILURE;
  }

  nsIScriptGlobalObject *sgo = rootNode->OwnerDoc()->GetScriptGlobalObject();
  nsCOMPtr<nsIScriptContext> scriptContext = sgo->GetContext();
  JSObject *global = sgo->GetGlobalJSObject();
  NS_ASSERTION(global && scriptContext, "What?");
  if (!global || !scriptContext) {
    return NS_ERROR_FAILURE;
  }

  JSContext *cx = (JSContext *)scriptContext->GetNativeContext();
  NS_ASSERTION(cx, "Yikes!");
  if (!cx) {
    return NS_ERROR_FAILURE;
  }

  jsval v;
  nsresult rv = xpconnect->WrapNativeToJSVal(cx, global, this, nullptr,
                                             &NS_GET_IID(nsISupports), false,
                                             &v, nullptr);
  if (NS_FAILED(rv)) {
    NS_ERROR("Failed to wrap native observer to jsval");
    return rv;
  }

  nsPIDOMWindow *owner = rootNode->OwnerDoc()->GetInnerWindow();

  mObserver = do_CreateInstance("@mozilla.org/dommutationobserver;1");
  NS_ASSERTION(mObserver, "Failed to create nsIDOMMutationObserver");
  if (!mObserver) {
    return NS_ERROR_FAILURE;
  }

  nsCOMPtr<nsIJSNativeInitializer> initializer = do_QueryInterface(mObserver);
  NS_ASSERTION(initializer, "Failed to QI to nsIJSNativeInitializer");
  if (!initializer) {
    return NS_ERROR_FAILURE;
  }

  rv = initializer->Initialize(owner, cx, nullptr, 1, &v);
  if (NS_FAILED(rv)) {
    NS_ERROR("Failed to initialize MutationObserver");
    return rv;
  }

  nsCOMPtr<nsIDOMNode> docNode = do_QueryInterface(rootNode->OwnerDoc());
  NS_ASSERTION(docNode, "Document failed QI to nsIDOMNode");
  if (!docNode) {
    return NS_ERROR_FAILURE;
  }

  JSAutoRequest ar(cx);

  JSObject *options = JS_NewObject(cx, nullptr, nullptr, nullptr);

#define SET_OBSERVER_OPTION(_option) \
  if (!JS_DefineProperty(cx, options, #_option, BOOLEAN_TO_JSVAL(JS_TRUE), \
                         nullptr, nullptr, 0)) { \
    NS_ERROR("Failed setting options for mutation observer"); \
    return NS_ERROR_FAILURE; \
  }

  SET_OBSERVER_OPTION(childList)
  SET_OBSERVER_OPTION(attributes)
  SET_OBSERVER_OPTION(subtree)

  rv = mObserver->Observe(docNode, OBJECT_TO_JSVAL(options), cx);
  if (NS_FAILED(rv)) {
    NS_ERROR("Failed to observe");
  }

  return rv;
}

void
uGlobalMenuDocListener::Destroy()
{
  if (mObserver) {
    mObserver->Disconnect();
    mObserver = nullptr;
  }
}

uGlobalMenuDocListener::uGlobalMenuDocListener()
{
  mContentToObserverTable.Init();
}

uGlobalMenuDocListener::~uGlobalMenuDocListener()
{
  NS_ASSERTION(mContentToObserverTable.Count() == 0,
               "Some nodes forgot to unregister listeners");
}

NS_IMETHODIMP
uGlobalMenuDocListener::HandleMutations(nsIVariant *aRecords,
                                        nsIDOMMutationObserver *aObserver)
{
  TRACE()

  uint16_t vtype;
  uint32_t count;
  void *ptr;
  nsIID iid;
  aRecords->GetAsArray(&vtype, &iid, &count, &ptr);

  LOG("Number of records %d", count);

  NS_ASSERTION(vtype == nsIDataType::VTYPE_INTERFACE, "Invalid variant");
  if (vtype != nsIDataType::VTYPE_INTERFACE) {
    return NS_ERROR_FAILURE;
  }

  nsTArray<nsCOMPtr<nsIDOMMutationRecord> > mods(count);
  nsIDOMMutationRecord **p = (nsIDOMMutationRecord **)ptr;
  for (uint32_t i = 0; i < count; i++, p++) {
    nsCOMPtr<nsIDOMMutationRecord> rec = dont_AddRef(*p);
    mods.AppendElement(rec);
  }

  nsMemory::Free(ptr);

  if (sInhibitDepth > 0) {
    mPendingMutations.AppendElements(mods);
    ScheduleListener(this);
  } else {
    HandleMutations(mods);
  }

  return NS_OK;
}

void
uGlobalMenuDocListener::HandleMutations(nsTArray<nsCOMPtr<nsIDOMMutationRecord> >& aRecords)
{
  for (uint32_t i = 0; i < aRecords.Length(); i++) {
    LOG("Beginning record %d", i);
    nsIDOMMutationRecord *record = aRecords[i];

    nsCOMPtr<nsIDOMNode> targetNode;
    record->GetTarget(getter_AddRefs(targetNode));
    if (!targetNode) {
      NS_ERROR("No target node?");
      continue;
    }

    nsCOMPtr<nsIContent> target = do_QueryInterface(targetNode);
    NS_ASSERTION(target, "Node failed to QI to nsIContent");
    if (!target) {
      continue;
    }

    nsAutoString type;
    record->GetType(type);

    if (type.Equals(NS_LITERAL_STRING("attributes"))) {
      nsAutoString attributeName;
      record->GetAttributeName(attributeName);

      AttributeChanged(target, attributeName);
    } else if (type.Equals(NS_LITERAL_STRING("childList"))) {
      nsCOMPtr<nsIDOMNodeList> removedNodes;
      record->GetRemovedNodes(getter_AddRefs(removedNodes));
      uint32_t removedLength;
      removedNodes->GetLength(&removedLength);

      nsCOMPtr<nsIDOMNodeList> addedNodes;
      record->GetAddedNodes(getter_AddRefs(addedNodes));
      uint32_t addedLength;
      addedNodes->GetLength(&addedLength);

      nsCOMPtr<nsIDOMNode> prev;
      record->GetPreviousSibling(getter_AddRefs(prev));

      nsCOMPtr<nsIContent> prevSibling;
      if (prev) {
        prevSibling = do_QueryInterface(prev);
      }

      for (uint32_t i = 0; i < removedLength; i++) {
        nsCOMPtr<nsIDOMNode> item;
        removedNodes->Item(i, getter_AddRefs(item));

        nsCOMPtr<nsIContent> removed = do_QueryInterface(item);
        NS_ASSERTION(removed, "Node failed to QI to nsIContent");
        if (!removed) {
          continue;
        }

        ContentRemoved(target, removed);
      }

      for (uint32_t i = 0; i < addedLength; i++) {
        nsCOMPtr<nsIDOMNode> item;
        addedNodes->Item(i, getter_AddRefs(item));

        nsCOMPtr<nsIContent> added = do_QueryInterface(item);
        NS_ASSERTION(added, "Node failed to QI to nsIContent");
        if (!added) {
          continue;
        }

        ContentInserted(target, added, prevSibling);
        prevSibling = added;
      }
    } else {
      NS_WARNING("Got a mutation record that we don't care about");
    }
  }
}

void
uGlobalMenuDocListener::AttributeChanged(nsIContent *aContent,
                                         nsAString& aAttribute)
{
  if (!aContent || aAttribute.IsEmpty()) {
    return;
  }

  nsIAtomService *as = uGlobalMenuService::GetAtomService();
  if (!as) {
    return;
  }

  nsIAtom *attribute;
  as->GetAtom(aAttribute, &attribute);

  nsTArray<uGlobalMenuObject *> *listeners =
    GetListenersForContent(aContent, false);

  if (listeners) {
    for (uint32_t i = 0; i < listeners->Length(); i++) {
      listeners->ElementAt(i)->ObserveAttributeChanged(aContent, attribute);
    }
  }
}

void
uGlobalMenuDocListener::ContentRemoved(nsIContent *aContainer,
                                       nsIContent *aChild)
{
  TRACE()
  LOG("aContainer=%p, aChild=%p",
      (void *)aContainer, (void *)aChild);

  if (!aContainer || !aChild) {
    return;
  }

  nsTArray<uGlobalMenuObject *> *listeners =
    GetListenersForContent(aContainer, false);

  if (listeners) {
    for (uint32_t i = 0; i < listeners->Length(); i++) {
      listeners->ElementAt(i)->ObserveContentRemoved(aContainer, aChild);
    }
  }
}

void
uGlobalMenuDocListener::ContentInserted(nsIContent *aContainer,
                                        nsIContent *aChild,
                                        nsIContent *aPrevSibling)
{
  TRACE()
  LOG("aContainer=%p, aChild=%p, aPrevSibling=%p",
      (void *)aContainer, (void *)aChild, (void *)aPrevSibling);

  if (!aContainer || !aChild) {
    return;
  }

  nsTArray<uGlobalMenuObject *> *listeners =
    GetListenersForContent(aContainer, false);

  if (listeners) {
    for (uint32_t i = 0; i < listeners->Length(); i++) {
      listeners->ElementAt(i)->ObserveContentInserted(aContainer, aChild,
                                                      aPrevSibling);
    }
  }
}

void
uGlobalMenuDocListener::RegisterForContentChanges(nsIContent *aContent,
                                                  uGlobalMenuObject *aMenuObject)
{
  NS_ASSERTION(aContent && aMenuObject, "Invalid args");
  if (!aContent || !aMenuObject) {
    return;
  }

  nsTArray<uGlobalMenuObject *> *listeners =
    GetListenersForContent(aContent, true);

  listeners->AppendElement(aMenuObject);
}

void
uGlobalMenuDocListener::UnregisterForContentChanges(nsIContent *aContent,
                                                    uGlobalMenuObject *aMenuObject)
{
  NS_ASSERTION(aContent && aMenuObject, "Invalid args");
  if (!aContent || !aMenuObject) {
    return;
  }

  nsTArray<uGlobalMenuObject *> *listeners =
    GetListenersForContent(aContent, false);
  if (!listeners) {
    return;
  }

  if (listeners->RemoveElement(aMenuObject) && listeners->Length() == 0) {
    mContentToObserverTable.Remove(aContent);
  }
}

nsTArray<uGlobalMenuObject *>*
uGlobalMenuDocListener::GetListenersForContent(nsIContent *aContent,
                                               bool aCreate)
{
  nsTArray<uGlobalMenuObject *> *listeners;
  if (!mContentToObserverTable.Get(aContent, &listeners) && aCreate) {
    listeners = new nsTArray<uGlobalMenuObject *>();
    mContentToObserverTable.Put(aContent, listeners);
  }

  return listeners;
}

void
uGlobalMenuDocListener::FlushPendingMutations()
{
  if (!mObserver) {
    // We've been destroyed already
    return;
  }

  HandleMutations(mPendingMutations);
  mPendingMutations.Clear();
}

/*static*/void
uGlobalMenuDocListener::LeaveCriticalZone()
{
  if (sInhibitDepth == 1 && sPendingListeners) {
    while (sPendingListeners->Length() > 0) {
      (*sPendingListeners)[0]->FlushPendingMutations();
      sPendingListeners->RemoveElementAt(0);
    }

    delete sPendingListeners;
    sPendingListeners = nullptr;
  }

  NS_ASSERTION(sInhibitDepth > 0, "Negative inhibit depth!");

  sInhibitDepth--;
}

/*static*/void
uGlobalMenuDocListener::ScheduleListener(uGlobalMenuDocListener *aListener)
{
  NS_ASSERTION(sInhibitDepth > 0, "Shouldn't be doing this now");

  if (!sPendingListeners) {
    sPendingListeners = new nsTArray<nsCOMPtr<uGlobalMenuDocListener> >;
  }

  if (sPendingListeners->IndexOf(aListener) == -1) {
    sPendingListeners->AppendElement(aListener);
  }
}

/*static*/void
uGlobalMenuDocListener::Shutdown()
{
  delete sPendingListeners;
  sPendingListeners = nullptr;
}
