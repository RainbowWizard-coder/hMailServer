// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{

   // class IScriptObject;

   class ScriptObject
   {  
   public:   
      enum ObjectType
      {
         OTResult = 1000,
         OTMessage = 1001,
         OTClient = 1002,
         OTEventLog = 1003,
         OTFetchAccount = 1004,
         OTAccount = 1005
      };

      // Made whole, with its type: two switches read eType
      // (ScriptObjectContainer.cpp, ScriptBinding.cpp), and an object made
      // without one would have them read whatever the heap held there.
      ScriptObject(const String &name, std::shared_ptr<void> object, ObjectType type) :
         sName(name),
         pObject(object),
         eType(type)
      {
      }

      String sName;
      std::shared_ptr<void> pObject;
      ObjectType eType;

   };

   // One value a JavaScript handler is called with.
   //
   // VBScript and JScript are handed their call as source text that the event's
   // caller builds and quotes - OnError(3, 5019, "source", "description") - and
   // that text is appended to the script. The embedded JavaScript engine is not:
   // it calls the handler with JS_Call and real values, so nothing is quoted and a
   // password or an error description cannot end a string literal early. A caller
   // that passes a value beside its objects adds it here, in the order the
   // handler's parameters take it; the objects come first, in the order
   // ScriptServer names them for the event. The Windows engines never read these.
   class ScriptArgument
   {
   public:
      enum Kind
      {
         KindText = 1,
         KindNumber = 2
      };

      explicit ScriptArgument(const String &value);
      explicit ScriptArgument(long long value);

      Kind kind;
      String text;
      long long number;
   };

   class ScriptObjectContainer
   {
   public:
      ScriptObjectContainer(void);
      ~ScriptObjectContainer(void);

      void AddObject(const String &sName, ScriptObject::ObjectType type);
      void AddObject(const String &sName, std::shared_ptr<void> pObj, ScriptObject::ObjectType type);

      // The object added under the name, or null. Named Find rather than Get
      // because GetObject is a Windows macro.
      std::shared_ptr<ScriptObject> FindObject(const String &sName) const;

      // The values the JavaScript engine passes after the event's objects - see
      // ScriptArgument. Added on every platform, because the caller does not know
      // which engine will run; the Windows engines ignore them.
      void AddTextArgument(const String &value);
      void AddNumberArgument(long long value);
      const std::vector<ScriptArgument> &GetArguments() const;

      // The function a rule's "Run function" action names (ScriptServer::EventCustom).
      // The Windows engines are handed "Name(HMAILSERVER_MESSAGE)" as text; the
      // JavaScript engine looks the name up and calls it.
      void SetProcedureName(const String &name);
      String GetProcedureName() const;

      std::vector<String> GetObjectNames();
      // Handing a named object to the script engine means handing it an IUnknown,
      // and the only caller is ScriptSite - the ATL Active Scripting host. COM has
      // no POSIX counterpart, so this accessor belongs to the Windows build alone;
      // see the roadmap section "Linux and AArch64". The container itself is not
      // Windows-specific and every other member of it compiles on both.
#ifdef _MSC_VER
      bool GetObjectByName(const String &sName, LPUNKNOWN* ppunkItem) const;
#endif
   
   private:

      std::map<String, std::shared_ptr<ScriptObject> > objects_;
      std::vector<ScriptArgument> arguments_;
      String procedure_name_;
   };
}