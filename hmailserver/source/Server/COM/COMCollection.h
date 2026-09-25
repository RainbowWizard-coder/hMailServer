// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The business object a COM item wraps, and the collection it came from or is
// saved into.
//
// A CHANGE TO AN ITEM OF A SHARED COLLECTION IS WRITTEN ONTO A COPY. Some of
// the collections COM hands out are the running server's own - the SMTP
// configuration's routes, incoming relays and remote domain policies, the
// anti-spam configuration's DNS black lists and SURBL servers, the server
// messages, the blocked attachments, the SSL certificates, the IMAP groups and
// a running route's addresses - and their items are what every session reads.
// A put_* that wrote such an item's fields in place wrote them under the
// sessions reading them: a string assigned while a session copies it frees the
// buffer being copied, the use-after-free of the heap-corruption abort one
// level below the vector, and a record written one field at a time hands a
// reader half of each. So an item handed out of such a collection
// (AttachSharedParent) is copied at the first put_*, from the running item
// with its id, and this object holds and writes the copy from then on; its
// get_* read the copy. Save() saves the copy and then reads the collection
// again, which swaps in new items under its lock while a session keeps the one
// it holds. A Save() with no put_* before it copies first as well (each item
// class's Save calls Writable_ before it saves): the running item is never
// what a persister is handed - PersistentRemoteDomainPolicy trims the key of
// the object it saves - and an object handed out before a refresh replaced
// its item saves the running item's values, not the ones it was handed. A new
// item - Add() - is nobody else's until its Save() appends it, and is written
// directly until then. Roadmap2, "A COM change to a shared configuration
// object is written onto a copy"; the REST updates have the same shape, and
// build/check-com-shared-copies.py holds the ten item classes to it.
//
// What a caller sees: a put_* is not in effect until Save() - the running
// server, and any other COM object read from the collection before the Save,
// keep the values they had - and a put_* never followed by a Save() changes
// nothing at all. After the Save a COM object read from the collection again
// has the saved values; one read before it keeps what it read.
//
// When the collection cannot be read again after a successful save (error
// 7390), the change is in the database and not in the running server, and the
// running item still has the old values. A later change to the same item
// starts from the running item, so its save writes those old values back over
// the first change; the error says so. The next successful read of the list
// ends it.
template <class T, class P>
class COMCollectionItem
{
public:

   COMCollectionItem() :
      object_is_in_collection_(false),
      parent_is_shared_(false),
      holds_copy_(false)
   {
   }

   void AttachItem(std::shared_ptr<T> pObject)
   {
      object_ = pObject;
   }

   void AttachParent(std::shared_ptr<P> pParentColl, bool bObjectIsInParent)
   {
      parent_collection_ = pParentColl;
      object_is_in_collection_ = bObjectIsInParent;
   }

   // As AttachParent, for a collection the running server reads from other
   // threads: a put_* on an item of it writes a copy (above).
   void AttachSharedParent(std::shared_ptr<P> pParentColl, bool bObjectIsInParent)
   {
      AttachParent(pParentColl, bObjectIsInParent);
      parent_is_shared_ = true;
   }

   void AddToParentCollection()
   {
      if (object_is_in_collection_ || !parent_collection_)
         return;

      parent_collection_->AddItem(object_);
      object_is_in_collection_ = true;
   }

   std::shared_ptr<P> parent_collection_;
   std::shared_ptr<T> object_;

protected:

   // True while object_ is itself an item of a shared collection, so that a
   // put_* must not write it.
   bool ObjectIsShared_() const
   {
      return parent_is_shared_ && object_is_in_collection_ && object_ != nullptr;
   }

   // True once object_ is this object's own copy of an item of the collection.
   bool HoldsCopy_() const
   {
      return holds_copy_;
   }

   // The item of the collection with object_'s id, as the running server has
   // it now - a refresh since this object was handed out, by another COM
   // object or by REST, has replaced the one it was given - or object_ when
   // the collection has no such item (a new one, or one deleted since).
   std::shared_ptr<T> RunningOrHeld_() const
   {
      if (parent_collection_ && object_ && object_->GetID() != 0)
      {
         std::shared_ptr<T> running = parent_collection_->GetItemByDBID(object_->GetID());
         if (running)
            return running;
      }

      return object_;
   }

   // What a put_* writes: object_, copied first while it is an item of a
   // shared collection. T's copy constructor makes the copy; a class whose
   // object cannot be copied that way (Route, which holds a lock) copies it
   // itself and hands the copy to HoldCopy_.
   std::shared_ptr<T> Writable_()
   {
      if (ObjectIsShared_())
         HoldCopy_(std::shared_ptr<T>(new T(*RunningOrHeld_())));

      return object_;
   }

   void HoldCopy_(std::shared_ptr<T> copy)
   {
      object_ = copy;
      object_is_in_collection_ = false;
      holds_copy_ = true;
   }

   // After a successful save: a new item joins the collection, and a copy
   // reaches the running server by the collection being read again. A read
   // that fails leaves the collection as it was (Collection::DBLoad_), so the
   // change is in the database and not yet in effect; that is reported, and
   // the caller's Save still succeeds, since the save did.
   void PublishSaved_(const HM::String &source)
   {
      if (!holds_copy_)
      {
         AddToParentCollection();
         return;
      }

      if (parent_collection_ && !parent_collection_->Refresh())
      {
         HM::ErrorManager::Instance()->ReportError(HM::ErrorManager::Medium, 7390, source,
            "The change was saved to the database, but the list it belongs to could not be read again, "
            "so the running server goes on using the item as it was until the list is next read. "
            "A change made to the same item before then starts from the item as it was, and saving it "
            "writes the old values back over this one.");
      }
   }

private:

   bool object_is_in_collection_;
   bool parent_is_shared_;
   bool holds_copy_;
};
