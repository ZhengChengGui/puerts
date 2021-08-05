/*
* Tencent is pleased to support the open source community by making Puerts available.
* Copyright (C) 2020 THL A29 Limited, a Tencent company.  All rights reserved.
* Puerts is licensed under the BSD 3-Clause License, except for the third-party components listed in the file 'LICENSE' which may be subject to their corresponding license terms.
* This file is subject to the terms and conditions defined in file 'LICENSE', which is part of this source code package.
*/

#include "CppObjectMapper.h"
#include "DataTransfer.h"

namespace puerts
{

void FCppObjectMapper::LoadCppType(const v8::FunctionCallbackInfo<v8::Value>& Info)
{
    v8::Isolate* Isolate = Info.GetIsolate();
    v8::Isolate::Scope IsolateScope(Isolate);
    v8::HandleScope HandleScope(Isolate);
    v8::Local<v8::Context> Context = Isolate->GetCurrentContext();
    v8::Context::Scope ContextScope(Context);

    if (!Info[0]->IsString())
    {
        DataTransfer::ThrowException(Isolate, "#0 argument expect a string");
        return;
    }

    std::string TypeName = *(v8::String::Utf8Value(Isolate, Info[0]));

    auto ClassDef = FindCppTypeClassByName(TypeName);
    if (ClassDef)
    {
        Info.GetReturnValue().Set(GetTemplateOfClass(Isolate, ClassDef)->GetFunction(Context).ToLocalChecked());
    }
    else
    {
        const std::string ErrMsg = "can not find type: " + TypeName;
        DataTransfer::ThrowException(Isolate, ErrMsg.c_str());
    }
}

static void PointerNew(const v8::FunctionCallbackInfo<v8::Value>& Info)
{
    //do nothing
}
    
void FCppObjectMapper::Initialize(v8::Isolate* InIsolate, v8::Local<v8::Context> InContext)
{
    auto LocalTemplate = v8::FunctionTemplate::New(InIsolate, PointerNew);
    LocalTemplate->InstanceTemplate()->SetInternalFieldCount(4);//0 Ptr, 1, CDataName
    PointerConstrutor = v8::UniquePersistent<v8::Function>(InIsolate, LocalTemplate->GetFunction(InContext).ToLocalChecked());
}

v8::Local<v8::Value> FCppObjectMapper::FindOrAddCppObject(v8::Isolate* Isolate, v8::Local<v8::Context> &Context, const char* CDataName, void *Ptr, bool PassByPointer)
{
    if (Ptr == nullptr)
    {
        return v8::Undefined(Isolate);
    }

    if (!PassByPointer)
    {
        auto Iter = CDataMap.find(Ptr);
        if (Iter != CDataMap.end())
        {
            return v8::Local<v8::Value>::New(Isolate, Iter->second);
        }
    }

    //create and link
    auto BindTo = v8::External::New(Context->GetIsolate(), Ptr);
    v8::Handle<v8::Value> Args[] = { BindTo, v8::Boolean::New(Isolate, PassByPointer) };
    auto ClassDefinition = FindClassByID(CDataName);
    if (ClassDefinition)
    {
        return GetTemplateOfClass(Isolate, ClassDefinition)->GetFunction(Context).ToLocalChecked()->NewInstance(Context, 2, Args).ToLocalChecked();
    }
    else
    {
        auto Result = PointerConstrutor.Get(Isolate)->NewInstance(Context, 0, nullptr).ToLocalChecked();
        DataTransfer::SetPointer(Isolate, Result, Ptr, 0);
        DataTransfer::SetPointer(Isolate, Result, const_cast<char*>(CDataName), 1);
        return Result;
    }
}	
	
bool FCppObjectMapper::IsInstanceOfCppObject(const char* CDataName, v8::Local<v8::Object> JsObject)
{
    return DataTransfer::GetPoninterFast<const char>(JsObject, 1) == CDataName;
}


static void CDataNew(const v8::FunctionCallbackInfo<v8::Value>& Info)
{
    v8::Isolate* Isolate = Info.GetIsolate();
    v8::Isolate::Scope IsolateScope(Isolate);
    v8::HandleScope HandleScope(Isolate);
    v8::Local<v8::Context> Context = Isolate->GetCurrentContext();
    v8::Context::Scope ContextScope(Context);

    if (Info.IsConstructCall())
    {
        auto Self = Info.This();
        JSClassDefinition* ClassDefinition = reinterpret_cast<JSClassDefinition*>((v8::Local<v8::External>::Cast(Info.Data()))->Value());
        void* Ptr = nullptr;
        bool PassByPointer = false;

        if (Info.Length() == 2 && Info[0]->IsExternal()) //Call by Native
        {
            Ptr = v8::Local<v8::External>::Cast(Info[0])->Value();
            PassByPointer = Info[1]->BooleanValue(Isolate);
        }
        else // Call by js new
        {
            if(ClassDefinition->Initialize) Ptr = ClassDefinition->Initialize(Info);
        }
        static_cast<ICppObjectMapper*>(Isolate->GetData(MAPPER_ISOLATE_DATA_POS))->BindCppObject(Isolate, ClassDefinition, Ptr, Self, PassByPointer);
    }
    else
    {
        DataTransfer::ThrowException(Isolate, "only call as Construct is supported!");
    }
}

v8::Local<v8::FunctionTemplate> FCppObjectMapper::GetTemplateOfClass(v8::Isolate* Isolate, const JSClassDefinition* ClassDefinition)
{
    auto Iter = CDataNameToTemplateMap.find(ClassDefinition->CPPTypeName);
    if (Iter == CDataNameToTemplateMap.end())
    {
        v8::EscapableHandleScope HandleScope(Isolate);

        auto Template = v8::FunctionTemplate::New(Isolate, CDataNew, v8::External::New(Isolate, const_cast<void *>(reinterpret_cast<const void*>(ClassDefinition))));
        Template->InstanceTemplate()->SetInternalFieldCount(4);

        JSPropertyInfo* PropertyInfo = ClassDefinition->Properties;
        while (PropertyInfo && PropertyInfo->Name && PropertyInfo->Getter)
        {
            v8::PropertyAttribute PropertyAttribute = v8::DontDelete;
            if (!PropertyInfo->Setter) PropertyAttribute = (v8::PropertyAttribute)(PropertyAttribute | v8::ReadOnly);
            Template->PrototypeTemplate()->SetAccessor(v8::String::NewFromUtf8(Isolate, PropertyInfo->Name).ToLocalChecked(), PropertyInfo->Getter, PropertyInfo->Setter,
                PropertyInfo->Data ? static_cast<v8::Local<v8::Value>>(v8::External::New(Isolate, PropertyInfo->Data)): v8::Local<v8::Value>(), v8::DEFAULT, PropertyAttribute);
            ++PropertyInfo;
        }

        JSFunctionInfo* FunctionInfo = ClassDefinition->Methods;
        while (FunctionInfo && FunctionInfo->Name && FunctionInfo->Callback)
        {
            Template->PrototypeTemplate()->Set(v8::String::NewFromUtf8(Isolate, FunctionInfo->Name).ToLocalChecked(), v8::FunctionTemplate::New(Isolate, FunctionInfo->Callback,
                FunctionInfo->Data ? static_cast<v8::Local<v8::Value>>(v8::External::New(Isolate, FunctionInfo->Data)): v8::Local<v8::Value>()));
            ++FunctionInfo;
        }
        FunctionInfo = ClassDefinition->Functions;
        while (FunctionInfo && FunctionInfo->Name && FunctionInfo->Callback)
        {
            Template->Set(v8::String::NewFromUtf8(Isolate, FunctionInfo->Name).ToLocalChecked(), v8::FunctionTemplate::New(Isolate, FunctionInfo->Callback,
                FunctionInfo->Data ? static_cast<v8::Local<v8::Value>>(v8::External::New(Isolate, FunctionInfo->Data)): v8::Local<v8::Value>()));
            ++FunctionInfo;
        }

        if (ClassDefinition->CPPSuperTypeName)
        {
            if (auto SuperDefinition = FindClassByID(ClassDefinition->CPPSuperTypeName))
            {
                Template->Inherit(GetTemplateOfClass(Isolate, SuperDefinition));
            }
        }

        CDataNameToTemplateMap[ClassDefinition->CPPTypeName] = v8::UniquePersistent<v8::FunctionTemplate>(Isolate, Template);

        return HandleScope.Escape(Template);
    }
    else
    {
        return v8::Local<v8::FunctionTemplate>::New(Isolate, Iter->second);
    }
}

static void CDataGarbageCollectedWithFree(const v8::WeakCallbackInfo<JSClassDefinition>& Data)
{
    JSClassDefinition *ClassDefinition = Data.GetParameter();
    void *Ptr = DataTransfer::MakeAddressWithHighPartOfTwo(Data.GetInternalField(0), Data.GetInternalField(1));
    if (ClassDefinition->Finalize) ClassDefinition->Finalize(Ptr);
    static_cast<ICppObjectMapper*>(Data.GetIsolate()->GetData(MAPPER_ISOLATE_DATA_POS))->UnBindCppObject(ClassDefinition, Ptr);
}

void FCppObjectMapper::BindCppObject(v8::Isolate* Isolate, JSClassDefinition* ClassDefinition, void *Ptr, v8::Local<v8::Object> JSObject, bool PassByPointer)
{
    DataTransfer::SetPointer(Isolate, JSObject, Ptr, 0);
    DataTransfer::SetPointer(Isolate, JSObject, const_cast<char*>(ClassDefinition->CPPTypeName), 1);

    if(!PassByPointer)//指针传递不用处理GC
    {
        CDataMap[Ptr] = v8::UniquePersistent<v8::Value>(Isolate, JSObject);
        CDataFinalizeMap[Ptr] = ClassDefinition->Finalize;
        CDataMap[Ptr].SetWeak<JSClassDefinition>(ClassDefinition, CDataGarbageCollectedWithFree, v8::WeakCallbackType::kInternalFields);
    }
}

void FCppObjectMapper::UnBindCppObject(JSClassDefinition* ClassDefinition, void *Ptr)
{
    CDataFinalizeMap.erase(Ptr);
    CDataMap.erase(Ptr);
}

void FCppObjectMapper::UnInitialize(v8::Isolate* InIsolate)
{
    for (auto Iter = CDataMap.begin(); Iter != CDataMap.end(); Iter++)
    {
        Iter->second.Reset();
    }

    for (auto Iter = CDataFinalizeMap.begin(); Iter != CDataFinalizeMap.end(); Iter++)
    {
        if(Iter->second) Iter->second(Iter->first);
    }

    for (auto Iter = CDataNameToTemplateMap.begin(); Iter != CDataNameToTemplateMap.end(); Iter++)
    {
        Iter->second.Reset();
    }
    
    PointerConstrutor.Reset();
}

}
